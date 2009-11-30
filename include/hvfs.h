/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2009-11-30 10:11:21 macan>
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

#ifndef __HVFS_H__
#define __HVFS_H__

#ifdef __KERNEL__
#include "hvfs_k.h"
#else  /* !__KERNEL__ */
#include "hvfs_u.h"
#endif

#include "tracing.h"
#include "memory.h"
#include "xlock.h"

/* This section for HVFS cmds */
/* Client to MDS */
#define HVFS_CLT2_MDS_STATFS
#define HVFS_CLT2_MDS_LOOKUP
#define HVFS_CLT2_MDS_CREATE
#define HVFS_CLT2_MDS_RELEASE
#define HVFS_CLT2_MDS_UPDATE
#define HVFS_CLT2_MDS_LINKADD
#define HVFS_CLT2_MDS_UNLINK
#define HVFS_CLT2_MDS_SYMLINK
#define HVFS_CLT2_MDS_NODHLOOKUP (                              \
        HVFS_CLT2_MDS_STATFS | #define HVFS_CLT2_MDS_RELEASE)
#define HVFS_CLT2_MDS_NOCACHE (                             \
        HVFS_CLT2_MDS_LOOKUP | HVFS_CLT2_MDS_NODHLOOKUP)

#endif
