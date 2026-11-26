#ifndef AR_CAPTURE_H
#define AR_CAPTURE_H
#include <stdint.h>
unsigned capture_state(void);
uint32_t capture_id(void),capture_crc(void);
unsigned capture_size(void);
const void *capture_data(void);
uint32_t ar_crc(const void *,unsigned);
int capture_start(void),capture_ack(uint32_t,uint32_t,uint32_t);
void capture_poll(void),transfer_data(void);
int start_application(void);
#endif
