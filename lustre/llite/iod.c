/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (c) 2002, 2003 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *  Copyright (C) 2002, 2003  Cluster File Systems, Inc
 *
 *  this started as an implementation of an io daemon that woke regularly 
 *  to force writeback.. the throttling in prepare_write and kupdate's usual
 *  writeback pressure got rid of our thread, but the file name remains.
 */
#include <linux/version.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/kmod.h>
#include <linux/pagemap.h>

#define DEBUG_SUBSYSTEM S_LLITE
#include <linux/lustre_lite.h>

#ifndef list_for_each_prev_safe
#define list_for_each_prev_safe(pos, n, head) \
        for (pos = (head)->prev, n = pos->prev; pos != (head); \
                pos = n, n = pos->prev )
#endif

extern spinlock_t inode_lock;

#define LLWP_MAX_PAGES (PTL_MD_MAX_IOV)
struct ll_writeback_pages {
        unsigned        has_whole_pages:1,
                        num_frags:2,
                        num_pages:29; 
        struct brw_page pgs[LLWP_MAX_PAGES];
};


/*
 * ugh, we want disk allocation on the target to happen in offset order.  we'll
 * follow sedgewicks advice and stick to the dead simple shellsort -- it'll do
 * fine for our small page arrays and doesn't require allocation.  its an
 * insertion sort that swaps elements that are strides apart, shrinking the
 * stride down until its '1' and the array is sorted.
 */
void sort_brw_pages(struct brw_page *array, int num)
{
        int stride, i, j;
        struct brw_page tmp;

        if ( num == 1 )
                return;

        for( stride = 1; stride < num ; stride = (stride*3) +1  )
                ;

	do {
		stride /= 3;
		for ( i = stride ; i < num ; i++ ) {
			tmp = array[i];
			j = i;
			while ( j >= stride && 
                                        array[j - stride].off > tmp.off ) {
				array[j] = array[j - stride];
				j -= stride;
			}
			array[j] = tmp;
		}
	} while ( stride > 1 );
}

/*
 * returns 0 if the page was inserted in the array because it was
 * within i_size.  if we raced with truncate and i_size was less
 * than the page we can unlock the page because truncate_inode_pages will
 * be waiting to cleanup the page
 */
static int llwp_consume_page(struct ll_writeback_pages *llwp, 
                             struct inode *inode, struct page *page)
{
        obd_off off = ((obd_off)page->index) << PAGE_SHIFT;
        struct brw_page *pg;

        /* we raced with truncate? */
        if ( off >= inode->i_size ) {
                unlock_page(page);
                goto out;
        }

        page_cache_get(page);
        pg = &llwp->pgs[llwp->num_pages];
        llwp->num_pages++;

        pg->pg = page;
        pg->off = off;
        pg->flag = OBD_BRW_CREATE;
        pg->count = PAGE_SIZE;

        /* catch partial writes for files that end mid-page */
        if ( pg->off + pg->count > inode->i_size )
                pg->count = inode->i_size & ~PAGE_MASK;

        if ( pg->count == PAGE_SIZE ) {
                if ( ! llwp->has_whole_pages ) {
                        llwp->has_whole_pages = 1;
                        llwp->num_frags++;
                }
        } else {
                llwp->num_frags++;
        }

        /*
         * matches ptlrpc_bulk_get assert that trickles down
         * from a 0 page length going through niobuf and into
         * the buffer regions being posted
         */
        LASSERT(pg->count >= 0);

        CDEBUG(D_CACHE, "brw_page %p: off %lld cnt %d, page %p: ind %ld\n",
                        pg, pg->off, pg->count, page, page->index);

out:
        if ( llwp->num_frags == 3 || llwp->num_pages == LLWP_MAX_PAGES )
                return -1;

        return 0;
}

/* 
 * returns the number of pages that it added to the pgs array
 *
 * this duplicates filemap_fdatasync and gives us an opportunity to grab lots
 * of dirty pages.. 
 */
static void ll_get_dirty_pages(struct inode *inode, 
                               struct ll_writeback_pages *llwp)
{
        struct address_space *mapping = inode->i_mapping;
        struct page *page;
        struct list_head *pos, *n;
        ENTRY;

        spin_lock(&pagecache_lock);

        list_for_each_prev_safe(pos, n, &mapping->dirty_pages) {
                page = list_entry(pos, struct page, list);

                if (TryLockPage(page))
                        continue;

                list_del(&page->list);
                list_add(&page->list, &mapping->locked_pages);

                if ( ! PageDirty(page) ) {
                        unlock_page(page);
                        continue;
                }
                ClearPageDirty(page);

                if ( llwp_consume_page(llwp, inode, page) != 0)
                        break;
        }

        spin_unlock(&pagecache_lock);
        EXIT;
}

static void ll_brw_pages_unlock( struct inode *inode, 
                                 struct ll_writeback_pages *llwp)
{
        int rc, i;
        struct obd_brw_set set;
        ENTRY;

        sort_brw_pages(llwp->pgs, llwp->num_pages);

        memset(&set, 0, sizeof(struct obd_brw_set));
        init_waitqueue_head(&set.brw_waitq);
        INIT_LIST_HEAD(&set.brw_desc_head);
        atomic_set(&set.brw_refcount, 0);
        set.brw_callback = ll_brw_sync_wait;

        rc = obd_brw(OBD_BRW_WRITE, ll_i2obdconn(inode),
                     ll_i2info(inode)->lli_smd, llwp->num_pages, llwp->pgs, 
                     &set, NULL);
        if (rc) {
                CERROR("error from obd_brw: rc = %d\n", rc);
        } else {
                rc = ll_brw_sync_wait(&set, CB_PHASE_START);
                if (rc)
                        CERROR("error from callback: rc = %d\n", rc);
        }

        /* XXX this doesn't make sense to me */
        rc = 0;

        for ( i = 0 ; i < llwp->num_pages ; i++) {
                struct page *page = llwp->pgs[i].pg;

                CDEBUG(D_CACHE, "cleaning page %p\n", page);
                LASSERT(PageLocked(page));
                unlock_page(page);
                page_cache_release(page);
        }

        EXIT;
}

/*
 * this is called by prepare_write when we're low on memory, it wants
 * to write back as much dirty data as it can.  we'd rather just
 * call fsync_dev and let the kernel call writepage on all our dirty
 * pages, but i_sem makes that hard.  prepare_write holds i_sem from
 * generic_file_write, but other writepage callers don't.   so we have
 * this seperate code path that writes back all the inodes it can get
 * i_sem on.
 */
int ll_sb_sync( struct super_block *sb, struct inode *callers_inode )
{
        unsigned long old_flags; /* hack? */
        int making_progress;
        struct ll_writeback_pages *llwp;
        int rc = 0;
        ENTRY;

        old_flags = current->flags;
        current->flags |= PF_MEMALLOC;
        llwp = kmalloc(sizeof(struct ll_writeback_pages), GFP_ATOMIC);
        if ( llwp == NULL )
                GOTO(cleanup, rc = -ENOMEM);
        memset(llwp, 0, offsetof(struct ll_writeback_pages, pgs));

        spin_lock(&inode_lock);

        do {
                struct list_head *pos;
                struct inode *inode = NULL;

                making_progress = 0;
                list_for_each_prev(pos, &sb->s_dirty) {
                        inode = list_entry(pos, struct inode, i_list);

                        if ( ! (inode->i_state & I_DIRTY_PAGES) ) {
                                inode = NULL;
                                continue; 
                        }
                        break;
                }

                if ( inode == NULL )
                        break;

                /* duplicate __sync_one, *sigh* */
                list_del(&inode->i_list);
                list_add(&inode->i_list, &inode->i_sb->s_locked_inodes);
                inode->i_state |= I_LOCK;
                inode->i_state &= ~I_DIRTY_PAGES;

                spin_unlock(&inode_lock);

                do { 
                        ll_get_dirty_pages(inode, llwp);
                        if ( llwp->num_pages ) {
                                ll_brw_pages_unlock(inode, llwp);
                                rc += llwp->num_pages;
                                making_progress = 1;
                        }
                } while (llwp->num_pages);

                spin_lock(&inode_lock);

                inode->i_state &= ~I_LOCK;
                /*
                 * we are sneaky and leave the inode on the dirty list, 
                 * even though it might not still be.. 
                 */
                if (!(inode->i_state & I_FREEING)) {
                        list_del(&inode->i_list);
                        list_add(&inode->i_list, &inode->i_sb->s_dirty);
                }
                wake_up(&inode->i_wait);

        } while ( making_progress );

        spin_unlock(&inode_lock);

cleanup:
        if ( llwp != NULL )
                kfree(llwp);
        current->flags = old_flags;

        RETURN(rc);
}

int ll_batch_writepage( struct inode *inode, struct page *page )
{
        unsigned long old_flags; /* hack? */
        struct ll_writeback_pages *llwp;
        int rc = 0;
        ENTRY;

        old_flags = current->flags;
        current->flags |= PF_MEMALLOC;
        llwp = kmalloc(sizeof(struct ll_writeback_pages), GFP_ATOMIC);
        if ( llwp == NULL )
                GOTO(cleanup, rc = -ENOMEM);
        memset(llwp, 0, offsetof(struct ll_writeback_pages, pgs));

        llwp_consume_page(llwp, inode, page);

        ll_get_dirty_pages(inode, llwp);
        if ( llwp->num_pages )
                ll_brw_pages_unlock(inode, llwp);

cleanup:
        if ( llwp != NULL )
                kfree(llwp);
        current->flags = old_flags;
        RETURN(rc);
}
