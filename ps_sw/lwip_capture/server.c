#include "capture.h"
#include "lwip/tcp.h"
#include <string.h>
static struct tcp_pcb *peer;
static unsigned rxlen,offset,limit,waiting;
static unsigned char rx[32],hdr[192];
static uint32_t get(const unsigned char *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void put(unsigned char *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void reset_peer(void){peer=0;rxlen=offset=limit=waiting=0;}
static void error_cb(void *arg,err_t e){(void)arg;(void)e;reset_peer();}
static err_t abort_peer(struct tcp_pcb *p){reset_peer();tcp_abort(p);return ERR_ABRT;}
static void response(unsigned result,unsigned payload){
    memset(hdr,0,sizeof hdr);memcpy(hdr,"ARGZ",4);
    put(hdr+4,1);put(hdr+8,sizeof hdr);put(hdr+12,result);
    put(hdr+16,capture_state());put(hdr+20,capture_id());put(hdr+24,payload);
    put(hdr+28,capture_crc());put(hdr+32,64);put(hdr+36,16);put(hdr+40,16);
    put(hdr+44,128);put(hdr+48,768);put(hdr+52,128);put(hdr+56,128);
    put(hdr+188,ar_crc(hdr,188));offset=0;limit=sizeof hdr+payload;
}
static err_t receive(void *arg,struct tcp_pcb *p,struct pbuf *b,err_t e){
    (void)arg;
    if(!b){return abort_peer(p);}
    if(e!=ERR_OK||waiting||limit||b->tot_len>sizeof rx-rxlen){pbuf_free(b);return abort_peer(p);}
    unsigned n=b->tot_len;
    pbuf_copy_partial(b,rx+rxlen,n,0);rxlen+=n;tcp_recved(p,n);pbuf_free(b);
    if(rxlen!=sizeof rx)return ERR_OK;
    rxlen=0;
    if(memcmp(rx,"ARGC",4)||get(rx+4)!=1||get(rx+28)!=ar_crc(rx,28))return abort_peer(p);
    unsigned op=get(rx+8);
    if(op==1){if(capture_start()==0)waiting=1;else response(1,0);}
    else if(op==2){response(capture_state()==5?0:1,capture_state()==5?capture_size():0);}
    else if(op==3){response(capture_ack(get(rx+12),get(rx+16),get(rx+20))?1:0,0);}
    else if(op==4){response(0,0);}
    else response(2,0);
    return ERR_OK;
}
static err_t accept_cb(void *arg,struct tcp_pcb *p,err_t e){
    (void)arg;if(e!=ERR_OK)return e;
    if(peer){tcp_abort(p);return ERR_ABRT;}
    peer=p;tcp_recv(p,receive);tcp_err(p,error_cb);return ERR_OK;
}
void transfer_data(void){
    capture_poll();
    if(!peer)return;
    if(waiting&&(capture_state()==5||capture_state()==6)){
        waiting=0;response(capture_state()==5?0:1,capture_state()==5?capture_size():0);
    }
    if(!limit)return;
    unsigned n=offset<sizeof hdr?sizeof hdr-offset:limit-offset;
    if(n>4096)n=4096;
    if(n>tcp_sndbuf(peer))n=tcp_sndbuf(peer);
    if(n){
        const unsigned char *p=offset<sizeof hdr?hdr+offset:(const unsigned char *)capture_data()+offset-sizeof hdr;
        err_t e=tcp_write(peer,p,n,TCP_WRITE_FLAG_COPY);
        if(e==ERR_OK)offset+=n;
        else if(e!=ERR_MEM){abort_peer(peer);return;}
    }
    tcp_output(peer);
    if(offset==limit)offset=limit=0;
}
int start_application(void){
    struct tcp_pcb *p=tcp_new();
    if(!p)return -1;
    if(tcp_bind(p,IP_ADDR_ANY,5001)!=ERR_OK){tcp_abort(p);return -1;}
    struct tcp_pcb *l=tcp_listen(p);
    if(!l){tcp_abort(p);return -1;}
    tcp_accept(l,accept_cb);return 0;
}
void print_app_header(void){}
