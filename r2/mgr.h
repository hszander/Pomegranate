/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2010-05-06 16:50:55 macan>
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

#ifndef __ROOT_MGR_H__
#define __ROOT_MGR_H__

#include "hvfs.h"
#include "ring.h"
#include "lib.h"

/* site manager to manage the registered site group, on chring changing mgr
 * should notify the registered sites. 
 *
 * site manager indexed by site id. constructed by Hash Table.
 */
struct site_mgr
{
#define HVFS_ROOT_SITE_MGR_SALT         (0xfeadf98424af)
#define HVFS_ROOT_SITE_MGR_HTSIZE       1024
    struct regular_hash *sht;
};

struct site_entry
{
    struct hlist_node hlist;
    u64 site_id;
#define SE_STATE_INIT           0x00
#define SE_STATE_NORMAL         0x01
#define SE_STATE_TRANSIENT      0x02
#define SE_STATE_ERROR          0x03
    u32 state;
    u8 hb_lost;                 /* # of lost heart beat messages */
};

/* ring manager to manage the consistent hash ring, we can support dynamic
 * ring point add and delete. Notifications should be sent to the subscribed
 * sites. indexed by ring group id? */
struct ring_mgr
{
    struct regular_hash *rht;
    xrwlock_t rwlock;
    u32 active_ring;
};

struct ring_entry
{
    struct hlist_node hlist;
    struct chring ring;
    atomic_t ref;
};

/* root manager to manage the HVFS root info, indexed by hvfs fsid */
struct root_mgr
{
    struct regular_hash *rht;
};

struct root_entry
{
    struct hlist_node hlist;
    u64 fsid;
    u64 gdt_uuid;
    u64 gdt_salt;

    u64 root_uuid;
    u64 root_salt;              /* root salt can be discovered from GDT
                                 * lookup either. */
};

/* address mgr to manage the global site address table, we do support dynamic
 * changes on this table */
struct addr_mgr
{
    struct hvfs_site *xs[1 << 20];
    xrwlock_t rwlock;
    u32 used_addr;              /* # of used addr */
    u32 active_site;            /* # of active site */
};

#endif