#ifndef TEST_CAPTURE_XIL_IO_H
#define TEST_CAPTURE_XIL_IO_H
#include <stdint.h>
typedef uintptr_t UINTPTR;
typedef intptr_t INTPTR;
uint32_t Xil_In32(UINTPTR address);
void Xil_Out32(UINTPTR address, uint32_t value);
#endif
