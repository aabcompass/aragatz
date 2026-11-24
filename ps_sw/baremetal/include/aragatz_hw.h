#ifndef ARAGATZ_HW_H
#define ARAGATZ_HW_H

#include <stdint.h>
#include "xparameters.h"
#include "xil_io.h"

/* These names are taken from the Vitis 2023.1 BSP xparameters.h.
 * Never copy a generated xparameters.h into the Git repository. */
#if !defined(XPAR_CAMERA_CAPTURE_BD_0_BASEADDR) || \
    !defined(XPAR_CAMERA_CAPTURE_BD_0_HIGHADDR) || \
    !defined(XPAR_CAMERA_TEST_GEN_BD_0_BASEADDR) || \
    !defined(XPAR_CAMERA_TEST_GEN_BD_0_HIGHADDR)
#error "Missing camera Module Reference addresses: regenerate BSP from Aragatz XSA"
#endif
#if !defined(XPAR_PS7_DDR_0_S_AXI_BASEADDR) || \
    !defined(XPAR_PS7_DDR_0_S_AXI_HIGHADDR)
#error "Missing PS7 DDR addresses in xparameters.h"
#endif
#if !defined(XPAR_PS7_UART_0_BASEADDR) || !defined(STDIN_BASEADDRESS)
#error "PS UART0 must be enabled and configured for stdin in the BSP"
#endif

#define CAP_BASE      XPAR_CAMERA_CAPTURE_BD_0_BASEADDR
#define GEN_BASE      XPAR_CAMERA_TEST_GEN_BD_0_BASEADDR

/* Software-reserved region! Keep the ELF, stacks and heap elsewhere.
 * The current HP0 Address Editor maps 0x00000000..0x1FFFFFFF. */
#ifndef ARAGATZ_TEST_DDR_BASE
#define ARAGATZ_TEST_DDR_BASE 0x10000000U
#endif
#ifndef ARAGATZ_TEST_DDR_SIZE
#define ARAGATZ_TEST_DDR_SIZE 0x00100000U
#endif
#define ARAGATZ_HP0_HIGHADDR  0x1FFFFFFFU
#define ARAGATZ_GUARD_BYTES   256U
#define ARAGATZ_FRAME_WORDS   96U
#define ARAGATZ_FRAME_BYTES   768U
#define ARAGATZ_PREAMBLE_BYTES 128U
#define ARAGATZ_HEADER_ID     0xC0DEC035U
#define ARAGATZ_AUX0          0x11223344U
#define ARAGATZ_AUX1          0x55667788U
#define ARAGATZ_TRIGGER_MAGIC 0x54524752U
#define ARAGATZ_IP_VERSION    0x00010000U

/* Test policy; these are not peripheral addresses. */
#ifndef ARAGATZ_REPEAT_RUNS
#define ARAGATZ_REPEAT_RUNS 100U
#endif
#ifndef ARAGATZ_ENDURANCE_RUNS
#define ARAGATZ_ENDURANCE_RUNS 1000U
#endif
#define ARAGATZ_PL_CLOCK_HZ 200000000U

#define ARAGATZ_SENTINEL(i) (0xA5A50000U ^ (uint32_t)(i))

/* Capture register offsets. */
enum {
    CAP_COMMAND = 0x000, CAP_STATUS = 0x004, CAP_ERRORS = 0x008,
    CAP_TRIGGER_ENABLE = 0x00C, CAP_N_FRAMES = 0x010,
    CAP_N_PREV = 0x014, CAP_N_FRAGMENTS = 0x018,
    CAP_MEM_ADDR = 0x01C, CAP_HEADER_ID = 0x020, CAP_AUX0 = 0x024,
    CAP_INT_PERIOD = 0x044, CAP_INT_CTRL = 0x048,
    CAP_LAST_ERROR_CODE = 0x050, CAP_LAST_ERROR_ADDR = 0x054,
    CAP_FIFO_LEVEL = 0x058, CAP_FRAMES_CAPTURED = 0x05C,
    CAP_FRAMES_SENT = 0x060, CAP_FRAMES_COMMITTED = 0x064,
    CAP_FRAGMENTS_TOTAL = 0x068, CAP_TRIG_ACCEPTED = 0x06C,
    CAP_TRIG_MISSED = 0x070, CAP_FIFO_OVERFLOWS = 0x074,
    CAP_AXI_ERRORS = 0x078, CAP_CURRENT_ADDR = 0x07C,
    CAP_LAST_TIMESTAMP_LO = 0x080, CAP_LAST_TIMESTAMP_HI = 0x084,
    CAP_LAST_TRIG_DATA_LO = 0x088,
    CAP_LAST_TRIG_DATA_HI = 0x08C, CAP_LAST_TRIG_SOURCE = 0x090,
    CAP_LAST_ADDR = 0x094, CAP_LAST_FRAMES = 0x098,
    CAP_LAST_BYTES = 0x09C, CAP_LAST_STATUS = 0x0A0,
    CAP_RUN_FRAGMENTS = 0x0A4,
    CAP_TRIG_ACCEPTED_HI = 0x0A8, CAP_TRIG_MISSED_HI = 0x0AC,
    CAP_VERSION = 0x0B0,
    CAP_BUFFER_BYTES = 0x0B4
};

enum {
    CAP_CMD_START = 1U, CAP_CMD_STOP = 2U,
    CAP_CMD_CLEAR_ERROR = 4U, CAP_CMD_CLEAR_STATS = 8U,
    CAP_STATE_IDLE = 0U, CAP_STATE_FILL = 1U,
    CAP_STATE_WAIT = 2U, CAP_STATE_XFER = 3U, CAP_STATE_AFTER = 4U,
    CAP_STATE_ERROR = 5U, CAP_STATE_END = 6U
};

/* Generator register offsets. */
enum {
    GEN_COMMAND = 0x000, GEN_PAUSE = 0x004, GEN_MODE = 0x008,
    GEN_FRAME_WORDS = 0x00C, GEN_RATE_NUM = 0x010,
    GEN_RATE_DEN = 0x014, GEN_CONSTANT_LO = 0x018,
    GEN_CONSTANT_HI = 0x01C, GEN_TRIG_MODE = 0x020,
    GEN_TRIG_FIRST = 0x024, GEN_TRIG_PERIOD = 0x028,
    GEN_TRIG_WORD = 0x02C, GEN_EXT_WIDTH = 0x030, GEN_STATUS = 0x034,
    GEN_FRAME_INDEX = 0x038, GEN_WORD_INDEX = 0x03C,
    GEN_WORDS_SENT = 0x040, GEN_FRAMES_SENT = 0x044,
    GEN_ALG_COUNT = 0x048, GEN_EXT_COUNT = 0x04C,
    GEN_LAST_TRIG_FRAME = 0x050,
    GEN_TIMESTAMP_LO = 0x054, GEN_TIMESTAMP_HI = 0x058,
    GEN_VERSION = 0x05C
};
enum {
    GEN_CMD_START = 1U, GEN_CMD_STOP = 2U, GEN_CMD_CLEAR_STATS = 4U
};

static inline uint32_t cap_rd(uint32_t off) {
    return Xil_In32((UINTPTR)CAP_BASE + off);
}
static inline void cap_wr(uint32_t off, uint32_t value) {
    Xil_Out32((UINTPTR)CAP_BASE + off, value);
}
static inline uint32_t gen_rd(uint32_t off) {
    return Xil_In32((UINTPTR)GEN_BASE + off);
}
static inline void gen_wr(uint32_t off, uint32_t value) {
    Xil_Out32((UINTPTR)GEN_BASE + off, value);
}
static inline uint32_t ddr_rd(uint32_t off) {
    return Xil_In32((UINTPTR)ARAGATZ_TEST_DDR_BASE + off);
}
static inline void ddr_wr(uint32_t off, uint32_t value) {
    Xil_Out32((UINTPTR)ARAGATZ_TEST_DDR_BASE + off, value);
}

#endif /* ARAGATZ_HW_H */
