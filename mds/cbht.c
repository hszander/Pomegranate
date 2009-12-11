/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2009-12-11 13:45:18 macan>
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
#include "xtable.h"
#include "mds.h"

#define SEG_BASE (16 * 1024)
#define SEG_TOTAL (SEG_BASE)    /* # of entries */

int mds_seg_alloc(struct segment *s, struct eh *eh)
{
    int err = 0;

    s->seg = xzalloc(s->alen * sizeof(struct bucket *));
    if (!s->seg) {
        hvfs_err(mds, "xzalloc() seg failed\n");
        err = -ENOMEM;
    }

    return 0;
}

void mds_seg_free(struct segment *s)
{
    if (s->seg)
        xfree(s->seg);
    s->len = 0;
    s->seg = NULL;
}

static inline struct segment *mds_segment_alloc()
{
    struct segment *s;
    
    s = xzalloc(sizeof(struct segment));
    if (s) {
        INIT_LIST_HEAD(&s->list);
    }
    return s;
}

int mds_segment_init(struct segment *s, u64 offset, u32 alen, struct eh *eh)
{
    s->offset = offset;
    s->alen = alen;
    s->len = 0;
    return mds_seg_alloc(s, eh);
}

struct bucket *cbht_bucket_alloc(int depth)
{
    struct bucket *b;
    struct bucket_entry *be;
    int i;

    b = xzalloc(sizeof(struct bucket));
    if (!b) {
        return NULL;
    }
#ifdef UNIT_TEST
    hvfs_debug(mds, "alloc bucket %p, depth %d\n", b, depth);
#endif
    b->content = xzalloc(sizeof(struct bucket_entry) * (1 << depth));
    if (!b->content) {
        return NULL;
    }
    /* init the bucket */
    b->adepth = depth;
    xrwlock_init(&b->lock);

    be = (struct bucket_entry *)(b->content);
    for (i = 0; i < (1 << depth); i++) {
        INIT_HLIST_HEAD(&(be + i)->h);
        xrwlock_init(&(be + i)->lock);
    }

    return b;
}

/*
 * Internal use, not export
 */
int cbht_bucket_init(struct eh *eh, struct segment *s)
{
    struct bucket *b;

    /* alloc bucket #0 */
    b = cbht_bucket_alloc(eh->bucket_depth);
    if (!b) {
        hvfs_err(mds, "cbht_bucket_alloc() failed\n");
        return -ENOMEM;
    }

    b->id = 0;                  /* set bucket id to 0 */
    *((struct bucket **)(s->seg)) = b;
    s->len = 1;
    
    return 0;
}

inline int segment_update_dir(struct eh *eh, u64 len, struct bucket *b)
{
    /* follow the <b->id> to change all matched dir entries */
    struct segment *s;
    u64 mask = (1 << atomic_read(&b->depth)) - 1;
    int i;
    
    list_for_each_entry(s, &eh->dir, list) {
        for (i = 0; i < s->len; i++) {
            if (((s->offset + i) & mask) == (b->id & mask)) {
                *(((struct bucket **)s->seg) + i) = b;
                hvfs_debug(mds, "D #%d Update @ %d w/ %p %ld\n", 
                           eh->dir_depth, i, b, b->id);
            }
            if (!(--len))
                break;
        }
    }
    
//    cbht_print_dir(eh);
    return 0;
}

void cbht_print_dir(struct eh *eh)
{
    struct segment *s;
    int i;

    hvfs_info(mds, "CBHT depth %d\n", eh->dir_depth);
    list_for_each_entry(s, &eh->dir, list) {
        for (i = 0; i < s->len; i++) {
            hvfs_info(mds, "offset %016d %p\n", i,
                      *(((struct bucket **)s->seg) + i));
        }
    }
}

/* cbht_copy_dir()
 */
void cbht_copy_dir(struct segment *s, u64 offset, u64 len, struct eh *eh)
{
    u64 clen, cplen = 0;
    struct segment *pos;
    
    /* NOTE: have not check the arguments */
    list_for_each_entry(pos, &eh->dir, list) {
        if (pos->offset <= offset && offset < (pos->offset + pos->len)) {
            clen = min(len, (pos->offset + pos->len - offset));
            hvfs_debug(mds, "copy to [%ld,%ld) from [%ld,%ld), clen %ld\n",
                       s->offset + s->len, s->offset + s->len + clen,
                       offset, offset + len, clen);
            memcpy(((struct bucket **)s->seg) + s->len + cplen, 
                   ((struct bucket **)pos->seg) + (offset - pos->offset), 
                   clen * sizeof(struct bucket **));
            len -= clen;
            cplen += clen;
            offset += clen;
        }
        if (!len)
            break;
    }
}

/* cbht_enlarge_dir()
 *
 * @tdepth: target depth
 *
 * double the directory
 */
int cbht_enlarge_dir(struct eh *eh, u32 tdepth)
{
    u32 olen = (1 << eh->dir_depth);
    u32 nlen = olen;
    struct segment *s, *ss = NULL;
    u64 offset;
    int err = 0;

    if (tdepth == eh->dir_depth) {
        /* already enlarged */
        goto out;
    }
    list_for_each_entry(s, &eh->dir, list) {
        if (olen == s->len) {
            /* enlarge from this segment */
            ss = s;
            break;
        } else {
            olen -= s->len;
        }
    }
    /* get the begining segment */
    if (!ss) {
        hvfs_err(mds, "internal error on cbht dir segment.\n");
        err = -EINVAL;
        goto out;
    }
    offset = 0;
    while (nlen > 0) {
        olen = ss->alen - ss->len; /* writable region in this segment */
        olen = min(olen, nlen);
        if (olen) {
            cbht_copy_dir(ss, offset, olen, eh);
            nlen -= olen;
            offset += olen;
            ss->len += olen;
        }
        /* next segment */
        if (nlen && (ss->list.next == &eh->dir)) {
            /* should allocate a new segment */
            s = mds_segment_alloc();
            if (!s) {
                err = -ENOMEM;
                goto out;
            }
            err = mds_segment_init(s, ss->offset + ss->alen, ss->alen, eh);
            if (err)
                goto out;
            ss = s;
            /* add to the dir list */
            list_add_tail(&s->list, &eh->dir);
        }
    }
    /* ok to change the depth */
    eh->dir_depth++;

out:
    return err;
}

/* cbht_update_dir()
 */
int cbht_update_dir(struct eh *eh, struct bucket *b)
{
    int err;

    xrwlock_wlock(&eh->lock);

    /* enlarge dir? */
    if (atomic_read(&b->depth) > eh->dir_depth) {
        err = cbht_enlarge_dir(eh, atomic_read(&b->depth));
        if (err)
            goto out;
    }

    err = segment_update_dir(eh, (1 << eh->dir_depth), b);
    
out:
    xrwlock_wunlock(&eh->lock);
    return err;
}

/* cbht_bucket_split()
 *
 * Note: holding the bucket.rlock
 */
int cbht_bucket_split(struct eh *eh, struct bucket *ob, u64 criminal,
                      struct bucket **out)
{
#define IN_NEW 0
#define IN_OLD 1
    /* Note that we do not know how many levels should we split, so just
     * repeat spliting until the bucket is all NOT full!
     */
    struct bucket *nb, *tb;
    struct bucket_entry *obe, *nbe;
    struct hlist_node *pos, *n;
    struct itbh *ih;
    int err = 0, i, in;

    xrwlock_runlock(&ob->lock);
    /* change it to wlock */
    xrwlock_wlock(&ob->lock);
    /* recheck if the bucket has been split */
    if (atomic_read(&ob->active) < (2 << ob->adepth)) {
        /* ok, this bucket has been split, release all the lock */
        xrwlock_wunlock(&ob->lock);
        return -EAGAIN;
    }

    /* ok, we get the bucket.wlock, and the bucket should be split */
retry:
    nb = cbht_bucket_alloc(eh->bucket_depth);
    if (!nb) {
        hvfs_err(mds, "cbht_bucket_alloc() failed\n");
        err = -ENOMEM;
        goto out_lock;
    }
    atomic_inc(&ob->depth);
    atomic_set(&nb->depth, atomic_read(&ob->depth));
    /* set bucket id */
    nb->id = ob->id | (1 << (atomic_read(&nb->depth) - 1));

    /* move ITBs */
    obe = ob->content;
    nbe = nb->content;

    /* we know that the bucket has been totally locked, so no be.*lock is
     * needed */
    for (i = 0; i < (1 << eh->bucket_depth); i++) {
        hlist_for_each_entry_safe(ih, pos, n, &(obe + i)->h, cbht) {
            hvfs_debug(mds, "hash 0x%lx\n", ih->hash);
            if ((ih->hash >> eh->bucket_depth) & 
                (1 << (atomic_read(&nb->depth) - 1))) {
                /* move the new ITB */
                hlist_del_init(pos);
                hlist_add_head(pos, &(nbe + i)->h);

                /* no need to lock the itb */
                ih->be = nbe + i;

                atomic_inc(&nb->active);
                atomic_dec(&ob->active);
            }
        }
    }
    /* one-level split has completed, but ... */
    if ((criminal >> eh->bucket_depth) & (1 << (atomic_read(&nb->depth) - 1)))
        in = IN_NEW;
    else
        in = IN_OLD;

    /* pre-locked new bucket, we know the two new buckets are all wlocked */
    xrwlock_wlock(&nb->lock);
    
    /* update the directory */
    err = cbht_update_dir(eh, nb);
    if (err) {
        xrwlock_wunlock(&nb->lock);
        goto out_lock;
    }
    hvfs_debug(mds, "in %d: ob %d nb %d. eh->depth %d\n", 
               in, atomic_read(&ob->active),
               atomic_read(&nb->active), eh->dir_depth);

    if (!atomic_read(&nb->active) && (in == IN_OLD)) {
        /* old bucket need deeply split */
        /* keep ob not changed and nb unlocked */
        tb = ob;
        xrwlock_wunlock(&nb->lock);
    } else if (!atomic_read(&ob->active) && (in == IN_NEW)) {
        /* new bucket need deeply split */
        /* keep nb locked */
        tb = nb;
        xrwlock_wunlock(&ob->lock);
    } else {
        /* ok, no one need deeply split */
        xrwlock_wunlock(&nb->lock);
        xrwlock_wunlock(&ob->lock);
        tb = NULL;
    }

    if (tb) {
        hvfs_debug(mds, "bucket %p need deeply split.\n", tb);
        ob = tb;
        goto retry;
    }

    if (in == IN_NEW) {
        xrwlock_rlock(&nb->lock);
        *out = nb;
    } else {
        xrwlock_rlock(&ob->lock);
        *out = ob;
    }

    err = 0;
    return err;

out_lock:
    xrwlock_wunlock(&ob->lock);
    xrwlock_rlock(&ob->lock);
    return err;
#undef IN_NEW
#undef IN_OLD
}

/* CBHT init
 *
 * @eh:     
 * @bdepth: bucket depth
 */
int mds_cbht_init(struct eh *eh, int bdepth)
{
    struct segment *s;
    int err = 0;
    
    /* do not move this region! */
    /* REGION BEGIN */
    eh->dir_depth = 0;
    eh->bucket_depth = bdepth;
    /* REGION END */

    INIT_LIST_HEAD(&eh->dir);
    xrwlock_init(&eh->lock);

    /* allocate the EH directory */
    s = mds_segment_alloc();
    if (!s) {
        hvfs_err(mds, "mds_segment_alloc() failed\n");
        return -ENOMEM;
    }

    /* init the segment */
    err = mds_segment_init(s, 0, SEG_TOTAL, eh);
    if (err)
        return err;

    /* init the bucket #0 */
    err = cbht_bucket_init(eh, s);
    if (err)
        return err;

    /* add to the dir list */
    xrwlock_wlock(&eh->lock);
    list_add_tail(&s->list, &eh->dir);
    xrwlock_wunlock(&eh->lock);

    return 0;
}

/* CBHT destroy w/o init
 *
 * @eh:
 *
 * Note: do NOT free any ITBs in CBHT, you must free them manually for now 
 */
void mds_cbht_destroy(struct eh *eh)
{
    struct segment *s, *n;
    
    xrwlock_wlock(&eh->lock);
    list_for_each_entry_safe(s, n, &eh->dir, list) {
        list_del(&s->list);
        mds_seg_free(s);
        xfree(s);
    }
    xrwlock_wunlock(&eh->lock);
}

/* CBHT insert with bucket/be rlocked at returning
 *
 * Note: holding nothing
 */
int mds_cbht_insert_bbrlocked(struct eh *eh, struct itb *i, struct bucket **ob,
                              struct bucket_entry **oe)
{
    u64 hash, offset;
    struct bucket *b, *sb;
    struct bucket_entry *be;
    u32 err;

    hash = hvfs_hash(i->h.puuid, i->h.itbid, sizeof(u64), HASH_SEL_CBHT);
    i->h.hash = hash;
    
retry:
    b = mds_cbht_search_dir(hash);
    if (IS_ERR(b)) {
        hvfs_err(mds, "No buckets exist? Find 0x%lx in the EH dir, "
                 "internal error!\n", i->h.itbid);
        return -ENOENT;
    }
    offset = hash & ((1 << eh->bucket_depth) - 1);
    be = b->content + offset;

    /* holding the bucket.rlock now */

    /* is the bucket will overflow? */
    if (atomic_read(&b->active) >= (2 << b->adepth)) {
        err = cbht_bucket_split(eh, b, hash, &sb);
        if (err == -EAGAIN)     /* already release the bucket.rlock? */
            goto retry;
        else if (err)
            goto out;
        b = sb;
        be = b->content + offset;
    }

    xrwlock_wlock(&be->lock);
    xrwlock_rlock(&i->h.lock);
    hlist_add_head(&i->h.cbht, &be->h);
    i->h.be = be;
    xrwlock_runlock(&i->h.lock);
    xrwlock_wunlock(&be->lock);

    atomic_inc(&b->active);
    xrwlock_rlock(&be->lock);
    *ob = b;
    *oe = be;
    return 0;
    
out:
    xrwlock_runlock(&b->lock);
    return err;
}

/* CBHT insert without any lock preservated at returning
 *
 * Note: holding nothing
 */
int mds_cbht_insert(struct eh *eh, struct itb *i)
{
    u64 hash, offset;
    struct bucket *b, *sb;
    struct bucket_entry *be;
    u32 err;
    
    hash = hvfs_hash(i->h.puuid, i->h.itbid, sizeof(u64), HASH_SEL_CBHT);
    i->h.hash = hash;
    
retry:
    b = mds_cbht_search_dir(hash);
    if (IS_ERR(b)) {
        hvfs_err(mds, "No buckets exist? Find 0x%lx in the EH dir, "
                 "internal error!\n", i->h.itbid);
        return -ENOENT;
    }
    offset = hash & ((1 << eh->bucket_depth) - 1);
    be = b->content + offset;

    /* holding the bucket.rlock now */

    /* is the bucket will overflow? */
    if (atomic_read(&b->active) >= (2 << b->adepth)) {
        err = cbht_bucket_split(eh, b, hash, &sb);
        if (err == -EAGAIN)     /* already release the bucket.rlock? */
            goto retry;
        else if (err)
            goto out;
        b = sb;
        be = b->content + offset;
    }
    
    xrwlock_wlock(&be->lock);
    xrwlock_rlock(&i->h.lock);
    hlist_add_head(&i->h.cbht, &be->h);
    i->h.be = be;
    xrwlock_runlock(&i->h.lock);
    xrwlock_wunlock(&be->lock);

    atomic_inc(&b->active);
    err = 0;
    
out:
    xrwlock_runlock(&b->lock);
    return err;
}

/* CBHT del
 *
 * Note: holding nothing
 */
int mds_cbht_del(struct eh *eh, struct itb *i)
{
    struct bucket_entry *be;
    struct bucket *b;
    u64 offset;

retry:
    b = mds_cbht_search_dir(i->h.hash);
    if (IS_ERR(b)) {
        hvfs_err(mds, "No buckets exist? Find 0x%lx in the EH dir, "
                 "internal error!\n", i->h.itbid);
        return -ENOENT;
    }
    offset = i->h.hash & ((1 << eh->bucket_depth) - 1);
    be = b->content +offset;

    /* holding the bucket.rlock now */

    /* is the bucket will underflow? FIXME: no underflow for now */
    xrwlock_wlock(&be->lock);
    xrwlock_rlock(&i->h.lock);
    /* recheck whether this itb has been moved? */
    if (i->h.be != be) {
        xrwlock_runlock(&i->h.lock);
        xrwlock_wunlock(&be->lock);
        xrwlock_runlock(&b->lock);
        goto retry;
    }
    hlist_del_init(&i->h.cbht);
    i->h.be = NULL;
    xrwlock_runlock(&i->h.lock);
    xrwlock_wunlock(&be->lock);

    xrwlock_runlock(&b->lock);
    return 0;
}

/* CBHT dir search
 *
 * if return value is not error, then the bucket rlock is holding
 *
 * Error Convention: kernel ptr-err!
 */
struct bucket *mds_cbht_search_dir(u64 hash)
{
    struct eh *eh = &hmo.cbht;
    struct segment *s;
    struct bucket *b = ERR_PTR(-ENOENT); /* ENOENT means can not find it */
    u64 offset;
    int found = 0, err = 0;
    u32 ldepth;                 /* saved current dir_depth */
    
retry:
    xrwlock_rlock(&eh->lock);
    ldepth = eh->dir_depth;
    offset = (hash >> eh->bucket_depth) & ((1 << ldepth) - 1);
    list_for_each_entry(s, &eh->dir, list) {
        if (s->offset <= offset && offset < (s->offset + s->len)) {
            found = 1;
            break;
        }
    }
    if (found) {
        offset -= s->offset;
        if (s->seg) {
            b = *(((struct bucket **)s->seg) + offset);
            hvfs_debug(mds, "hash 0x%lx, offset %ld\n", hash, offset);
            /* ok, holding the bucket rlock */
            err = xrwlock_tryrlock(&b->lock);
            if (err == EBUSY || err == EAGAIN) {
                /* EBUSY: somebody wlock the bucket for spliting? */
                /* EAGAIN: max rlock got, retry */
                /* OK: retry myself! */
                xrwlock_runlock(&eh->lock);
                found = 0;
                goto retry;
            } else if (err){
                b = ERR_PTR(err);
            }
        }
    }
            
    xrwlock_runlock(&eh->lock);

    return b;
}

/*
 * Note: holding the bucket.rlock, be.rlock
 */
int cbht_itb_hit(struct itb *i, struct hvfs_index *hi, 
                 struct hvfs_md_reply *hmr, struct hvfs_txg *txg)
{
    struct mdu *m;
    int err;
    char mdu_rpy[HVFS_MDU_SIZE];

    xrwlock_rlock(&i->h.lock);
    if (unlikely(hi->flag & INDEX_BY_ITB)) {
        /* readdir, read-only */
        err = itb_readdir(hi, i, hmr);
        return err;
    }
    err = itb_search(hi, i, mdu_rpy, txg);
    if (err) {
        hvfs_debug(mds, "Oh, itb_search() return %d.\n", err);
        goto out;
    }
    /* FIXME: fill hmr with mdu_rpy */
    /* determine the flags */
    m = (struct mdu *)(mdu_rpy);
    if (hi->flag & INDEX_BY_ITB)
        hmr->flag |= MD_REPLY_READDIR;

    if (S_ISDIR(m->mode) && hi->puuid != hmi.gdt_uuid)
        hmr->flag |= MD_REPLY_DIR_SDT;
    if (m->flags & HVFS_MDU_IF_LINKT) {
        hmr->flag |= MD_REPLY_WITH_LS;
        hmr->len += sizeof(struct link_source);
    } else {
        hmr->flag |= MD_REPLY_WITH_MDU;
        hmr->len += HVFS_MDU_SIZE;
        hmr->mdu_no = 1;        /* only ONE mdu */
    }
    
    hmr->flag |= MD_REPLY_WITH_HI;
    hmr->len += sizeof(*hi);

    hmr->data = xmalloc(hmr->len);
    if (!hmr->data) {
        hvfs_err(mds, "xalloc() hmr->data failed\n");
        /* do not retry myself */
        err = -ENOMEM;
        goto out;
    }
    memcpy(hmr->data, hi, sizeof(*hi));
    if (m->flags & HVFS_MDU_IF_LINKT) {
        memcpy(hmr->data + sizeof(*hi), mdu_rpy, sizeof(struct link_source));
    } else
        memcpy(hmr->data + sizeof(*hi), mdu_rpy, HVFS_MDU_SIZE);
out:
    xrwlock_runlock(&i->h.lock);
    return err;
}

/*
 * Note: holding nothing
 */
int cbht_itb_miss(struct hvfs_index *hi, 
                  struct hvfs_md_reply *hmr, struct hvfs_txg *txg)
{
    struct itb *i;
    struct bucket *b;
    struct bucket_entry *e;
    int err = 0;

    /* Step1: read the itb from mdsl */
    i = mds_read_itb(hi->puuid, hi->psalt, hi->itbid);
    if (IS_ERR(i)) {
        /* read itb failed, for what? */
        /* FIXME: why this happened? bitmap say this ITB exists! */
        /* FIXME: we should act on ENOENT, other errors should not handle */
        hvfs_debug(mds, "Why this happened? bitmap say this ITB exists!\n");
        if (hi->flag & INDEX_CREATE || hi->flag & INDEX_SYMLINK) {
            /* FIXME: create ITB and ITE */
            i = get_free_itb();
            if (!i) {
                hvfs_debug(mds, "get_free_itb() failed\n");
                err = -ENOMEM;
                goto out;
            }
            i->h.itbid = hi->itbid;
            i->h.puuid = hi->puuid;
            err = mds_cbht_insert_bbrlocked(&hmo.cbht, i, &b, &e);
            if (err) {
                goto out;
            }
            err = cbht_itb_hit(i, hi, hmr, txg);
            if (err == -EAGAIN) {
                /* release all the locks */
                xrwlock_runlock(&b->lock);
                xrwlock_runlock(&e->lock);
                goto out;
            }
            xrwlock_runlock(&e->lock);
            xrwlock_runlock(&b->lock);
            /* FIXME: */
        } else {
            /* return -ENOENT */
            err = -ENOENT;
            goto out;
        }
    } else {
        /* get it, do search on it */
        /* insert into the cbht */
        err = mds_cbht_insert_bbrlocked(&hmo.cbht, i, &b, &e);
        if (err) {
            goto out;
        }
        err = cbht_itb_hit(i, hi, hmr, txg);
        if (err == -EAGAIN) {
            /* release all the lockes */
            xrwlock_runlock(&b->lock);
            xrwlock_runlock(&e->lock);
            goto out;
        }
        xrwlock_runlock(&e->lock);
        xrwlock_runlock(&b->lock);
    }
out:
    return err;
}

/* General search in CBHT
 *
 * @hi: index to search, including flags, refer to "mds_api.h"
 * @hmr:should allocate the data region
 *
 * Search by key(puuid, itbid)
 * FIXME: how about LOCK!
 */
int mds_cbht_search(struct hvfs_index *hi, struct hvfs_md_reply *hmr,
                    struct hvfs_txg *txg)
{
    struct bucket *b;
    struct bucket_entry *be;
    struct itbh *ih;
    struct eh *eh = &hmo.cbht;
    struct hlist_node *pos;
    u64 hash, offset;
    int err = 0;

    hash = hvfs_hash(hi->puuid, hi->itbid, sizeof(u64), HASH_SEL_CBHT);

retry_dir:
    b = mds_cbht_search_dir(hash);
    if (IS_ERR(b)) {
        hvfs_err(mds, "No buckets exist? Find 0x%lx in the EH dir, "
                 "internal error!\n", hi->itbid);
        return -ENOENT;
    }
    /* OK, we get the bucket, and holding the bucket.rlock, no bucket spliting
     * can happen!
     */

    /* check the bucket */
    if (atomic_read(&b->active)) {
        offset = hash & ((1 << eh->bucket_depth) - 1);
        be = b->content + offset;
    } else {
        /* the bucket is empty, you can do creating */
        hvfs_debug(mds, "OK, empty bucket!\n");
        if (hi->flag & INDEX_CREATE) {
            /* FIXME: */
            xrwlock_runlock(&b->lock);
            err = cbht_itb_miss(hi, hmr, txg);
            if (err == -EAGAIN)
                goto retry_dir;
            return err;
        } else {
            err = -ENOENT;
            goto out;
        }
    }

    /* always holding the bucket.rlock */
    xrwlock_rlock(&be->lock);
retry:
    if (!hlist_empty(&be->h)) {
        hlist_for_each_entry(ih, pos, &be->h, cbht) {
            if (ih->puuid == hi->puuid && ih->itbid == hi->itbid) {
                /* OK, find the itb in the CBHT */
                err = cbht_itb_hit((struct itb *)ih, hi, hmr, txg);
                if (err == -EAGAIN) {
                    /* no need to release the be.rlock */
                    goto retry;
                }
                xrwlock_runlock(&be->lock);
                goto out;
            }
        }
    }
    xrwlock_runlock(&be->lock);

    /* Can not find it in CBHT, holding the bucket.rlock */
    /* Step1: release the bucket.rlock */
    hvfs_debug(mds, "hash 0x%lx bucket %p but can not find the ITB in it.\n", 
               hash, b);
    xrwlock_runlock(&b->lock);
    err = cbht_itb_miss(hi, hmr, txg);
    if (err == -EAGAIN)         /* all locks are released */
        goto retry_dir;
    hmr->err = err;
    return err;

out:
    /* put the bucket lock */
    xrwlock_runlock(&b->lock);
    hmr->err = err;
    return err;
}

#ifdef UNIT_TEST
void hmr_print(struct hvfs_md_reply *hmr)
{
    struct hvfs_index *hi;
    struct mdu *m;
    struct link_source *ls;
    void *p = hmr->data;

    hvfs_info(mds, "hmr-> err %d, mdu_no %d, len %d, flag 0x%lx.\n", 
              hmr->err, hmr->mdu_no, hmr->len, hmr->flag);
    if (!p)
        return;
    hi = (struct hvfs_index *)p;
    hvfs_info(mds, "hmr-> HI: len %d, flag 0x%x, uuid %ld, hash %ld, itbid %ld, "
              "puuid %ld, psalt %ld\n", hi->len, hi->flag, hi->uuid, hi->hash,
              hi->itbid, hi->puuid, hi->psalt);
    p += sizeof(struct hvfs_index);
    if (hmr->flag & MD_REPLY_WITH_MDU) {
        m = (struct mdu *)p;
        hvfs_info(mds, "hmr->MDU: size %ld, dev %ld, mode 0x%x, nlink %d, uid %d, "
                  "gid %d, flags 0x%x, atime %lx, ctime %lx, mtime %lx, dtime %lx, "
                  "version %d\n", m->size, m->dev, m->mode, m->nlink, m->uid,
                  m->gid, m->flags, m->atime, m->ctime, m->mtime, m->dtime,
                  m->version);
        p += sizeof(struct mdu);
    }
    if (hmr->flag & MD_REPLY_WITH_LS) {
        ls = (struct link_source *)p;
        hvfs_info(mds, "hmr-> LS: hash %ld, puuid %ld, uuid %ld\n",
                  ls->s_hash, ls->s_puuid, ls->s_uuid);
        p += sizeof(struct link_source);
    }
    if (hmr->flag & MD_REPLY_WITH_BITMAP) {
        hvfs_info(mds, "hmr-> BM: ...\n");
    }
}

void insert_itb(u64 puuid, u64 itbid, u64 txg)
{
    struct itb *i;
    int err;
    
    i = get_free_itb();
    i->h.puuid = puuid;
    i->h.itbid = itbid;
    i->h.txg = txg;
    i->h.state = ITB_STATE_CLEAN;

    err = mds_cbht_insert(&hmo.cbht, i);
    if (err) {
        hvfs_err(mds, "mds_cbht_insert() failed %d\n", err);
    }
}

void insert_ite(u64 puuid, u64 itbid, char *name, struct mdu_update *imu,
                struct hvfs_md_reply *hmr, struct hvfs_txg *txg)
{
    struct hvfs_index *hi;
    struct mdu_update *mu;
    int len, err;

    len = sizeof(struct hvfs_index) + strlen(name) + sizeof(struct mdu_update);

    hi = xzalloc(len);
    if (!hi)
        return;

    hi->hash = hvfs_hash(puuid, (u64)name, strlen(name), HASH_SEL_EH);
    hi->puuid = puuid;
    hi->itbid = itbid;
    hi->flag |= INDEX_CREATE;
    memcpy(hi->name, name, strlen(name));
    hi->len = strlen(name);
    mu = (struct mdu_update *)((void *)hi + sizeof(struct hvfs_index) + 
                               strlen(name));
    memcpy(mu, imu, sizeof(struct mdu_update));
    hi->data = mu;

    memset(hmr, 0, sizeof(*hmr));
    err = mds_cbht_search(hi, hmr, txg);
    if (err) {
        hvfs_err(mds, "mds_cbht_search(%ld, %ld, %s) failed %d\n", 
                 puuid, itbid, name, err);
    }
/*     hmr_print(hmr); */
    if (!hmr->err) {
        xfree(hmr->data);
    }

    xfree(hi);
}

void remove_ite(u64 puuid, u64 itbid, char *name, struct hvfs_md_reply *hmr,
                struct hvfs_txg *txg)
{
    struct hvfs_index *hi;
    int len, err;

    len = sizeof(struct hvfs_index) + strlen(name);

    hi = xzalloc(len);
    if (!hi)
        return;

    hi->hash = hvfs_hash(puuid, (u64)name, strlen(name), HASH_SEL_EH);
    hi->puuid = puuid;
    hi->itbid = itbid;
    hi->flag = INDEX_UNLINK | INDEX_BY_NAME;
    memcpy(hi->name, name, strlen(name));
    hi->len = strlen(name);
    hi->data = NULL;

    memset(hmr, 0, sizeof(*hmr));
    err = mds_cbht_search(hi, hmr, txg);
    if (err) {
        hvfs_err(mds, "mds_cbht_search(%ld, %ld, %s) failed %d\n", 
                 puuid, itbid, name, err);
    }
/*     hmr_print(hmr); */
    if (!hmr->err) {
        xfree(hmr->data);
    }
}

void lookup_ite(u64 puuid, u64 itbid, char *name, u64 flag , 
                struct hvfs_md_reply *hmr, struct hvfs_txg *txg)
{
    struct hvfs_index *hi;
    int len, err;

    len = sizeof(struct hvfs_index) + strlen(name);

    hi = xzalloc(len);
    if (!hi)
        return;

    hi->hash = hvfs_hash(puuid, (u64)name, strlen(name), HASH_SEL_EH);
    hi->puuid = puuid;
    hi->itbid = itbid;
    hi->flag = flag;
    memcpy(hi->name, name, strlen(name));
    hi->len = strlen(name);
    
    memset(hmr, 0, sizeof(*hmr));
    err = mds_cbht_search(hi, hmr, txg);
    if (err) {
        hvfs_err(mds, "mds_cbht_search(%ld, %ld, %s) failed %d\n", 
                 puuid, itbid, name, err);
        hvfs_err(mds, "hash 0x%20lx.\n", hvfs_hash(puuid, itbid, 
                                                   sizeof(u64), HASH_SEL_CBHT));
    }
/*     hmr_print(hmr); */
    if (!hmr->err) {
        xfree(hmr->data);
    }
    
    xfree(hi);
}

int st_main(int argc, char *argv[])
{
    struct timeval begin, end;
    struct mdu_update mu;
    struct hvfs_md_reply hmr;
    struct hvfs_txg txg = {.txg = 5,};
    u64 puuid, itbid;
    int i, j, k, x, bdepth, icsize;
    char name[HVFS_MAX_NAME_LEN];

    int err = 0;

    if (argc == 5) {
        /* the argv[1] is k, argv[2] is x, argv[3] is bucket.depth, argv[4] is
         * ITB Cache init size */
        k = atoi(argv[1]);
        x = atoi(argv[2]);
        bdepth = atoi(argv[3]);
        icsize = atoi(argv[4]);
    } else {
        k = 100;
        x = 100;
        bdepth = 4;
        icsize = 50;
    }
#ifdef HVFS_DEBUG_LOCK
    lock_table_init();
#endif
    hvfs_info(mds, "CBHT UNIT TESTing (single thread)...(%d,%d,%d,%d)\n", 
              k, x, bdepth, icsize);
    err = mds_cbht_init(&hmo.cbht, bdepth);
    if (err) {
        hvfs_err(mds, "mds_cbht_init failed %d\n", err);
        goto out;
    }
    /* print the init cbht */
    cbht_print_dir(&hmo.cbht);
    /* init hte itb cache */
    itb_cache_init(&hmo.ic, icsize);
    
    hvfs_info(mds, "ITC init success ...\n");

    hvfs_info(mds, "total struct itb = %ld\n", sizeof(struct itb) + 
              ITB_SIZE * sizeof(struct ite));
    hvfs_info(mds, "sizeof(struct itb) = %ld\n", sizeof(struct itb));
    hvfs_info(mds, "sizeof(struct itbh) = %ld\n", sizeof(struct itbh));
    hvfs_info(mds, "sizeof(struct ite) = %ld\n", sizeof(struct ite));
    
    /* alloc one ITB */
    insert_itb(0, 134, 5);

    /* insert the ite! */
    lib_timer_start(&begin);
    for (i = 0; i < k; i++) {
        puuid = 0;
        itbid = i;
        for (j = 0; j < x; j++) {
            mu.valid = MU_MODE | MU_UID;
            mu.mode = i;
            mu.uid = j;
            sprintf(name, "macan-%d-%d", i, j);
            insert_ite(puuid, itbid, name, &mu, &hmr, &txg);
        }
    }
    lib_timer_stop(&end);
    lib_timer_echo(&begin, &end, x * k);
    hvfs_info(mds, "Insert ite is done ...\n");

    /* lookup the ite! */
    lib_timer_start(&begin);
    for (i = 0; i < k; i++) {
        puuid = 0;
        itbid = i;
        for (j = 0; j < x; j++) {
            u64 flag;
            sprintf(name, "macan-%d-%d", i, j);
            flag = INDEX_LOOKUP | INDEX_BY_NAME | INDEX_ITE_ACTIVE;
            lookup_ite(puuid, itbid, name, flag, &hmr, &txg);
        }
    }
    lib_timer_stop(&end);
    lib_timer_echo(&begin, &end, x * k);
    hvfs_info(mds, "Lookup ite is done ...\n");

    /* unlink the ite, change state to SHADOW */
    lib_timer_start(&begin);
    for (i = 0; i < k; i++) {
        puuid = 0;
        itbid = i;
        for (j = 0; j < x; j++) {
            sprintf(name, "macan-%d-%d", i, j);
            remove_ite(puuid, itbid, name, &hmr, &txg);
        }
    }
    lib_timer_stop(&end);
    lib_timer_echo(&begin, &end, x * k);
    hvfs_info(mds, "Unlink ite is done ...\n");

    /* shadow lookup */
    lib_timer_start(&begin);
    for (i = 0; i < k; i++) {
        puuid = 0;
        itbid = i;
        for (j = 0; j < x; j++) {
            u64 flag;
            sprintf(name, "macan-%d-%d", i, j);
            flag = INDEX_LOOKUP | INDEX_BY_NAME | INDEX_ITE_SHADOW;
            lookup_ite(puuid, itbid, name, flag, &hmr, &txg);
        }
    }
    lib_timer_stop(&end);
    lib_timer_echo(&begin, &end, x * k);
    hvfs_info(mds, "Shadow lookup ite is done ...\n");
    
    itb_cache_destroy(&hmo.ic);
    /* print the init cbht */
    hvfs_info(mds, "CBHT dir depth %d\n", hmo.cbht.dir_depth);
#ifdef HVFS_DEBUG_LOCK
    lock_table_print();
#endif
/*     cbht_print_dir(&hmo.cbht); */
    mds_cbht_destroy(&hmo.cbht);
out:
    return err;
}

struct pthread_args
{
    int tid;                    /* thread index */
    int k, x, icsize;           /* dynamic */
    int bdepth;                 /* should be const */
    pthread_barrier_t *pb;
    double acc[4];              /* 4 operations */
};

void *pt_main(void *arg)
{
    struct pthread_args *pa = (struct pthread_args *)arg;
    struct timeval begin, end;
    struct mdu_update mu;
    struct hvfs_md_reply hmr;
    struct hvfs_txg txg = {.txg = 5,};
    u64 puuid, itbid, flag;
    int i, j;
    char name[HVFS_MAX_NAME_LEN];

    /* wait for other threads */
    pthread_barrier_wait(pa->pb);

    /* parallel insert the ite! */
    lib_timer_start(&begin);
    for (i = 0; i < pa->k; i++) {
        puuid = pa->tid;
        itbid = i;
        for (j = 0; j < pa->x; j++) {
            sprintf(name, "macan-%d-%d-%d", pa->tid, i, j);
            insert_ite(puuid, itbid, name, &mu, &hmr, &txg);
        }
    }
    lib_timer_stop(&end);
    lib_timer_acc(&begin, &end, &pa->acc[0]);

    /* wait for other threads */
    pthread_barrier_wait(pa->pb);

    /* parallel lookup the ite! */
    lib_timer_start(&begin);
    for (i = 0; i < pa->k; i++) {
        puuid = pa->tid;
        itbid = i;
        for (j = 0; j < pa->x; j++) {
            sprintf(name, "macan-%d-%d-%d", pa->tid, i, j);
            flag = INDEX_LOOKUP | INDEX_BY_NAME | INDEX_ITE_ACTIVE;
            lookup_ite(puuid, itbid, name, flag, &hmr, &txg);
        }
    }
    lib_timer_stop(&end);
    lib_timer_acc(&begin, &end, &pa->acc[1]);

    /* wait for other threads */
    pthread_barrier_wait(pa->pb);

    /* parallel unlink the ite! */
    lib_timer_start(&begin);
    for (i = 0; i < pa->k; i++) {
        puuid = pa->tid;
        itbid = i;
        for (j = 0; j < pa->x; j++) {
            sprintf(name, "macan-%d-%d-%d", pa->tid, i, j);
            remove_ite(puuid, itbid, name, &hmr, &txg);
        }
    }
    lib_timer_stop(&end);
    lib_timer_acc(&begin, &end, &pa->acc[2]);

    /* wait for other threads */
    pthread_barrier_wait(pa->pb);
    
   /* parallel shadow lookup the ite! */
    lib_timer_start(&begin);
    for (i = 0; i < pa->k; i++) {
        puuid = pa->tid;
        itbid = i;
        for (j = 0; j < pa->x; j++) {
            sprintf(name, "macan-%d-%d-%d", pa->tid, i, j);
            flag = INDEX_LOOKUP | INDEX_BY_NAME | INDEX_ITE_SHADOW;
            lookup_ite(puuid, itbid, name, flag, &hmr, &txg);
        }
    }
    lib_timer_stop(&end);
    lib_timer_acc(&begin, &end, &pa->acc[3]);

    pthread_barrier_wait(pa->pb);

    pthread_exit(0);
}

static inline char *idx2str(int i)
{
    switch (i) {
    case 0:
        return "start";
    case 1:
        return "insert";
    case 2:
        return "lookup";
    case 3:
        return "unlink";
    case 4:
        return "shadow";
    default:
        return "NULL";
    }
}

int mt_main(int argc, char *argv[])
{
    int err;
    int i, i1, j, k, x, bdepth, icsize;
    pthread_t *t;
    pthread_barrier_t pb;
    struct pthread_args *pa;
    double acc[4];

    if (argc == 5) {
        /* the argv[1] is k, argv[2] is x, argv[3] is bucket.depth, argv[4] is
         * ITB Cache init size */
        k = atoi(argv[1]);
        x = atoi(argv[2]);
        bdepth = atoi(argv[3]);
        icsize = atoi(argv[4]);
    } else {
        k = 100;
        x = 100;
        bdepth = 4;
        icsize = 50;
    }
#ifdef HVFS_DEBUG_LOCK
    lock_table_init();
#endif
    hvfs_info(mds, "CBHT UNIT TESTing (multi thread)...(%d,%d,%d,%d)\n", 
              k, x, bdepth, icsize);
    err = mds_cbht_init(&hmo.cbht, bdepth);
    if (err) {
        hvfs_err(mds, "mds_cbht_init failed %d\n", err);
        goto out;
    }
    /* print the init cbht */
    cbht_print_dir(&hmo.cbht);
    /* init hte itb cache */
    itb_cache_init(&hmo.ic, icsize);
    hvfs_info(mds, "ITC init success ...\n");

    hvfs_info(mds, "total struct itb = %ld\n", sizeof(struct itb) + 
              ITB_SIZE * sizeof(struct ite));
    hvfs_info(mds, "sizeof(struct itb) = %ld\n", sizeof(struct itb));
    hvfs_info(mds, "sizeof(struct itbh) = %ld\n", sizeof(struct itbh));
    hvfs_info(mds, "sizeof(struct ite) = %ld\n", sizeof(struct ite));

    /* setup multi-threads */
    t = xzalloc(atoi(argv[5]) * sizeof(pthread_t));
    if (!t) {
        hvfs_err(mds, "xzalloc() pthread_t failed.\n");
        err = ENOMEM;
        goto out;
    }
    pa = xzalloc(atoi(argv[5]) * sizeof(struct pthread_args));
    if (!pa) {
        hvfs_err(mds, "xzalloc() pthread_args failed.\n");
        err = ENOMEM;
        goto out_free;
    }

    pthread_barrier_init(&pb, NULL, atoi(argv[5]) + 1);

    /* determine the arguments */
    j = atoi(argv[5]);
    k = k / j * j;
    for (i = 0; i < j; i++) {
        pa[i].tid = i;
        pa[i].k = k / j;
        pa[i].x = x;
        pa[i].bdepth = bdepth;
        pa[i].icsize = icsize;
        pa[i].pb = &pb;
        err = pthread_create(&t[i], NULL, pt_main, (void *)&pa[i]);
        if (err) {
            hvfs_err(mds, "pthread_create err %d\n", err);
            goto out_free2;
        }
    }

    /* barrier 4 times */
    for (i = 0; i < 5; i++) {
        pthread_barrier_wait(&pb);
        hvfs_info(mds, "[%s] done.\n", idx2str(i));
    }
    
    /* waiting for all the threads */
    for (i = 0; i < j; i++) {
        pthread_join(t[i], NULL);
    }

    /* get the test result */
    hvfs_info(mds, "TEST result:\n");
    for (i1 = 0; i1 < 4; i1++) {
        acc[i1] = 0.0;
        for (i = 0; i < j; i++) {
            acc[i1] += pa[i].acc[i1];
        }
        acc[i1] /= k * x;
    }
    hvfs_info(mds, "[insert lookup unlink shadow] %lf %lf %lf %lf\n", 
              acc[0], acc[1], acc[2], acc[3]);

    hvfs_info(mds, "CBHT dir depth %d\n", hmo.cbht.dir_depth);
    hvfs_info(mds, "Average ITB search depth %lf\n", 
              atomic64_read(&hmo.profiling.itb.rsearch_depth) / 4.0 / (k * x));
    /* print the dir */
/*     cbht_print_dir(&hmo.cbht); */

out:
    return err;
out_free:
    xfree(t);
    goto out;
out_free2:
    xfree(pa);
    goto out_free;
}

int main(int argc, char *argv[])
{
    int err;

    if (argc == 6) {
        /* may be multi-thread test */
        argc--;
        if (atoi(argv[5]) > 0) {
            err = mt_main(argc, argv);
            goto out;
        }
    }

    err = st_main(argc, argv);
out:
    return err;
}

#endif
