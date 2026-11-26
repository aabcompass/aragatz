#ifndef TEST_LWIP_PBUF_H
#define TEST_LWIP_PBUF_H
#include "tcp.h"
struct pbuf {struct pbuf *next; void *payload; u16_t len,tot_len;};
u8_t pbuf_free(struct pbuf*);
#endif
