#ifndef ARAGATZ_SERVER_H
#define ARAGATZ_SERVER_H
#include <stdint.h>
int ar_server_init(uint16_t port, int capture_enabled);
void ar_server_set_link(uint32_t mbps);
void ar_server_poll(uint64_t now_us);
uint64_t ar_now_us(void);
#endif
