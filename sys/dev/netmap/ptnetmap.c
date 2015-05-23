/*
 * common headers
 */

#include <bsd_glue.h>
#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include <dev/netmap/netmap_mem2.h>
#include <dev/netmap/paravirt.h>

#ifdef WITH_PTNETMAP_HOST

#define PTN_RX_NOWORK_CYCLE   10                               /* RX cycle without receive any packets */
#define PTN_TX_BATCH_LIM      ((nkr_num_slots >> 1))     /* Limit Batch TX to half ring */

#define PTN_AVOID_NM_PROLOGUE /* XXX: avoid nm_*sync_prologue() */

#define DEBUG  /* Enables communication debugging. */
#ifdef DEBUG
#define DBG(x) x
#else
#define DBG(x)
#endif


#undef RATE
//#define RATE  /* Enables communication statistics. */
#ifdef RATE
#define IFRATE(x) x
struct rate_batch_info {
    uint64_t events;
    uint64_t zero_events;
    uint64_t slots;
};

struct rate_stats {
    unsigned long gtxk;     /* Guest --> Host Tx kicks. */
    unsigned long grxk;     /* Guest --> Host Rx kicks. */
    unsigned long htxk;     /* Host --> Guest Tx kicks. */
    unsigned long hrxk;     /* Host --> Guest Rx Kicks. */
    unsigned long btxwu;    /* Backend Tx wake-up. */
    unsigned long brxwu;    /* Backend Rx wake-up. */
    unsigned long txpkts;   /* Transmitted packets. */
    unsigned long rxpkts;   /* Received packets. */
    unsigned long txfl;     /* TX flushes requests. */
    struct rate_batch_info bf_tx;
    struct rate_batch_info bf_rx;
};

struct rate_context {
    struct timer_list timer;
    struct rate_stats new;
    struct rate_stats old;
};

#define RATE_PERIOD  2
static void
rate_callback(unsigned long arg)
{
    struct rate_context * ctx = (struct rate_context *)arg;
    struct rate_stats cur = ctx->new;
    struct rate_batch_info *bf_tx = &cur.bf_tx;
    struct rate_batch_info *bf_rx = &cur.bf_rx;
    struct rate_batch_info *bf_tx_old = &ctx->old.bf_tx;
    struct rate_batch_info *bf_rx_old = &ctx->old.bf_rx;
    uint64_t tx_batch, rx_batch;
    int r;

    tx_batch = ((bf_tx->events - bf_tx_old->events) > 0) ?
        (bf_tx->slots - bf_tx_old->slots) / (bf_tx->events - bf_tx_old->events): 0;
    rx_batch = ((bf_rx->events - bf_rx_old->events) > 0) ?
        (bf_rx->slots - bf_rx_old->slots) / (bf_rx->events - bf_rx_old->events): 0;

    printk("txp  = %lu Hz\n", (cur.txpkts - ctx->old.txpkts)/RATE_PERIOD);
    printk("gtxk = %lu Hz\n", (cur.gtxk - ctx->old.gtxk)/RATE_PERIOD);
    printk("htxk = %lu Hz\n", (cur.htxk - ctx->old.htxk)/RATE_PERIOD);
    printk("btxw = %lu Hz\n", (cur.btxwu - ctx->old.btxwu)/RATE_PERIOD);
    printk("rxp  = %lu Hz\n", (cur.rxpkts - ctx->old.rxpkts)/RATE_PERIOD);
    printk("grxk = %lu Hz\n", (cur.grxk - ctx->old.grxk)/RATE_PERIOD);
    printk("hrxk = %lu Hz\n", (cur.hrxk - ctx->old.hrxk)/RATE_PERIOD);
    printk("brxw = %lu Hz\n", (cur.brxwu - ctx->old.brxwu)/RATE_PERIOD);
    printk("txfl = %lu Hz\n", (cur.txfl - ctx->old.txfl)/RATE_PERIOD);
    printk("tx_batch = %llu avg\n", tx_batch);
    printk("rx_batch = %llu avg\n", rx_batch);
    printk("\n");

    ctx->old = cur;
    r = mod_timer(&ctx->timer, jiffies +
            msecs_to_jiffies(RATE_PERIOD * 1000));
    if (unlikely(r))
        D("[ptnetmap] Error: mod_timer()\n");
}

static void
rate_batch_info_update(struct rate_batch_info *bf, uint32_t pre_tail, uint32_t act_tail, uint32_t lim)
{
    int n_slots;

    n_slots = (int)act_tail - pre_tail;
    if (n_slots) {
        if (n_slots < 0)
            n_slots += lim;

        bf->events++;
        bf->slots += (uint64_t) n_slots;
    } else {
        bf->zero_events++;
    }
}

#else /* !RATE */
#define IFRATE(x)
#endif /* RATE */

struct ptnetmap_state {
    struct ptn_kthread *ptk_tx, *ptk_rx;        /* kthreads pointers */

    struct ptn_cfg config;                      /* rings configuration */
    struct paravirt_csb __user *csb;            /* shared page with the guest */

    bool configured;
    bool stopped;

    struct netmap_pt_host_adapter *pth_na;   /* netmap adapter of the backend */

    IFRATE(struct rate_context rate_ctx;)
};

static inline void
ptnetmap_kring_dump(const char *title, const struct netmap_kring *kring)
{
    D("%s - name: %s hwcur: %d hwtail: %d rhead: %d rcur: %d rtail: %d head: %d cur: %d tail: %d",
            title, kring->name, kring->nr_hwcur,
            kring->nr_hwtail, kring->rhead, kring->rcur, kring->rtail,
            kring->ring->head, kring->ring->cur, kring->ring->tail);
}

static inline void
ptnetmap_ring_reinit(struct netmap_kring *kring, uint32_t g_head, uint32_t g_cur)
{
    struct netmap_ring *ring = kring->ring;

    //XXX: trust guest?
    ring->head = g_head;
    ring->cur = g_cur;
    ring->tail = ACCESS_ONCE(kring->nr_hwtail);

    netmap_ring_reinit(kring);
    ptnetmap_kring_dump("kring reinit", kring);
}


/*
 * TX functions to set/get and to handle host/guest kick.
 */


/* Enable or disable TX kick to the host */
static inline void
ptnetmap_tx_set_hostkick(struct paravirt_csb __user *csb, uint32_t val)
{
    CSB_WRITE(csb, host_need_txkick, val);
}

/* Check if TX kick to the guest is enable or disable */
static inline uint32_t
ptnetmap_tx_get_guestkick(struct paravirt_csb __user *csb)
{
    uint32_t v;

    CSB_READ(csb, guest_need_txkick, v);

    return v;
}

/* Enable or disable TX kick to the guest */
static inline void
ptnetmap_tx_set_guestkick(struct paravirt_csb __user *csb, uint32_t val)
{
    CSB_WRITE(csb, guest_need_txkick, val);
}

/* Handle TX events: from the guest or from the backend */
static void
ptnetmap_tx_handler(void *data)
{
    struct ptnetmap_state *pts = (struct ptnetmap_state *) data;
    struct netmap_kring *kring;
    struct paravirt_csb __user *csb;
    struct pt_ring __user *csb_ring;
    uint32_t g_cur, g_head, g_flags = 0; /* guest variables; init for compiler */
    uint32_t nkr_num_slots;
    bool work = false;
    int batch;
    IFRATE(uint32_t pre_tail;)

    if (unlikely(!pts)) {
        D("ptnetmap_state is NULL");
        return;
    }

    if (unlikely(!pts->pth_na || pts->stopped || !pts->configured)) {
        D("backend netmap is not configured or stopped");
        goto leave;
    }

    kring = &pts->pth_na->parent->tx_rings[0];

    if (unlikely(nm_kr_tryget(kring))) {
        D("ERROR nm_kr_tryget()");
        goto leave_kr_put;
    }

    csb = pts->csb;
    csb_ring = &csb->tx_ring; /* netmap TX kring pointers in CSB */
    nkr_num_slots = kring->nkr_num_slots;

    g_head = kring->rhead;
    g_cur = kring->rcur;

    /* Disable notifications. */
    ptnetmap_tx_set_hostkick(csb, 0);
    /* Copy the guest kring pointers from the CSB */
    ptnetmap_host_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags, nkr_num_slots);

    for (;;) {
#ifdef PTN_TX_BATCH_LIM
        batch = g_head - kring->nr_hwcur;

        if (batch < 0)
            batch += nkr_num_slots;

        if (batch > PTN_TX_BATCH_LIM) {
            uint32_t new_head = kring->nr_hwcur + PTN_TX_BATCH_LIM;
            if (new_head >= nkr_num_slots)
                new_head -= nkr_num_slots;
            ND(1, "batch: %d old_head: %d new_head: %d", batch, g_head, new_head);
            g_head = new_head;
        }
#endif /* PTN_TX_BATCH_LIM */

        if (nm_kr_txspace(kring) <= (nkr_num_slots >> 1)) {
            g_flags |= NAF_FORCE_RECLAIM;
        }
#ifndef PTN_AVOID_NM_PROLOGUE
        /* Netmap prologue */
        if (unlikely(nm_txsync_prologue(kring, g_head, g_cur, NULL)
                >= nkr_num_slots)) {
            ptnetmap_ring_reinit(kring, g_head, g_cur);
            /* Reenable notifications. */
            ptnetmap_tx_set_hostkick(csb, 1);
            break;
        }
#else /* PTN_AVOID_NM_PROLOGUE */
        kring->rhead = g_head;
        kring->rcur = g_cur;
#endif /* !PTN_AVOID_NM_PROLOGUE */
        if (unlikely(netmap_verbose & NM_VERB_TXSYNC))
            ptnetmap_kring_dump("pre txsync", kring);

        IFRATE(pre_tail = kring->rtail;)

        if (likely(kring->nm_sync(kring, g_flags) == 0)) {
            /*
             * Finalize
             * Copy host hwcur and hwtail into the CSB for the guest sync()
             */
            ptnetmap_host_write_kring_csb(csb_ring, kring->nr_hwcur, ACCESS_ONCE(kring->nr_hwtail));
            if (kring->rtail != ACCESS_ONCE(kring->nr_hwtail)) {
                kring->rtail = ACCESS_ONCE(kring->nr_hwtail);
                work = true;
            }
        } else {
            /* Reenable notifications. */
            ptnetmap_tx_set_hostkick(csb, 1);
            D("ERROR txsync()");
            goto leave_kr_put;
        }

        IFRATE(rate_batch_info_update(&pts->rate_ctx.new.bf_tx, pre_tail, kring->rtail, kring->nkr_num_slots);)

        if (unlikely(netmap_verbose & NM_VERB_TXSYNC))
            ptnetmap_kring_dump("post txsync", kring);

//#define BUSY_WAIT
#ifndef BUSY_WAIT
        /* Send kick to the guest if it needs them */
        if (work && ptnetmap_tx_get_guestkick(csb)) {
            /* Disable guest kick to avoid sending unnecessary kicks */
            ptnetmap_tx_set_guestkick(csb, 0);
            ptn_kthread_send_irq(pts->ptk_tx);
            IFRATE(pts->rate_ctx.new.htxk++);
            work = false;
        }
#endif
        /* We read the CSB before deciding to continue or stop. */
        ptnetmap_host_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags, nkr_num_slots);
#ifndef BUSY_WAIT
        /*
         * Ring empty, nothing to transmit. We enable notification and
         * go to sleep. We need a notification when the guest has
         * new slots to transmit.
         */
        if (g_head == kring->rhead) {
            usleep_range(1,1);
            /* Reenable notifications. */
            ptnetmap_tx_set_hostkick(csb, 1);
            /* Doublecheck. */
            ptnetmap_host_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags, nkr_num_slots);
            /* If there are new packets, disable notifications and redo new sync() */
            if (g_head != kring->rhead) {
                ptnetmap_tx_set_hostkick(csb, 0);
                continue;
            } else
                break;
        }

        /*
         * Ring full. We stop without reenable notification
         * because we await the BE.
         */
        if (unlikely(ACCESS_ONCE(kring->nr_hwtail) == kring->rhead)) {
            ND(1, "TX ring FULL");
            break;
        }
#endif
        if (unlikely(pts->stopped || !pts->configured)) {
            D("backend netmap is not configured or stopped");
            break;
        }
    }

leave_kr_put:
    nm_kr_put(kring);

leave:
    /* Send kick to the guest if it needs them */
    if (work && ptnetmap_tx_get_guestkick(csb)) {
        ptnetmap_tx_set_guestkick(csb, 0);
        ptn_kthread_send_irq(pts->ptk_tx);
        IFRATE(pts->rate_ctx.new.htxk++);
    }
}


/*
 * RX functions to set/get and to handle host/guest kick.
 */


/* Enable or disable RX kick to the host */
static inline void
ptnetmap_rx_set_hostkick(struct paravirt_csb __user *csb, uint32_t val)
{
    CSB_WRITE(csb, host_need_rxkick, val);
}

/* Check if RX kick to the guest is enable or disable */
static inline uint32_t
ptnetmap_rx_get_guestkick(struct paravirt_csb __user *csb)
{
    uint32_t v;

    CSB_READ(csb, guest_need_rxkick, v);

    return v;
}

/* Enable or disable RX kick to the guest */
static inline void
ptnetmap_rx_set_guestkick(struct paravirt_csb __user *csb, uint32_t val)
{
    CSB_WRITE(csb, guest_need_rxkick, val);
}

/*
 * We needs kick from the guest when:
 *
 * - RX: tail == head - 1
 *       ring is full
 *       We need to wait that the guest gets some packets from the ring and then it notifies us.
 */
static inline int
ptnetmap_kr_rxfull(struct netmap_kring *kring, uint32_t g_head)
{
    return (ACCESS_ONCE(kring->nr_hwtail) == nm_prev(g_head, kring->nkr_num_slots - 1));
}

/* Handle RX events: from the guest or from the backend */
static void
ptnetmap_rx_handler(void *data)
{
    struct ptnetmap_state *pts = (struct ptnetmap_state *) data;
    struct netmap_kring *kring;
    struct paravirt_csb __user *csb;
    struct pt_ring __user *csb_ring;
    uint32_t g_cur, g_head, g_flags = 0; /* guest variables; init for compiler */
    uint32_t nkr_num_slots;
    int cicle_nowork = 0;
    bool work = false;
    IFRATE(uint32_t pre_tail;)

    if (unlikely(!pts)) {
        D("ptnetmap_state is NULL");
        return;
    }

    if (unlikely(!pts->pth_na || pts->stopped || !pts->configured)) {
        D("backend netmap is not configured or stopped");
        goto leave;
    }

    kring = &pts->pth_na->parent->rx_rings[0];

    if (unlikely(nm_kr_tryget(kring))) {
        D("ERROR nm_kr_tryget()");
        goto leave;
    }

    csb = pts->csb;
    csb_ring = &csb->rx_ring; /* netmap RX kring pointers in CSB */
    nkr_num_slots = kring->nkr_num_slots;

    g_head = kring->rhead;
    g_cur = kring->rcur;

    /* Disable notifications. */
    ptnetmap_rx_set_hostkick(csb, 0);
    /* Copy the guest kring pointers from the CSB */
    ptnetmap_host_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags, nkr_num_slots);

    for (;;) {
#ifndef PTN_AVOID_NM_PROLOGUE
        /* Netmap prologue */
        if (unlikely(nm_rxsync_prologue(kring, g_head, g_cur, NULL)
                >= nkr_num_slots)) {
            ptnetmap_ring_reinit(kring, g_head, g_cur);
            /* Reenable notifications. */
            ptnetmap_rx_set_hostkick(csb, 1);
            break;
        }
#else /* PTN_AVOID_NM_PROLOGUE */
        kring->rhead = g_head;
        kring->rcur = g_cur;
#endif /* !PTN_AVOID_NM_PROLOGUE */

        if (unlikely(netmap_verbose & NM_VERB_RXSYNC))
            ptnetmap_kring_dump("pre rxsync", kring);

        IFRATE(pre_tail = kring->rtail;)

        if (likely(kring->nm_sync(kring, g_flags) == 0)) {
            /*
             * Finalize
             * Copy host hwcur and hwtail into the CSB for the guest sync()
             */
            ptnetmap_host_write_kring_csb(csb_ring, kring->nr_hwcur, ACCESS_ONCE(kring->nr_hwtail));
            if (kring->rtail != ACCESS_ONCE(kring->nr_hwtail)) {
                kring->rtail = ACCESS_ONCE(kring->nr_hwtail);
                work = true;
                cicle_nowork = 0;
            } else {
                cicle_nowork++;
            }
        } else {
            /* Reenable notifications. */
            ptnetmap_rx_set_hostkick(csb, 1);
            D("ERROR rxsync()");
            goto leave_kr_put;
        }

        IFRATE(rate_batch_info_update(&pts->rate_ctx.new.bf_rx, pre_tail, kring->rtail, kring->nkr_num_slots);)

        if (unlikely(netmap_verbose & NM_VERB_RXSYNC))
            ptnetmap_kring_dump("post rxsync", kring);

#ifndef BUSY_WAIT
        /* Send kick to the guest if it needs them */
        if (work && ptnetmap_rx_get_guestkick(csb)) {
            /* Disable guest kick to avoid sending unnecessary kicks */
            ptnetmap_rx_set_guestkick(csb, 0);
            ptn_kthread_send_irq(pts->ptk_rx);
            IFRATE(pts->rate_ctx.new.hrxk++);
            work = false;
        }
#endif
        /* We read the CSB before deciding to continue or stop. */
        ptnetmap_host_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags, nkr_num_slots);
#ifndef BUSY_WAIT
        /*
         * Ring full. No space to receive. We enable notification and
         * go to sleep. We need a notification when the guest has
         * new free slots.
         */
        if (ptnetmap_kr_rxfull(kring, g_head)) {
            usleep_range(1,1);
            /* Reenable notifications. */
            ptnetmap_rx_set_hostkick(csb, 1);
            /* Doublecheck. */
            ptnetmap_host_read_kring_csb(csb_ring, &g_head, &g_cur, &g_flags, nkr_num_slots);
            /* If there are new free slots, disable notifications and redo new sync() */
            if (!ptnetmap_kr_rxfull(kring, g_head)) {
                ptnetmap_rx_set_hostkick(csb, 0);
                continue;
            } else
                break;
        }

        /*
         * Ring empty. We stop without reenable notification
         * because we await the BE.
         */
        if (unlikely(ACCESS_ONCE(kring->nr_hwtail) == kring->rhead
                    || cicle_nowork >= PTN_RX_NOWORK_CYCLE)) {
            ND(1, "nr_hwtail: %d rhead: %d cicle_nowork: %d", ACCESS_ONCE(kring->nr_hwtail), kring->rhead, cicle_nowork);
            break;
        }
#endif
        if (unlikely(pts->stopped || !pts->configured)) {
            D("backend netmap is not configured or stopped");
            break;
        }
    }

leave_kr_put:
    nm_kr_put(kring);

leave:
    /* Send kick to the guest if it needs them */
    if (work && ptnetmap_rx_get_guestkick(csb)) {
        ptnetmap_rx_set_guestkick(csb, 0);
        ptn_kthread_send_irq(pts->ptk_rx);
        IFRATE(pts->rate_ctx.new.hrxk++);
    }
}

/* Notify kthreads (wake up if needed) */
static void inline
ptnetmap_tx_notify(struct ptnetmap_state *pts) {
    if (unlikely(!pts))
        return;

    ptn_kthread_wakeup_worker(pts->ptk_tx);
    IFRATE(pts->rate_ctx.new.btxwu++);
}

static void inline
ptnetmap_rx_notify(struct ptnetmap_state *pts) {
    if (unlikely(!pts))
        return;

    ptn_kthread_wakeup_worker(pts->ptk_rx);
    IFRATE(pts->rate_ctx.new.brxwu++);
}

#ifdef DEBUG
static void
ptnetmap_print_configuration(struct ptnetmap_state *pts)
{
    struct ptn_cfg *cfg = &pts->config;

    D("[PTN] configuration:");
    D("TX: iofd=%u, irqfd=%u",
            cfg->tx_ring.ioeventfd, cfg->tx_ring.irqfd);
    D("RX: iofd=%u, irqfd=%u",
            cfg->rx_ring.ioeventfd, cfg->rx_ring.irqfd);
    D("CSB: csb_addr=%p", cfg->csb);

}
#endif

/* Copy actual state of the host ring into the CSB for the guest init */
static int
ptnetmap_kring_snapshot(struct netmap_kring *kring, struct pt_ring __user *ptr)
{
    if(CSB_WRITE(ptr, head, kring->rhead))
        goto err;
    if(CSB_WRITE(ptr, cur, kring->rcur))
        goto err;

    if(CSB_WRITE(ptr, hwcur, kring->nr_hwcur))
        goto err;
    if(CSB_WRITE(ptr, hwtail, ACCESS_ONCE(kring->nr_hwtail)))
        goto err;

    DBG(ptnetmap_kring_dump("ptnetmap_kring_snapshot", kring);)

    return 0;
err:
    return EFAULT;
}

static int
ptnetmap_krings_snapshot(struct ptnetmap_state *pts, struct netmap_pt_host_adapter *pth_na)
{
    struct netmap_kring *kring;
    int error = 0;

    kring = &pth_na->parent->tx_rings[0];
    if((error = ptnetmap_kring_snapshot(kring, &pts->csb->tx_ring)))
        goto err;

    kring = &pth_na->parent->rx_rings[0];
    error = ptnetmap_kring_snapshot(kring, &pts->csb->rx_ring);

err:
    return error;
}

/*
 * Functions to create, start and stop the kthreads
 */

static int
ptnetmap_create_kthreads(struct ptnetmap_state *pts)
{
    struct ptn_kthread_cfg ptk_cfg;

    ptk_cfg.worker_private = pts;

    /* TX kthread */
    ptk_cfg.type = PTK_TX;
    ptk_cfg.ring = pts->config.tx_ring;
    ptk_cfg.worker_fn = ptnetmap_tx_handler;
    pts->ptk_tx = ptn_kthread_create(&ptk_cfg);
    if (pts->ptk_tx == NULL) {
        goto err;
    }

    /* RX kthread */
    ptk_cfg.type = PTK_RX;
    ptk_cfg.ring = pts->config.rx_ring;
    ptk_cfg.worker_fn = ptnetmap_rx_handler;
    pts->ptk_rx = ptn_kthread_create(&ptk_cfg);
    if (pts->ptk_rx == NULL) {
        goto err;
    }

    return 0;
err:
    if (pts->ptk_tx) {
        ptn_kthread_delete(pts->ptk_tx);
        pts->ptk_tx = NULL;
    }
    return EFAULT;
}

static int
ptnetmap_start_kthreads(struct ptnetmap_state *pts)
{
    int error;

    /* check if ptnetmap is configured */
    if (!pts) {
        D("ptnetmap is not configured");
        return EFAULT;
    }

    pts->stopped = false;

    /* TX kthread */
    error = ptn_kthread_start(pts->ptk_tx);
    if (error) {
        return error;
    }
    /* RX kthread */
    error = ptn_kthread_start(pts->ptk_rx);
    if (error) {
        ptn_kthread_stop(pts->ptk_tx);
        return error;
    }

    return 0;
}

static void
ptnetmap_stop_kthreads(struct ptnetmap_state *pts)
{
    /* check if it is configured */
    if (!pts)
        return;

    pts->stopped = true;

    /* TX kthread */
    ptn_kthread_stop(pts->ptk_tx);
    /* RX kthread */
    ptn_kthread_stop(pts->ptk_rx);
}

static int nm_pt_host_notify(struct netmap_kring *, int);


/* Switch adapter in passthrough mode and create kthreads */
static int
ptnetmap_create(struct netmap_pt_host_adapter *pth_na, const void __user *buf, uint16_t buf_len)
{
    struct ptnetmap_state *pts;
    int ret, i;

    /* check if already in pt mode */
    if (pth_na->ptn_state) {
        D("ERROR adapter already in netmap passthrough mode");
        return EFAULT;
    }

    pts = malloc(sizeof(*pts), M_DEVBUF, M_NOWAIT | M_ZERO);
    if (!pts)
        return ENOMEM;
    pts->configured = false;
    pts->stopped = true;

    /* Read the configuration from userspace. */
    if (buf_len != sizeof(struct ptn_cfg)) {
        D("ERROR - buf_len %d, expected %d", (int)buf_len, (int)sizeof(struct ptn_cfg));
        ret = EINVAL;
        goto err;
    }
    if (copyin(buf, &pts->config, sizeof(struct ptn_cfg))) {
        D("ERROR copy_from_user()");
        ret = EFAULT;
        goto err;
    }
    pts->csb = pts->config.csb;
    DBG(ptnetmap_print_configuration(pts);)

    /* Create kthreads */
    if ((ret = ptnetmap_create_kthreads(pts))) {
        D("ERROR ptnetmap_create_kthreads()");
        goto err;
    }
    /* Copy krings state into the CSB for the guest initialization */
    if ((ret = ptnetmap_krings_snapshot(pts, pth_na))) {
        D("ERROR ptnetmap_krings_snapshot()");
        goto err;
    }

    pts->configured = true;
    pth_na->ptn_state = pts;
    pts->pth_na = pth_na;

    /* overwrite parent nm_notify callback */
    pth_na->parent->na_private = pth_na;
    pth_na->parent_nm_notify = pth_na->parent->nm_notify;
    pth_na->parent->nm_notify = nm_pt_host_notify;

    for (i = 0; i < pth_na->parent->num_rx_rings; i++) {
        pth_na->parent->rx_rings[i].save_notify = pth_na->parent->rx_rings[i].nm_notify;
        pth_na->parent->rx_rings[i].nm_notify = nm_pt_host_notify;
    }
    for (i = 0; i < pth_na->parent->num_tx_rings; i++) {
        pth_na->parent->tx_rings[i].save_notify = pth_na->parent->tx_rings[i].nm_notify;
        pth_na->parent->tx_rings[i].nm_notify = nm_pt_host_notify;
    }

#ifdef RATE
    memset(&pts->rate_ctx, 0, sizeof(pts->rate_ctx));
    setup_timer(&pts->rate_ctx.timer, &rate_callback,
            (unsigned long)&pts->rate_ctx);
    if (mod_timer(&pts->rate_ctx.timer, jiffies + msecs_to_jiffies(1500)))
        D("[ptn] Error: mod_timer()\n");
#endif

    DBG(D("[%s] ptnetmap configuration DONE", pth_na->up.name));

    return 0;

err:
    free(pts, M_DEVBUF);
    return ret;
}

/* Switch adapter in normal netmap mode and delete kthreads */
static void
ptnetmap_delete(struct netmap_pt_host_adapter *pth_na)
{
    struct ptnetmap_state *pts = pth_na->ptn_state;
    int i;

    /* check if ptnetmap is configured */
    if (!pts)
        return;

    /* restore parent adapter callbacks */
    pth_na->parent->nm_notify = pth_na->parent_nm_notify;
    pth_na->parent->na_private = NULL;

    for (i = 0; i < pth_na->parent->num_rx_rings; i++) {
        pth_na->parent->rx_rings[i].nm_notify = pth_na->parent->rx_rings[i].save_notify;
        pth_na->parent->rx_rings[i].save_notify = NULL;
    }
    for (i = 0; i < pth_na->parent->num_tx_rings; i++) {
        pth_na->parent->tx_rings[i].nm_notify = pth_na->parent->tx_rings[i].save_notify;
        pth_na->parent->tx_rings[i].save_notify = NULL;
    }

    pts->configured = false;

    /* delete kthreads */
    ptn_kthread_delete(pts->ptk_tx);
    ptn_kthread_delete(pts->ptk_rx);

    IFRATE(del_timer(&pts->rate_ctx.timer));

    free(pts, M_DEVBUF);

    pth_na->ptn_state = NULL;

    DBG(D("[%s] ptnetmap deleted", pth_na->up.name));
}

/*
 * Called by netmap_ioctl().
 * Operation is indicated in nmr->nr_cmd.
 *
 * Called without NMG_LOCK.
 */
int
ptnetmap_ctl(struct nmreq *nmr, struct netmap_adapter *na)
{
    struct netmap_pt_host_adapter *pth_na;
    char *name;
    int cmd, error = 0;
    void __user *buf;
    uint16_t buf_len;

    name = nmr->nr_name;
    cmd = nmr->nr_cmd;

    DBG(D("name: %s", name);)

    if (!nm_passthrough_host_on(na)) {
        D("ERROR interface not support passthrough mode. na = %p", na);
        error = ENXIO;
        goto done;
    }
    pth_na = (struct netmap_pt_host_adapter *)na;

    NMG_LOCK();
    switch (cmd) {
        case NETMAP_PT_HOST_CREATE:     /* create kthreads and switch in pt mode */
            nmr_read_buf(nmr, &buf, &buf_len);

            /* create kthreads */
            error = ptnetmap_create(pth_na, buf, buf_len);
            if (error)
                break;
            /* start kthreads */
            error = ptnetmap_start_kthreads(pth_na->ptn_state);
            if (error)
                ptnetmap_delete(pth_na);

            break;
        case NETMAP_PT_HOST_DELETE:     /* delete kthreads and restore parent adapter */
            /* stop kthreads */
            ptnetmap_stop_kthreads(pth_na->ptn_state);
            /* delete kthreads */
            ptnetmap_delete(pth_na);
            break;
        default:
            D("ERROR invalid cmd (nmr->nr_cmd) (0x%x)", cmd);
            error = EINVAL;
            break;
    }
    NMG_UNLOCK();

done:
    return error;
}

/* nm_notify callback for passthrough */
static int
nm_pt_host_notify(struct netmap_kring *kring, int flags)
{
    struct netmap_adapter *na = kring->na;
    struct netmap_pt_host_adapter *pth_na =
        (struct netmap_pt_host_adapter *)na->na_private;
    enum txrx t = kring->tx;

    /* TODO-ste: avoid if with array on pt_host_adapter */
    if (likely(pth_na)) {
        if (t == NR_TX) {
                ptnetmap_tx_notify(pth_na->ptn_state);
        } else {
                ptnetmap_rx_notify(pth_na->ptn_state);
        }
    }

    OS_selwakeup(&kring->si, PI_NET);
    /* optimization: avoid a wake up on the global
     * queue if nobody has registered for more
     * than one ring
     */
    if (na->si_users[t] > 0)
	OS_selwakeup(&na->si[t], PI_NET);
    return 0;
}

//XXX maybe is unnecessary redefine the *xsync
/* nm_txsync callback for passthrough */
static int
nm_pt_host_txsync(struct netmap_kring *kring, int flags)
{
    struct netmap_pt_host_adapter *pth_na =
        (struct netmap_pt_host_adapter *)kring->na;
    struct netmap_adapter *parent = pth_na->parent;
    int n;

    DBG(D("%s", pth_na->up.name);)

    n = parent->nm_txsync(kring, flags);

    return n;
}

/* nm_rxsync callback for passthrough */
static int
nm_pt_host_rxsync(struct netmap_kring *kring, int flags)
{
    struct netmap_pt_host_adapter *pth_na =
        (struct netmap_pt_host_adapter *)kring->na;
    struct netmap_adapter *parent = pth_na->parent;
    int n;

    DBG(D("%s", pth_na->up.name);)

    n = parent->nm_rxsync(kring, flags);

    return n;
}

/* nm_config callback for bwrap */
static int
nm_pt_host_config(struct netmap_adapter *na, u_int *txr, u_int *txd,
        u_int *rxr, u_int *rxd)
{
    struct netmap_pt_host_adapter *pth_na =
        (struct netmap_pt_host_adapter *)na;
    struct netmap_adapter *parent = pth_na->parent;
    int error;

    //XXX: maybe call parent->nm_config is better

    /* forward the request */
    error = netmap_update_config(parent);

    *rxr = na->num_rx_rings = parent->num_rx_rings;
    *txr = na->num_tx_rings = parent->num_tx_rings;
    *txd = na->num_tx_desc = parent->num_tx_desc;
    *rxd = na->num_rx_desc = parent->num_rx_desc;

    DBG(D("rxr: %d txr: %d txd: %d rxd: %d", *rxr, *txr, *txd, *rxd);)

    return error;
}

/* nm_krings_create callback for passthrough */
static int
nm_pt_host_krings_create(struct netmap_adapter *na)
{
    struct netmap_pt_host_adapter *pth_na =
        (struct netmap_pt_host_adapter *)na;
    struct netmap_adapter *parent = pth_na->parent;
    int error;

    DBG(D("%s", pth_na->up.name);)

    /* create the parent krings */
    error = parent->nm_krings_create(parent);
    if (error) {
        return error;
    }

    na->tx_rings = parent->tx_rings;
    na->rx_rings = parent->rx_rings;
    na->tailroom = parent->tailroom; //XXX

    return 0;
}

/* nm_krings_delete callback for passthrough */
static void
nm_pt_host_krings_delete(struct netmap_adapter *na)
{
    struct netmap_pt_host_adapter *pth_na =
        (struct netmap_pt_host_adapter *)na;
    struct netmap_adapter *parent = pth_na->parent;

    DBG(D("%s", pth_na->up.name);)

    parent->nm_krings_delete(parent);

    na->tx_rings = na->rx_rings = na->tailroom = NULL;
}

/* nm_register callback */
static int
nm_pt_host_register(struct netmap_adapter *na, int onoff)
{
    struct netmap_pt_host_adapter *pth_na =
        (struct netmap_pt_host_adapter *)na;
    struct netmap_adapter *parent = pth_na->parent;
    int error;
    DBG(D("%s onoff %d", pth_na->up.name, onoff);)

    if (onoff) {
        /* netmap_do_regif has been called on the
         * passthrough na.
         * We need to pass the information about the
         * memory allocator to the parent before
         * putting it in netmap mode
         */
        parent->na_lut = na->na_lut;
    }

    /* forward the request to the parent */
    error = parent->nm_register(parent, onoff);
    if (error)
        return error;


    if (onoff) {
        na->na_flags |= NAF_NETMAP_ON | NAF_PASSTHROUGH_HOST;
    } else {
        ptnetmap_delete(pth_na);
        na->na_flags &= ~(NAF_NETMAP_ON | NAF_PASSTHROUGH_HOST);
    }

    return 0;
}

/* nm_dtor callback */
static void
nm_pt_host_dtor(struct netmap_adapter *na)
{
    struct netmap_pt_host_adapter *pth_na =
        (struct netmap_pt_host_adapter *)na;
    struct netmap_adapter *parent = pth_na->parent;

    DBG(D("%s", pth_na->up.name);)

    parent->na_flags &= ~NAF_BUSY;

    netmap_adapter_put(pth_na->parent);
    pth_na->parent = NULL;
}

/* check if nmr is a request for a passthrough adapter that we can satisfy */
int
netmap_get_pt_host_na(struct nmreq *nmr, struct netmap_adapter **na, int create)
{
    struct nmreq parent_nmr;
    struct netmap_adapter *parent; /* target adapter */
    struct netmap_pt_host_adapter *pth_na;
    int error;

    /* Check if it is a request for a passthrough adapter */
    if ((nmr->nr_flags & (NR_PASSTHROUGH_HOST)) == 0) {
        D("not a passthrough");
        return 0;
    }

    pth_na = malloc(sizeof(*pth_na), M_DEVBUF, M_NOWAIT | M_ZERO);
    if (pth_na == NULL) {
        D("ERROR malloc");
        return ENOMEM;
    }

    /* first, try to find the adapter that we want to passthrough
     * We use the same nmr, after we have turned off the passthrough flag.
     * In this way we can potentially passthrough everything netmap understands.
     */
    memcpy(&parent_nmr, nmr, sizeof(parent_nmr));
    parent_nmr.nr_flags &= ~(NR_PASSTHROUGH_HOST);
    error = netmap_get_na(&parent_nmr, &parent, create);
    if (error) {
        D("parent lookup failed: %d", error);
        goto put_out_noputparent;
    }
    DBG(D("found parent: %s", parent->name);)

    /* make sure the interface is not already in use */
    if (NETMAP_OWNED_BY_ANY(parent)) {
        D("NIC %s busy, cannot passthrough", parent->name);
        error = EBUSY;
        goto put_out;
    }

    pth_na->parent = parent;

    //XXX pth_na->up.na_flags = parent->na_flags;
    pth_na->up.num_rx_rings = parent->num_rx_rings;
    pth_na->up.num_tx_rings = parent->num_tx_rings;
    pth_na->up.num_tx_desc = parent->num_tx_desc;
    pth_na->up.num_rx_desc = parent->num_rx_desc;

    pth_na->up.nm_dtor = nm_pt_host_dtor;
    pth_na->up.nm_register = nm_pt_host_register;

    //XXX maybe is unnecessary redefine the *xsync
    pth_na->up.nm_txsync = nm_pt_host_txsync;
    pth_na->up.nm_rxsync = nm_pt_host_rxsync;

    pth_na->up.nm_krings_create = nm_pt_host_krings_create;
    pth_na->up.nm_krings_delete = nm_pt_host_krings_delete;
    pth_na->up.nm_config = nm_pt_host_config;
    pth_na->up.nm_notify = nm_pt_host_notify;

    pth_na->up.nm_mem = parent->nm_mem;
    error = netmap_attach_common(&pth_na->up);
    if (error) {
        D("ERROR netmap_attach_common()");
        goto put_out;
    }

    *na = &pth_na->up;
    netmap_adapter_get(*na);

    /* write the configuration back */
    nmr->nr_tx_rings = pth_na->up.num_tx_rings;
    nmr->nr_rx_rings = pth_na->up.num_rx_rings;
    nmr->nr_tx_slots = pth_na->up.num_tx_desc;
    nmr->nr_rx_slots = pth_na->up.num_rx_desc;

    /* set parent busy, because attached for passthrough */
    parent->na_flags |= NAF_BUSY;

    strncpy(pth_na->up.name, parent->name, sizeof(pth_na->up.name));
    strcat(pth_na->up.name, "-PTN");

    DBG(D("%s passthrough request DONE", pth_na->up.name);)

    return 0;

put_out:
    netmap_adapter_put(parent);
put_out_noputparent:
    free(pth_na, M_DEVBUF);
    return error;
}
#endif /* WITH_PTNETMAP_HOST */