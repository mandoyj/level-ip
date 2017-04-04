#ifndef TCP_H_
#define TCP_H_
#include "syshead.h"
#include "ip.h"
#include "timer.h"
#include "utils.h"

#define TCP_HDR_LEN sizeof(struct tcphdr)

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_URG 0x20
#define TCP_ECN 0x40
#define TCP_WIN 0x80

#define TCP_SYN_BACKOFF 1000
#define TCP_CONN_RETRIES 3

#define tcp_sk(sk) ((struct tcp_sock *)sk)
#define tcp_hlen(tcp) (tcp->hl << 2)

#ifdef DEBUG_TCP
#define tcp_in_dbg(hdr, sk, skb)                                        \
    do {                                                                \
        print_debug("TCP %hhu.%hhu.%hhu.%hhu.%u > %hhu.%hhu.%hhu.%hhu.%u: " \
                    "Flags [S%hhuA%hhuP%hhuF%hhuR%hhu], seq %u:%u, ack %u, win %u", \
                    sk->daddr >> 24, sk->daddr >> 16, sk->daddr >> 8, sk->daddr >> 0, sk->dport, \
                    sk->saddr >> 24, sk->saddr >> 16, sk->saddr >> 8, sk->saddr >> 0, sk->sport, \
                    hdr->syn, hdr->ack, hdr->psh, hdr->fin, hdr->rst, hdr->seq - tcp_sk(sk)->tcb.irs, \
                    hdr->seq + skb->dlen - tcp_sk(sk)->tcb.irs,         \
                    hdr->ack_seq - tcp_sk(sk)->tcb.iss, hdr->win);      \
    } while (0) 

#define tcp_out_dbg(hdr, sk, skb)                                       \
    do {                                                                \
        print_debug("TCP %hhu.%hhu.%hhu.%hhu.%u > %hhu.%hhu.%hhu.%hhu.%u: " \
                    "Flags [S%hhuA%hhuP%hhuF%hhuR%hhu], seq %u:%u, ack %u, win %u", \
                    sk->saddr >> 24, sk->saddr >> 16, sk->saddr >> 8, sk->saddr >> 0, sk->sport, \
                    sk->daddr >> 24, sk->daddr >> 16, sk->daddr >> 8, sk->daddr >> 0, sk->dport, \
                    hdr->syn, hdr->ack, hdr->psh, hdr->fin, hdr->rst, hdr->seq - tcp_sk(sk)->tcb.iss, \
                    hdr->seq + skb->dlen - tcp_sk(sk)->tcb.iss,         \
                    hdr->ack_seq - tcp_sk(sk)->tcb.irs, hdr->win);      \
    } while (0)

#define tcpsock_dbg(msg, sk)                                            \
    do {                                                                \
        print_debug("TCP x:%u > %hhu.%hhu.%hhu.%hhu.%u (snd_una %u, snd_nxt %u, snd_wnd %u, " \
                    "snd_wl1 %u, snd_wl2 %u, rcv_nxt %u, rcv_wnd %u) state %s: "msg, \
                    sk->sport, sk->daddr >> 24, sk->daddr >> 16, sk->daddr >> 8, sk->daddr >> 0, \
                    sk->dport, tcp_sk(sk)->tcb.snd_una - tcp_sk(sk)->tcb.iss,      \
                    tcp_sk(sk)->tcb.snd_nxt - tcp_sk(sk)->tcb.iss, tcp_sk(sk)->tcb.snd_wnd, \
                    tcp_sk(sk)->tcb.snd_wl1, tcp_sk(sk)->tcb.snd_wl2,   \
                    tcp_sk(sk)->tcb.rcv_nxt - tcp_sk(sk)->tcb.irs, tcp_sk(sk)->tcb.rcv_wnd, \
                    tcp_dbg_states[sk->state]);                                        \
    } while (0)

#define tcp_set_state(sk, state)                                        \
    do {                                                                \
        tcpsock_dbg("state is now "#state, sk);                         \
        __tcp_set_state(sk, state);                                     \
    } while (0)

#else
#define tcp_in_dbg(hdr, sk, skb)
#define tcp_out_dbg(hdr, sk, skb)
#define tcpsock_dbg(msg, sk)
#define tcp_set_state(sk, state)  __tcp_set_state(sk, state)
#endif

struct tcphdr {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack_seq;
    uint8_t rsvd : 4;
    uint8_t hl : 4;
    uint8_t fin : 1,
            syn : 1,
            rst : 1,
            psh : 1,
            ack : 1,
            urg : 1,
            ece : 1,
            cwr : 1;
    uint16_t win;
    uint16_t csum;
    uint16_t urp;
    uint8_t data[];
} __attribute__((packed));

struct tcp_options {
    uint16_t options;
    uint16_t mss;
};

struct tcpiphdr {
    uint32_t saddr;
    uint32_t daddr;
    uint8_t zero;
    uint8_t proto;
    uint16_t tlen;
} __attribute__((packed));

enum tcp_states {
    TCP_LISTEN, /* represents waiting for a connection request from any remote
                   TCP and port. */
    TCP_SYN_SENT, /* represents waiting for a matching connection request
                     after having sent a connection request. */
    TCP_SYN_RECEIVED, /* represents waiting for a confirming connection
                         request acknowledgment after having both received and sent a
                         connection request. */
    TCP_ESTABLISHED, /* represents an open connection, data received can be
                        delivered to the user.  The normal state for the data transfer phase
                        of the connection. */
    TCP_FIN_WAIT_1, /* represents waiting for a connection termination request
                       from the remote TCP, or an acknowledgment of the connection
                       termination request previously sent. */
    TCP_FIN_WAIT_2, /* represents waiting for a connection termination request
                       from the remote TCP. */
    TCP_CLOSE, /* represents no connection state at all. */
    TCP_CLOSE_WAIT, /* represents waiting for a connection termination request
                       from the local user. */
    TCP_CLOSING, /* represents waiting for a connection termination request
                    acknowledgment from the remote TCP. */
    TCP_LAST_ACK, /* represents waiting for an acknowledgment of the
                     connection termination request previously sent to the remote TCP
                     (which includes an acknowledgment of its connection termination
                     request). */
    TCP_TIME_WAIT, /* represents waiting for enough time to pass to be sure
                      the remote TCP received the acknowledgment of its connection
                      termination request. */
};

#ifdef DEBUG_TCP
static const char *tcp_dbg_states[] = {
    "TCP_LISTEN", "TCP_SYNSENT", "TCP_SYN_RECEIVED", "TCP_ESTABLISHED", "TCP_FIN_WAIT_1",
    "TCP_FIN_WAIT_2", "TCP_CLOSE", "TCP_CLOSE_WAIT", "TCP_CLOSING", "TCP_LAST_ACK", "TCP_TIME_WAIT",
};
#endif

struct tcb {
    uint32_t snd_una; /* oldest unacknowledged sequence number */
    uint32_t snd_nxt; /* next sequence number to be sent */
    uint32_t snd_wnd;
    uint32_t snd_up;
    uint32_t snd_wl1;
    uint32_t snd_wl2;
    uint32_t iss;
    uint32_t rcv_nxt; /* next sequence number expected on an incoming segments, and
                         is the left or lower edge of the receive window */
    uint32_t rcv_wnd;
    uint32_t rcv_up;
    uint32_t irs;
};

struct tcp_sock {
    struct sock sk;
    int fd;
    uint16_t tcp_header_len;
    struct tcb tcb;
    uint8_t flags;
    uint8_t backoff;
    struct timer *retransmit;
    struct timer *delack;
    struct timer *keepalive;
    struct timer *linger;
    uint8_t delacks;
    uint16_t mss;
    struct sk_buff_head ofo_queue; /* Out-of-order queue */
};

static inline struct tcphdr *tcp_hdr(const struct sk_buff *skb)
{
    return (struct tcphdr *)(skb->head + ETH_HDR_LEN + IP_HDR_LEN);
}

void tcp_init();
void tcp_in(struct sk_buff *skb);
int tcp_checksum(struct tcp_sock *sock, struct tcphdr *thdr);
void tcp_select_initial_window(uint32_t *rcv_wnd);

int generate_iss();
struct sock *tcp_alloc_sock();
int tcp_v4_init_sock(struct sock *sk);
int tcp_init_sock(struct sock *sk);
void __tcp_set_state(struct sock *sk, uint32_t state);
int tcp_v4_checksum(struct sk_buff *skb, uint32_t saddr, uint32_t daddr);
int tcp_v4_connect(struct sock *sk, const struct sockaddr *addr, int addrlen, int flags);
int tcp_connect(struct sock *sk);
int tcp_disconnect(struct sock *sk, int flags);
int tcp_write(struct sock *sk, const void *buf, int len);
int tcp_read(struct sock *sk, void *buf, int len);
int tcp_receive(struct tcp_sock *tsk, void *buf, int len);
int tcp_input_state(struct sock *sk, struct tcphdr *th, struct sk_buff *skb);
int tcp_send_synack(struct sock *sk);
int tcp_send_ack(struct sock *sk);
void tcp_send_delack(uint32_t ts, void *arg);
int tcp_queue_fin(struct sock *sk);
int tcp_send_fin(struct sock *sk);
int tcp_send(struct tcp_sock *tsk, const void *buf, int len);
int tcp_send_reset(struct tcp_sock *tsk);
int tcp_send_challenge_ack(struct sock *sk, struct sk_buff *skb);
int tcp_recv_notify(struct sock *sk);
int tcp_close(struct sock *sk);
int tcp_abort(struct sock *sk);
int tcp_free(struct sock *sk);
int tcp_done(struct sock *sk);
void tcp_handle_fin_state(struct sock *sk);
void tcp_enter_time_wait(struct sock *sk);
void tcp_clear_timers(struct sock *sk);
void tcp_rearm_rto_timer(struct tcp_sock *tsk);
void tcp_stop_rto_timer(struct tcp_sock *tsk);
void tcp_release_rto_timer(struct tcp_sock *tsk);
void tcp_stop_delack_timer(struct tcp_sock *tsk);
void tcp_release_delack_timer(struct tcp_sock *tsk);

#endif
