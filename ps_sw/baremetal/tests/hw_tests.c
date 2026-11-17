/* Real-hardware, self-checking tests for Aragatz PS / PL / DDR.
 * No Linux, no DMA, and no assumptions about fixed peripheral base addresses.
 * The DDR test window MUST be excluded from the ELF linker's regions. */
#include <stdint.h>
#include "aragatz_hw.h"
#include "hw_tests.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "sleep.h"

#define MAX_DIAGNOSTICS 12U
#define STATUS_TIMEOUT_MS 2000U
#define TRIG_WORD 47U
#define TRIG_FIRST_NORMAL 128U
#define TRIG_PERIOD_MULTI 128U
#define POST_FRAMES 3U

static unsigned errors;
static unsigned comparisons;

static void expect32(const char *what, uint32_t got, uint32_t expected)
{
    comparisons++;
    if (got != expected) {
        if (errors < MAX_DIAGNOSTICS)
            xil_printf("FAIL %s got=%08x expected=%08x\r\n",
                       what, (unsigned)got, (unsigned)expected);
        errors++;
    }
}

static void check_ddr64(uint32_t byte_off, uint32_t hi, uint32_t lo)
{
    uint32_t actual_lo = ddr_rd(byte_off);
    uint32_t actual_hi = ddr_rd(byte_off + 4U);
    comparisons++;
    if (actual_lo != lo || actual_hi != hi) {
        if (errors < MAX_DIAGNOSTICS)
            xil_printf("FAIL DDR +%08x got=%08x_%08x exp=%08x_%08x\r\n",
                       (unsigned)byte_off, (unsigned)actual_hi,
                       (unsigned)actual_lo, (unsigned)hi, (unsigned)lo);
        errors++;
    }
}

static int board_safety_check(void)
{
    uint64_t first = (uint64_t)ARAGATZ_TEST_DDR_BASE;
    uint64_t last = first + (uint64_t)ARAGATZ_TEST_DDR_SIZE - 1U;
    volatile uint32_t stack_probe = 0;
    uint64_t sp = (uint64_t)(UINTPTR)&stack_probe;

    if (last > XPAR_PS7_DDR_0_S_AXI_HIGHADDR ||
        first < XPAR_PS7_DDR_0_S_AXI_BASEADDR ||
        last > ARAGATZ_HP0_HIGHADDR ||
        (first & 127U) != 0U) {
        xil_printf("FAIL: DDR buffer is outside PS DDR / HP0 window\r\n");
        return -1;
    }
    if (sp >= first && sp <= last) {
        xil_printf("FAIL: application stack overlaps reserved DDR buffer\r\n");
        return -1;
    }
    /* The ELF code, data, heap and every stack must also lie outside
     * this region; see README and the application's lscript.ld. */
    return 0;
}

static int prepare_idle(void)
{
    uint32_t state;

    gen_wr(GEN_COMMAND, GEN_CMD_STOP);
    usleep(200U);
    state = cap_rd(CAP_STATUS) & 15U;
    if (state == CAP_STATE_END) {
        cap_wr(CAP_COMMAND, CAP_CMD_STOP);
        usleep(200U);
        state = cap_rd(CAP_STATUS) & 15U;
    }
    if (state != CAP_STATE_IDLE) {
        xil_printf("FAIL: Capture is not IDLE: status=%08x errors=%08x\r\n",
                   (unsigned)cap_rd(CAP_STATUS), (unsigned)cap_rd(CAP_ERRORS));
        xil_printf("Reset/reprogram PL before repeating this test.\r\n");
        return -1;
    }
    if (cap_rd(CAP_ERRORS) != 0U) {
        cap_wr(CAP_COMMAND, CAP_CMD_CLEAR_ERROR);
        usleep(200U);
        if (cap_rd(CAP_ERRORS) != 0U) {
            xil_printf("FAIL: Capture errors do not clear\r\n");
            return -1;
        }
    }
    cap_wr(CAP_COMMAND, CAP_CMD_CLEAR_STATS);
    gen_wr(GEN_COMMAND, GEN_CMD_CLEAR_STATS);
    usleep(200U);
    return 0;
}

static void program_generator(uint32_t first, uint32_t period)
{
    gen_wr(GEN_PAUSE, 0U);
    gen_wr(GEN_MODE, 0U);                 /* COUNTER */
    gen_wr(GEN_FRAME_WORDS, ARAGATZ_FRAME_WORDS);
    gen_wr(GEN_RATE_NUM, 24U);
    gen_wr(GEN_RATE_DEN, 125U);
    gen_wr(GEN_TRIG_MODE, 1U);            /* algorithmic */
    gen_wr(GEN_TRIG_FIRST, first);
    gen_wr(GEN_TRIG_PERIOD, period);
    gen_wr(GEN_TRIG_WORD, TRIG_WORD);
}

static void program_capture(uint32_t frames, uint32_t prev,
                            uint32_t fragments)
{
    unsigned i;
    cap_wr(CAP_TRIGGER_ENABLE, 1U);
    cap_wr(CAP_N_FRAMES, frames);
    cap_wr(CAP_N_PREV, prev);
    cap_wr(CAP_N_FRAGMENTS, fragments);
    cap_wr(CAP_MEM_ADDR, ARAGATZ_TEST_DDR_BASE);
    cap_wr(CAP_HEADER_ID, ARAGATZ_HEADER_ID);
    cap_wr(CAP_AUX0, ARAGATZ_AUX0);
    cap_wr(CAP_AUX0 + 4U, ARAGATZ_AUX1);
    for (i = 2U; i < 8U; ++i)
        cap_wr(CAP_AUX0 + 4U * i, 0U);
    cap_wr(CAP_INT_PERIOD, 0U);
    cap_wr(CAP_INT_CTRL, 0U);
    cap_wr(CAP_BUFFER_BYTES, ARAGATZ_TEST_DDR_SIZE);
}

static void fill_ddr(uint32_t used_bytes)
{
    uint32_t i;
    for (i = 0U; i < used_bytes / 4U; ++i)
        ddr_wr(4U * i, ARAGATZ_SENTINEL(i));
    Xil_DCacheFlushRange((INTPTR)ARAGATZ_TEST_DDR_BASE,
                         (uint32_t)used_bytes);
}

static void verify_ddr(uint32_t first_trigger, uint32_t period,
                       uint32_t prev, uint32_t frames,
                       uint32_t fragments, uint32_t used_bytes)
{
    uint32_t f, i, w, b;
    uint32_t frag_bytes = ARAGATZ_PREAMBLE_BYTES +
                          ARAGATZ_FRAME_BYTES * frames;
    uint32_t total = frag_bytes * fragments;

    Xil_DCacheInvalidateRange((INTPTR)ARAGATZ_TEST_DDR_BASE,
                              (uint32_t)used_bytes);
    for (f = 0U; f < fragments; ++f) {
        uint32_t base = f * frag_bytes;
        uint32_t trigger_frame = first_trigger + f * period;
        uint32_t first_saved = trigger_frame - prev;
        uint32_t timestamp_lo = ddr_rd(base + 8U);
        uint32_t timestamp_hi = ddr_rd(base + 12U);

        check_ddr64(base, f, ARAGATZ_HEADER_ID);
        /* Event timestamps are non-deterministic, but must be overwritten. */
        comparisons++;
        if (timestamp_lo == ARAGATZ_SENTINEL((base + 8U) / 4U) &&
            timestamp_hi == ARAGATZ_SENTINEL((base + 12U) / 4U)) {
            if (errors < MAX_DIAGNOSTICS)
                xil_printf("FAIL timestamp not written, fragment %d\r\n", f);
            errors++;
        }
        check_ddr64(base + 16U, ARAGATZ_TRIGGER_MAGIC, trigger_frame);
        check_ddr64(base + 24U, ARAGATZ_AUX1, ARAGATZ_AUX0);
        for (w = 4U; w < 7U; ++w)
            check_ddr64(base + 8U * w, 0U, 0U);
        for (w = 7U; w < 16U; ++w)
            check_ddr64(base + 8U * w, 0x12345678U, 0x12345678U);

        for (i = 0U; i < frames; ++i) {
            for (w = 0U; w < ARAGATZ_FRAME_WORDS; ++w) {
                b = base + ARAGATZ_PREAMBLE_BYTES +
                    i * ARAGATZ_FRAME_BYTES + w * 8U;
                /* COUNTER: {frame_index, 16'd0, word_index}. */
                check_ddr64(b, first_saved + i, w);
            }
        }
    }
    for (i = total / 4U; i < used_bytes / 4U; ++i)
        expect32("DDR guard", ddr_rd(4U * i), ARAGATZ_SENTINEL(i));
}

static void dump_regs(void)
{
    xil_printf("CAP status=%08x errors=%08x last=%08x\r\n",
               (unsigned)cap_rd(CAP_STATUS), (unsigned)cap_rd(CAP_ERRORS),
               (unsigned)cap_rd(CAP_LAST_STATUS));
    xil_printf("CAP last_addr=%08x frames=%d bytes=%d fragments=%d\r\n",
               (unsigned)cap_rd(CAP_LAST_ADDR),
               (unsigned)cap_rd(CAP_LAST_FRAMES),
               (unsigned)cap_rd(CAP_LAST_BYTES),
               (unsigned)cap_rd(CAP_RUN_FRAGMENTS));
    xil_printf("CAP accepted=%d missed=%d GEN frames=%d alg=%d\r\n",
               (unsigned)cap_rd(CAP_TRIG_ACCEPTED),
               (unsigned)cap_rd(CAP_TRIG_MISSED),
               (unsigned)gen_rd(GEN_FRAMES_SENT),
               (unsigned)gen_rd(GEN_ALG_COUNT));
}

static int run_scenario(const char *name, uint32_t prev,
                        uint32_t post, uint32_t nfragments,
                        uint32_t first_trigger, uint32_t period)
{
    uint32_t i, state, status = 0U;
    uint32_t frames = prev + post;
    uint64_t frag_bytes = (uint64_t)ARAGATZ_PREAMBLE_BYTES +
                          (uint64_t)frames * ARAGATZ_FRAME_BYTES;
    uint64_t total_bytes = frag_bytes * nfragments;
    uint32_t used;
    int rc;

    xil_printf("\r\n[RUN] %s: prev=%d post=%d fragments=%d\r\n",
               name, (unsigned)prev, (unsigned)post,
               (unsigned)nfragments);
    if (first_trigger < prev || post == 0U || frames < 2U ||
        frames > 131072U || nfragments == 0U ||
        total_bytes + ARAGATZ_GUARD_BYTES > ARAGATZ_TEST_DDR_SIZE ||
        (nfragments > 1U && period == 0U)) {
        xil_printf("FAIL: invalid test configuration / buffer too small\r\n");
        return 1;
    }
    if (board_safety_check() != 0 || prepare_idle() != 0)
        return 1;

    errors = 0U;
    comparisons = 0U;
    used = (uint32_t)total_bytes + ARAGATZ_GUARD_BYTES;
    fill_ddr(used);
    program_generator(first_trigger, period);
    program_capture(frames, prev, nfragments);
    expect32("GEN frame_words", gen_rd(GEN_FRAME_WORDS), 96U);
    expect32("CAP DDR base", cap_rd(CAP_MEM_ADDR), ARAGATZ_TEST_DDR_BASE);
    expect32("CAP prev", cap_rd(CAP_N_PREV), prev);
    expect32("CAP frames", cap_rd(CAP_N_FRAMES), frames);
    if (errors != 0U) return 1;

    cap_wr(CAP_COMMAND, CAP_CMD_START);
    usleep(1000U);                 /* wait for guard validation / XPM FIFO */
    state = cap_rd(CAP_STATUS) & 15U;
    if (state != CAP_STATE_FILL) {
        xil_printf("FAIL: Capture did not enter FILL; status=%08x\r\n",
                   (unsigned)cap_rd(CAP_STATUS));
        return 1;
    }
    gen_wr(GEN_COMMAND, GEN_CMD_START);

    for (i = 0U; i < STATUS_TIMEOUT_MS; ++i) {
        status = cap_rd(CAP_STATUS);
        state = status & 15U;
        if (state == CAP_STATE_END || state == CAP_STATE_ERROR)
            break;
        usleep(1000U);
    }
    gen_wr(GEN_COMMAND, GEN_CMD_STOP);
    usleep(200U);
    dump_regs();
    if ((status & 15U) != CAP_STATE_END) {
        xil_printf("FAIL: Capture timeout or ERROR, status=%08x\r\n",
                   (unsigned)status);
        return 1;
    }

    expect32("CAP error flags", cap_rd(CAP_ERRORS), 0U);
    expect32("CAP run fragments", cap_rd(CAP_RUN_FRAGMENTS), nfragments);
    expect32("CAP last addr", cap_rd(CAP_LAST_ADDR),
             ARAGATZ_TEST_DDR_BASE + (uint32_t)(frag_bytes * (nfragments - 1U)));
    expect32("CAP last frames", cap_rd(CAP_LAST_FRAMES), frames);
    expect32("CAP last bytes", cap_rd(CAP_LAST_BYTES), (uint32_t)frag_bytes);
    expect32("CAP last status", cap_rd(CAP_LAST_STATUS), 1U);
    expect32("CAP last trigger source", cap_rd(CAP_LAST_TRIG_SOURCE), 1U);
    expect32("CAP last trigger data LO", cap_rd(CAP_LAST_TRIG_DATA_LO),
             first_trigger + (nfragments - 1U) * period);
    expect32("CAP last trigger data HI", cap_rd(CAP_LAST_TRIG_DATA_HI),
             ARAGATZ_TRIGGER_MAGIC);
    expect32("CAP alg accepted", cap_rd(CAP_TRIG_ACCEPTED), nfragments);
    expect32("GEN alg count", gen_rd(GEN_ALG_COUNT), nfragments);
    /* A nonzero missed count is unexpected at the low nominal rate. */
    expect32("CAP alg missed", cap_rd(CAP_TRIG_MISSED), 0U);

    if (errors == 0U)
        verify_ddr(first_trigger, period, prev, frames, nfragments, used);
    rc = (errors == 0U) ? 0 : 1;
    xil_printf("[%s] %s: %d checks, %d errors\r\n",
               rc == 0 ? "PASS" : "FAIL", name,
               comparisons, errors);
    return rc;
}

int aragatz_test_registers(void)
{
    xil_printf("\r\n[RUN] AXI-Lite registers\r\n");
    if (board_safety_check() != 0) return 1;
    errors = 0U; comparisons = 0U;
    expect32("CAP version", cap_rd(CAP_VERSION), ARAGATZ_IP_VERSION);
    expect32("GEN version", gen_rd(GEN_VERSION), ARAGATZ_IP_VERSION);
    if (errors != 0U || prepare_idle() != 0) return 1;
    cap_wr(CAP_HEADER_ID, ARAGATZ_HEADER_ID);
    gen_wr(GEN_FRAME_WORDS, ARAGATZ_FRAME_WORDS);
    gen_wr(GEN_RATE_NUM, 24U);
    gen_wr(GEN_RATE_DEN, 125U);
    expect32("CAP header readback", cap_rd(CAP_HEADER_ID), ARAGATZ_HEADER_ID);
    expect32("GEN words readback", gen_rd(GEN_FRAME_WORDS), ARAGATZ_FRAME_WORDS);
    expect32("GEN rate num", gen_rd(GEN_RATE_NUM), 24U);
    expect32("GEN rate den", gen_rd(GEN_RATE_DEN), 125U);
    xil_printf("[%s] AXI-Lite: %d checks\r\n",
               errors == 0U ? "PASS" : "FAIL", comparisons);
    return errors != 0U;
}

int aragatz_test_smoke(void)
{
    return run_scenario("smoke", 0U, 3U, 1U, 32U, 0U);
}

int aragatz_test_pretrigger(void)
{
    static const uint32_t prevs[] = {1U, 4U, 16U, 64U};
    unsigned k;
    int fails = 0;
    for (k = 0U; k < sizeof(prevs) / sizeof(prevs[0]); ++k) {
        fails += run_scenario("pretrigger", prevs[k], POST_FRAMES,
                              1U, TRIG_FIRST_NORMAL, 0U);
        if (fails != 0) break;  /* preserve first failing state */
    }
    return fails != 0;
}

int aragatz_test_fragments(void)
{
    static const uint32_t fragment_counts[] = {2U, 4U, 16U};
    unsigned k;
    int fails = 0;
    for (k = 0U; k < sizeof(fragment_counts) / sizeof(fragment_counts[0]); ++k) {
        fails += run_scenario("multi-fragment", 4U, 4U,
                              fragment_counts[k], TRIG_FIRST_NORMAL,
                              TRIG_PERIOD_MULTI);
        if (fails != 0) break;
    }
    return fails != 0;
}

int aragatz_test_all(void)
{
    int failures = 0;
    xil_printf("\r\n=== ARAGATZ FULL HARDWARE REGRESSION ===\r\n");
    if (aragatz_test_registers() != 0) failures++;
    if (aragatz_test_smoke() != 0) failures++;
    if (aragatz_test_pretrigger() != 0) failures++;
    if (aragatz_test_fragments() != 0) failures++;
    xil_printf("=== FULL REGRESSION %s (%d failed groups) ===\r\n",
               failures == 0 ? "PASS" : "FAIL", failures);
    return failures != 0;
}
