#ifndef TEST_LWIP_TCP_H
#define TEST_LWIP_TCP_H
#include <stdint.h>
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
typedef int err_t;
#define ERR_OK 0
#define ERR_VAL -6
#define ERR_MEM -1
#define ERR_BUF -2
#define ERR_RTE -4
#define ERR_IF -12
#define ERR_ABRT -13
#define ERR_CONN -11
#define TCP_WRITE_FLAG_COPY 1
#define TCP_WRITE_FLAG_MORE 2
#define IP_ADDR_ANY ((void*)0)
struct pbuf;
struct tcp_pcb {
 void *arg;
 err_t (*recv)(void*,struct tcp_pcb*,struct pbuf*,err_t);
 err_t (*sent)(void*,struct tcp_pcb*,u16_t);
 err_t (*poll)(void*,struct tcp_pcb*);
 void (*error)(void*,err_t);
 err_t (*accept)(void*,struct tcp_pcb*,err_t);
 uint32_t space;
};
struct tcp_pcb *tcp_new(void);
err_t tcp_bind(struct tcp_pcb*, const void*, u16_t);
struct tcp_pcb *tcp_listen(struct tcp_pcb*);
void tcp_arg(struct tcp_pcb*,void*);
void tcp_recv(struct tcp_pcb*,err_t (*)(void*,struct tcp_pcb*,struct pbuf*,err_t));
void tcp_sent(struct tcp_pcb*,err_t (*)(void*,struct tcp_pcb*,u16_t));
void tcp_err(struct tcp_pcb*,void (*)(void*,err_t));
void tcp_poll(struct tcp_pcb*,err_t (*)(void*,struct tcp_pcb*),u8_t);
void tcp_accept(struct tcp_pcb*,err_t (*)(void*,struct tcp_pcb*,err_t));
void tcp_nagle_disable(struct tcp_pcb*);
err_t tcp_close(struct tcp_pcb*);
void tcp_abort(struct tcp_pcb*);
u16_t tcp_sndbuf(struct tcp_pcb*);
err_t tcp_write(struct tcp_pcb*,const void*,u16_t,u8_t);
err_t tcp_output(struct tcp_pcb*);
void tcp_recved(struct tcp_pcb*,u16_t);
#endif
