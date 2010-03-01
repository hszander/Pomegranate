/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2010-03-01 21:40:49 macan>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef __MDSL_H__
#define __MDSL_H__

#include "hvfs.h"

#ifdef HVFS_TRACING
extern u32 hvfs_mdsl_tracing_flags;
#endif

#define HVFS_HOME "/tmp/hvfs"

/* mmap window */
struct mmap_window 
{
    struct list_head list;
    loff_t offset;                 /* the data offset with respect to window */
    size_t len;
    void *addr;
};

struct mmap_window_cache 
{
    struct list_head mw_list;
    xrwlock_t rwlock;
};

struct txg_compact_cache
{
    struct list_head open_list; /* txg_open_entry list */
    struct list_head wbed_list; /* txg entry waiting for TXG_END */
    xrwlock_t open_lock;
    xrwlock_t wbed_lock;
};

struct directw_log
{
};

struct mdsl_conf
{
    /* section for dynamic configuration */
    char dcaddr[MDS_DCONF_MAX_NAME_LEN];
    int dcfd, dcepfd;
    pthread_t dcpt;

    /* section for file name */
    char *profiling_file;
    char *conf_file;
    char *log_file;

    /* section for file fd */
    int pf_fd, cf_fd, lf_fd;

    /* # of threads */
    /* NOTE: # of profiling thread is always ONE */

    /* misc configs */

    /* intervals */
    int profiling_thread_interval;
    int gc_interval;

    /* conf */
    u64 option;
};

struct hvfs_mdsl_info
{
#define HMI_STATE_CLEAN         0x01
#define HMI_STATE_LASTOPEN      0x02
#define HMI_STATE_LASTMIG       0x03
#define HMI_STATE_LASTPAUSE     0x04
    u32 state;
    u64 gdt_salt;
    u64 gdt_uuid;
    u64 group;
    atomic64_t mi_tx;           /* next tx # */
    atomic64_t mi_txg;          /* next txg # */
    atomic64_t mi_uuid;         /* next file uuid */
    atomic64_t mi_fnum;         /* total allocated file # */
};

struct hvfs_mdsl_object
{
    u64 site_id;                /* this site */
    struct xnet_context *xc;

    struct mmap_window_cache mwc;
    struct txg_compact_cache tcc;
    struct directw_log dl;
    struct mdsl_conf conf;
#define HMO_STATE_LAUNCH        0x00
#define HMO_STATE_RUNNING       0x01
#define HMO_STATE_PAUSE         0x02
#define HMO_STATE_RDONLY        0x03
    u32 state;

    /* the following region is used for threads */
    sem_t timer_sem;            /* for timer thread wakeup */
    
    pthread_t timer_thread;

    u8 timer_thread_stop;       /* running flag for timer thread */
};

extern struct hvfs_mdsl_info hmi;
extern struct hvfs_mdsl_object hmo;

/* APIs */
int mdsl_init(void);
int mdsl_destroy(void);

#endif
