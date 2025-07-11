#include <linux/module.h>
#include <net/tcp.h>
#include <linux/inet.h>

static int rate = 100000000;
module_param(rate, int, 0644);
static int feedback = 2;
module_param(feedback, int, 0644);
static int gamma = 3;
module_param(gamma, int, 0644);

struct sample {
    u32    _acked;
    u32    _losses;
    u32    _tstamp_us;
};

struct pixie {
    u64    rate;
    u16    start;
    u16    end;
    u32    curr_acked;
    u32    curr_losses;
    u32    currRTT;      // 当前 RTT（us）
    u32    minRTT;       // 最小 RTT（us）
    struct sample *samples;
};

static void pixie_acked(struct sock *sk, const struct ack_sample *sample)
{
    struct pixie *pixie = inet_csk_ca(sk);
    
    if (sample->rtt_us < 0)
        return;  // 无效 RTT
    
    // 更新 currRTT 和 minRTT
    pixie->currRTT = sample->rtt_us;
    if (pixie->minRTT == 0 || sample->rtt_us < pixie->minRTT)
        pixie->minRTT = sample->rtt_us;
}

static void pixie_main(struct sock *sk, const struct rate_sample *rs)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct pixie *pixie = inet_csk_ca(sk);
    u32 now = tp->tcp_mstamp;
    u32 cwnd;
    u16 start, end;
    u64 prate;

    if (rs->delivered < 0 || rs->interval_us <= 0)
        return;

    cwnd = pixie->rate;
    if (!pixie->samples) {
        cwnd /= tp->mss_cache;
        cwnd *= (tp->srtt_us >> 3);
        cwnd /= USEC_PER_SEC;
        tp->snd_cwnd = min(2 * cwnd, tp->snd_cwnd_clamp);
        sk->sk_pacing_rate = min_t(u64, pixie->rate, READ_ONCE(sk->sk_max_pacing_rate));
        return;
    }

    // 丢包突然增加时采用紧缩策略，以在保吞吐时尽量减少进一步丢包
    // 丢包突然减少时采用激进策略，以挤占比实际更多资源
    pixie->curr_acked += rs->acked_sacked;
    pixie->curr_losses += rs->losses;
    end = pixie->end++;
    pixie->samples[end]._acked = rs->acked_sacked;
    pixie->samples[end]._losses = rs->losses;
    pixie->samples[end]._tstamp_us = now;

    start = pixie->start;
    while ((__s16)(start - end) < 0) {
        // 至少保持半个 srtt 反馈周期
        if (2 * (now - pixie->samples[start]._tstamp_us) > feedback * tp->srtt_us) {
            pixie->curr_acked -= pixie->samples[start]._acked;
            pixie->curr_losses -= pixie->samples[start]._losses;
            pixie->start++;
        }
        start++;
    }

    // 如果当前 RTT 远小于 minRTT（gamma 倍），可以激进增长
    if (pixie->currRTT && pixie->minRTT && 
        pixie->currRTT < gamma * pixie->minRTT) {
        cwnd = tp->snd_cwnd * 2;
    } else {
        cwnd /= tp->mss_cache;
        cwnd *= pixie->curr_acked + pixie->curr_losses;
        cwnd /= pixie->curr_acked;
        cwnd *= (tp->srtt_us >> 3);
        cwnd /= USEC_PER_SEC;
    }

    prate = (pixie->curr_acked + pixie->curr_losses) << 10;
    prate /= pixie->curr_acked;
    prate *= pixie->rate;
    prate = prate >> 10;

    printk("##### curr_ack:%u curr_loss:%u currRTT:%u minRTT:%u cwnd:%u rate:%llu prate:%llu\n",
           pixie->curr_acked,
           pixie->curr_losses,
           pixie->currRTT,
           pixie->minRTT,
           cwnd,
           rate,
           prate);

    tp->snd_cwnd = min(cwnd, tp->snd_cwnd_clamp);
    sk->sk_pacing_rate = min_t(u64, prate, sk->sk_max_pacing_rate);
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
    pixie->samples = kmalloc(U16_MAX * sizeof(struct sample), GFP_ATOMIC);
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
}

static void pixie_release(struct sock *sk)
{
    struct pixie *pixie = inet_csk_ca(sk);

    if (pixie->samples)
        kfree(pixie->samples);
}

static u32 pixie_ssthresh(struct sock *sk)
{
    return TCP_INFINITE_SSTHRESH;
}

static u32 pixie_undo_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    return tp->snd_cwnd;
}

static struct tcp_congestion_ops tcp_pixie_cong_ops __read_mostly = {
    .flags        = TCP_CONG_NON_RESTRICTED,
    .name        = "pixie",
    .owner        = THIS_MODULE,
    .init        = pixie_init,
    .release    = pixie_release,
    .cong_control    = pixie_main,
    .ssthresh    = pixie_ssthresh,
    .undo_cwnd     = pixie_undo_cwnd,
    .acked        = pixie_acked,  // 新增 RTT 更新回调
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