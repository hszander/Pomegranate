/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2011-04-26 16:39:07 macan>
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

#include "hvfs.h"
#include "xnet.h"
#include "root.h"
#include "lib.h"
#include "ring.h"

#ifdef UNIT_TEST
#define TYPE_MDS        0
#define TYPE_CLIENT     1
#define TYPE_MDSL       2
#define TYPE_RING       3

char *ipaddr[] = {
    "127.0.0.1",              /* mds */
    "127.0.0.1",              /* client */
    "127.0.0.1",              /* mdsl */
    "127.0.0.1",              /* ring */
};

short port[5][5] = {
    {8210, 8211, 8212, 8213, 8214,},  /* mds */
    {8412, 8413, 8414, 8415,},  /* client */
    {8810, 8811, 8812, 8813,},  /* mdsl */
    {8710, 8711, 8712, 8713,},  /* ring */
};

#define HVFS_TYPE(type, idx) ({                 \
            u64 __sid = -1UL;                   \
            switch (type){                      \
            case TYPE_MDS:                      \
                __sid = HVFS_MDS(idx);          \
                break;                          \
            case TYPE_CLIENT:                   \
                __sid = HVFS_CLIENT(idx);       \
                break;                          \
            case TYPE_MDSL:                     \
                __sid = HVFS_MDSL(idx);         \
                break;                          \
            case TYPE_RING:                     \
                __sid = HVFS_RING(idx);         \
                break;                          \
            default:;                           \
            }                                   \
            __sid;                              \
        })

static inline
u64 HVFS_TYPE_SEL(int type, int id)
{
    u64 site_id = -1UL;

    switch (type) {
    case TYPE_MDS:
        site_id = HVFS_MDS(id);
        break;
    case TYPE_CLIENT:
        site_id = HVFS_CLIENT(id);
        break;
    case TYPE_MDSL:
        site_id = HVFS_MDSL(id);
        break;
    case TYPE_RING:
        site_id = HVFS_RING(id);
        break;
    default:;
    }

    return site_id;
}

int msg_wait()
{
    while (1) {
        xnet_wait_any(hro.xc);
    }
    return 0;
}

/* ring_add() add one site to the CH ring
 */
int ring_add(struct chring *r, u64 site, char *str)
{
    struct chp *p;
    char buf[256];
    int vid_max, i, err;

    vid_max = hro.conf.ring_vid_max ? hro.conf.ring_vid_max : 
        HVFS_RING_VID_MAX;

    p = (struct chp *)xzalloc(vid_max * sizeof(struct chp));
    if (!p) {
        hvfs_err(xnet, "xzalloc() chp failed\n");
        return -ENOMEM;
    }

    for (i = 0; i < vid_max; i++) {
        snprintf(buf, 256, "%s.%ld.%d", str, site, i);
        (p + i)->point = hvfs_hash(site, (u64)buf, strlen(buf), HASH_SEL_VSITE);
        (p + i)->vid = i;
        (p + i)->type = CHP_AUTO;
        (p + i)->site_id = site;
        err = ring_add_point_nosort(p + i, r);
        if (err) {
            hvfs_err(xnet, "ring_add_point() failed.\n");
            return err;
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    struct xnet_type_ops ops = {
        .buf_alloc = NULL,
        .buf_free = NULL,
        .recv_handler = root_spool_dispatch,
        .dispatcher = root_dispatch,
    };
    int err = 0, i, j;
    int self, sport = -1, mode, do_create;
    char profiling_fname[256];
    char *value;
    char *conf_file;
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
    };
    int nr = 10000;
    struct conf_site cs[nr];

    hvfs_info(xnet, "R2 Unit Testing ...\n");

    if (argc < 3) {
        hvfs_err(xnet, "Self ID or config file is not provided.\n");
        err = EINVAL;
        goto out;
    } else {
        self = atoi(argv[1]);
        hvfs_info(xnet, "Self type+ID is R2:%d.\n", self);
        conf_file = argv[2];
        if (argc == 4) {
            sport = atoi(argv[3]);
        }
    }

    value = getenv("mode");
    if (value) {
        mode = atoi(value);
    } else 
        mode = 0;

    value = getenv("create");
    if (value) {
        do_create = atoi(value);
    } else 
        do_create = 0;

    st_init();
    root_pre_init();

    /* setup the profiling file */
    memset(profiling_fname, 0, sizeof(profiling_fname));
    sprintf(profiling_fname, "./CP-BACK-root.%d", self);
    hro.conf.profiling_file = strdup(profiling_fname);
    hro.conf.prof_plot = ROOT_PROF_PLOT;
    
    err = root_init();
    if (err) {
        hvfs_err(xnet, "root_init() failed w/ %d\n", err);
        goto out;
    }

    /* init misc configurations */
    hro.prof.xnet = &g_xnet_prof;

    if (sport == -1)
        sport = port[TYPE_RING][self];
    self = HVFS_ROOT(self);

    hro.xc = xnet_register_type(0, sport, self, &ops);
    if (IS_ERR(hro.xc)) {
        err = PTR_ERR(hro.xc);
        goto out;
    }

    hro.site_id = self;
    root_verify();

    /* we should setup the global address table and then export it as the
     * st_table */

    {
        struct addr_entry *ae;
        
        /* setup a file system id 0 */
        err = addr_mgr_lookup_create(&hro.addr, 0UL, &ae);
        if (err > 0) {
            hvfs_info(xnet, "Create addr table for fsid %ld\n", 0UL);
        } else if (err < 0) {
            hvfs_err(xnet, "addr_mgr_lookup_create fsid 0 failed w/ %d\n",
                     err);
            goto out;
        }

        if (mode == 0) {
            for (i = 0; i < 4; i++) {
                for (j = 0; j < 4; j++) {
                    sin.sin_port = htons(port[i][j]);
                    inet_aton(ipaddr[i], &sin.sin_addr);
                    
                    err = addr_mgr_update_one(ae, 
                                              HVFS_SITE_PROTOCOL_TCP |
                                              HVFS_SITE_ADD,
                                              HVFS_TYPE(i, j),
                                              &sin);
                    if (err) {
                        hvfs_err(xnet, "addr mgr update entry %lx failed w/"
                                 " %d\n", HVFS_TYPE(i, j), err);
                        goto out;
                    }
                }
            }
        } else {
            err = conf_parse(conf_file, cs, &nr);
            if (err) {
                hvfs_err(xnet, "conf_parse failed w/ %d\n", err);
                goto out;
            }
            for (i = 0; i < nr; i++) {
                sin.sin_port = htons(cs[i].port);
                inet_aton(cs[i].node, &sin.sin_addr);

                err = addr_mgr_update_one(ae,
                                          HVFS_SITE_PROTOCOL_TCP |
                                          HVFS_SITE_ADD,
                                          conf_site_id(cs[i].type, cs[i].id),
                                          &sin);
                if (err) {
                    hvfs_err(xnet, "addr_mgr_update entry %lx failed w/ "
                             " %d\n", conf_site_id(cs[i].type, cs[i].id), err);
                    goto out;
                }
            }
        }
        
        /* export the addr mgr to st_table */
        {
            void *data;
            int len;
            
            err = addr_mgr_compact(ae, &data, &len);
            if (err) {
                hvfs_err(xnet, "compact addr mgr faild w/ %d\n", err);
                goto out;
            }
            
            err = hst_to_xsst(data, len);
            if (err) {
                hvfs_err(xnet, "hst to xsst failed w/ %d\n", err);
                goto out;
            }
            xfree(data);
        }
    }

    /* next, we setup the defalt ring mgr */
    if (mode == 0) {
        struct ring_entry *re, *res;

        re = ring_mgr_alloc_re();
        if (!re) {
            hvfs_err(xnet, "alloc ring entry failed\n");
            err = ENOMEM;
            goto out;
        }
        re->ring.group = CH_RING_MDS;
        ring_add(&re->ring, HVFS_MDS(0), "mds");
        ring_add(&re->ring, HVFS_MDS(1), "mds");
#if 1
        ring_add(&re->ring, HVFS_MDS(2), "mds");
        ring_add(&re->ring, HVFS_MDS(3), "mds");
#endif
        res = ring_mgr_insert(&hro.ring, re);
        if (IS_ERR(res)) {
            hvfs_err(xnet, "ring_mgr_insert %d failed w/ %ld\n",
                     re->ring.group, PTR_ERR(res));
            err = PTR_ERR(res);
            goto out;
        }
        ASSERT(res == re, xnet);
        ring_resort_nolock(&re->ring);
        ring_mgr_put(re);

        re = ring_mgr_alloc_re();
        if (!re) {
            hvfs_err(xnet, "alloc ring entry failed\n");
            err = ENOMEM;
            goto out;
        }
        re->ring.group = CH_RING_MDSL;
        ring_add(&re->ring, HVFS_MDSL(0), "mdsl");
        ring_add(&re->ring, HVFS_MDSL(1), "mdsl");
        res = ring_mgr_insert(&hro.ring, re);
        if (IS_ERR(res)) {
            hvfs_err(xnet, "ring_mgr_insert %d failed w/ %ld\n",
                     re->ring.group, PTR_ERR(res));
            err = PTR_ERR(res);
            goto out;
        }
        ASSERT(res == re, xnet);
        ring_resort_nolock(&re->ring);
        ring_mgr_put(re);
    } else {
        struct ring_entry *re, *res;

        /* Add mds sites to the ring */
        re = ring_mgr_alloc_re();
        if (!re) {
            hvfs_err(xnet, "alloc ring entry failed\n");
            err = ENOMEM;
            goto out;
        }
        re->ring.group = CH_RING_MDS;

        for (i = 0; i < nr; i++) {
            if (strcmp(cs[i].type, "mdsl") == 0) {
                continue;
            } else if (strcmp(cs[i].type, "mds") == 0) {
                ring_add(&re->ring, HVFS_MDS(cs[i].id), "mds-*-");
            }
        }
        res = ring_mgr_insert(&hro.ring, re);
        if (IS_ERR(res)) {
            hvfs_err(xnet, "ring_mgr_insert %d failed w/ %ld\n",
                     re->ring.group, PTR_ERR(res));
            err = PTR_ERR(res);
            goto out;
        }
        ASSERT(res == re, xnet);
        ring_resort_nolock(&re->ring);
        ring_mgr_put(re);

        /* Add mdsl sites to the ring */
        re = ring_mgr_alloc_re();
        if (!re) {
            hvfs_err(xnet, "alloc ring entry failed\n");
            err = ENOMEM;
            goto out;
        }
        re->ring.group = CH_RING_MDSL;

        for (i = 0; i < nr; i++) {
            if (strcmp(cs[i].type, "mdsl") == 0) {
                ring_add(&re->ring, HVFS_MDSL(cs[i].id), "mdsl-$-");
            }
        }
        res = ring_mgr_insert(&hro.ring, re);
        if (IS_ERR(res)) {
            hvfs_err(xnet, "ring_mgr_insert %d failed w/ %ld\n",
                     re->ring.group, PTR_ERR(res));
            err = PTR_ERR(res);
            goto out;
        }
        ASSERT(res == re, xnet);
        ring_resort_nolock(&re->ring);
        ring_mgr_put(re);

        /* Add bp sites to the ring */
        re = ring_mgr_alloc_re();
        if (!re) {
            hvfs_err(xnet, "alloc ring entry failed\n");
            err = ENOMEM;
            goto out;
        }
        re->ring.group = CH_RING_BP;

        for (i = 0; i < nr; i++) {
            if (strcmp(cs[i].type, "bp") == 0) {
                ring_add(&re->ring, HVFS_BP(cs[i].id), "bp-#-");
            }
        }
        res = ring_mgr_insert(&hro.ring, re);
        if (IS_ERR(res)) {
            hvfs_err(xnet, "ring_mgr_insert %d failed w/ %ld\n",
                     re->ring.group, PTR_ERR(res));
            err = PTR_ERR(res);
            goto out;
        }
        ASSERT(res == re, xnet);
        ring_resort_nolock(&re->ring);
        ring_mgr_put(re);
    }

    /* next, we setup the root entry for fsid == 0 */
    {
        struct root_entry *re, __attribute__((unused))*res;

        err = root_mgr_lookup_create(&hro.root, 0, &re);
        if (err > 0) {
            /* create a new root entry, and read in the content from the
             * disk  */
            hvfs_info(xnet, "Read in the fs %ld: gdt_salt %lx\n", 
                      0UL, re->gdt_salt);
        } else if (err == -ENOENT) {
            /* create a new root entry and insert it */
            re = root_mgr_alloc_re();
            if (!re) {
                hvfs_err(xnet, "root mgr alloc re failed\n");
                err = -ENOMEM;
                goto out;
            }
            re->fsid = 0;
            re->gdt_salt = lib_random(0xf135dae9);
            re->root_uuid = 1;
            re->root_salt = -1UL;
            
            re->gdt_flen = XTABLE_BITMAP_BYTES;
            re->gdt_bitmap = xzalloc(re->gdt_flen);
            if (!re->gdt_bitmap) {
                hvfs_err(xnet, "xzalloc bitmap failed\n");
                err = -ENOMEM;
                goto out;
            }
            re->gdt_bitmap[0] = 0xff;

            if (do_create) {
                /* change the root_salt to a random value */
                re->root_salt = lib_random(0xfadffdf);
                res = root_mgr_insert(&hro.root, re);
                if (IS_ERR(res)) {
                    hvfs_err(xnet, "insert root entry faild w/ %ld\n",
                             PTR_ERR(res));
                    err = PTR_ERR(res);
                    goto out;
                }
                hvfs_info(root, "We just do create a new fs %ld\n", re->fsid);
            } else {
                /* do not create a root entry, we just read from the disk
                 * now */
                ;
            }
        } else if (err < 0) {
            hvfs_err(xnet, "lookup create root 0 failed w/ %d\n", err);
        }
    }

    /* finally, we setup a test shell to recv commands */
#if 0
    {
        struct ring_entry *re;

        re = ring_mgr_lookup(&hro.ring, CH_RING_MDS);
        if (IS_ERR(re)) {
            hvfs_err(xnet, "lookup ring %d failed\n", CH_RING_MDS);
            goto out;
        }

        err = cli_dynamic_add_site(re, HVFS_MDS(0));
        if (err) {
            hvfs_err(xnet, "cli_dynamic_add_site() failed w/ %d\n", err);
            goto out;
        }
#if 0
        SET_TRACING_FLAG(lib, HVFS_DEBUG);
        ring_dump(&re->ring);
#endif
        ring_mgr_put(re);
    }
#endif

    hvfs_info(xnet, "R2 is UP for serving requests now.\n");

//    SET_TRACING_FLAG(xnet, HVFS_DEBUG);
//    SET_TRACING_FLAG(root, HVFS_DEBUG);
    msg_wait();

    root_destroy();
    xnet_unregister_type(hro.xc);
out:
    return err;
}
#endif
