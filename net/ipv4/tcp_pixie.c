#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet.h>
#include <linux/random.h>
#include <linux/slab.h> // 添加kmalloc依赖

/* 可调参数 */
static int rate = 100000000;
module_param(rate, int, 0644);
MODULE_PARM_DESC(rate, "Initial sending rate in bytes/sec");

static int feedback = 2;
module_param(feedback, int, 0644);
MODULE_PARM_DESC(feedback, "Feedback window multiplier");

static int gamma = 3;
module_param(gamma, int, 0644);
MODULE_PARM_DESC(gamma, "RTT aggresiveness factor");

static int burst_gain = 2;
module_param(burst_gain, int, 0644);
MODULE_PARM_DESC(burst_gain, "Burst gain multiplier");

static int loss_thresh = 3;
module_param(loss_thresh, int, 0644);
MODULE_PARM_DESC(loss_thresh, "Loss ignore threshold");

struct sample {
    u32    _acked;
    u32    _losses;
    u32    _tstamp_us;
};

struct pixie {
    /* 基础字段 */
    u64    rate;
    u16    start;
    u16    end;
    u32    curr_acked;
    u32    curr_losses;
    struct sample *samples;
    
    /* RTT跟踪 */
    u32    currRTT;
    u32    minRTT;
    
    /* 突发控制 */
    u32    burst_quota;
    u32    burst_window;
    u32    last_burst;
    
    /* 重传控制 */
    u32    retrans_offset;
    u32    retrans_len;
    u8     need_retrans:1,
           ignore_loss:1,
           unused:6;
};

static void pixie_acked(struct sock *sk, const struct ack_sample *sample)
{
    struct pixie *pixie = inet_csk_ca(sk);
    
    if (sample->rtt_us < 0)
        return;

    pixie->currRTT = sample->rtt_us;
    if (pixie->minRTT == 0 || sample->rtt_us < pixie->minRTT) {
        pixie->minRTT = sample->rtt_us;
        pixie->burst_window = (pixie->rate * pixie->minRTT) / (2 * USEC_PER_SEC);
    }
    
    if (sample->pkts_acked == 0) {
        pixie->need_retrans = 1;
        pixie->retrans_offset = tcp_sk(sk)->snd_una;
    }
}

static void pixie_update_burst(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct pixie *pixie = inet_csk_ca(sk);
    u32 now = tp->tcp_mstamp;

    if (pixie->last_burst == 0 || 
        (now - pixie->last_burst) > (pixie->currRTT ?: 100000)) {
        pixie->burst_quota = pixie->burst_window;
        pixie->last_burst = now;
        
        if (pixie->burst_quota > 0) {
            pixie->burst_quota += prandom_u32_max(tp->mss_cache * 4);
        }
    }
}

static void pixie_main(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct pixie *pixie = inet_csk_ca(sk);
    u32 now = tp->tcp_mstamp;
    u32 cwnd;
    u64 prate;

    if (rs->delivered < 0 || rs->interval_us <= 0)
        return;

    if (!pixie->samples) {
        cwnd = pixie->rate / tp->mss_cache;
        cwnd *= (tp->srtt_us >> 3);
        cwnd /= USEC_PER_SEC;
        tp->snd_cwnd = min(2 * cwnd, tp->snd_cwnd_clamp);
        sk->sk_pacing_rate = min_t(u64, pixie->rate, sk->sk_max_pacing_rate);
        return;
    }

    if (rs->losses > 0) {
        pixie->need_retrans = 1;
        pixie->retrans_offset = tp->snd_una;
        pixie->retrans_len = rs->losses * tp->mss_cache;
        pixie->ignore_loss = (rs->losses < loss_thresh);
    }

    pixie->curr_acked += rs->acked_sacked;
    pixie->curr_losses += rs->losses;
    pixie->samples[pixie->end++] = (struct sample){
        ._acked = rs->acked_sacked,
        ._losses = rs->losses,
        ._tstamp_us = now
    };

    while ((__s16)(pixie->start - pixie->end) < 0) {
        if (2 * (now - pixie->samples[pixie->start]._tstamp_us) > feedback * tp->srtt_us) {
            pixie->curr_acked -= pixie->samples[pixie->start]._acked;
            pixie->curr_losses -= pixie->samples[pixie->start]._losses;
            pixie->start++;
        } else {
            break;
        }
    }

    cwnd = pixie->rate / tp->mss_cache;
    cwnd *= (pixie->curr_acked + pixie->curr_losses);
    cwnd /= max(pixie->curr_acked, 1U);
    cwnd *= (tp->srtt_us >> 3);
    cwnd /= USEC_PER_SEC;

    pixie_update_burst(sk);
    if (pixie->currRTT && pixie->minRTT && 
        pixie->currRTT < gamma * pixie->minRTT &&
        pixie->burst_quota > 0) {
        u32 burst = min(pixie->burst_quota, pixie->burst_window);
        cwnd += burst / tp->mss_cache;
        pixie->burst_quota -= min(burst, rs->acked_sacked * tp->mss_cache);
    }

    if (pixie->need_retrans && !pixie->ignore_loss && 
        pixie->currRTT < 2 * pixie->minRTT) {
        u32 retrans = min(pixie->retrans_len, pixie->rate * pixie->currRTT / USEC_PER_SEC);
        cwnd += retrans / tp->mss_cache;
        pixie->retrans_len -= retrans;
        if (pixie->retrans_len == 0) {
            pixie->need_retrans = 0;
        }
    }

    prate = pixie->rate;
    if (pixie->burst_quota > 0 || pixie->need_retrans) {
        prate *= burst_gain;
    }

    tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
    sk->sk_pacing_rate = min_t(u64, prate, sk->sk_max_pacing_rate);

    printk(KERN_DEBUG "pixie: cwnd=%u burst=%u/%u retrans=%u/%u RTT=%u/%u\n",
           tp->snd_cwnd, pixie->burst_quota, pixie->burst_window,
           pixie->retrans_len, pixie->need_retrans,
           pixie->currRTT, pixie->minRTT);
}

static void pixie_init(struct sock *sk)
{
    struct pixie *pixie = inet_csk_ca(sk);

    pixie->rate = (u64)rate;
    pixie->start = 0;
    pixie->end = 0;
    pixie->curr_acked = 0;
    pixie->curr_losses = 0;
    pixie->currRTT = 0;
    pixie->minRTT = ~0U;
    pixie->burst_quota = 0;
    pixie->burst_window = 0;
    pixie->last_burst = 0;
    pixie->need_retrans = 0;
    pixie->ignore_loss = 0;
    pixie->samples = kmalloc_array(U16_MAX, sizeof(struct sample), GFP_ATOMIC);
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void pixie_release(struct sock *sk)
{
    struct pixie *pixie = inet_csk_ca(sk);

    kfree(pixie->samples);
}

static u32 pixie_ssthresh(struct sock *sk)
{
    return TCP_INFINITE_SSTHRESH;
}

static u32 pixie_undo_cwnd(struct sock *sk)
{
    return tcp_sk(sk)->snd_cwnd;
}

static struct tcp_congestion_ops tcp_pixie_cong_ops __read_mostly = {
    .flags          = TCP_CONG_NON_RESTRICTED,
    .name           = "pixie",
    .owner          = THIS_MODULE,
    .init           = pixie_init,
    .release        = pixie_release,
    .cong_control   = pixie_main,
    .ssthresh       = pixie_ssthresh,
    .undo_cwnd      = pixie_undo_cwnd,
    .acked          = pixie_acked,
};

static int __init pixie_register(void)
{
    BUILD_BUG_ON(sizeof(struct pixie) > ICSK_CA_PRIV_SIZE);
    return tcp_register_congestion_control(&tcp_pixie_cong_ops);
}

static void __exit pixie_unregister(void)
{
    tcp_unregister_congestion_control(&tcp_pixie_cong_ops);
}

module_init(pixie_register);
module_exit(pixie_unregister);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixie TCP Congestion Control");
MODULE_AUTHOR("Your Name");