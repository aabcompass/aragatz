/* Behavioural host test of the real server.c with a controllable raw TCP API.
 * Checks the ownership boundary and backpressure, not vendor lwIP hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../server.c"

static struct tcp_pcb pcbs[32];
static unsigned pcb_next, aborts, close_calls, begins, write_calls, fail_write;
static int fail_close;
static uint64_t clock_us;
static enum ar_capture_state mock_state;
static struct ar_capture_metadata mock_meta;
static uint8_t payload[1664], output[32768];
static unsigned output_used;
static uint32_t window=500;
static unsigned checks;
#define CHECK(x) do { ++checks; assert(x); } while(0)

uint64_t ar_now_us(void) { return clock_us; }
enum ar_capture_state ar_capture_state(void) { return mock_state; }
const struct ar_capture_metadata *ar_capture_meta(void) { return &mock_meta; }
const char *ar_capture_fault_reason(void) { return "mock"; }
int ar_capture_init(void) { return 0; }
void ar_capture_poll(uint64_t now) { (void)now; }
int ar_capture_begin(uint64_t id,uint32_t frames,uint32_t prev,uint32_t fragments,uint64_t now) {
 (void)now;
 if (frames!=2 || prev!=0 || fragments!=1) return -1;
 ++begins; mock_meta.id=id; mock_state=AR_CAPTURE_CAPTURING; return 0;
}
int ar_capture_verified(uint64_t id,uint32_t crc,uint32_t bytes) {
 if ((mock_state!=AR_CAPTURE_READY && mock_state!=AR_CAPTURE_VERIFIED) ||
     id!=mock_meta.id || crc!=mock_meta.payload_crc32 || bytes!=sizeof payload) return -1;
 mock_state=AR_CAPTURE_VERIFIED; return 0;
}
int ar_capture_release(void) {
 if (mock_state!=AR_CAPTURE_VERIFIED) return -1;
 mock_state=AR_CAPTURE_IDLE;mock_meta.frames=mock_meta.prev=mock_meta.fragments=0;
 mock_meta.payload_bytes=mock_meta.payload_crc32=mock_meta.gen_period=0;
 return 0;
}
struct tcp_pcb *tcp_new(void) {
 assert(pcb_next<32); memset(&pcbs[pcb_next],0,sizeof pcbs[0]);
 pcbs[pcb_next].space=window; return &pcbs[pcb_next++];
}
err_t tcp_bind(struct tcp_pcb *p,const void *ip,u16_t port) {(void)p;(void)ip;(void)port;return ERR_OK;}
struct tcp_pcb *tcp_listen(struct tcp_pcb *p) {return p;}
void tcp_arg(struct tcp_pcb *p,void *a) {p->arg=a;}
void tcp_recv(struct tcp_pcb *p,err_t (*f)(void*,struct tcp_pcb*,struct pbuf*,err_t)) {p->recv=f;}
void tcp_sent(struct tcp_pcb *p,err_t (*f)(void*,struct tcp_pcb*,u16_t)) {p->sent=f;}
void tcp_err(struct tcp_pcb *p,void (*f)(void*,err_t)) {p->error=f;}
void tcp_poll(struct tcp_pcb *p,err_t (*f)(void*,struct tcp_pcb*),u8_t n) {(void)n;p->poll=f;}
void tcp_accept(struct tcp_pcb *p,err_t (*f)(void*,struct tcp_pcb*,err_t)) {p->accept=f;}
void tcp_nagle_disable(struct tcp_pcb *p) {(void)p;}
err_t tcp_close(struct tcp_pcb *p) {(void)p;++close_calls;return fail_close?ERR_MEM:ERR_OK;}
void tcp_abort(struct tcp_pcb *p) {(void)p;++aborts;}
u16_t tcp_sndbuf(struct tcp_pcb *p) {return (u16_t)p->space;}
err_t tcp_write(struct tcp_pcb *p,const void *buf,u16_t n,u8_t flags) {
 ++write_calls;
 CHECK(flags&TCP_WRITE_FLAG_COPY);
 if (write_calls==fail_write) return ERR_MEM;
 CHECK(n<=p->space); CHECK(output_used+n<=sizeof output);
 memcpy(output+output_used,buf,n);output_used+=n;p->space-=n;return ERR_OK;
}
err_t tcp_output(struct tcp_pcb *p) {(void)p;return ERR_OK;}
void tcp_recved(struct tcp_pcb *p,u16_t n) {(void)p;(void)n;}
u8_t pbuf_free(struct pbuf *p) {(void)p;return 1;}
static void be32(uint8_t *p,uint32_t n) {p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;}
static void le32(uint8_t *p,uint32_t n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);}
static uint32_t u32be(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void wire(uint8_t *p,unsigned op,uint64_t id,uint32_t a,uint32_t b,uint32_t d) {
 memset(p,0,32);memcpy(p,"ARGC",4);be32(p+4,1);be32(p+8,op);assert(id<=UINT32_MAX);be32(p+12,(uint32_t)id);
 be32(p+16,a);be32(p+20,b);be32(p+24,d);be32(p+28,ar_crc32_update(0,p,28));
}
static void payload_init(void) {
 unsigned i,f,w;memset(payload,0,sizeof payload);
 le32(payload,0xc0dec035);le32(payload+8,123456);
 le32(payload+16,128);le32(payload+20,0x54524752);
 le32(payload+24,0x11223344);le32(payload+28,0x55667788);
 for(i=56;i<128;i+=4)le32(payload+i,0x12345678);
 for(f=0;f<2;++f)for(w=0;w<96;++w){uint8_t *p=payload+128+f*768+w*8;le32(p,w);le32(p+4,128+f);}
 memset(&mock_meta,0,sizeof mock_meta);
 mock_meta.id=0x01020304ULL;mock_meta.payload_bytes=sizeof payload;
 mock_meta.payload_crc32=ar_crc32_update(0,payload,sizeof payload);
 mock_meta.cap_status=6|(1U<<9);mock_meta.fragments=1;mock_meta.frames=2;
 mock_meta.gen_first=128;mock_meta.gen_period=512;mock_meta.gen_word=47;
 mock_meta.rate_num=24;mock_meta.rate_den=125;mock_meta.cap_version=mock_meta.gen_version=0x10000;
 mock_meta.completed_fragments=1;mock_meta.last_frames=2;mock_meta.last_bytes=sizeof payload;mock_meta.last_status=1;
 mock_meta.capture_us=1234;mock_meta.cache_us=20;mock_meta.crc_us=40;
 mock_meta.data=payload;
}
static void step(void) {clock_us+=100;ar_server_poll(clock_us);}
static void receive(const uint8_t *p,unsigned n) {
 struct pbuf q={0,(void*)p,(u16_t)n,(u16_t)n};
 CHECK(c.pcb!=0);CHECK(c.pcb->recv(c.pcb->arg,c.pcb,&q,ERR_OK)==ERR_OK);
}
static void acknowledge(void) {
 uint32_t outstanding=c.queued-c.acked;
 if(outstanding && c.pcb) {
  u16_t n=(u16_t)(outstanding>65535?65535:outstanding);
  c.pcb->space+=n;clock_us+=100;
  CHECK(c.pcb->sent(c.pcb->arg,c.pcb,n)==ERR_OK);
 }
}
static void drain(void) {
 unsigned budget=100;
 while(c.pcb && (c.tx_active || c.count)) {
  step();acknowledge();CHECK(budget--!=0);
 }
}
static void reset_case(int enabled,enum ar_capture_state state) {
 memset(&c,0,sizeof c);memset(pcbs,0,sizeof pcbs);
 pcb_next=aborts=close_calls=begins=write_calls=fail_write=output_used=0;fail_close=0;
 clock_us=1000;last_tx_us=last_tx_acked=0;next_id=0;payload_init();mock_state=state;
 CHECK(ar_server_init(5001,enabled)==0);ar_server_set_link(1000);
 CHECK(listener->accept(0,tcp_new(),ERR_OK)==ERR_OK);
}
static void send_cmd(unsigned op,uint64_t id,uint32_t a,uint32_t b,uint32_t d) {
 uint8_t cmd[32];wire(cmd,op,id,a,b,d);receive(cmd,32);step();
}
static void response(unsigned type,unsigned code) {
 (void)type; CHECK(output_used>=AR_HEADER_BYTES); CHECK(u32be(output)==0x4152475aU);
 CHECK(u32be(output+4)==1 && u32be(output+8)==64); CHECK(u32be(output+12)==code);
 CHECK(u32be(output+60)==ar_crc32_update(0,output,60));
}
static void clear_output(void) {drain();output_used=0;}
static void fetch(void) {send_cmd(AR_FETCH,mock_meta.id,0,0,0);drain();response(AR_DATA,AR_OK);
 CHECK(output_used==AR_HEADER_BYTES+sizeof payload);CHECK(memcmp(output+AR_HEADER_BYTES,payload,sizeof payload)==0);}
int main(int argc,char **argv) {
 uint8_t cmd[32];uint64_t id;unsigned old;struct tcp_pcb *p;
 CHECK(ar_crc32_update(0,(const uint8_t*)"123456789",9)==0xcbf43926U);
 CHECK(ar_crc32_update(ar_crc32_update(0,(const uint8_t*)"1234",4),(const uint8_t*)"56789",5)==0xcbf43926U);
 reset_case(0,AR_CAPTURE_IDLE);
 wire(cmd,AR_STATUS,0,0,0,0);
 receive(cmd,1);step();CHECK(output_used==0);receive(cmd+1,3);step();CHECK(output_used==0);
 receive(cmd+4,28);drain();response(AR_STATUS_REPLY,AR_OK);
 clear_output();send_cmd(AR_START,0,2,0,1);drain();response(AR_ERROR,AR_NOT_READY);CHECK(begins==0);
 clear_output();cmd[28]^=1;receive(cmd,32);drain();response(AR_ERROR,AR_BAD_COMMAND);CHECK(begins==0);

 reset_case(1,AR_CAPTURE_IDLE);send_cmd(AR_START,0,2,0,1);
 CHECK(begins==1 && c.waiting_capture && output_used==0); id=mock_meta.id; CHECK(id==1);
 mock_state=AR_CAPTURE_READY;fail_write=2;step();
 CHECK(c.queued==AR_HEADER_BYTES && output_used==AR_HEADER_BYTES);
 fail_write=0;
 /* A queued application ACK cannot release a buffer before TCP delivery. */
 wire(cmd,AR_ACK,id,sizeof payload,mock_meta.payload_crc32,0);receive(cmd,32);step();
 CHECK(mock_state==AR_CAPTURE_READY);
 drain();CHECK(mock_state==AR_CAPTURE_IDLE);
 CHECK(last_tx_acked==sizeof payload && last_tx_us>0);
 CHECK(memcmp(output+AR_HEADER_BYTES,payload,sizeof payload)==0);
 CHECK(output_used==AR_HEADER_BYTES+sizeof payload+AR_HEADER_BYTES);
 CHECK(u32be(output+AR_HEADER_BYTES+sizeof payload+12)==AR_OK);
 CHECK(u32be(output+AR_HEADER_BYTES+sizeof payload+16)==0);
 CHECK(u32be(output+AR_HEADER_BYTES+sizeof payload+36)==0);
 clear_output();send_cmd(AR_START,0,2,0,1);CHECK(begins==2 && c.waiting_capture);

 reset_case(1,AR_CAPTURE_READY);id=mock_meta.id;fetch();
 CHECK(mock_state==AR_CAPTURE_READY && last_tx_acked==sizeof payload);
 clear_output();send_cmd(AR_START,0,2,0,1);drain();response(AR_ERROR,AR_BUSY);CHECK(begins==0);
 clear_output();send_cmd(AR_ACK,id,sizeof payload,0,0);drain();response(AR_ERROR,AR_VERIFY_FAILED);CHECK(mock_state==AR_CAPTURE_READY);
 clear_output();send_cmd(AR_ACK,id,sizeof payload-1,mock_meta.payload_crc32,0);drain();response(AR_ERROR,AR_VERIFY_FAILED);
 clear_output();send_cmd(AR_ACK,id+1,sizeof payload,mock_meta.payload_crc32,0);drain();response(AR_ERROR,AR_ID_MISMATCH);
 clear_output();send_cmd(AR_ACK,id,sizeof payload,mock_meta.payload_crc32,1);drain();response(AR_ERROR,AR_BAD_COMMAND);
 clear_output();send_cmd(AR_ACK,id,sizeof payload,mock_meta.payload_crc32,0);drain();response(AR_ACKED,AR_OK);CHECK(mock_state==AR_CAPTURE_IDLE);
 clear_output();send_cmd(AR_ACK,id,sizeof payload,mock_meta.payload_crc32,0);drain();response(AR_ERROR,AR_VERIFY_FAILED);

 reset_case(1,AR_CAPTURE_READY);id=mock_meta.id;
 send_cmd(AR_ACK,id,sizeof payload,mock_meta.payload_crc32,0);drain();response(AR_ERROR,AR_VERIFY_FAILED);
 clear_output();send_cmd(AR_FETCH,id,0,0,0);CHECK(c.tx_active);
 p=c.pcb;p->error(p->arg,ERR_CONN);CHECK(!c.pcb && mock_state==AR_CAPTURE_READY);
 output_used=0;CHECK(listener->accept(0,tcp_new(),ERR_OK)==ERR_OK);fetch();CHECK(begins==0);

 reset_case(1,AR_CAPTURE_READY);clock_us+=PEER_TIMEOUT_US+1;ar_server_poll(clock_us);
 CHECK(!c.pcb && mock_state==AR_CAPTURE_READY && aborts==1);
 reset_case(1,AR_CAPTURE_READY);fail_close=1;p=c.pcb;
 CHECK(p->recv(p->arg,p,0,ERR_OK)==ERR_ABRT);CHECK(close_calls==1 && aborts==1 && mock_state==AR_CAPTURE_READY);
 reset_case(1,AR_CAPTURE_READY);p=c.pcb;
 CHECK(p->recv(p->arg,p,0,ERR_OK)==ERR_OK);CHECK(close_calls==1 && aborts==0 && mock_state==AR_CAPTURE_READY);

 reset_case(1,AR_CAPTURE_READY);send_cmd(AR_FETCH,mock_meta.id,0,0,0);
 old=output_used;mock_state=AR_CAPTURE_FAULT;step();CHECK(!c.pcb && output_used==old);
 reset_case(1,AR_CAPTURE_IDLE);
 {
  uint8_t flood[32*9];struct pbuf q={0,flood,sizeof flood,sizeof flood};
  unsigned i;for(i=0;i<9;i++)wire(flood+i*32,AR_START,0,2,0,1);
  CHECK(c.pcb->recv(c.pcb->arg,c.pcb,&q,ERR_OK)==ERR_ABRT);CHECK(!c.pcb && begins==0);
 }
 reset_case(1,AR_CAPTURE_READY);
 {
  uint8_t pair[32];struct pbuf a,b,d;
  wire(pair,AR_STATUS,0,0,0,0);
  d=(struct pbuf){0,pair+17,15,15};b=(struct pbuf){&d,pair+5,12,27};a=(struct pbuf){&b,pair,5,32};
  CHECK(c.pcb->recv(c.pcb->arg,c.pcb,&a,ERR_OK)==ERR_OK);drain();response(AR_STATUS_REPLY,AR_OK);
 }
 /* PHY failures and invalid speeds never enable a new capture. */
 reset_case(1,AR_CAPTURE_IDLE);ar_server_set_link(1);
 send_cmd(AR_START,0,2,0,1);drain();response(AR_ERROR,AR_NOT_READY);CHECK(begins==0);
 clear_output();ar_server_set_link(1000);send_cmd(AR_START,5,2,0,1);
 drain();response(AR_ERROR,AR_BAD_COMMAND);CHECK(begins==0);
 clear_output();send_cmd(AR_STATUS,0,1,0,0);drain();response(AR_ERROR,AR_BAD_COMMAND);
 clear_output();send_cmd(AR_FETCH,0,0,0,0);drain();response(AR_ERROR,AR_BAD_COMMAND);
 clear_output();send_cmd(AR_START,0,3,0,1);drain();response(AR_ERROR,AR_BAD_CONFIG);
 CHECK(begins==0);
 reset_case(1,AR_CAPTURE_IDLE);next_id=UINT32_MAX;
 send_cmd(AR_START,0,2,0,1);CHECK(mock_meta.id==1 && begins==1);
 p=c.pcb;CHECK(listener->accept(0,tcp_new(),ERR_OK)==ERR_ABRT);
 CHECK(c.pcb==p && begins==1);
 CHECK(listener->accept(0,0,ERR_OK)==ERR_VAL);
 /* Legacy 192-byte-draft request layout must not parse as this v1 format. */
 reset_case(1,AR_CAPTURE_IDLE);wire(cmd,AR_START,0,2,0,1);
 cmd[4]=0;cmd[5]=1;cmd[6]=0;cmd[7]=1;be32(cmd+28,ar_crc32_update(0,cmd,28));
 receive(cmd,32);drain();response(AR_ERROR,AR_BAD_COMMAND);CHECK(begins==0);
 if(argc==2) {
  uint8_t header[AR_HEADER_BYTES];FILE *f;payload_init();mock_state=AR_CAPTURE_READY;
  ar_encode_header(header,AR_DATA,0,&mock_meta,AR_CAPTURE_READY,1000,0,0);
  f=fopen(argv[1],"wb");CHECK(f!=0);CHECK(fwrite(header,1,AR_HEADER_BYTES,f)==AR_HEADER_BYTES);
  CHECK(fwrite(payload,1,sizeof payload,f)==sizeof payload);CHECK(fclose(f)==0);
 }
 printf("SERVER HOST PASS: %u checks (fragmentation, ERR_MEM, TCP/app ACK, retained replay, faults)\n",checks);
 return 0;
}
