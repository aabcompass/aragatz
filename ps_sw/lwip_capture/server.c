/* One raw-API context, one client, one immutable capture.
 * No TCP API is called from an interrupt handler. */
#include "server.h"
#include "capture.h"
#include "protocol.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"
#include <string.h>

#define COMMAND_QUEUE 8U
#define TX_CHUNK 4096U
#define TX_BUDGET 65536U
#define PEER_TIMEOUT_US UINT64_C(120000000)
struct connection {
    struct tcp_pcb *pcb;
    uint8_t rx[AR_COMMAND_BYTES];
    unsigned rx_used;
    uint8_t commands[COMMAND_QUEUE][AR_COMMAND_BYTES];
    unsigned count, rd, wr;
    uint8_t header[AR_HEADER_BYTES];
    const uint8_t *payload;
    uint32_t payload_bytes, queued, acked, total;
    uint64_t tx_started_us, progress_us;
    uint64_t delivered_id;
    int tx_active, tx_data, payload_started, waiting_capture;
};
static struct connection c;
static uint32_t link_mbps;
static int can_capture;
static uint64_t last_tx_us, last_tx_acked;
static struct tcp_pcb *listener;
static uint32_t next_id;

static void detach(struct tcp_pcb *pcb)
{
    tcp_arg(pcb,0); tcp_recv(pcb,0); tcp_sent(pcb,0); tcp_err(pcb,0);
    tcp_poll(pcb,0,0);
}
/* Return whether tcp_abort actually ran: callbacks must return ERR_ABRT then. */
static int disconnect_peer(int graceful)
{
    struct tcp_pcb *pcb=c.pcb;
    int aborted=0;
    if (pcb) {
        detach(pcb);
        /* No application buffer is released here, including tcp_close errors. */
        if (!graceful || tcp_close(pcb)!=ERR_OK) { tcp_abort(pcb); aborted=1; }
    }
    memset(&c,0,sizeof c);
    return aborted;
}
static void on_error(void *arg, err_t err)
{
    (void)arg; (void)err;
    /* lwIP has already freed the PCB. Capture keeps running or stays READY. */
    memset(&c,0,sizeof c);
}
static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)arg; (void)pcb;
    if (!c.tx_active || (uint32_t)len>c.queued-c.acked) {
        disconnect_peer(0); return ERR_ABRT;
    }
    c.acked+=len; c.progress_us=ar_now_us();
    if (c.acked==c.total && c.queued==c.total) {
        if (c.tx_data) {
            last_tx_us=c.progress_us-c.tx_started_us;
            last_tx_acked=c.payload_bytes; /* Header excluded. */
            c.delivered_id=ar_capture_meta()->id;
        }
        c.tx_active=0;
    }
    return ERR_OK;
}
static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
    (void)arg; (void)pcb;
    /* All sending and timeouts are handled by the cooperative main loop. */
    return ERR_OK;
}
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    struct pbuf *q;
    (void)arg; (void)pcb;
    if (err!=ERR_OK) {
        if (p) pbuf_free(p);
        disconnect_peer(0); return ERR_ABRT;
    }
    if (!p) {
        int graceful=!c.tx_active;
        int aborted=disconnect_peer(graceful);
        return aborted?ERR_ABRT:ERR_OK;
    }
    for (q=p;q;q=q->next) {
        const uint8_t *bytes=(const uint8_t *)q->payload;
        u16_t i;
        for (i=0;i<q->len;++i) {
            if (c.count==COMMAND_QUEUE) {
                pbuf_free(p); disconnect_peer(0); return ERR_ABRT;
            }
            c.rx[c.rx_used++]=bytes[i];
            if (c.rx_used==AR_COMMAND_BYTES) {
                memcpy(c.commands[c.wr],c.rx,AR_COMMAND_BYTES);
                c.wr=(c.wr+1U)%COMMAND_QUEUE; ++c.count; c.rx_used=0;
            }
        }
    }
    c.progress_us=ar_now_us();
    tcp_recved(c.pcb,p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    if (!pcb) return ERR_VAL;
    if (err!=ERR_OK || c.pcb) { tcp_abort(pcb); return ERR_ABRT; }
    memset(&c,0,sizeof c);
    c.pcb=pcb; c.progress_us=ar_now_us();
    tcp_arg(pcb,&c); tcp_recv(pcb,on_recv); tcp_sent(pcb,on_sent);
    tcp_err(pcb,on_error); tcp_poll(pcb,on_poll,2);
    tcp_nagle_disable(pcb);
    return ERR_OK;
}
int ar_server_init(uint16_t port, int capture_enabled)
{
    struct tcp_pcb *pcb=tcp_new(), *listening;
    can_capture=capture_enabled;
    if (!pcb) return -1;
    if (tcp_bind(pcb,IP_ADDR_ANY,port)!=ERR_OK) {
        if (tcp_close(pcb)!=ERR_OK) tcp_abort(pcb);
        return -1;
    }
    listening=tcp_listen(pcb);
    if (!listening) {
        if (tcp_close(pcb)!=ERR_OK) tcp_abort(pcb);
        return -1;
    }
    listener=listening; tcp_accept(listener,on_accept);
    return 0;
}
void ar_server_set_link(uint32_t mbps)
{
    link_mbps = (mbps==10U || mbps==100U || mbps==1000U) ? mbps : 0U;
}
static void reply(uint16_t type, uint16_t code)
{
    const struct ar_capture_metadata *m=ar_capture_meta();
    ar_encode_header(c.header,type,code,m,(unsigned)ar_capture_state(),
                     link_mbps,last_tx_us,last_tx_acked);
    c.payload=type==AR_DATA?m->data:0;
    c.payload_bytes=type==AR_DATA?(uint32_t)m->payload_bytes:0;
    c.total=AR_HEADER_BYTES+c.payload_bytes; c.queued=0; c.acked=0;
    c.tx_active=1; c.tx_data=type==AR_DATA; c.payload_started=0;
    c.tx_started_us=0; c.progress_us=ar_now_us();
}
static void command(const uint8_t *wire, uint64_t now_us)
{
    struct ar_command cmd;
    enum ar_capture_state state=ar_capture_state();
    const struct ar_capture_metadata *m=ar_capture_meta();
    if (ar_decode_command(&cmd,wire)!=0) { reply(AR_ERROR,AR_BAD_COMMAND); return; }
    switch (cmd.opcode) {
    case AR_STATUS:
        reply(AR_STATUS_REPLY,AR_OK); break;
    case AR_START:
        if (!can_capture || !link_mbps) { reply(AR_ERROR,AR_NOT_READY); break; }
        if (state==AR_CAPTURE_FAULT) { reply(AR_ERROR,AR_CAPTURE_FAILED); break; }
        if (state!=AR_CAPTURE_IDLE && state!=AR_CAPTURE_VERIFIED) {
            reply(AR_ERROR,AR_BUSY); break;
        }
        if (++next_id == 0U) ++next_id;
        if (ar_capture_begin(next_id,cmd.arg[0],cmd.arg[1],cmd.arg[2],now_us)!=0) {
            reply(AR_ERROR,ar_capture_state()==AR_CAPTURE_FAULT?
                  AR_CAPTURE_FAILED:AR_BAD_CONFIG); break;
        }
        c.waiting_capture=1; c.delivered_id=0;
        last_tx_us=0; last_tx_acked=0;
        break;
    case AR_FETCH:
        if (!m->id) { reply(AR_ERROR,AR_NO_CAPTURE); break; }
        if (cmd.id!=m->id) { reply(AR_ERROR,AR_ID_MISMATCH); break; }
        if (state==AR_CAPTURE_FAULT) { reply(AR_ERROR,AR_CAPTURE_FAILED); break; }
        if (state!=AR_CAPTURE_READY && state!=AR_CAPTURE_VERIFIED) {
            reply(AR_ERROR,AR_NOT_READY); break;
        }
        reply(AR_DATA,AR_OK); break;
    case AR_ACK:
        if (cmd.id!=m->id) { reply(AR_ERROR,AR_ID_MISMATCH); break; }
        if (cmd.arg[2] || c.delivered_id!=cmd.id ||
            ar_capture_verified(cmd.id,cmd.arg[1],cmd.arg[0])!=0 ||
            ar_capture_release()!=0) {
            reply(AR_ERROR,AR_VERIFY_FAILED); break;
        }
        c.delivered_id=0;
        reply(AR_ACKED,AR_OK); break;
    default: reply(AR_ERROR,AR_BAD_COMMAND); break;
    }
}
static void pump(void)
{
    uint32_t budget=TX_BUDGET;
    while (c.pcb && c.tx_active && c.queued<c.total && budget) {
        uint32_t avail=(uint32_t)tcp_sndbuf(c.pcb);
        uint32_t remaining, amount;
        const uint8_t *p;
        uint64_t enqueue_started;
        err_t err;
        u8_t flags=TCP_WRITE_FLAG_COPY;
        if (!avail) break;
        if (c.queued<AR_HEADER_BYTES) {
            p=c.header+c.queued; remaining=AR_HEADER_BYTES-c.queued;
        } else {
            p=c.payload+c.queued-AR_HEADER_BYTES;
            remaining=c.total-c.queued;
        }
        amount=remaining;
        if (amount>TX_CHUNK) amount=TX_CHUNK;
        if (amount>avail) amount=avail;
        if (amount>budget) amount=budget;
        if (c.queued+amount<c.total) flags|=TCP_WRITE_FLAG_MORE;
        enqueue_started=ar_now_us();
        err=tcp_write(c.pcb,p,(u16_t)amount,flags);
        if (err==ERR_MEM) break; /* Retry after ACK/poll; do not move offset. */
        if (err!=ERR_OK) { disconnect_peer(0); return; }
        if (c.tx_data && c.queued>=AR_HEADER_BYTES && !c.payload_started) {
            c.payload_started=1; c.tx_started_us=enqueue_started;
        }
        c.queued+=amount; budget-=amount; c.progress_us=ar_now_us();
    }
    if (c.pcb && c.tx_active && c.queued>c.acked) {
        err_t err=tcp_output(c.pcb);
        if (err!=ERR_OK && err!=ERR_MEM && err!=ERR_BUF &&
            err!=ERR_RTE && err!=ERR_IF) disconnect_peer(0);
    }
}
void ar_server_poll(uint64_t now_us)
{
    if (!c.pcb) return;
    if (c.tx_active && c.tx_data &&
        ar_capture_state()!=AR_CAPTURE_READY &&
        ar_capture_state()!=AR_CAPTURE_VERIFIED) {
        disconnect_peer(0); return;
    }
    if (now_us-c.progress_us>PEER_TIMEOUT_US) { disconnect_peer(0); return; }
    if (c.waiting_capture) {
        enum ar_capture_state state=ar_capture_state();
        if (state==AR_CAPTURE_READY) {
            c.waiting_capture=0; reply(AR_DATA,AR_OK);
        } else if (state==AR_CAPTURE_FAULT) {
            c.waiting_capture=0; reply(AR_ERROR,AR_CAPTURE_FAILED);
        }
    }
    if (!c.tx_active && !c.waiting_capture && c.count) {
        uint8_t wire[AR_COMMAND_BYTES];
        memcpy(wire,c.commands[c.rd],sizeof wire);
        c.rd=(c.rd+1U)%COMMAND_QUEUE; --c.count;
        command(wire,now_us);
    }
    pump();
}
