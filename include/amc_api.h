/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2010-07-26 23:39:58 macan>
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

#ifndef __AMC_API_H__
#define __AMC_API_H__

/* the general index structre between AMC client and MDS */
struct amc_index
{
#define INDEX_PUT       0x00000001
#define INDEX_GET       0x00000002
#define INDEX_MPUT      0x00000004
#define INDEX_MGET      0x00000008

#define INDEX_DEL       0x00000010
#define INDEX_UPDATE    0x00000020
#define INDEX_CUPDATE   0x00000040 /* conditional update */

#define INDEX_CU_EXIST          0x0080000
#define INDEX_CU_NOTEXIST       0x0040000
    u32 flag;
    int column;                 /* which column you want to get/put */

    u64 key;                    /* used to search in EH, should be unique */
    u64 sid;                    /* table slice id, may not precise */
    u64 tid;                    /* table id */

    u64 ptid;                   /* parent table id */
    u64 psalt;                  /* parent salt */
    void *data;                 /* pointer to data payload */
    u64 dlen;                   /* intransfer length of payload */
};

/* APIs */
int __core_main(int argc, char *argv[]);
void __core_exit(void);

int hvfs_lookup_root(void);
int hvfs_create_root(void);
int hvfs_create_table(char *name);
int hvfs_find_table(char *name, u64 *uuid, u64 *salt);
int hvfs_drop_table(char *name);
int hvfs_put(char *table, u64 key, char *value, int column);
int hvfs_get(char *table, u64 key, char **value, int column);
int hvfs_del(char *table, u64 key, int column);
int hvfs_update(char *table, u64 key, char *value, int column);

#define LIST_OP_SCAN         0
#define LIST_OP_COUNT        1
struct list_result
{
    int cnt;
};
int hvfs_list(char *table, int op);

#endif