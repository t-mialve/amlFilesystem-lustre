/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
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
 * version 2 along with this program; If not, see [sun.com URL with a
 * copy of GPLv2].
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lnet/klnds/ralnd/ralnd_modparams.c
 *
 * Author: Eric Barton <eric@bartonsoftware.com>
 */

#include "ralnd.h"

static int n_connd = 4;
CFS_MODULE_PARM(n_connd, "i", int, 0444,
                "# of connection daemons");

static int min_reconnect_interval = 1;
CFS_MODULE_PARM(min_reconnect_interval, "i", int, 0644,
                "minimum connection retry interval (seconds)");

static int max_reconnect_interval = 60;
CFS_MODULE_PARM(max_reconnect_interval, "i", int, 0644,
                "maximum connection retry interval (seconds)");

static int ntx = 256;
CFS_MODULE_PARM(ntx, "i", int, 0444,
                "# of transmit descriptors");

static int credits = 128;
CFS_MODULE_PARM(credits, "i", int, 0444,
                "# concurrent sends");

static int peer_credits = 32;
CFS_MODULE_PARM(peer_credits, "i", int, 0444,
                "# concurrent sends to 1 peer");

static int fma_cq_size = 8192;
CFS_MODULE_PARM(fma_cq_size, "i", int, 0444,
                "size of the completion queue");

static int timeout = 30;
CFS_MODULE_PARM(timeout, "i", int, 0644,
                "communications timeout (seconds)");

static int max_immediate = (2<<10);
CFS_MODULE_PARM(max_immediate, "i", int, 0644,
                "immediate/RDMA breakpoint");

kra_tunables_t kranal_tunables = {
        .kra_n_connd                = &n_connd,
        .kra_min_reconnect_interval = &min_reconnect_interval,
        .kra_max_reconnect_interval = &max_reconnect_interval,
        .kra_ntx                    = &ntx,
        .kra_credits                = &credits,
        .kra_peercredits            = &peer_credits,
        .kra_fma_cq_size            = &fma_cq_size,
        .kra_timeout                = &timeout,
        .kra_max_immediate          = &max_immediate,
};

#if CONFIG_SYSCTL && !CFS_SYSFS_MODULE_PARM
static cfs_sysctl_table_t kranal_ctl_table[] = {
        {
                .ctl_name = 1,
                .procname = "n_connd",
                .data     = &n_connd,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = 2,
                .procname = "min_reconnect_interval",
                .data     = &min_reconnect_interval,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = 3,
                .procname = "max_reconnect_interval",
                .data     = &max_reconnect_interval,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = 4,
                .procname = "ntx",
                .data     = &ntx,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = 5,
                .procname = "credits",
                .data     = &credits,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = 6,
                .procname = "peer_credits",
                .data     = &peer_credits,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = 7,
                .procname = "fma_cq_size",
                .data     = &fma_cq_size,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = 8,
                .procname = "timeout",
                .data     = &timeout,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = 9,
                .procname = "max_immediate",
                .data     = &max_immediate,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec
        },
        {0}
};

static cfs_sysctl_table_t kranal_top_ctl_table[] = {
        {
                .ctl_name = 202,
                .procname = "ranal",
                .data     = NULL,
                .maxlen   = 0,
                .mode     = 0555,
                .child    = kranal_ctl_table
        },
        {0}
};

int
kranal_tunables_init ()
{
        kranal_tunables.kra_sysctl =
                cfs_register_sysctl_table(kranal_top_ctl_table, 0);

        if (kranal_tunables.kra_sysctl == NULL)
                CWARN("Can't setup /proc tunables\n");

        return 0;
}

void
kranal_tunables_fini ()
{
        if (kranal_tunables.kra_sysctl != NULL)
                cfs_unregister_sysctl_table(kranal_tunables.kra_sysctl);
}

#else

int
kranal_tunables_init ()
{
        return 0;
}

void
kranal_tunables_fini ()
{
}

#endif
