/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Internal interfaces of LOV layer.
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 *   Author: Jinshan Xiong <jinshan.xiong@intel.com>
 */

#ifndef LOV_CL_INTERNAL_H
#define LOV_CL_INTERNAL_H

#include <libcfs/libcfs.h>
#include <obd.h>
#include <cl_object.h>
#include "lov_internal.h"

/** \defgroup lov lov
 * Logical object volume layer. This layer implements data striping (raid0).
 *
 * At the lov layer top-entity (object, page, lock, io) is connected to one or
 * more sub-entities: top-object, representing a file is connected to a set of
 * sub-objects, each representing a stripe, file-level top-lock is connected
 * to a set of per-stripe sub-locks, top-page is connected to a (single)
 * sub-page, and a top-level IO is connected to a set of (potentially
 * concurrent) sub-IO's.
 *
 * Sub-object, sub-page, and sub-io have well-defined top-object and top-page
 * respectively, while a single sub-lock can be part of multiple top-locks.
 *
 * Reference counting models are different for different types of entities:
 *
 *     - top-object keeps a reference to its sub-objects, and destroys them
 *       when it is destroyed.
 *
 *     - top-page keeps a reference to its sub-page, and destroys it when it
 *       is destroyed.
 *
 *     - IO's are not reference counted.
 *
 * To implement a connection between top and sub entities, lov layer is split
 * into two pieces: lov ("upper half"), and lovsub ("bottom half"), both
 * implementing full set of cl-interfaces. For example, top-object has vvp and
 * lov layers, and it's sub-object has lovsub and osc layers. lovsub layer is
 * used to track child-parent relationship.
 *
 * @{
 */

struct lovsub_device;
struct lovsub_object;
struct lovsub_lock;

enum lov_device_flags {
        LOV_DEV_INITIALIZED = 1 << 0
};

/*
 * Upper half.
 */

struct lov_device {
        /*
         * XXX Locking of lov-private data is missing.
         */
        struct cl_device          ld_cl;
        struct lov_obd           *ld_lov;
        /** size of lov_device::ld_target[] array */
        __u32                     ld_target_nr;
        struct lovsub_device    **ld_target;
        __u32                     ld_flags;
};

/**
 * Layout type.
 */
enum lov_layout_type {
	LLT_EMPTY,	/** empty file without body (mknod + truncate) */
	LLT_RELEASED,	/** file with no objects (data in HSM) */
	LLT_COMP,	/** support composite layout */
	LLT_NR
};

static inline char *llt2str(enum lov_layout_type llt)
{
	switch (llt) {
	case LLT_EMPTY:
		return "EMPTY";
	case LLT_RELEASED:
		return "RELEASED";
	case LLT_COMP:
		return "COMPOSITE";
	case LLT_NR:
		LBUG();
	}
	LBUG();
	return "";
}

struct lov_layout_raid0 {
	unsigned               lo_nr;
	/**
	 * When this is true, lov_object::lo_attr contains
	 * valid up to date attributes for a top-level
	 * object. This field is reset to 0 when attributes of
	 * any sub-object change.
	 */
	int		       lo_attr_valid;
	/**
	 * Array of sub-objects. Allocated when top-object is
	 * created (lov_init_raid0()).
	 *
	 * Top-object is a strict master of its sub-objects:
	 * it is created before them, and outlives its
	 * children (this later is necessary so that basic
	 * functions like cl_object_top() always
	 * work). Top-object keeps a reference on every
	 * sub-object.
	 *
	 * When top-object is destroyed (lov_delete_raid0())
	 * it releases its reference to a sub-object and waits
	 * until the latter is finally destroyed.
	 */
	struct lovsub_object **lo_sub;
	/**
	 * protect lo_sub
	 */
	spinlock_t		lo_sub_lock;
	/**
	 * Cached object attribute, built from sub-object
	 * attributes.
	 */
	struct cl_attr         lo_attr;
};

/**
 * lov-specific file state.
 *
 * lov object has particular layout type, determining how top-object is built
 * on top of sub-objects. Layout type can change dynamically. When this
 * happens, lov_object::lo_type_guard semaphore is taken in exclusive mode,
 * all state pertaining to the old layout type is destroyed, and new state is
 * constructed. All object methods take said semaphore in the shared mode,
 * providing serialization against transition between layout types.
 *
 * To avoid multiple `if' or `switch' statements, selecting behavior for the
 * current layout type, object methods perform double-dispatch, invoking
 * function corresponding to the current layout type.
 */
struct lov_object {
	struct cl_object       lo_cl;
	/**
	 * Serializes object operations with transitions between layout types.
	 *
	 * This semaphore is taken in shared mode by all object methods, and
	 * is taken in exclusive mode when object type is changed.
	 *
	 * \see lov_object::lo_type
	 */
	struct rw_semaphore	lo_type_guard;
	/**
	 * Type of an object. Protected by lov_object::lo_type_guard.
	 */
	enum lov_layout_type	lo_type;
	/**
	 * True if layout is invalid. This bit is cleared when layout lock
	 * is lost.
	 */
	bool			lo_layout_invalid;
	/**
	 * How many IOs are on going on this object. Layout can be changed
	 * only if there is no active IO.
	 */
	atomic_t	       lo_active_ios;
	/**
	 * Waitq - wait for no one else is using lo_lsm
	 */
	wait_queue_head_t	lo_waitq;
	/**
	 * Layout metadata. NULL if empty layout.
	 */
	struct lov_stripe_md  *lo_lsm;

	union lov_layout_state {
		struct lov_layout_state_empty {
		} empty;
		struct lov_layout_state_released {
		} released;
		struct lov_layout_composite {
			/**
			 * Current valid entry count of lo_entries.
			 */
			unsigned int lo_entry_count;
			struct lov_layout_entry {
				struct lu_extent lle_extent;
				struct lov_layout_raid0 lle_raid0;
			} *lo_entries;
		} composite;
	} u;
	/**
	 * Thread that acquired lov_object::lo_type_guard in an exclusive
	 * mode.
	 */
	struct task_struct            *lo_owner;
};

#define lov_foreach_layout_entry(lov, entry)			\
	for (entry = &lov->u.composite.lo_entries[0];		\
	     entry < &lov->u.composite.lo_entries		\
			[lov->u.composite.lo_entry_count];	\
	     entry++)

/**
 * State lov_lock keeps for each sub-lock.
 */
struct lov_lock_sub {
	/** sub-lock itself */
	struct cl_lock		sub_lock;
	/** Set if the sublock has ever been enqueued, meaning it may
	 * hold resources of underlying layers */
	unsigned int		sub_is_enqueued:1,
				sub_initialized:1;
	int			sub_index;
};

/**
 * lov-specific lock state.
 */
struct lov_lock {
	struct cl_lock_slice	lls_cl;
	/** Number of sub-locks in this lock */
	int			lls_nr;
	/** sublock array */
	struct lov_lock_sub	lls_sub[0];
};

struct lov_page {
	struct cl_page_slice	lps_cl;
	/** layout_entry + stripe index, composed using lov_comp_index() */
	unsigned int		lps_index;
};

/*
 * Bottom half.
 */

struct lovsub_device {
        struct cl_device   acid_cl;
        struct cl_device  *acid_next;
};

struct lovsub_object {
        struct cl_object_header lso_header;
        struct cl_object        lso_cl;
        struct lov_object      *lso_super;
        int                     lso_index;
};

/**
 * Lock state at lovsub layer.
 */
struct lovsub_lock {
        struct cl_lock_slice  lss_cl;
};

/**
 * Describe the environment settings for sublocks.
 */
struct lov_sublock_env {
        const struct lu_env *lse_env;
        struct cl_io        *lse_io;
};

struct lovsub_page {
        struct cl_page_slice lsb_cl;
};


struct lov_thread_info {
	struct cl_object_conf   lti_stripe_conf;
	struct lu_fid           lti_fid;
	struct ost_lvb          lti_lvb;
	struct cl_2queue        lti_cl2q;
	struct cl_page_list     lti_plist;
	wait_queue_t		lti_waiter;
};

/**
 * State that lov_io maintains for every sub-io.
 */
struct lov_io_sub {
	/**
	 * Linkage into a list (hanging off lov_io::lis_subios)
	 */
	struct list_head	sub_list;
	/**
	 * Linkage into a list (hanging off lov_io::lis_active) of all
	 * sub-io's active for the current IO iteration.
	 */
	struct list_head	sub_linkage;
	unsigned int		sub_subio_index;
	/**
	 * sub-io for a stripe. Ideally sub-io's can be stopped and resumed
	 * independently, with lov acting as a scheduler to maximize overall
	 * throughput.
	 */
	struct cl_io		sub_io;
	/**
	 * environment, in which sub-io executes.
	 */
	struct lu_env		*sub_env;
	/**
	 * environment's refcheck.
	 *
	 * \see cl_env_get()
	 */
	__u16			sub_refcheck;
	__u16			sub_reenter;
};

/**
 * IO state private for LOV.
 */
struct lov_io {
        /** super-class */
        struct cl_io_slice lis_cl;
        /**
         * Pointer to the object slice. This is a duplicate of
         * lov_io::lis_cl::cis_object.
         */
        struct lov_object *lis_object;
        /**
         * Original end-of-io position for this IO, set by the upper layer as
         * cl_io::u::ci_rw::pos + cl_io::u::ci_rw::count. lov remembers this,
         * changes pos and count to fit IO into a single stripe and uses saved
         * value to determine when IO iterations have to stop.
         *
         * This is used only for CIT_READ and CIT_WRITE io's.
         */
        loff_t             lis_io_endpos;

        /**
         * starting position within a file, for the current io loop iteration
         * (stripe), used by ci_io_loop().
         */
	loff_t			lis_pos;
	/**
	 * end position with in a file, for the current stripe io. This is
	 * exclusive (i.e., next offset after last byte affected by io).
	 */
	loff_t			lis_endpos;
	int			lis_nr_subios;

	/**
	 * the index of ls_single_subio in ls_subios array
	 */
	int			lis_single_subio_index;
	struct lov_io_sub	lis_single_subio;

	/**
	 * List of active sub-io's. Active sub-io's are under the range
	 * of [lis_pos, lis_endpos).
	 */
	struct list_head	lis_active;
	/**
	 * All sub-io's created in this lov_io.
	 */
	struct list_head	lis_subios;
};

struct lov_session {
        struct lov_io          ls_io;
        struct lov_sublock_env ls_subenv;
};

extern struct lu_device_type lov_device_type;
extern struct lu_device_type lovsub_device_type;

extern struct lu_context_key lov_key;
extern struct lu_context_key lov_session_key;

extern struct kmem_cache *lov_lock_kmem;
extern struct kmem_cache *lov_object_kmem;
extern struct kmem_cache *lov_thread_kmem;
extern struct kmem_cache *lov_session_kmem;

extern struct kmem_cache *lovsub_lock_kmem;
extern struct kmem_cache *lovsub_object_kmem;

int   lov_object_init     (const struct lu_env *env, struct lu_object *obj,
                           const struct lu_object_conf *conf);
int   lovsub_object_init  (const struct lu_env *env, struct lu_object *obj,
                           const struct lu_object_conf *conf);
int   lov_lock_init       (const struct lu_env *env, struct cl_object *obj,
                           struct cl_lock *lock, const struct cl_io *io);
int   lov_io_init         (const struct lu_env *env, struct cl_object *obj,
                           struct cl_io *io);
int   lovsub_lock_init    (const struct lu_env *env, struct cl_object *obj,
                           struct cl_lock *lock, const struct cl_io *io);

int   lov_lock_init_composite(const struct lu_env *env, struct cl_object *obj,
                           struct cl_lock *lock, const struct cl_io *io);
int   lov_lock_init_empty (const struct lu_env *env, struct cl_object *obj,
                           struct cl_lock *lock, const struct cl_io *io);
int   lov_io_init_composite(const struct lu_env *env, struct cl_object *obj,
                           struct cl_io *io);
int   lov_io_init_empty   (const struct lu_env *env, struct cl_object *obj,
                           struct cl_io *io);
int   lov_io_init_released(const struct lu_env *env, struct cl_object *obj,
                           struct cl_io *io);

struct lov_io_sub *lov_sub_get(const struct lu_env *env, struct lov_io *lio,
                               int stripe);

int   lov_page_init       (const struct lu_env *env, struct cl_object *ob,
			   struct cl_page *page, pgoff_t index);
int   lovsub_page_init    (const struct lu_env *env, struct cl_object *ob,
			   struct cl_page *page, pgoff_t index);
int   lov_page_init_empty (const struct lu_env *env, struct cl_object *obj,
			   struct cl_page *page, pgoff_t index);
int   lov_page_init_composite(const struct lu_env *env, struct cl_object *obj,
			   struct cl_page *page, pgoff_t index);
struct lu_object *lov_object_alloc   (const struct lu_env *env,
                                      const struct lu_object_header *hdr,
                                      struct lu_device *dev);
struct lu_object *lovsub_object_alloc(const struct lu_env *env,
                                      const struct lu_object_header *hdr,
                                      struct lu_device *dev);

struct lov_stripe_md *lov_lsm_addref(struct lov_object *lov);
int lov_page_stripe(const struct cl_page *page);
int lov_lsm_entry(const struct lov_stripe_md *lsm, __u64 offset);

#define lov_foreach_target(lov, var)                    \
        for (var = 0; var < lov_targets_nr(lov); ++var)

/*****************************************************************************
 *
 * Type conversions.
 *
 * Accessors.
 *
 */

static inline struct lov_session *lov_env_session(const struct lu_env *env)
{
        struct lov_session *ses;

        ses = lu_context_key_get(env->le_ses, &lov_session_key);
        LASSERT(ses != NULL);
        return ses;
}

static inline struct lov_io *lov_env_io(const struct lu_env *env)
{
        return &lov_env_session(env)->ls_io;
}

static inline int lov_is_object(const struct lu_object *obj)
{
        return obj->lo_dev->ld_type == &lov_device_type;
}

static inline int lovsub_is_object(const struct lu_object *obj)
{
        return obj->lo_dev->ld_type == &lovsub_device_type;
}

static inline struct lu_device *lov2lu_dev(struct lov_device *lov)
{
        return &lov->ld_cl.cd_lu_dev;
}

static inline struct lov_device *lu2lov_dev(const struct lu_device *d)
{
        LINVRNT(d->ld_type == &lov_device_type);
        return container_of0(d, struct lov_device, ld_cl.cd_lu_dev);
}

static inline struct cl_device *lovsub2cl_dev(struct lovsub_device *lovsub)
{
        return &lovsub->acid_cl;
}

static inline struct lu_device *lovsub2lu_dev(struct lovsub_device *lovsub)
{
        return &lovsub2cl_dev(lovsub)->cd_lu_dev;
}

static inline struct lovsub_device *lu2lovsub_dev(const struct lu_device *d)
{
        LINVRNT(d->ld_type == &lovsub_device_type);
        return container_of0(d, struct lovsub_device, acid_cl.cd_lu_dev);
}

static inline struct lovsub_device *cl2lovsub_dev(const struct cl_device *d)
{
        LINVRNT(d->cd_lu_dev.ld_type == &lovsub_device_type);
        return container_of0(d, struct lovsub_device, acid_cl);
}

static inline struct lu_object *lov2lu(struct lov_object *lov)
{
        return &lov->lo_cl.co_lu;
}

static inline struct cl_object *lov2cl(struct lov_object *lov)
{
        return &lov->lo_cl;
}

static inline struct lov_object *lu2lov(const struct lu_object *obj)
{
        LINVRNT(lov_is_object(obj));
        return container_of0(obj, struct lov_object, lo_cl.co_lu);
}

static inline struct lov_object *cl2lov(const struct cl_object *obj)
{
        LINVRNT(lov_is_object(&obj->co_lu));
        return container_of0(obj, struct lov_object, lo_cl);
}

static inline struct lu_object *lovsub2lu(struct lovsub_object *los)
{
        return &los->lso_cl.co_lu;
}

static inline struct cl_object *lovsub2cl(struct lovsub_object *los)
{
        return &los->lso_cl;
}

static inline struct lovsub_object *cl2lovsub(const struct cl_object *obj)
{
        LINVRNT(lovsub_is_object(&obj->co_lu));
        return container_of0(obj, struct lovsub_object, lso_cl);
}

static inline struct lovsub_object *lu2lovsub(const struct lu_object *obj)
{
        LINVRNT(lovsub_is_object(obj));
        return container_of0(obj, struct lovsub_object, lso_cl.co_lu);
}

static inline struct lovsub_lock *
cl2lovsub_lock(const struct cl_lock_slice *slice)
{
        LINVRNT(lovsub_is_object(&slice->cls_obj->co_lu));
        return container_of(slice, struct lovsub_lock, lss_cl);
}

static inline struct lovsub_lock *cl2sub_lock(const struct cl_lock *lock)
{
        const struct cl_lock_slice *slice;

        slice = cl_lock_at(lock, &lovsub_device_type);
        LASSERT(slice != NULL);
        return cl2lovsub_lock(slice);
}

static inline struct lov_lock *cl2lov_lock(const struct cl_lock_slice *slice)
{
        LINVRNT(lov_is_object(&slice->cls_obj->co_lu));
        return container_of(slice, struct lov_lock, lls_cl);
}

static inline struct lov_page *cl2lov_page(const struct cl_page_slice *slice)
{
        LINVRNT(lov_is_object(&slice->cpl_obj->co_lu));
        return container_of0(slice, struct lov_page, lps_cl);
}

static inline struct lovsub_page *
cl2lovsub_page(const struct cl_page_slice *slice)
{
        LINVRNT(lovsub_is_object(&slice->cpl_obj->co_lu));
        return container_of0(slice, struct lovsub_page, lsb_cl);
}

static inline struct lov_io *cl2lov_io(const struct lu_env *env,
                                const struct cl_io_slice *ios)
{
        struct lov_io *lio;

        lio = container_of(ios, struct lov_io, lis_cl);
        LASSERT(lio == lov_env_io(env));
        return lio;
}

static inline int lov_targets_nr(const struct lov_device *lov)
{
        return lov->ld_lov->desc.ld_tgt_count;
}

static inline struct lov_thread_info *lov_env_info(const struct lu_env *env)
{
        struct lov_thread_info *info;

        info = lu_context_key_get(&env->le_ctx, &lov_key);
        LASSERT(info != NULL);
        return info;
}

static inline struct lov_layout_raid0 *lov_r0(struct lov_object *lov, int i)
{
	LASSERT(lov->lo_type == LLT_COMP);
	LASSERTF(i < lov->u.composite.lo_entry_count,
		 "entry %d entry_count %d", i, lov->u.composite.lo_entry_count);

	return &lov->u.composite.lo_entries[i].lle_raid0;
}

static inline struct lov_stripe_md_entry *lov_lse(struct lov_object *lov, int i)
{
	LASSERT(lov->lo_lsm != NULL);
	LASSERT(i < lov->lo_lsm->lsm_entry_count);

	return lov->lo_lsm->lsm_entries[i];
}

/* lov_pack.c */
int lov_getstripe(const struct lu_env *env, struct lov_object *obj,
		  struct lov_stripe_md *lsm, struct lov_user_md __user *lump);

/** @} lov */

#endif
