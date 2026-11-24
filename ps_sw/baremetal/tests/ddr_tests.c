/* Destructive CPU write/read tests of the software-reserved DDR window.
 * No access outside ARAGATZ_TEST_DDR_BASE / ARAGATZ_TEST_DDR_SIZE.
 * This does not measure total DDR capacity or test the PL / HP0 data path. */
#include <stdint.h>
#include "aragatz_hw.h"
#include "hw_tests.h"
#include "xil_cache.h"
#include "xil_printf.h"

#define DDR_CACHE_LINE_BYTES 32U   /* Cortex-A9 standalone BSP */
#define DDR_MAX_DIAGNOSTICS 12U
#define DDR_ALIAS_POINTS 32U

static unsigned ddr_errors;
static unsigned ddr_checks;

static int validate_cpu_window(void)
{
    uint64_t base = (uint64_t)ARAGATZ_TEST_DDR_BASE;
    uint64_t bytes = (uint64_t)ARAGATZ_TEST_DDR_SIZE;

    /* Full cache lines keep maintenance inside the reserved allocation.
     * Four lines are needed for four distinct walking-pattern locations. */
    if (base > UINT32_MAX || bytes > UINT32_MAX ||
        bytes < 4U * DDR_CACHE_LINE_BYTES ||
        (base & (DDR_CACHE_LINE_BYTES - 1U)) != 0U ||
        (bytes & (DDR_CACHE_LINE_BYTES - 1U)) != 0U ||
        base + bytes > (UINT64_C(1) << 32) ||
        base < (uint64_t)XPAR_PS7_DDR_0_S_AXI_BASEADDR ||
        base + bytes - 1U > (uint64_t)XPAR_PS7_DDR_0_S_AXI_HIGHADDR) {
        xil_printf("FAIL DDR: invalid/alignment/out-of-BSP reserved window\r\n");
        return 1;
    }
    return 0;
}

static void sync_written_range(uint32_t off, uint32_t bytes)
{
    INTPTR addr = (INTPTR)((UINTPTR)ARAGATZ_TEST_DDR_BASE + off);
    /* Commit CPU writes to memory, then force subsequent reads to refill.
     * Both arguments describe complete lines inside the validated window. */
    Xil_DCacheFlushRange(addr, bytes);
    Xil_DCacheInvalidateRange(addr, bytes);
}

static void check_word(uint32_t off, uint32_t expected)
{
    uint32_t got = ddr_rd(off);
    ddr_checks++;
    if (got != expected) {
        if (ddr_errors < DDR_MAX_DIAGNOSTICS)
            xil_printf("FAIL DDR addr=%08x got=%08x expected=%08x\r\n",
                       (unsigned)((UINTPTR)ARAGATZ_TEST_DDR_BASE + off),
                       (unsigned)got, (unsigned)expected);
        ddr_errors++;
    }
}

static uint32_t full_pattern(unsigned pattern, uint32_t off)
{
    uint32_t address = (uint32_t)((UINTPTR)ARAGATZ_TEST_DDR_BASE + off);
    switch (pattern) {
    case 0U: return 0U;
    case 1U: return UINT32_MAX;
    case 2U: return UINT32_C(0x55555555);
    case 3U: return UINT32_C(0xAAAAAAAA);
    case 4U: return address;
    default: return ~address;
    }
}

static int run_full_patterns(void)
{
    static const char *const names[] = {
        "all zero", "all one", "55555555", "AAAAAAAA",
        "address", "inverse address"
    };
    uint32_t off;
    unsigned pattern;
    const uint32_t bytes = (uint32_t)ARAGATZ_TEST_DDR_SIZE;

    for (pattern = 0U; pattern < 6U; ++pattern) {
        unsigned before = ddr_checks;
        xil_printf("[RUN] DDR full window: %s\r\n", names[pattern]);
        for (off = 0U; off < bytes; off += 4U)
            ddr_wr(off, full_pattern(pattern, off));
        sync_written_range(0U, bytes);
        for (off = 0U; off < bytes; off += 4U)
            check_word(off, full_pattern(pattern, off));
        if (ddr_errors != 0U)
            return 1;  /* No subsequent writes: preserve failing phase. */
        xil_printf("[PASS] DDR %s: %d checks\r\n", names[pattern],
                   (unsigned)(ddr_checks - before));
    }
    return 0;
}

static uint32_t alias_pattern(unsigned index, unsigned inverse)
{
    /* Distinct values at every simultaneously populated probe address. */
    uint32_t value = UINT32_C(0x13579BDF) ^
                     ((uint32_t)index * UINT32_C(0x01010101));
    return inverse != 0U ? ~value : value;
}

static int run_address_alias(void)
{
    uint32_t offsets[DDR_ALIAS_POINTS];
    uint32_t off;
    const uint32_t bytes = (uint32_t)ARAGATZ_TEST_DDR_SIZE;
    unsigned count = 1U, i, inverse;
    unsigned before = ddr_checks;

    offsets[0] = 0U;
    for (off = 4U; off < bytes; off <<= 1U) {
        if (count >= DDR_ALIAS_POINTS) {
            xil_printf("FAIL DDR: alias probe table overflow\r\n");
            return 1;
        }
        offsets[count++] = off;
        if (off > UINT32_MAX / 2U)
            break;
    }
    xil_printf("[RUN] DDR address alias: %d simultaneous probes\r\n", count);
    for (inverse = 0U; inverse < 2U; ++inverse) {
        /* Reverse the write order as well as the values on the second pass. */
        for (i = 0U; i < count; ++i) {
            unsigned index = inverse != 0U ? count - 1U - i : i;
            ddr_wr(offsets[index], alias_pattern(index, inverse));
        }
        sync_written_range(0U, bytes);
        for (i = 0U; i < count; ++i)
            check_word(offsets[i], alias_pattern(i, inverse));
        if (ddr_errors != 0U)
            return 1;
    }
    xil_printf("[PASS] DDR address alias: %d checks\r\n",
               (unsigned)(ddr_checks - before));
    return 0;
}

static uint32_t walking_pattern(unsigned bit, unsigned lane, unsigned inverse)
{
    /* Every bit walks through each of the eight 32-bit words in a line.
     * Rotate by lane so neighboring words contain different values. */
    uint32_t value = UINT32_C(1) << ((bit + lane) & 31U);
    return inverse != 0U ? ~value : value;
}

static int run_walking_patterns(void)
{
    uint32_t locations[4];
    const uint32_t bytes = (uint32_t)ARAGATZ_TEST_DDR_SIZE;
    unsigned inverse, point, bit, lane;

    locations[0] = 0U;
    locations[1] = (bytes / 4U) & ~(DDR_CACHE_LINE_BYTES - 1U);
    locations[2] = (bytes / 2U) & ~(DDR_CACHE_LINE_BYTES - 1U);
    locations[3] = bytes - DDR_CACHE_LINE_BYTES;
    for (inverse = 0U; inverse < 2U; ++inverse) {
        unsigned before = ddr_checks;
        xil_printf("[RUN] DDR walking %s: four cache lines\r\n",
                   inverse != 0U ? "zero" : "one");
        for (point = 0U; point < 4U; ++point) {
            for (bit = 0U; bit < 32U; ++bit) {
                for (lane = 0U; lane < DDR_CACHE_LINE_BYTES / 4U; ++lane)
                    ddr_wr(locations[point] + 4U * lane,
                           walking_pattern(bit, lane, inverse));
                sync_written_range(locations[point], DDR_CACHE_LINE_BYTES);
                for (lane = 0U; lane < DDR_CACHE_LINE_BYTES / 4U; ++lane)
                    check_word(locations[point] + 4U * lane,
                               walking_pattern(bit, lane, inverse));
                if (ddr_errors != 0U)
                    return 1;
            }
        }
        xil_printf("[PASS] DDR walking %s: %d checks\r\n",
                   inverse != 0U ? "zero" : "one",
                   (unsigned)(ddr_checks - before));
    }
    return 0;
}

int aragatz_test_ddr(void)
{
    int failed;
    uint64_t last;

    xil_printf("\r\n=== CPU DDR RESERVED-WINDOW TEST ===\r\n");
    if (aragatz_check_buffer() != 0 || aragatz_prepare_idle() != 0 ||
        validate_cpu_window() != 0) {
        xil_printf("[FAIL] CPU DDR prerequisites; no test patterns written\r\n");
        return 1;
    }
    last = (uint64_t)ARAGATZ_TEST_DDR_BASE + ARAGATZ_TEST_DDR_SIZE - 1U;
    xil_printf("DDR range=%08x..%08x bytes=%d; CPU path only\r\n",
               (unsigned)ARAGATZ_TEST_DDR_BASE, (unsigned)last,
               (unsigned)ARAGATZ_TEST_DDR_SIZE);
    xil_printf("INFO: this replaces the reserved buffer contents.\r\n");
    ddr_errors = 0U;
    ddr_checks = 0U;
    failed = run_full_patterns();
    if (failed == 0)
        failed = run_address_alias();
    if (failed == 0)
        failed = run_walking_patterns();
    xil_printf("[%s] CPU DDR reserved window: %d checks, %d errors\r\n",
               failed == 0 ? "PASS" : "FAIL", ddr_checks, ddr_errors);
    return failed;
}
