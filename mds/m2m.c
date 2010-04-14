/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2010-04-14 18:46:43 macan>
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
#include "mds.h"
#include "xtable.h"
#include "tx.h"
#include "xnet.h"
#include "ring.h"
#include "lib.h"

static inline 
void mds_send_reply(struct xnet_msg *msg, struct hvfs_md_reply *hmr,
                    int err)
{
    struct xnet_msg *rpy = xnet_alloc_msg(XNET_MSG_CACHE);

    if (!rpy) {
        hvfs_err(mds, "xnet_alloc_msg() failed\n");
        /* do not retry myself */
        return;
    }

    hmr->err = err;
#ifdef XNET_EAGER_WRITEV
    xnet_msg_add_sdata(rpy, &rpy->tx, sizeof(struct xnet_msg_tx));
#endif
    if (!err) {
        xnet_msg_add_sdata(rpy, hmr, sizeof(*hmr));
        if (hmr->len)
            xnet_msg_add_sdata(rpy, hmr->data, hmr->len);
    } else {
        xnet_msg_set_err(rpy, hmr->err);
        if (hmr->data)
            xfree(hmr->data);
        xfree(hmr);
    }

    xnet_msg_fill_tx(rpy, XNET_MSG_RPY, XNET_NEED_DATA_FREE, hmo.site_id,
                     msg->tx.ssite_id);
    xnet_msg_fill_reqno(rpy, msg->tx.reqno);
    xnet_msg_fill_cmd(rpy, XNET_RPY_DATA, 0, 0);
    /* match the original request at the source site */
    rpy->tx.handle = msg->tx.handle;

    if (xnet_send(hmo.xc, rpy)) {
        hvfs_err(mds, "xnet_send() failed\n");
        /* do not retry myself */
    }
    xnet_free_msg(rpy);         /* data auto free */
}

static inline
void __customized_send_bitmap(struct xnet_msg *msg, struct iovec iov[], int nr)
{
    struct xnet_msg *rpy = xnet_alloc_msg(XNET_MSG_CACHE);
    int i;

    if (!rpy) {
        hvfs_err(mds, "xnet_alloc_msg() failed\n");
        /* do not retry myself */
        return;
    }
#ifdef XNET_EAGER_WRITEV
    xnet_msg_add_sdata(rpy, &rpy->tx, sizeof(rpy->tx));
#endif
    for (i = 0; i < nr; i++) {
        xnet_msg_add_sdata(rpy, iov[i].iov_base, iov[i].iov_len);
    }
    xnet_msg_fill_tx(rpy, XNET_MSG_RPY, 0, hmo.site_id,
                     msg->tx.ssite_id);
    xnet_msg_fill_reqno(rpy, msg->tx.reqno);
    xnet_msg_fill_cmd(rpy, XNET_RPY_DATA, 0, 0);
    /* match the original request at the source site */
    rpy->tx.handle = msg->tx.handle;

    if (xnet_send(hmo.xc, rpy)) {
        hvfs_err(mds, "xnet_isend() failed\n");
        /* do not retyr myself, client is forced to retry */
    }
    xnet_free_msg(rpy);
}

/* mds_ldh() use the hvfs_index interface, so request forward is working.
 */
void mds_ldh(struct xnet_msg *msg)
{
    struct hvfs_index *hi = NULL;
    struct hvfs_md_reply *hmr = NULL;
    struct hvfs_txg *txg;
    struct dhe *e;
    struct chp *p;
    u64 itbid;
    int err = 0;

    /* sanity checking */
    if (msg->tx.len < sizeof(*hi)) {
        hvfs_err(mds, "Invalid LDH request %d received from %lx\n", 
                 msg->tx.reqno, msg->tx.ssite_id);
        err = -EINVAL;
        goto send_rpy;
    }

    if (msg->xm_datacheck)
        hi = msg->xm_data;
    else {
        hvfs_err(mds, "Internal error, data lossing ...\n");
        err = -EINVAL;
        goto send_rpy;
    }

    if (!(hi->flag & INDEX_BY_UUID)) {
        err = -EINVAL;
        goto send_rpy;
    }

    e = mds_dh_search(&hmo.dh, hmi.gdt_uuid);
    if (IS_ERR(e)) {
        /* fatal error */
        hvfs_err(mds, "This is a fatal error, we can not find the GDT DHE.\n");
        err = PTR_ERR(e);
        goto send_rpy;
    }
    itbid = mds_get_itbid(e, hi->hash);
    if (itbid != hi->itbid || hmo.conf.option & HVFS_MDS_CHRECHK) {
        p = ring_get_point(itbid, hmi.gdt_salt, hmo.chring[CH_RING_MDS]);
        if (IS_ERR(p)) {
            hvfs_err(mds, "ring_get_point() failed w/ %ld\n", PTR_ERR(p));
            err = -ECHP;
            goto send_rpy;
        }
        if (hmo.site_id != p->site_id) {
            /* FIXME: forward */
            if (itbid == hi->itbid) {
                /* itbid is correct, but ring changed */
                err = -ERINGCHG;
                goto send_rpy;
            }
            /* doing the forward now */
            hi->flag |= INDEX_BIT_FLIP;
            hi->itbid = itbid;
            err = mds_do_forward(msg, p->site_id);
            goto out;
        }
        hi->itbid = itbid;
    }

    /* alloc hmr */
    hmr = get_hmr();
    if (!hmr) {
        hvfs_err(mds, "get_hmr() failed\n");
        /* do not retry myself */
        goto out;
    }
    hi->flag |= INDEX_LOOKUP;
    hi->puuid = hmi.gdt_uuid;
    hi->psalt = hmi.gdt_salt;

    /* search in the CBHT */
    txg = mds_get_open_txg(&hmo);
    err = mds_cbht_search(hi, hmr, txg, &txg);
    txg_put(txg);

actually_send:
    mds_send_reply(msg, hmr, err);
out:
    xnet_free_msg(msg);
    return;
send_rpy:
    hmr = get_hmr();
    if (!hmr) {
        hvfs_err(mds, "get_hmr() failed\n");
        /* do not retry myself */
        return;
    }
    goto actually_send;
}

void mds_ausplit(struct xnet_msg *msg)
{
    struct itb *i, *ti;
    struct bucket *nb;
    struct bucket_entry *nbe;
    struct hvfs_txg *t;
    int err = 0;

    /* sanity checking */
    if (msg->tx.len < sizeof(struct itb)) {
        hvfs_err(mds, "Invalid SPITB request %d received from %lx\n",
                 msg->tx.reqno, msg->tx.ssite_id);
        err = -EINVAL;
        goto send_rpy;
    }

    if (msg->xm_datacheck)
        i = msg->xm_data;
    else {
        hvfs_err(mds, "Internal error, data lossing ...\n");
        err = -EINVAL;
        goto send_rpy;
    }

    /* checking the ITB */
    ASSERT(msg->tx.len == atomic_read(&i->h.len), mds);
    
    /* pre-dirty the itb */
    t = mds_get_open_txg(&hmo);
    i->h.txg = t->txg;
    i->h.state = ITB_STATE_DIRTY;
    /* re-init */
    itb_reinit(i);

    txg_add_itb(t, i);
    txg_put(t);

    /* insert the ITB to CBHT */
    err = mds_cbht_insert_bbrlocked(&hmo.cbht, i, &nb, &nbe, &ti);
    if (err == -EEXIST) {
        /* someone has already create the new ITB, we just ignore ourself? */
        hvfs_err(mds, "Someone create ITB %ld, maybe data lossing ...\n",
                 i->h.itbid);
        xrwlock_runlock(&nbe->lock);
        xrwlock_runlock(&nb->lock);
    } else if (err) {
        hvfs_err(mds, "Internal error %d, data lossing.\n", err);
    }

    /* it is ok, we need to free the locks */
    xrwlock_runlock(&nbe->lock);
    xrwlock_runlock(&nb->lock);

    mds_dh_bitmap_update(&hmo.dh, i->h.puuid, i->h.itbid,
                         MDS_BITMAP_SET);
    /* FIXME: if we using malloc to alloc the ITB, then we need to inc the
     * csize counter */
    atomic64_inc(&hmo.prof.mds.ausplit);
    atomic64_add(atomic_read(&i->h.entries), &hmo.prof.cbht.aentry);

    hvfs_debug(mds, "We update the bit of ITB %ld\n", i->h.itbid);

send_rpy:
    {
        struct xnet_msg *rpy;

    alloc_retry:
        rpy = xnet_alloc_msg(XNET_MSG_CACHE);
        if (!rpy) {
            hvfs_err(mds, "xnet_alloc_msg() failed, we should retry!\n");
            goto alloc_retry;
        }
#ifdef XNET_EAGER_WRITEV
        xnet_msg_add_sdata(rpy, &rpy->tx, sizeof(struct xnet_msg_tx));
#endif
        xnet_msg_set_err(rpy, err);
        xnet_msg_fill_tx(rpy, XNET_MSG_RPY, 0, hmo.site_id,
                         msg->tx.ssite_id);
        xnet_msg_fill_reqno(rpy, msg->tx.reqno);
        xnet_msg_fill_cmd(rpy, XNET_RPY_ACK, 0, 0);
        /* match the original request at the source site */
        rpy->tx.handle = msg->tx.handle;

        if (xnet_send(hmo.xc, rpy)) {
            hvfs_err(mds, "xnet_send() failed\n");
        }
        hvfs_debug(mds, "We have sent the AU reply msg from %lx to %lx\n",
                   rpy->tx.ssite_id, rpy->tx.dsite_id);
        xnet_free_msg(rpy);
    }
    xnet_free_msg(msg);         /* do not free the allocated ITB */
}

void mds_forward(struct xnet_msg *msg)
{
    struct mds_fwd *mf;
    struct xnet_msg_tx *tx;
    /* FIXME: we know we are using xnet-simple, so all the receiving iovs are
     * packed into one buf, we should save the begin address here */
    
    xnet_set_auto_free(msg);

    /* sanity checking */
    if (likely(msg->xm_datacheck)) {
        tx = msg->xm_data;
        mf = msg->xm_data + tx->len + sizeof(*tx);
    } else {
        hvfs_err(mds, "Internal error, data lossing ...\n");
        goto out;
    }
#if 0
    {
        int i, pos = 0;
        char line[256];

        memset(line, 0, sizeof(line));
        pos += snprintf(line, 256, "FW request from %lx route ", tx->ssite_id);
        for (i = 0; i < ((mf->len - sizeof(*mf)) / sizeof(u64)); i++) {
            pos += snprintf(line + pos, 256 - pos, "%lx->", mf->route[i]);
        }
        pos += snprintf(line + pos, 256 - pos, "%lx(E).\n", hmo.site_id);
        hvfs_err(mds, "%s", line);
    }
#endif
    memcpy(&msg->tx, tx, sizeof(*tx));
    /* FIXME: we know there is only one iov entry */
    msg->tx.flag |= (XNET_PTRESTORE | XNET_FWD);
    msg->tx.reserved = (u64)msg->xm_data;
    msg->xm_data += sizeof(*tx);
    msg->tx.dsite_id = hmo.site_id;

    atomic64_inc(&hmo.prof.mds.forward);
    mds_fe_dispatch(msg);

    return;
out:
    xnet_free_msg(msg);
}

/* actually we do not send the normal reply message, we send another request
 * message.
 */
void mds_aubitmap(struct xnet_msg *msg)
{
    struct bc_delta *bd;
    struct dhe *e;
    struct chp *p;
    u64 hash, itbid;
    int err = 0;

    /* sanity checking */
    if (msg->tx.len != 0) {
        hvfs_err(mds, "Invalid AUBITMAP request %d received from %lx\n",
                 msg->tx.reqno, msg->tx.ssite_id);
        err = -EINVAL;
        goto send_rpy;
    }

    /* recheck whether ourself is the target site for the bitmap cache
     * entry */
    e = mds_dh_search(&hmo.dh, hmi.gdt_uuid);
    if (IS_ERR(e)) {
        /* fatal error */
        hvfs_err(mds, "This is a fatal error, we can not find the GDT DHE.\n");
        err = PTR_ERR(e);
        goto send_rpy;
    }
    hash = hvfs_hash_gdt(msg->tx.arg0, hmi.gdt_salt);
    itbid = mds_get_itbid(e, hash);
    if (itbid != msg->tx.arg1 || hmo.conf.option & HVFS_MDS_CHRECHK) {
        p = ring_get_point(itbid, hmi.gdt_salt, hmo.chring[CH_RING_MDS]);
        if (IS_ERR(p)) {
            hvfs_err(mds, "ring_get_point() failed w/ %ld\n", PTR_ERR(p));
            err = -ECHP;
            goto send_rpy;
        }
        if (hmo.site_id != p->site_id) {
            /* forward it */
            if (itbid == msg->tx.arg1) {
                /* itbid is correct, but ring changed */
                err = -ERINGCHG;
                goto send_rpy;
            }
#if 0
            /* we should do the forward, but if we do forward, we need a hi
             * struct to log some additional info. It is a little bad, so we
             * just reply w/ a bitmap change error. */
            err = -EBITMAP;
            goto send_rpy;
#else
            err = mds_do_forward(msg, p->site_id);
            goto out;
#endif
        }
    }

    bd = mds_bc_delta_alloc();
    if (!bd) {
        hvfs_err(mds, "mds_bc_delta_alloc() failed.\n");
    }
    /* set site_id to ssite_id to send the reply message */
    bd->site_id = msg->tx.ssite_id;
    bd->uuid = msg->tx.arg0;
    bd->itbid = msg->tx.arg1;

    /* Then, we should add this bc_delta to the BC */
    xlock_lock(&hmo.bc.delta_lock);
    list_add(&bd->list, &hmo.bc.deltas);
    xlock_unlock(&hmo.bc.delta_lock);
    
out:
    return;
send_rpy:
    /* Note that: we do NOT reply actually, the sender do not block on the
     * receiving! if isend is working, maybe we can do the reply 8-) */
    hvfs_err(mds, "handle AUBITMAP w/ %d.\n", err);
}

void mds_m2m_lb(struct xnet_msg *msg)
{
    /* arg0: uuid; arg1: offset(aligned) */
    struct ibmap ibmap;
    struct hvfs_index hi;
    struct hvfs_md_reply *hmr;
    struct bc_entry *be;
    struct dhe *gdte;
    u64 location, size, offset;
    int err = 0;

    /* first, we should get hte bc_entry */
    gdte = mds_dh_search(&hmo.dh, hmi.gdt_uuid);
    if (IS_ERR(gdte)) {
        /* fatal error */
        hvfs_err(mds, "This is a fatal error, we can not find the GDT DHE.\n");
        err = PTR_ERR(gdte);
        goto out_free;
    }
    
    memset(&hi, 0, sizeof(hi));
    hi.uuid = msg->tx.arg0;
    hi.puuid = hmi.gdt_uuid;
    hi.psalt = hmi.gdt_salt;
    hi.hash = hvfs_hash_gdt(hi.uuid, hmi.gdt_salt);
    hi.itbid = mds_get_itbid(gdte, hi.hash);

    offset = msg->tx.arg1;
    /* cut the bitmap to valid range */
    err = mds_bc_dir_lookup(&hi, &location, &size);
    if (err) {
        hvfs_err(mds, "bc_dir_lookup failed w/ %d\n", err);
        goto send_err_rpy;
    }

    if (size == 0) {
        /* this means that offset should be ZERO */
        offset = 0;
    } else {
        /* Caution: we should cut the offset to the valid bitmap range
         * by size! */
        offset = mds_bitmap_cut(offset, size << 3);
        offset = BITMAP_ROUNDUP(offset);
    }

    be = mds_bc_get(msg->tx.arg0, offset);
    if (IS_ERR(be)) {
        if (be == ERR_PTR(-ENOENT)) {
            struct iovec iov[2];
            struct bc_entry *nbe;

            /* ok, we should create one bc_entry now */
            be = mds_bc_new();
            if (!be) {
                hvfs_err(mds, "New BC entry failed\n");
                err = -ENOMEM;
                goto send_err_rpy;
            }
            mds_bc_set(be, hi.uuid, offset);

            /* we should load the bitmap from mdsl */
            err = mds_bc_dir_lookup(&hi, &location, &size);
            if (err) {
                hvfs_err(mds, "bc_dir_lookup failed w/ %d\n", err);
                goto send_err_rpy;
            }

            if (size == 0) {
                /* this means that we should just return a new default bitmap
                 * slice */
                be->array[0] = 0xff;
            } else {
                /* load the bitmap slice from MDSL */
                err = mds_bc_backend_load(be, hi.itbid, location);
                if (err) {
                    hvfs_err(mds, "bc_backend_load failed w/ %d\n", err);
                    mds_bc_free(be);
                    goto send_err_rpy;
                }
            }

            /* finally, we insert the bc into the cache, should we check
             * whether there is a conflict? */
            nbe = mds_bc_insert(be);
            if (nbe != be) {
                mds_bc_free(be);
                be = nbe;
            }
            /* we need to send the reply w/ the bitmap data */
            ibmap.offset = be->offset;
            /* FIXME */
            ibmap.flag = ((size - (be->offset >> 3) > XTABLE_BITMAP_BYTES) ? 0 :
                          BITMAP_END);
            ibmap.ts = time(NULL);
            iov[0].iov_base = &ibmap;
            iov[0].iov_len = sizeof(struct ibmap);
            iov[1].iov_base = be->array;
            iov[1].iov_len = XTABLE_BITMAP_BYTES;
            __customized_send_bitmap(msg, iov, 2);

            mds_bc_put(be);
        } else {
            hvfs_err(mds, "bc_get() failed w/ %d\n", err);
            goto send_err_rpy;
        }
    } else {
        /* we find the entry in the cache, jsut return the bitmap array */
        /* FIXME: be sure to put the bc_entry after copied */
        struct iovec iov[2];

        ibmap.offset = be->offset;
        ibmap.flag = ((size - (be->offset >> 3) > XTABLE_BITMAP_BYTES) ? 0 :
                      BITMAP_END);
        ibmap.ts = time(NULL);
        iov[0].iov_base = &ibmap;
        iov[0].iov_len = sizeof(struct ibmap);
        iov[1].iov_base = be->array;
        iov[1].iov_len = XTABLE_BITMAP_BYTES;
        __customized_send_bitmap(msg, iov, 2);

        mds_bc_put(be);
    }

out_free:
    xnet_free_msg(msg);

    return;
send_err_rpy:
    hmr = get_hmr();
    if (!hmr) {
        hvfs_err(mds, "get_hmr() failed\n");
        return;
    }
    mds_send_reply(msg, hmr, err);
    goto out_free;
}

void mds_aubitmap_r(struct xnet_msg *msg)
{
    /* we should call async_aubitmap_cleanup to remove the entry in the
     * g_bitmap_deltas list */
    async_aubitmap_cleanup(msg->tx.arg0, msg->tx.arg1);
    xnet_free_msg(msg);
}
