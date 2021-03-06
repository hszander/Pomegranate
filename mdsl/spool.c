/**
 * Copyright (c) 2009 Ma Can <ml.macana@gmail.com>
 *                           <macan@ncic.ac.cn>
 *
 * Armed with EMACS.
 * Time-stamp: <2011-01-05 10:01:49 macan>
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
#include "ring.h"
#include "lib.h"

struct spool_mgr
{
    struct list_head reqin;
    xlock_t rin_lock;
    sem_t rin_sem;
};

struct spool_thread_arg
{
    int tid;
};

static struct spool_mgr spool_mgr;

int mdsl_spool_dispatch(struct xnet_msg *msg)
{
    xlock_lock(&spool_mgr.rin_lock);
    list_add_tail(&msg->list, &spool_mgr.reqin);
    xlock_unlock(&spool_mgr.rin_lock);
    atomic64_inc(&hmo.prof.misc.reqin_total);
    sem_post(&spool_mgr.rin_sem);

    return 0;
}

static inline
int __serv_request(void)
{
    struct xnet_msg *msg = NULL, *pos, *n;

    xlock_lock(&spool_mgr.rin_lock);
    list_for_each_entry_safe(pos, n, &spool_mgr.reqin, list) {
        list_del_init(&pos->list);
        msg = pos;
        break;
    }
    xlock_unlock(&spool_mgr.rin_lock);

    if (!msg)
        return -EHSTOP;

    /* check if this request can be dealed right now */
    if (mdsl_dispatch_check(msg)) {
        /* reinsert the request to the queue */
        xlock_lock(&spool_mgr.rin_lock);
        list_add_tail(&msg->list, &spool_mgr.reqin);
        xlock_unlock(&spool_mgr.rin_lock);
        sem_post(&spool_mgr.rin_sem);

        return 0;
    }

    /* ok, deal with it, we just calling the secondary dispatcher */
    ASSERT(msg->xc, mdsl);
    ASSERT(msg->xc->ops.dispatcher, mdsl);
    atomic64_inc(&hmo.prof.misc.reqin_handle);
    return msg->xc->ops.dispatcher(msg);
}

static
void *spool_main(void *arg)
{
    struct spool_thread_arg *sta = (struct spool_thread_arg *)arg;
    sigset_t set;
    int err = 0;

    /* first, let us block the SIGALRM and SIGCHLD */
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &set, NULL); /* oh, we do not care about the
                                             * errs */
    while (!hmo.spool_thread_stop) {
        err = sem_wait(&spool_mgr.rin_sem);
        if (err == EINTR)
            continue;
        hvfs_debug(mdsl, "Service thread %d wakeup to handle the requests.\n",
                   sta->tid);
        /* trying to handle more and more requsts. */
        while (1) {
            err = __serv_request();
            if (err == -EHSTOP)
                break;
            else if (err) {
                hvfs_err(mdsl, "Service thread handle request w/ error %d\n",
                         err);
            }
        }
    }
    pthread_exit(0);
}

int mdsl_spool_create(void)
{
    pthread_attr_t attr;
    struct spool_thread_arg *sta;
    int i, err = 0, stacksize;
    
    /* init the thread stack size */
    err = pthread_attr_init(&attr);
    if (err) {
        hvfs_err(mdsl, "Init pthread attr failed\n");
        goto out;
    }
    stacksize = (hmo.conf.stacksize > (1 << 20) ? 
                 hmo.conf.stacksize : (2 << 20));
    err = pthread_attr_setstacksize(&attr, stacksize);
    if (err) {
        hvfs_err(mdsl, "set thread stack size to %d failed w/ %d\n", 
                 stacksize, err);
        goto out;
    }

    /* init the mgr struct */
    INIT_LIST_HEAD(&spool_mgr.reqin);
    xlock_init(&spool_mgr.rin_lock);
    sem_init(&spool_mgr.rin_sem, 0, 0);

    /* init service threads' pool */
    if (!hmo.conf.spool_threads)
        hmo.conf.spool_threads = 4;

    hmo.spool_thread = xzalloc(hmo.conf.spool_threads * sizeof(pthread_t));
    if (!hmo.spool_thread) {
        hvfs_err(mdsl, "xzalloc() pthread_t failed\n");
        return -ENOMEM;
    }

    sta = xzalloc(hmo.conf.spool_threads * sizeof(struct spool_thread_arg));
    if (!sta) {
        hvfs_err(mdsl, "xzalloc() struct spool_thread_arg failed\n");
        err = -ENOMEM;
        goto out_free;
    }

    for (i = 0; i < hmo.conf.spool_threads; i++) {
        (sta + i)->tid = i;
        err = pthread_create(hmo.spool_thread + i, &attr, &spool_main,
                             sta + i);
        if (err)
            goto out;
    }

out:
    return err;
out_free:
    xfree(hmo.spool_thread);
    goto out;
}

void mdsl_spool_destroy(void)
{
    int i;

    hmo.spool_thread_stop = 1;
    for (i = 0; i < hmo.conf.spool_threads; i++) {
        sem_post(&spool_mgr.rin_sem);
    }
    for (i = 0; i < hmo.conf.spool_threads; i++) {
        pthread_join(*(hmo.spool_thread + i), NULL);
    }
    sem_destroy(&spool_mgr.rin_sem);
}
