/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2010-03-26 20:15:53 macan>
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
#include "mdsl.h"

#ifdef UNIT_TEST

int __test_append_buf()
{
    struct fdhash_entry fde = {
        .uuid = 0,
        .arg = 0,
        .type = MDSL_STORAGE_ITB,
        .fd = 0,
        .state = FDE_FREE,
    };

    append_buf_create(&fde, "/tmp/mdsl_abuf", FDE_OPEN);
    append_buf_create(&fde, "/tmp/mdsl_abuf", FDE_ABUF);
    close(fde.fd);

    return 0;
}

int __test_fdht()
{
    char buf[1024] = {"hello, world!\n"};
    struct iovec test_iov = {
        .iov_base = buf,
        .iov_len = 1024,
    };
    struct itb_info ii = {{0,}, 0, };
    struct mdsl_storage_access msa = {
        .iov = &test_iov,
        .arg = &ii,
        .iov_nr = 1,
    };
    struct fdhash_entry *fde;
    int err = 0, i;
    
    hvfs_info(mdsl, "begin create ...\n");
    fde = mdsl_storage_fd_lookup_create(0, MDSL_STORAGE_ITB, 0);
    if (IS_ERR(fde)) {
        hvfs_err(mdsl, "lookup create failed w/ %ld\n", PTR_ERR(fde));
        return PTR_ERR(fde);
    }
    hvfs_info(mdsl, "begin write ...\n");
    for (i = 0; i < (64 * 1024 * 16 * 10); i++) {
        err = mdsl_storage_fd_write(fde, &msa);
        if (err) {
            hvfs_err(mdsl, "fd_write failed w/ %d\n", err);
            mdsl_storage_fd_put(fde);
            goto out;
        }
    }
    hvfs_info(mdsl, "end write ...\n");
    mdsl_storage_fd_put(fde);
    hvfs_info(mdsl, "fd [.uuid %ld, .type %x, .fd = %d, .state %x, .ref %d]\n",
              fde->uuid, fde->type, fde->fd, fde->state,
              atomic_read(&fde->ref));

out:
    return err;
}

int __test_all()
{
    struct mmap_args ma;
    struct fdhash_entry *fde;
    range_t *range;
    u64 location;
    int err = 0, i;

    fde = mdsl_storage_fd_lookup_create(0, MDSL_STORAGE_MD, 0);
    if (IS_ERR(fde)) {
        hvfs_err(mdsl, "lookup create MD file failed w/ %ld\n", PTR_ERR(fde));
        return PTR_ERR(fde);
    }
    if (!fde->mdisk.ranges) {
        for (i = 0; i < 100; i++) {
            __mdisk_add_range(fde, i * (1 << 20), (i + 1) * (1 << 20) -1, i);
        }
        ASSERT(fde->state == FDE_MDISK, mdsl);
        err = mdsl_storage_fd_write(fde, NULL);
        if (err) {
            hvfs_err(mdsl, "fd write failed w/ %d\n", err);
            goto out_put;
        }
    }
    ma.win = MDSL_STORAGE_DEFAULT_RANGE_SIZE;
    err = __mdisk_lookup(fde, MDSL_MDISK_RANGE, 100, &range);
    if (err) {
        hvfs_err(mdsl, "mdisk_lookup failed w/ %d\n", err);
        goto out_put;
    }
    ma.foffset = 0;
    ma.range_id = range->range_id;
    ma.range_begin = range->begin;

    err = __range_lookup(0, 0, &ma, &location);
    if (err) {
        hvfs_err(mdsl, "range lookup failed w/ %d\n", err);
        goto out_put;
    }
    hvfs_info(mdsl, "Range lookup got %ld\n", location);
    
    err = __range_write(0, 0, &ma, 10000);
    if (err) {
        hvfs_err(mdsl, "range write failed w/ %d\n", err);
        goto out_put;
    }

    err = __range_lookup(0, 0, &ma, &location);
    if (err) {
        hvfs_err(mdsl, "range lookup failed w/ %d\n", err);
        goto out_put;
    }
    hvfs_info(mdsl, "Range lookup got %ld\n", location);
    
out_put:
    mdsl_storage_fd_put(fde);

    return err;
}

int __test_read()
{
    struct iovec itb_iov = {0, };
    struct mdsl_storage_access msa = {
        .iov = &itb_iov,
        .iov_nr = 1,
    };
    struct mmap_args ma;
    struct fdhash_entry *fde;
    range_t *range;
    struct itb *itb;
    void *data;
    u64 location;
    int master;
    int err = 0;

    itb = xmalloc(sizeof(*itb));
    if (!itb) {
        hvfs_err(mdsl, "xmalloc struct itb failed\n");
        err = -ENOMEM;
        goto out;
    }

    fde = mdsl_storage_fd_lookup_create(1, MDSL_STORAGE_MD, 0);
    if (IS_ERR(fde)) {
        hvfs_err(mdsl, "lookup create MD file failed w/ %ld\n", PTR_ERR(fde));
        err = PTR_ERR(fde);
        goto out;
    }
    if (!fde->mdisk.ranges) {
        err = -ENOENT;
        goto out;
    }
    ma.win = MDSL_STORAGE_DEFAULT_RANGE_SIZE;

    err = __mdisk_lookup(fde, MDSL_MDISK_RANGE, 0, &range);
    if (err == -ENOENT) {
        goto out;
    }
    ma.foffset = 0;
    ma.range_id = range->range_id;
    ma.range_begin = range->begin;

    err = __range_lookup(1, 0, &ma, &location);
    if (err) {
        goto out;
    }
    if (!location) {
        err = -ENOENT;
        goto out;
    }
    
    master = fde->mdisk.itb_master;
    mdsl_storage_fd_put(fde);

    /* ok, get the itb location now, try to read the itb in file itb-* */
    fde = mdsl_storage_fd_lookup_create(1, MDSL_STORAGE_ITB, 
                                        master);
    if (IS_ERR(fde)) {
        hvfs_err(mdsl, "lookup create ITB file failed w/ %ld\n", PTR_ERR(fde));
        err = PTR_ERR(fde);
        goto out_put2;
    }

    hvfs_err(mdsl, "read from offset %ld\n", location);
    msa.offset = location;
    itb_iov.iov_base = itb;
    itb_iov.iov_len = sizeof(*itb);
    err = mdsl_storage_fd_read(fde, &msa);
    if (err) {
        hvfs_err(mdsl, "fd read failed w/ %d\n", err);
        goto out_put2;
    }

    hvfs_err(mdsl, "Read ITB %ld len %d\n", itb->h.itbid, atomic_read(&itb->h.len));
    data_len = atomic_read(&itb->h.len) - sizeof(*itb);
    if (data_len) {
        data = xmalloc(data_len);
        if (!data) {
            hvfs_err(mdsl, "shit\n");
            goto out_put2;
        }
        /* FIXME! */
    }
    
out_put2:
    mdsl_storage_fd_put(fde);
out:

    return err;
}

int main(int argc, char *argv[])
{
    int err = 0;

    hvfs_info(mdsl, "MDSL storage Unit Test ...\n");

    mdsl_pre_init();
    mdsl_init();
    hmo.site_id = HVFS_MDSL(0);
    mdsl_verify();

#if 0
    err = __test_append_buf();
    if (err) {
        hvfs_err(mdsl, "append buf test failed w/ %d\n", err);
        goto out;
    }

    err = __test_fdht();
    if (err) {
        hvfs_err(mdsl, "test fdht failed w/ %d\n", err);
        goto out;
    }

    err = __test_all();
    if (err) {
        hvfs_err(mdsl, "test all failed w/ %d\n", err);
        goto out;
    }
#endif
    err = __test_read();
    if (err) {
        hvfs_err(mdsl, "test read failed w/ %d\n", err);
        goto out;
    }

    mdsl_destroy();

out:
    return err;
}
#endif
