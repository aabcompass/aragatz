#ifndef TEST_CAPTURE_XIL_CACHE_H
#define TEST_CAPTURE_XIL_CACHE_H
#include "xil_io.h"
void Xil_DCacheFlushRange(INTPTR address, uint32_t length);
void Xil_DCacheInvalidateRange(INTPTR address, uint32_t length);
#endif
