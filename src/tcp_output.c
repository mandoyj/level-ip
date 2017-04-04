#include "syshead.h"
#include "utils.h"
#include "tcp.h"
#include "ip.h"
#include "skbuff.h"
#include "timer.h"

static void tcp_retransmission_timeout(uint32_t ts, void *arg);

static struct sk_buff *tcp_alloc_skb(int optlen, int size)
{
    int reserved = ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN + optlen + size;
    struct sk_buff *skb = alloc_skb(reserved);

    skb_reserve(skb, reserved);
    skb->protocol = IP_TCP;
    skb->dlen = size;

    return skb;
}

static int tcp_write_options(struct tcphdr *th, struct tcp_options *opts, int optlen)
{
    struct tcp_opt_mss *opt_mss = (struct tcp_opt_mss *) th->data;

    opt_mss->kind = TCP_OPT_MSS;
    opt_mss->len = TCP_OPTLEN_MSS;
    opt_mss->mss = htons(opts->mss);

    th->hl = TCP_DOFFSET + (optlen / 4);

    return 0;
}

static int tcp_syn_options(struct sock *sk, struct tcp_options *opts)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    int optlen = 0;

    opts->mss = tsk->mss;
    optlen += TCP_OPTLEN_MSS;
    
    return optlen;
}

static int tcp_transmit_skb(struct sock *sk, struct sk_buff *skb)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr *thdr = tcp_hdr(skb);

    /* No options were previously set */
    if (thdr->hl == 0) thdr->hl = TCP_DOFFSET;

    skb_push(skb, thdr->hl * 4);

    thdr->sport = sk->sport;
    thdr->dport = sk->dport;
    thdr->seq = tcb->snd_nxt;
    thdr->ack_seq = tcb->rcv_nxt;
    thdr->rsvd = 0;
    thdr->win = tcb->rcv_wnd;
    thdr->csum = 0;
    thdr->urp = 0;

    tcp_out_dbg(thdr, sk, skb);

    /* Store sequence information into the socket buffer */
    skb->seq = tcb->snd_una;
    skb->end_seq = tcb->snd_una + skb->dlen;
    
    tcb->snd_nxt += skb->dlen;
    if (thdr->syn) tcb->snd_nxt++;
    if (thdr->fin) tcb->snd_nxt++;
    
    thdr->sport = htons(thdr->sport);
    thdr->dport = htons(thdr->dport);
    thdr->seq = htonl(thdr->seq);
    thdr->ack_seq = htonl(thdr->ack_seq);
    thdr->win = htons(thdr->win);
    thdr->csum = htons(thdr->csum);
    thdr->urp = htons(thdr->urp);
    thdr->csum = tcp_v4_checksum(skb, htonl(sk->saddr), htonl(sk->daddr));
    
    return ip_output(sk, skb);
}

static int tcp_queue_transmit_skb(struct sock *sk, struct sk_buff *skb)
{
    int rc = 0;
    
    pthread_mutex_lock(&sk->write_queue.lock);
    skb_queue_tail(&sk->write_queue, skb);
    rc = tcp_transmit_skb(sk, skb);
    pthread_mutex_unlock(&sk->write_queue.lock);

    return rc;
}

int tcp_send_synack(struct sock *sk)
{
    if (sk->state != TCP_SYN_SENT) {
        print_err("TCP synack: Socket was not in correct state (SYN_SENT)\n");
        return 1;
    }

    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(0, 0);
    th = tcp_hdr(skb);

    th->syn = 1;
    th->ack = 1;

    rc = tcp_transmit_skb(sk, skb);
    free_skb(skb);

    return rc;
}

/* Routine for timer-invoked delayed acknowledgment */
void tcp_send_delack(uint32_t ts, void *arg)
{
    struct sock *sk = (struct sock *) arg;
    struct tcp_sock *tsk = tcp_sk(sk);

    tsk->delacks = 0;
    tcp_release_delack_timer(tsk);

    tcp_send_ack(sk);
}

int tcp_send_ack(struct sock *sk)
{
    if (sk->state == TCP_CLOSE) return 0;
    
    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(0, 0);
    
    th = tcp_hdr(skb);
    th->ack = 1;

    rc = tcp_transmit_skb(sk, skb);
    free_skb(skb);

    return rc;
}

static int tcp_send_syn(struct sock *sk)
{
    if (sk->state != TCP_SYN_SENT && sk->state != TCP_CLOSE && sk->state != TCP_LISTEN) {
        print_err("Socket was not in correct state (closed or listen)\n");
        return 1;
    }

    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcp_options opts = { 0 };
    int tcp_options_len = 0;

    tcp_options_len = tcp_syn_options(sk, &opts);
    skb = tcp_alloc_skb(tcp_options_len, 0);
    th = tcp_hdr(skb);

    tcp_write_options(th, &opts, tcp_options_len);
    sk->state = TCP_SYN_SENT;
    th->syn = 1;

    return tcp_queue_transmit_skb(sk, skb);
}

int tcp_send_fin(struct sock *sk)
{
    if (sk->state == TCP_CLOSE) return 0;

    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(0, 0);
    
    th = tcp_hdr(skb);
    th->fin = 1;
    th->ack = 1;

    rc = tcp_queue_transmit_skb(sk, skb);

    return rc;
}

void tcp_select_initial_window(uint32_t *rcv_wnd)
{
    *rcv_wnd = 44477;
}

/**
 * TCP connection retransmission timeout
 */
static void tcp_connect_rto(struct tcp_sock *tsk)
{
    struct sock *sk = &tsk->sk;

    if (sk->state != TCP_ESTABLISHED) {
        if (tsk->backoff > TCP_CONN_RETRIES) {
            tsk->sk.err = -ETIMEDOUT;
            tcp_set_state(sk, TCP_CLOSE);
            wait_wakeup(&tsk->sk.sock->sleep);
            //free_socket
        } else {
            tsk->backoff++;
            tsk->retransmit = timer_add(TCP_SYN_BACKOFF << tsk->backoff, &tcp_retransmission_timeout, tsk);
        }
    } else {
        print_err("TCP connect RTO triggered even when Established\n");
    }
}

static void tcp_notify_user(struct sock *sk)
{
    switch (sk->state) {
    case TCP_CLOSE_WAIT:
        wait_wakeup(&sk->sock->sleep);
        break;
    }
}

static void tcp_retransmission_timeout(uint32_t ts, void *arg)
{
    struct tcp_sock *tsk = (struct tcp_sock *) arg;
    struct tcb *tcb = &tsk->tcb;
    struct sock *sk = &tsk->sk;

    pthread_mutex_lock(&sk->write_queue.lock);

    tcp_release_rto_timer(tsk);

    struct sk_buff *skb = write_queue_head(sk);
    if (!skb) {
        tcp_notify_user(sk);
        goto unlock;
    }

    struct tcphdr *th = tcp_hdr(skb);
    skb_reset_header(skb);
    
    tcb->snd_nxt = tcb->snd_una;
    tcp_transmit_skb(sk, skb);
    
    if (th->syn) {
        tcp_connect_rto(tsk);
    } else {
        tsk->retransmit = timer_add(500, &tcp_retransmission_timeout, tsk);
    }

    if (th->fin) {
        tcp_handle_fin_state(sk);
    }

unlock:
    pthread_mutex_unlock(&sk->write_queue.lock);
}

void tcp_rearm_rto_timer(struct tcp_sock *tsk)
{
    tsk->retransmit = timer_add(500, &tcp_retransmission_timeout, tsk);
}

int tcp_connect(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    
    tsk->tcp_header_len = sizeof(struct tcphdr);
    tcb->iss = generate_iss();
    tcb->snd_wnd = 0;
    tcb->snd_wl1 = 0;

    tcb->snd_una = tcb->iss;
    tcb->snd_up = tcb->iss;
    tcb->snd_nxt = tcb->iss;
    tcb->rcv_nxt = 0;

    tcp_select_initial_window(&tsk->tcb.rcv_wnd);
    
    tsk->retransmit = timer_add(TCP_SYN_BACKOFF << tsk->backoff, &tcp_retransmission_timeout, tsk);

    return tcp_send_syn(sk);
}

int tcp_send(struct tcp_sock *tsk, const void *buf, int len)
{
    struct sk_buff *skb;
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr *th;
    int slen = len;
    int mss = tsk->mss;
    int dlen = 0;

    while (slen > 0) {
        dlen = slen > mss ? mss : slen;
        slen -= dlen;

        skb = tcp_alloc_skb(0, dlen);
        skb_push(skb, dlen);
        memcpy(skb->data, buf, dlen);
        
        buf += dlen;

        th = tcp_hdr(skb);
        th->ack = 1;

        if (slen == 0) {
            th->psh = 1;
        }

        if (tcp_queue_transmit_skb(&tsk->sk, skb) == -1) {
            perror("Error on TCP skb queueing");
        };
    }

    tcp_rearm_rto_timer(tsk);

    return len;
}

int tcp_send_reset(struct tcp_sock *tsk)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcb *tcb;
    int rc = 0;

    skb = tcp_alloc_skb(0, 0);
    th = tcp_hdr(skb);
    tcb = &tsk->tcb;

    th->rst = 1;
    tcb->snd_una = tcb->snd_nxt;

    rc = tcp_transmit_skb(&tsk->sk, skb);
    free_skb(skb);

    return rc;
}

int tcp_send_challenge_ack(struct sock *sk, struct sk_buff *skb)
{
    // TODO: implement me
    return 0;
}

int tcp_queue_fin(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct sk_buff *skb;
    struct tcphdr *th;
    int ready = 0;
    int rc = 0;

    skb = tcp_alloc_skb(0, 0);
    th = tcp_hdr(skb);

    th->fin = 1;
    th->ack = 1;
    
    pthread_mutex_lock(&sk->write_queue.lock);

    ready = skb_queue_empty(&sk->write_queue);
    skb_queue_tail(&sk->write_queue, skb);

    if (ready) {
        tcp_release_rto_timer(tsk);
        tsk->retransmit = timer_add(200, &tcp_retransmission_timeout, tsk);

        /* If nothing in write queue, send FIN immediately */
        rc = tcp_transmit_skb(sk, skb);

        tcp_handle_fin_state(sk);
    }

    pthread_mutex_unlock(&sk->write_queue.lock);

    return rc;
}
