/* Reconstructed TCP v1 capture application; standalone Zynq-7000.
 * Add to Vitis 2023.1 lwIP Echo Server template in place of echo.c/main.c.
 * Keep template platform.c, platform_zynq.c, platform_config.h and BSP.
 * Hardware test application is not linked here. */
#include <stdint.h>
#include <string.h>
#include "xparameters.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "xtime_l.h"
#include "platform.h"
#include "platform_config.h"
#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "netif/xadapter.h"
#include "aragatz_hw.h"
#include "site_config.h"

extern volatile int TcpFastTmrFlag;
extern volatile int TcpSlowTmrFlag;

#define CMD_MAGIC 0x41524743U
#define RESP_MAGIC 0x4152475aU
#define START 1U
#define FETCH 2U
#define ACK 3U
#define STATUS 4U
#define OK 0U
#define INVALID 1U
#define BUSY 2U
#define HWFAULT 3U
#define NOTFOUND 4U
#define EMPTY 0U
#define ACQUIRING 1U
#define RETAINED 2U
#define FAULT 3U
#define FIRST 128U
#define CHUNK 4096U
#define AXI_BUSY (1U << 8)
#define GUARD 256U

static struct netif netif0;
static struct tcp_pcb *peer;
static uint8_t cmd[32], header[64];
static uint32_t cmd_used, header_pos, payload_pos, tx_payload;
static uint32_t state, id, frames, prev, fragments, one, size, period, crc;
static uint32_t last_status, last_errors, phase, stop_sent;
static uint32_t waiting, sending, queued_all, outstanding;
static XTime deadline, peer_deadline;

static XTime now(void)
{
    XTime t;
    XTime_GetTime(&t);
    return t;
}
static XTime after_ms(uint32_t ms)
{
    return now() + ((XTime)COUNTS_PER_SECOND * ms) / 1000U;
}
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|p[3];
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static uint32_t crc32(const void *data, uint32_t n)
{
    const uint8_t *p = data;
    uint32_t c=~0U, j;
    while (n--) {
        c ^= *p++;
        for (j=0;j<8;j++) c=(c>>1)^(0xedb88320U & (0U-(c&1U)));
    }
    return ~c;
}
static void clear_peer(void)
{
    peer=0; cmd_used=0; waiting=0; sending=0; outstanding=0; queued_all=0;
}
static void drop_peer(void)
{
    struct tcp_pcb *p=peer;
    clear_peer();
    if (p) {
        tcp_arg(p,0); tcp_err(p,0);
        tcp_abort(p);
    }
}
static void reply(uint32_t result, uint32_t payload)
{
    uint32_t fields[15]={
        RESP_MAGIC,1,64,result,state,id,frames,prev,fragments,size,crc,
        FIRST,period,last_status,last_errors
    };
    unsigned i;
    if (!peer) return;
    for (i=0;i<15;i++) put32(header+4*i,fields[i]);
    put32(header+60,crc32(header,60));
    header_pos=0; payload_pos=0; tx_payload=payload;
    waiting=0; sending=1; queued_all=0; outstanding=0;
    peer_deadline=after_ms(30000);
}
static int buffer_ok(void)
{
    uint64_t base=ARAGATZ_TEST_DDR_BASE, end=base+ARAGATZ_TEST_DDR_SIZE;
    volatile uint32_t stack=0;
    return ARAGATZ_TEST_DDR_SIZE>=4096U &&
        !(base&127U) && !(ARAGATZ_TEST_DDR_SIZE&127U) &&
        base>=XPAR_PS7_DDR_0_S_AXI_BASEADDR &&
        end-1<=XPAR_PS7_DDR_0_S_AXI_HIGHADDR &&
        end-1<=ARAGATZ_HP0_HIGHADDR &&
        ((UINTPTR)&stack<base || (UINTPTR)&stack>=end);
}
static void fault(void)
{
    last_status=cap_rd(CAP_STATUS); last_errors=cap_rd(CAP_ERRORS);
    state=FAULT; stop_sent=0; deadline=after_ms(500);
    xil_printf("Capture fault: status=%08x errors=%08x\r\n",
               last_status,last_errors);
    if (waiting) reply(HWFAULT,0);
}
static void program(void)
{
    unsigned i;
    gen_wr(GEN_PAUSE,0);
    gen_wr(GEN_MODE,0);
    gen_wr(GEN_FRAME_WORDS,96);
    gen_wr(GEN_RATE_NUM,24);
    gen_wr(GEN_RATE_DEN,125);
    gen_wr(GEN_TRIG_MODE,1);
    gen_wr(GEN_TRIG_FIRST,FIRST);
    gen_wr(GEN_TRIG_PERIOD,period);
    gen_wr(GEN_TRIG_WORD,47);
    gen_wr(GEN_EXT_WIDTH,4);
    cap_wr(CAP_TRIGGER_ENABLE,1);
    cap_wr(CAP_N_FRAMES,frames);
    cap_wr(CAP_N_PREV,prev);
    cap_wr(CAP_N_FRAGMENTS,fragments);
    cap_wr(CAP_MEM_ADDR,ARAGATZ_TEST_DDR_BASE);
    cap_wr(CAP_HEADER_ID,ARAGATZ_HEADER_ID);
    cap_wr(CAP_AUX0,ARAGATZ_AUX0);
    cap_wr(CAP_AUX0+4,ARAGATZ_AUX1);
    for(i=2;i<8;i++) cap_wr(CAP_AUX0+4*i,0);
    cap_wr(CAP_INT_PERIOD,0);
    cap_wr(CAP_INT_CTRL,0);
    cap_wr(CAP_BUFFER_BYTES,size);
}
static void start(uint32_t f,uint32_t p,uint32_t n)
{
    uint64_t a=128U+768ULL*f, total=a*n;
    uint32_t s;
    if (state!=EMPTY) {reply(BUSY,0);return;}
    if(f<2 || f>131072 || p>64 || p>=f || !n ||
       total>ARAGATZ_TEST_DDR_SIZE-GUARD || !buffer_ok()) {
        reply(INVALID,0);return;
    }
    frames=f; prev=p; fragments=n; one=(uint32_t)a; size=(uint32_t)total;
    period=f+128U; if(period<512U) period=512U;
    ++id; if(!id) ++id;
    crc=0; waiting=1; state=ACQUIRING;
    s=cap_rd(CAP_STATUS)&15U;
    if ((s!=CAP_STATE_IDLE && s!=CAP_STATE_END) ||
        cap_rd(CAP_ERRORS) || cap_rd(CAP_VERSION)!=ARAGATZ_IP_VERSION ||
        gen_rd(GEN_VERSION)!=ARAGATZ_IP_VERSION) {fault();return;}
    gen_wr(GEN_COMMAND,GEN_CMD_STOP);
    if(s==CAP_STATE_END) cap_wr(CAP_COMMAND,CAP_CMD_STOP);
    phase=0; deadline=after_ms(500);
}
static void capture_step(void)
{
    uint32_t s,i;
    if(state==FAULT) {
        /* Bounded cleanup; source must continue until CAP is quiescent. */
        if(now()>=deadline) return;
        s=cap_rd(CAP_STATUS);
        if(((s&15U)==CAP_STATE_IDLE || (s&15U)==CAP_STATE_END ||
            (s&15U)==CAP_STATE_ERROR) && !(s&AXI_BUSY)) {
            gen_wr(GEN_COMMAND,GEN_CMD_STOP);
            deadline=0;
        } else if (!stop_sent && ((s&15U)==CAP_STATE_FILL ||
                   (s&15U)==CAP_STATE_WAIT || (s&15U)==CAP_STATE_XFER)) {
            gen_wr(GEN_PAUSE,0);
            cap_wr(CAP_COMMAND,CAP_CMD_STOP); stop_sent=1;
        }
        return;
    }
    if(state!=ACQUIRING) return;
    s=cap_rd(CAP_STATUS);
    if(cap_rd(CAP_ERRORS) || (s&15U)==CAP_STATE_ERROR ||
       now()>=deadline) {fault();return;}
    switch(phase) {
    case 0:
        if((s&15U)!=CAP_STATE_IDLE || (s&AXI_BUSY) || gen_rd(GEN_STATUS)) return;
        cap_wr(CAP_COMMAND,CAP_CMD_CLEAR_STATS);
        gen_wr(GEN_COMMAND,GEN_CMD_CLEAR_STATS);
        for(i=0;i<GUARD/4;i++) ddr_wr(size+4*i,ARAGATZ_SENTINEL(i));
        Xil_DCacheFlushRange((INTPTR)ARAGATZ_TEST_DDR_BASE,size+GUARD);
        program();
        cap_wr(CAP_COMMAND,CAP_CMD_START);
        phase=1; deadline=after_ms(100);
        break;
    case 1:
        if((s&15U)!=CAP_STATE_FILL) return;
        gen_wr(GEN_COMMAND,GEN_CMD_START);
        phase=2; deadline=after_ms(10000);
        break;
    case 2:
        if((s&15U)!=CAP_STATE_END || (s&AXI_BUSY)) return;
        gen_wr(GEN_COMMAND,GEN_CMD_STOP);
        phase=3; deadline=after_ms(500);
        break;
    case 3:
        if(gen_rd(GEN_STATUS)) return;
        last_status=s; last_errors=cap_rd(CAP_ERRORS);
        if((s&15U)!=CAP_STATE_END || (s&AXI_BUSY) || last_errors ||
            cap_rd(CAP_RUN_FRAGMENTS)!=fragments ||
            cap_rd(CAP_FRAGMENTS_TOTAL)!=fragments ||
            cap_rd(CAP_LAST_STATUS)!=1 ||
            cap_rd(CAP_LAST_FRAMES)!=frames ||
            cap_rd(CAP_LAST_BYTES)!=one ||
            cap_rd(CAP_LAST_ADDR)!=ARAGATZ_TEST_DDR_BASE+size-one ||
            cap_rd(CAP_TRIG_ACCEPTED)!=fragments ||
            cap_rd(CAP_TRIG_ACCEPTED_HI) ||
            cap_rd(CAP_TRIG_MISSED) || cap_rd(CAP_TRIG_MISSED_HI) ||
            cap_rd(CAP_AXI_ERRORS) || cap_rd(CAP_FIFO_OVERFLOWS) ||
            cap_rd(CAP_LAST_TRIG_SOURCE)!=1 ||
            cap_rd(CAP_LAST_TRIG_DATA_LO)!=FIRST+(fragments-1)*period ||
            cap_rd(CAP_LAST_TRIG_DATA_HI)!=ARAGATZ_TRIGGER_MAGIC) {fault();return;}
        Xil_DCacheInvalidateRange((INTPTR)ARAGATZ_TEST_DDR_BASE,size+GUARD);
        for(i=0;i<GUARD/4;i++)
            if(ddr_rd(size+4*i)!=ARAGATZ_SENTINEL(i)) {fault();return;}
        crc=crc32((const void *)(UINTPTR)ARAGATZ_TEST_DDR_BASE,size);
        state=RETAINED;
        if(waiting) reply(OK,size);
        break;
    }
}
static void command(void)
{
    uint32_t op=get32(cmd+8), rid=get32(cmd+12),
        f=get32(cmd+16), p=get32(cmd+20), n=get32(cmd+24);
    if(get32(cmd)!=CMD_MAGIC || get32(cmd+4)!=1 ||
       get32(cmd+28)!=crc32(cmd,28)) {reply(INVALID,0);return;}
    switch(op) {
    case START:
#if ARAGATZ_ENABLE_CAPTURE
        if(!rid) start(f,p,n); else reply(INVALID,0);
#else
        (void)start; reply(INVALID,0);
#endif
        break;
    case FETCH:
        if(f||p||n||!rid) reply(INVALID,0);
        else if(state==RETAINED && rid==id) reply(OK,size);
        else reply(NOTFOUND,0);
        break;
    case ACK:
        if(n||!rid) reply(INVALID,0);
        else if(state==RETAINED && rid==id && f==size && p==crc) {
            state=EMPTY; frames=prev=fragments=size=crc=period=0;
            reply(OK,0);
        } else reply(NOTFOUND,0);
        break;
    case STATUS:
        if(rid||f||p||n) reply(INVALID,0);
        else reply(OK,0);
        break;
    default: reply(INVALID,0);break;
    }
}
static err_t recv_cb(void *arg, struct tcp_pcb *p, struct pbuf *b,err_t err)
{
    uint16_t n;
    (void)arg;
    if(!b || err!=ERR_OK) {
        if(b) pbuf_free(b);
        drop_peer(); return ERR_ABRT;
    }
    n=b->tot_len;
    if(waiting || sending || n>sizeof(cmd)-cmd_used ||
       pbuf_copy_partial(b,cmd+cmd_used,n,0)!=n) {
        pbuf_free(b);drop_peer();return ERR_ABRT;
    }
    tcp_recved(p,n); pbuf_free(b);
    cmd_used+=n; peer_deadline=after_ms(30000);
    if(cmd_used==sizeof(cmd)) {cmd_used=0;command();}
    return ERR_OK;
}
static void err_cb(void *arg,err_t err)
{
    (void)arg;(void)err;clear_peer();
}
static err_t sent_cb(void *arg,struct tcp_pcb *p,uint16_t n)
{
    (void)arg;(void)p;
    if(n>outstanding) {drop_peer();return ERR_ABRT;}
    outstanding-=n;
    peer_deadline=after_ms(30000);
    if(queued_all && !outstanding) sending=0;
    return ERR_OK;
}
static err_t accept_cb(void *arg,struct tcp_pcb *p,err_t err)
{
    (void)arg;
    if(err!=ERR_OK || !p) return ERR_VAL;
    if(peer) {tcp_abort(p);return ERR_ABRT;}
    peer=p; peer_deadline=after_ms(30000);
    tcp_arg(p,0); tcp_recv(p,recv_cb); tcp_sent(p,sent_cb);tcp_err(p,err_cb);
    return ERR_OK;
}
static void tx_step(void)
{
    uint32_t n,left; const uint8_t *p; err_t e;
    if(!peer) return;
    if(now()>=peer_deadline) {drop_peer();return;}
    if(!sending || queued_all) return;
    if(header_pos<sizeof(header)) {
        p=header+header_pos;left=sizeof(header)-header_pos;
    } else {
        p=(const uint8_t *)(UINTPTR)ARAGATZ_TEST_DDR_BASE+payload_pos;
        left=tx_payload-payload_pos;
    }
    n=tcp_sndbuf(peer);if(n>CHUNK)n=CHUNK;if(n>left)n=left;
    if(n) {
        e=tcp_write(peer,p,(u16_t)n,TCP_WRITE_FLAG_COPY);
        if(e==ERR_MEM) {tcp_output(peer);return;}
        if(e!=ERR_OK) {drop_peer();return;}
        outstanding+=n;
        if(header_pos<sizeof(header)) header_pos+=n;else payload_pos+=n;
    }
    if(header_pos==sizeof(header) && payload_pos==tx_payload) {
        queued_all=1;
        if(!outstanding) sending=0;
    }
    e=tcp_output(peer);
    if(e!=ERR_OK && e!=ERR_MEM && e!=ERR_RTE) drop_peer();
}
int main(void)
{
    ip_addr_t ip,mask,gw;
    unsigned char mac[]={ARAGATZ_MAC};
    struct tcp_pcb *p,*listener;
    init_platform();
    lwip_init();
    IP4_ADDR(&ip,ARAGATZ_IP_A,ARAGATZ_IP_B,ARAGATZ_IP_C,ARAGATZ_IP_D);
    IP4_ADDR(&mask,255,255,255,0);
    IP4_ADDR(&gw,0,0,0,0);
    if(!xemac_add(&netif0,&ip,&mask,&gw,mac,PLATFORM_EMAC_BASEADDR)) {
        xil_printf("xemac_add failed\r\n");return 1;
    }
    netif_set_default(&netif0);
    platform_enable_interrupts();
    netif_set_up(&netif0);
    p=tcp_new();
    if(!p)return 1;
    if(tcp_bind(p,IP_ADDR_ANY,ARAGATZ_PORT)!=ERR_OK) {tcp_abort(p);return 1;}
    listener=tcp_listen(p);
    if(!listener) {tcp_abort(p);return 1;}
    tcp_accept(listener,accept_cb);
    xil_printf("Aragatz TCP v1 port %d capture=%d\r\n",
               ARAGATZ_PORT,ARAGATZ_ENABLE_CAPTURE);
    for(;;) {
        if(TcpFastTmrFlag) {tcp_fasttmr();TcpFastTmrFlag=0;}
        if(TcpSlowTmrFlag) {tcp_slowtmr();TcpSlowTmrFlag=0;}
        xemacif_input(&netif0);
        capture_step();tx_step();
    }
}
