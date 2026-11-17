/* Aragatz Zynq-7000 PS standalone hardware verification, Vitis 2023.1.
 * Board UART0: 115200 8N1. The BSP supplies xparameters.h. */
#include "xparameters.h"
#include "xil_printf.h"
#include "xuartps_hw.h"
#include "aragatz_hw.h"
#include "hw_tests.h"

#ifndef ARAGATZ_AUTORUN
#define ARAGATZ_AUTORUN 1
#endif

int main(void)
{
    int result;
    xil_printf("\r\n*** ARAGATZ HARDWARE TESTS V2 ***\r\n");
    xil_printf("CAP=%08x GEN=%08x DDR=%08x..%08x\r\n",
               (unsigned)CAP_BASE, (unsigned)GEN_BASE,
               (unsigned)XPAR_PS7_DDR_0_S_AXI_BASEADDR,
               (unsigned)XPAR_PS7_DDR_0_S_AXI_HIGHADDR);
    xil_printf("UART0=%08x test_buf=%08x size=%d\r\n",
               (unsigned)XPAR_PS7_UART_0_BASEADDR,
               (unsigned)ARAGATZ_TEST_DDR_BASE,
               (unsigned)ARAGATZ_TEST_DDR_SIZE);
#if ARAGATZ_AUTORUN
    result = aragatz_test_all();
    xil_printf("BOOT TESTS: %s\r\n", result == 0 ? "PASS" : "FAIL");
#endif
    for (;;) {
        char ch;
        xil_printf("\r\n1: AXI-Lite  2: Smoke  3: Pretrigger  4: Fragments"
                   "  a: All  q: Quit\r\n> ");
        ch = (char)XUartPs_RecvByte((uint32_t)STDIN_BASEADDRESS);
        xil_printf("%c\r\n", ch);
        result = 0;
        switch (ch) {
        case '1': result = aragatz_test_registers(); break;
        case '2': result = aragatz_test_smoke(); break;
        case '3': result = aragatz_test_pretrigger(); break;
        case '4': result = aragatz_test_fragments(); break;
        case 'a': case 'A': result = aragatz_test_all(); break;
        case 'q': case 'Q':
            xil_printf("Done.\r\n");
            return 0;
        case '\r': case '\n': continue;
        default: xil_printf("Unknown command\r\n"); continue;
        }
        xil_printf("RESULT: %s\r\n", result == 0 ? "PASS" : "FAIL");
    }
}
