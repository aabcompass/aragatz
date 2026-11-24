/* Aragatz V3: standalone, self-checking PS / PL / reserved DDR tests.
 * Keep the ELF, all stacks and heap outside the reserved DDR window.
 * Host checks validate the software; hardware PASS requires the real board. */
#include <stdint.h>
#include "aragatz_hw.h"
#include "hw_tests.h"
#include "test_model.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "sleep.h"

#define MAX_DIAGNOSTICS 12U
#define TIMEOUT_US 2000000U
#define NORMAL_WORD 47U
#define NORMAL_FIRST 128U
#define NORMAL_PERIOD 128U
#define STATUS_BUSY (1U << 4)
#define STATUS_AXI_BUSY (1U << 8)

struct scenario {
    const char *name;
    uint32_t prev, post, fragments, first, period, word, num, den;
    uint32_t mode, constant_lo, constant_hi, buffer_bytes;
};

static unsigned errors;
static unsigned comparisons;

static struct scenario scenario_make(const char *name, uint32_t prev,
                                      uint32_t post, uint32_t fragments,
                                      uint32_t first, uint32_t period)
{
    struct scenario s = {
        name, prev, post, fragments, first, period, NORMAL_WORD, 24U, 125U,
        0U, 0U, 0U, ARAGATZ_TEST_DDR_SIZE
    };
    return s;
}

static void begin_checks(void)
{
    errors = 0U;
    comparisons = 0U;
}

static void expect32(const char *what, uint32_t got, uint32_t expected)
{
    ++comparisons;
    if (got != expected) {
        if (errors < MAX_DIAGNOSTICS)
            xil_printf("FAIL %s got=%08x expected=%08x\r\n", what,
                       (unsigned)got, (unsigned)expected);
        ++errors;
    }
}

static void expect_true(const char *what, int condition)
{
    ++comparisons;
    if (!condition) {
        if (errors < MAX_DIAGNOSTICS)
            xil_printf("FAIL %s\r\n", what);
        ++errors;
    }
}

static int finish_checks(const char *name, int quiet)
{
    if (!quiet || errors != 0U)
        xil_printf("[%s] %s: %d checks, %d errors\r\n",
                   errors == 0U ? "PASS" : "FAIL", name, comparisons, errors);
    return errors != 0U;
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
    xil_printf("CAP accepted=%d missed=%d fifo_overflow=%d axi_errors=%d\r\n",
               (unsigned)cap_rd(CAP_TRIG_ACCEPTED),
               (unsigned)cap_rd(CAP_TRIG_MISSED),
               (unsigned)cap_rd(CAP_FIFO_OVERFLOWS),
               (unsigned)cap_rd(CAP_AXI_ERRORS));
    xil_printf("GEN status=%08x frames=%d word=%d alg=%d ext=%d\r\n",
               (unsigned)gen_rd(GEN_STATUS),
               (unsigned)gen_rd(GEN_FRAMES_SENT),
               (unsigned)gen_rd(GEN_WORD_INDEX),
               (unsigned)gen_rd(GEN_ALG_COUNT),
               (unsigned)gen_rd(GEN_EXT_COUNT));
}

/* Polling budget is expressed in delay microseconds, not a throughput timer. */
static int wait_state(uint32_t wanted, uint32_t step_us, uint32_t budget_us)
{
    uint32_t elapsed = 0U;
    for (;;) {
        uint32_t status = cap_rd(CAP_STATUS);
        uint32_t state = status & 15U;
        if (state == wanted)
            return 0;
        if (wanted != CAP_STATE_ERROR &&
            (state == CAP_STATE_ERROR || cap_rd(CAP_ERRORS) != 0U))
            return 1;
        if (elapsed >= budget_us)
            return 1;
        usleep(step_us);
        elapsed += step_us;
    }
}

static void generator_stop(void)
{
    gen_wr(GEN_COMMAND, GEN_CMD_STOP);
    usleep(200U);  /* Command, counters and registered BD outputs settle. */
}

/* Preserve the failing diagnostics before best-effort bounded cleanup.
 * Never stop the source first while Capture may need a final TLAST. */
static int abort_case(const char *why)
{
    uint32_t i, state;
    xil_printf("FAIL: %s\r\n", why);
    dump_regs();
    gen_wr(GEN_PAUSE, 0U);
    for (i = 0U; i < 20000U; ++i) {
        state = cap_rd(CAP_STATUS) & 15U;
        if (state == CAP_STATE_IDLE || state == CAP_STATE_END ||
            state == CAP_STATE_ERROR)
            break;
        if (state == CAP_STATE_FILL || state == CAP_STATE_WAIT ||
            state == CAP_STATE_XFER) {
            cap_wr(CAP_COMMAND, CAP_CMD_STOP);
            break;
        }
        usleep(10U); /* AFTER does not latch a new STOP. */
    }
    for (i = 0U; i < 20000U; ++i) {
        state = cap_rd(CAP_STATUS) & 15U;
        if (state == CAP_STATE_IDLE || state == CAP_STATE_END ||
            state == CAP_STATE_ERROR)
            break;
        usleep(10U);
    }
    state = cap_rd(CAP_STATUS) & 15U;
    if (state == CAP_STATE_IDLE || state == CAP_STATE_END ||
        state == CAP_STATE_ERROR) {
        generator_stop();
        xil_printf("Stopped after failure; CAP state=%d. Error flags retained.\r\n",
                   (unsigned)state);
    } else {
        xil_printf("Capture did not quiesce. Reset/reprogram PL before retry.\r\n");
    }
    return 1;
}

int aragatz_check_buffer(void)
{
    uint64_t first = (uint64_t)ARAGATZ_TEST_DDR_BASE;
    uint64_t bytes = (uint64_t)ARAGATZ_TEST_DDR_SIZE;
    uint64_t end = first + bytes;
    volatile uint32_t stack_probe = 0U;
    uint64_t sp = (uint64_t)(UINTPTR)&stack_probe;
    if (bytes < 4096U || bytes > UINT32_MAX || first > UINT32_MAX ||
        end > (UINT64_C(1) << 32) ||
        first < XPAR_PS7_DDR_0_S_AXI_BASEADDR ||
        end - 1U > XPAR_PS7_DDR_0_S_AXI_HIGHADDR ||
        end - 1U > ARAGATZ_HP0_HIGHADDR ||
        (first & 127U) != 0U || (bytes & 127U) != 0U) {
        xil_printf("FAIL: reserved DDR buffer outside configured/aligned window\r\n");
        return 1;
    }
    if (sp >= first && sp < end) {
        xil_printf("FAIL: stack overlaps test buffer\r\n");
        return 1;
    }
    /* This check does not prove that every ELF section/heap/other stack
     * is outside the buffer. The linker map must reserve it explicitly. */
    return 0;
}

int aragatz_prepare_idle(void)
{
    uint32_t state = cap_rd(CAP_STATUS) & 15U;
    if (state != CAP_STATE_IDLE && state != CAP_STATE_END) {
        xil_printf("FAIL: expected IDLE/END before a new test; state=%d\r\n",
                   (unsigned)state);
        dump_regs();
        return 1;
    }
    generator_stop();
    gen_wr(GEN_PAUSE, 0U);
    if (state == CAP_STATE_END) {
        cap_wr(CAP_COMMAND, CAP_CMD_STOP);
        if (wait_state(CAP_STATE_IDLE, 10U, 2000U) != 0)
            return 1;
    }
    if (cap_rd(CAP_ERRORS) != 0U) {
        xil_printf("FAIL: sticky errors retained; use reset/recovery first\r\n");
        return 1;
    }
    cap_wr(CAP_COMMAND, CAP_CMD_CLEAR_STATS);
    gen_wr(GEN_COMMAND, GEN_CMD_CLEAR_STATS);
    usleep(200U);
    return 0;
}

static void program_generator(const struct scenario *s)
{
    gen_wr(GEN_PAUSE, 0U);
    gen_wr(GEN_MODE, s->mode);
    gen_wr(GEN_FRAME_WORDS, ARAGATZ_FRAME_WORDS);
    gen_wr(GEN_RATE_NUM, s->num);
    gen_wr(GEN_RATE_DEN, s->den);
    gen_wr(GEN_CONSTANT_LO, s->constant_lo);
    gen_wr(GEN_CONSTANT_HI, s->constant_hi);
    gen_wr(GEN_TRIG_MODE, 1U);
    gen_wr(GEN_TRIG_FIRST, s->first);
    gen_wr(GEN_TRIG_PERIOD, s->period);
    gen_wr(GEN_TRIG_WORD, s->word);
    gen_wr(GEN_EXT_WIDTH, 4U);
}

static void program_capture(const struct scenario *s)
{
    unsigned i;
    cap_wr(CAP_TRIGGER_ENABLE, 1U);
    cap_wr(CAP_N_FRAMES, s->prev + s->post);
    cap_wr(CAP_N_PREV, s->prev);
    cap_wr(CAP_N_FRAGMENTS, s->fragments);
    cap_wr(CAP_MEM_ADDR, ARAGATZ_TEST_DDR_BASE);
    cap_wr(CAP_HEADER_ID, ARAGATZ_HEADER_ID);
    cap_wr(CAP_AUX0, ARAGATZ_AUX0);
    cap_wr(CAP_AUX0 + 4U, ARAGATZ_AUX1);
    for (i = 2U; i < 8U; ++i)
        cap_wr(CAP_AUX0 + 4U * i, 0U);
    cap_wr(CAP_INT_PERIOD, 0U);
    cap_wr(CAP_INT_CTRL, 0U);
    cap_wr(CAP_BUFFER_BYTES, s->buffer_bytes);
}

static int fill_ddr(uint32_t bytes)
{
    uint32_t i;
    if (bytes == 0U || bytes > ARAGATZ_TEST_DDR_SIZE || (bytes & 31U) != 0U) {
        xil_printf("FAIL: initialized range exceeds/alters reserved cache lines\r\n");
        return 1;
    }
    for (i = 0U; i < bytes / 4U; ++i)
        ddr_wr(i * 4U, ARAGATZ_SENTINEL(i));
    Xil_DCacheFlushRange((INTPTR)ARAGATZ_TEST_DDR_BASE, bytes);
    return 0;
}

static void check_ddr64(uint32_t off, uint32_t hi, uint32_t lo)
{
    uint32_t actual_lo = ddr_rd(off);
    uint32_t actual_hi = ddr_rd(off + 4U);
    ++comparisons;
    if (actual_lo != lo || actual_hi != hi) {
        if (errors < MAX_DIAGNOSTICS)
            xil_printf("FAIL DDR +%08x got=%08x_%08x exp=%08x_%08x\r\n",
                       (unsigned)off, (unsigned)actual_hi, (unsigned)actual_lo,
                       (unsigned)hi, (unsigned)lo);
        ++errors;
    }
}

static void verify_sentinel(uint32_t begin, uint32_t end)
{
    uint32_t off;
    for (off = begin; off < end; off += 4U)
        expect32("DDR guard/untouched", ddr_rd(off), ARAGATZ_SENTINEL(off / 4U));
}

static void verify_untouched(uint32_t bytes)
{
    Xil_DCacheInvalidateRange((INTPTR)ARAGATZ_TEST_DDR_BASE, bytes);
    verify_sentinel(0U, bytes);
}

/* frames may be less than the programmed goal in the partial STOP test.
 * All frames, every preamble field except an independently known timestamp,
 * and the complete initialized suffix are checked. */
static void verify_ddr(const struct scenario *s, uint32_t frames,
                       uint32_t fragments, uint32_t used)
{
    uint32_t f, i, w, off;
    uint32_t frag_bytes = ARAGATZ_PREAMBLE_BYTES + frames * ARAGATZ_FRAME_BYTES;
    uint32_t total = frag_bytes * fragments;
    uint64_t previous_ts = 0U;
    Xil_DCacheInvalidateRange((INTPTR)ARAGATZ_TEST_DDR_BASE, used);
    for (f = 0U; f < fragments; ++f) {
        uint32_t base = f * frag_bytes;
        uint32_t trigger_frame = s->first + f * s->period;
        uint32_t first_saved = trigger_frame - s->prev;
        uint32_t ts_lo = ddr_rd(base + 8U);
        uint32_t ts_hi = ddr_rd(base + 12U);
        uint64_t ts = ((uint64_t)ts_hi << 32) | ts_lo;
        check_ddr64(base, f, ARAGATZ_HEADER_ID);
        expect_true("timestamp overwritten",
                    ts_lo != ARAGATZ_SENTINEL((base + 8U) / 4U) ||
                    ts_hi != ARAGATZ_SENTINEL((base + 12U) / 4U));
        if (f != 0U)
            expect_true("fragment timestamps strictly increase", ts > previous_ts);
        previous_ts = ts;
        check_ddr64(base + 16U, ARAGATZ_TRIGGER_MAGIC, trigger_frame);
        check_ddr64(base + 24U, ARAGATZ_AUX1, ARAGATZ_AUX0);
        for (w = 4U; w < 7U; ++w)
            check_ddr64(base + w * 8U, 0U, 0U);
        for (w = 7U; w < 16U; ++w)
            check_ddr64(base + w * 8U, 0x12345678U, 0x12345678U);
        for (i = 0U; i < frames; ++i)
            for (w = 0U; w < ARAGATZ_FRAME_WORDS; ++w) {
                off = base + ARAGATZ_PREAMBLE_BYTES +
                      i * ARAGATZ_FRAME_BYTES + w * 8U;
                check_ddr64(off, s->mode == 0U ? first_saved + i : s->constant_hi,
                            s->mode == 0U ? w : s->constant_lo);
            }
        if (f + 1U == fragments) {
            expect32("last timestamp LO matches DDR",
                     cap_rd(CAP_LAST_TIMESTAMP_LO), ts_lo);
            expect32("last timestamp HI matches DDR",
                     cap_rd(CAP_LAST_TIMESTAMP_HI), ts_hi);
        }
    }
    verify_sentinel(total, used);
}

static void verify_clean_capture(uint32_t accepted)
{
    expect32("CAP errors", cap_rd(CAP_ERRORS), 0U);
    expect32("CAP accepted LO", cap_rd(CAP_TRIG_ACCEPTED), accepted);
    expect32("CAP accepted HI", cap_rd(CAP_TRIG_ACCEPTED_HI), 0U);
    expect32("CAP missed LO", cap_rd(CAP_TRIG_MISSED), 0U);
    expect32("CAP missed HI", cap_rd(CAP_TRIG_MISSED_HI), 0U);
    expect32("CAP FIFO overflow", cap_rd(CAP_FIFO_OVERFLOWS), 0U);
    expect32("CAP AXI errors", cap_rd(CAP_AXI_ERRORS), 0U);
    expect32("CAP AXI idle", cap_rd(CAP_STATUS) & STATUS_AXI_BUSY, 0U);
}

static void verify_generator(const struct scenario *s, uint32_t accepted, int quiet)
{
    uint32_t frames = gen_rd(GEN_FRAMES_SENT);
    uint32_t word = gen_rd(GEN_WORD_INDEX);
    uint32_t events = gen_rd(GEN_ALG_COUNT);
    uint32_t expected = ar_expected_events(s->first, s->period, s->word,
                                           frames, word);
    expect32("GEN stopped/flags", gen_rd(GEN_STATUS), 0U);
    expect32("GEN frame cursor", gen_rd(GEN_FRAME_INDEX), frames);
    expect_true("GEN word index in range", word < ARAGATZ_FRAME_WORDS);
    expect32("GEN words/cursor", gen_rd(GEN_WORDS_SENT),
             (uint32_t)((uint64_t)frames * ARAGATZ_FRAME_WORDS + word));
    expect32("GEN algorithmic schedule", events, expected);
    expect32("GEN external count", gen_rd(GEN_EXT_COUNT), 0U);
    expect_true("GEN triggers cover captured fragments", events >= accepted);
    if (events != 0U)
        expect32("GEN last trigger frame", gen_rd(GEN_LAST_TRIG_FRAME),
                 s->first + (events - 1U) * s->period);
    if (!quiet && events > accepted)
        xil_printf("INFO: GEN made %d extra scheduled trigger(s) before STOP\r\n",
                   (unsigned)(events - accepted));
}

static int start_capture(void)
{
    cap_wr(CAP_COMMAND, CAP_CMD_START);
    return wait_state(CAP_STATE_FILL, 10U, 10000U);
}

static int run_scenario(const struct scenario *s, int quiet)
{
    uint32_t frames, one, total, used;
    begin_checks();
    if (!quiet)
        xil_printf("\r\n[RUN] %s: prev=%d post=%d fragments=%d word=%d rate=%d/%d\r\n",
                   s->name, (unsigned)s->prev, (unsigned)s->post,
                   (unsigned)s->fragments, (unsigned)s->word,
                   (unsigned)s->num, (unsigned)s->den);
    if (s->prev > 64U || s->post == 0U || s->post > 131072U ||
        s->first <= s->prev || s->word >= 96U ||
        s->num == 0U || s->num > s->den || s->den > 65535U ||
        s->mode > 1U || (s->fragments > 1U && s->period == 0U)) {
        xil_printf("FAIL: invalid software scenario\r\n");
        return 1;
    }
    frames = s->prev + s->post;
    if (ar_layout(frames, s->fragments, ARAGATZ_TEST_DDR_SIZE,
                  ARAGATZ_GUARD_BYTES, &one, &total) != 0 ||
        s->buffer_bytes < total || s->buffer_bytes > ARAGATZ_TEST_DDR_SIZE) {
        xil_printf("FAIL: scenario exceeds reserved buffer\r\n");
        return 1;
    }
    if (aragatz_check_buffer() != 0 || aragatz_prepare_idle() != 0)
        return 1;
    used = total + ARAGATZ_GUARD_BYTES;
    if (fill_ddr(used) != 0) return 1;
    program_generator(s);
    program_capture(s);
    expect32("GEN frame words", gen_rd(GEN_FRAME_WORDS), 96U);
    expect32("GEN trigger word", gen_rd(GEN_TRIG_WORD), s->word);
    expect32("CAP DDR base", cap_rd(CAP_MEM_ADDR), ARAGATZ_TEST_DDR_BASE);
    expect32("CAP prev", cap_rd(CAP_N_PREV), s->prev);
    expect32("CAP frames", cap_rd(CAP_N_FRAMES), frames);
    expect32("CAP buffer bytes", cap_rd(CAP_BUFFER_BYTES), s->buffer_bytes);
    if (errors != 0U)
        return finish_checks(s->name, 0);
    if (start_capture() != 0)
        return abort_case("Capture did not enter FILL");
    gen_wr(GEN_COMMAND, GEN_CMD_START);
    if (wait_state(CAP_STATE_END, 1000U, TIMEOUT_US) != 0)
        return abort_case("Capture timeout or ERROR before END");
    generator_stop();
    if (!quiet) dump_regs();
    verify_clean_capture(s->fragments);
    expect32("CAP run fragments", cap_rd(CAP_RUN_FRAGMENTS), s->fragments);
    expect32("CAP total fragments", cap_rd(CAP_FRAGMENTS_TOTAL), s->fragments);
    expect32("CAP last address", cap_rd(CAP_LAST_ADDR),
             ARAGATZ_TEST_DDR_BASE + one * (s->fragments - 1U));
    expect32("CAP last frames", cap_rd(CAP_LAST_FRAMES), frames);
    expect32("CAP last bytes", cap_rd(CAP_LAST_BYTES), one);
    expect32("CAP last status", cap_rd(CAP_LAST_STATUS), 1U);
    expect32("CAP trigger source", cap_rd(CAP_LAST_TRIG_SOURCE), 1U);
    expect32("CAP trigger LO", cap_rd(CAP_LAST_TRIG_DATA_LO),
             s->first + (s->fragments - 1U) * s->period);
    expect32("CAP trigger HI", cap_rd(CAP_LAST_TRIG_DATA_HI), ARAGATZ_TRIGGER_MAGIC);
    verify_generator(s, s->fragments, quiet);
    /* With END and a validated bounded layout, verify data even if a
     * statistics assertion failed, so one failure does not hide payload. */
    verify_ddr(s, frames, s->fragments, used);
    if (quiet && errors != 0U) dump_regs();
    return finish_checks(s->name, quiet);
}

int aragatz_test_registers(void)
{
    xil_printf("\r\n[RUN] AXI-Lite registers\r\n");
    if (aragatz_check_buffer() != 0) return 1;
    begin_checks();
    expect32("CAP version", cap_rd(CAP_VERSION), ARAGATZ_IP_VERSION);
    expect32("GEN version", gen_rd(GEN_VERSION), ARAGATZ_IP_VERSION);
    if (errors != 0U || aragatz_prepare_idle() != 0) return 1;
    cap_wr(CAP_HEADER_ID, ARAGATZ_HEADER_ID);
    gen_wr(GEN_FRAME_WORDS, ARAGATZ_FRAME_WORDS);
    gen_wr(GEN_RATE_NUM, 24U);
    gen_wr(GEN_RATE_DEN, 125U);
    expect32("CAP header readback", cap_rd(CAP_HEADER_ID), ARAGATZ_HEADER_ID);
    expect32("GEN frame words readback", gen_rd(GEN_FRAME_WORDS), 96U);
    expect32("GEN rate numerator", gen_rd(GEN_RATE_NUM), 24U);
    expect32("GEN rate denominator", gen_rd(GEN_RATE_DEN), 125U);
    return finish_checks("AXI-Lite", 0);
}

int aragatz_test_smoke(void)
{
    struct scenario s = scenario_make("smoke", 0U, 3U, 1U, 32U, 0U);
    return run_scenario(&s, 0);
}

int aragatz_test_pretrigger(void)
{
    static const uint32_t prevs[] = {1U, 4U, 16U, 64U};
    unsigned k;
    for (k = 0U; k < sizeof(prevs) / sizeof(prevs[0]); ++k) {
        struct scenario s = scenario_make("pretrigger", prevs[k], 3U, 1U,
                                          NORMAL_FIRST, 0U);
        if (run_scenario(&s, 0) != 0) return 1;
    }
    return 0;
}

int aragatz_test_fragments(void)
{
    static const uint32_t counts[] = {2U, 4U, 16U};
    unsigned k;
    for (k = 0U; k < sizeof(counts) / sizeof(counts[0]); ++k) {
        struct scenario s = scenario_make("multi-fragment", 4U, 4U, counts[k],
                                          NORMAL_FIRST, NORMAL_PERIOD);
        if (run_scenario(&s, 0) != 0) return 1;
    }
    return 0;
}

int aragatz_test_boundaries(void)
{
    static const uint32_t prevs[] = {0U, 64U};
    static const uint32_t words[] = {0U, 95U};
    unsigned p, w;
    struct scenario s;
    for (p = 0U; p < 2U; ++p)
        for (w = 0U; w < 2U; ++w) {
            s = scenario_make("trigger boundary", prevs[p], 3U, 2U,
                              NORMAL_FIRST, 512U);
            s.word = words[w];
            if (run_scenario(&s, 0) != 0) return 1;
        }
    s = scenario_make("minimum frame count", 0U, 2U, 1U, 32U, 0U);
    s.word = 0U;
    if (run_scenario(&s, 0) != 0) return 1;
    s = scenario_make("one post frame", 1U, 1U, 1U, 32U, 0U);
    s.word = 95U;
    if (run_scenario(&s, 0) != 0) return 1;
    s = scenario_make("maximum prev / one post", 64U, 1U, 1U, 128U, 0U);
    s.word = 95U;
    if (run_scenario(&s, 0) != 0) return 1;
    s = scenario_make("exact buffer fit", 4U, 4U, 4U, 128U, 128U);
    s.buffer_bytes = 4U * (128U + 8U * 768U);
    if (run_scenario(&s, 0) != 0) return 1;
    s = scenario_make("constant pattern", 4U, 4U, 2U, 128U, 128U);
    s.mode = 1U;
    s.constant_hi = 0x01234567U;
    s.constant_lo = 0x89ABCDEFU;
    return run_scenario(&s, 0);
}

static int stop_before_trigger(int with_stream)
{
    struct scenario s = scenario_make(with_stream ? "STOP WAIT" : "STOP FILL",
                                      4U, 4U, 1U, 128U, 0U);
    uint32_t old_last[4], i;
    const uint32_t used = 128U + 8U * 768U + ARAGATZ_GUARD_BYTES;
    begin_checks();
    xil_printf("\r\n[RUN] %s\r\n", s.name);
    if (aragatz_check_buffer() != 0 || aragatz_prepare_idle() != 0) return 1;
    for (i = 0U; i < 4U; ++i)
        old_last[i] = cap_rd(CAP_LAST_ADDR + 4U * i);
    if (fill_ddr(used) != 0) return 1;
    s.num = 1U; s.den = 1000U;
    program_generator(&s);
    gen_wr(GEN_TRIG_MODE, 0U);
    program_capture(&s);
    if (start_capture() != 0) return abort_case("STOP setup did not enter FILL");
    if (with_stream) {
        gen_wr(GEN_COMMAND, GEN_CMD_START);
        if (wait_state(CAP_STATE_WAIT, 10U, TIMEOUT_US) != 0)
            return abort_case("STOP setup did not enter WAIT");
    }
    cap_wr(CAP_COMMAND, CAP_CMD_STOP);
    if (wait_state(CAP_STATE_IDLE, 10U, TIMEOUT_US) != 0)
        return abort_case("STOP before trigger did not return IDLE");
    generator_stop();
    verify_clean_capture(0U);
    expect32("STOP no completed fragments", cap_rd(CAP_RUN_FRAGMENTS), 0U);
    expect32("STOP no total fragments", cap_rd(CAP_FRAGMENTS_TOTAL), 0U);
    expect32("STOP no generator trigger", gen_rd(GEN_ALG_COUNT), 0U);
    expect32("STOP generator stopped", gen_rd(GEN_STATUS), 0U);
    for (i = 0U; i < 4U; ++i)
        expect32("STOP preserves LAST metadata", cap_rd(CAP_LAST_ADDR + i * 4U),
                 old_last[i]);
    verify_untouched(used);
    return finish_checks(s.name, 0);
}

static int stop_partial(void)
{
    struct scenario s = scenario_make("STOP partial", 4U, 60U, 1U, 128U, 0U);
    uint32_t frame, word, expected_frames = 0U;
    const uint32_t goal = 64U;
    const uint32_t used = 128U + goal * 768U + ARAGATZ_GUARD_BYTES;
    begin_checks();
    xil_printf("\r\n[RUN] STOP partial with frozen GEN cursor\r\n");
    if (aragatz_check_buffer() != 0 || aragatz_prepare_idle() != 0) return 1;
    if (fill_ddr(used) != 0) return 1;
    s.num = 1U; s.den = 1000U;
    program_generator(&s);
    program_capture(&s);
    if (start_capture() != 0) return abort_case("partial STOP setup failed");
    gen_wr(GEN_COMMAND, GEN_CMD_START);
    if (wait_state(CAP_STATE_XFER, 10U, TIMEOUT_US) != 0)
        return abort_case("partial STOP did not observe XFER");
    gen_wr(GEN_PAUSE, 1U);
    usleep(200U);
    frame = gen_rd(GEN_FRAME_INDEX);
    word = gen_rd(GEN_WORD_INDEX);
    expect32("GEN paused/running", gen_rd(GEN_STATUS), 3U);
    expect32("CAP remains XFER while GEN paused", cap_rd(CAP_STATUS) & 15U,
             CAP_STATE_XFER);
    usleep(100U);
    expect32("PAUSE freezes frame", gen_rd(GEN_FRAME_INDEX), frame);
    expect32("PAUSE freezes word", gen_rd(GEN_WORD_INDEX), word);
    if (ar_partial_frames(s.first - s.prev, frame, word, goal,
                          &expected_frames) != 0 || errors != 0U)
        return abort_case("partial STOP window/cursor was not established");
    xil_printf("PAUSE frame=%d next_word=%d expected_partial_frames=%d\r\n",
               (unsigned)frame, (unsigned)word, (unsigned)expected_frames);
    /* Stream is frozen: the prediction cannot race a live MMIO cursor. */
    cap_wr(CAP_COMMAND, CAP_CMD_STOP);
    usleep(20U);
    gen_wr(GEN_PAUSE, 0U);  /* Supply the rest of the current frame and TLAST. */
    if (wait_state(CAP_STATE_IDLE, 10U, TIMEOUT_US) != 0)
        return abort_case("partial STOP did not return IDLE");
    generator_stop();
    dump_regs();
    verify_clean_capture(1U);
    expect32("partial status", cap_rd(CAP_LAST_STATUS), 2U);
    expect32("partial first address", cap_rd(CAP_LAST_ADDR), ARAGATZ_TEST_DDR_BASE);
    expect32("partial exact frames", cap_rd(CAP_LAST_FRAMES), expected_frames);
    expect32("partial exact bytes", cap_rd(CAP_LAST_BYTES),
             128U + expected_frames * 768U);
    expect32("partial no full fragments", cap_rd(CAP_RUN_FRAGMENTS), 0U);
    expect32("partial no accumulated full fragments",
             cap_rd(CAP_FRAGMENTS_TOTAL), 0U);
    verify_generator(&s, 1U, 0);
    verify_ddr(&s, expected_frames, 1U, used);
    return finish_checks(s.name, 0);
}

int aragatz_test_stop_restart(void)
{
    struct scenario s = scenario_make("restart after STOP", 4U, 4U, 2U,
                                      128U, 128U);
    if (stop_before_trigger(0) != 0 || run_scenario(&s, 0) != 0) return 1;
    if (stop_before_trigger(1) != 0 || run_scenario(&s, 0) != 0) return 1;
    if (stop_partial() != 0 || run_scenario(&s, 0) != 0) return 1;
    return 0;
}

static int rejected_capture_config(void)
{
    unsigned k;
    /* All programmed memory windows remain inside the reserved allocation. */
    static const char *const names[] = {
        "frames=1", "prev=65", "prev>=frames", "fragments=0",
        "unaligned address", "zero buffer", "buffer one byte short",
        "zero periodic interval"
    };
    static const uint32_t masks[] = {
        0x10U, 0x20U, 0x20U, 0x40U, 0x80U, 0x200U, 0x200U, 0x800U
    };
    if (ARAGATZ_TEST_DDR_SIZE < 128U + 68U * 768U) {
        xil_printf("FAIL: config rejection matrix needs at least 52352 reserved bytes\r\n");
        return 1;
    }
    for (k = 0U; k < 8U; ++k) {
        struct scenario s = scenario_make(names[k], 0U, 3U, 1U, 32U, 0U);
        uint32_t i, old_run_fragments;
        begin_checks();
        xil_printf("\r\n[RUN] reject CAP configuration: %s\r\n", names[k]);
        if (aragatz_check_buffer() != 0 || aragatz_prepare_idle() != 0) return 1;
        old_run_fragments = cap_rd(CAP_RUN_FRAGMENTS);
        if (fill_ddr(4096U) != 0) return 1;
        program_capture(&s);
        switch (k) {
        case 0U: cap_wr(CAP_N_FRAMES, 1U); break;
        case 1U: cap_wr(CAP_N_FRAMES, 68U); cap_wr(CAP_N_PREV, 65U); break;
        case 2U: cap_wr(CAP_N_PREV, 3U); break;
        case 3U: cap_wr(CAP_N_FRAGMENTS, 0U); break;
        case 4U:
            cap_wr(CAP_MEM_ADDR, ARAGATZ_TEST_DDR_BASE + 4U);
            cap_wr(CAP_BUFFER_BYTES, ARAGATZ_TEST_DDR_SIZE - 4U);
            break;
        case 5U: cap_wr(CAP_BUFFER_BYTES, 0U); break;
        case 6U: cap_wr(CAP_BUFFER_BYTES, 2431U); break;
        default: cap_wr(CAP_INT_CTRL, 2U); cap_wr(CAP_INT_PERIOD, 0U); break;
        }
        cap_wr(CAP_COMMAND, CAP_CMD_START);
        for (i = 0U; i < 1000U; ++i) {
            if (cap_rd(CAP_ERRORS) != 0U &&
                (cap_rd(CAP_STATUS) & STATUS_BUSY) == 0U)
                break;
            usleep(10U);
        }
        expect32("startup rejection flags", cap_rd(CAP_ERRORS), masks[k]);
        expect32("startup rejection stays IDLE", cap_rd(CAP_STATUS) & 15U, 0U);
        expect32("startup rejection not busy", cap_rd(CAP_STATUS) & STATUS_BUSY, 0U);
        expect32("startup rejection preserves run counter",
                 cap_rd(CAP_RUN_FRAGMENTS), old_run_fragments);
        expect32("startup rejection no new fragments",
                 cap_rd(CAP_FRAGMENTS_TOTAL), 0U);
        verify_untouched(4096U);
        if (errors != 0U) return abort_case("configuration rejection mismatch");
        cap_wr(CAP_COMMAND, CAP_CMD_CLEAR_ERROR);
        usleep(200U);
        expect32("startup error cleared", cap_rd(CAP_ERRORS), 0U);
        if (finish_checks(names[k], 0) != 0) return 1;
    }
    return 0;
}

static int rejected_generator_config(void)
{
    static const uint32_t offsets[] = {
        GEN_FRAME_WORDS, GEN_RATE_NUM, GEN_RATE_DEN,
        GEN_RATE_NUM, GEN_TRIG_WORD, GEN_EXT_WIDTH
    };
    static const uint32_t values[] = {0U, 0U, 0U, 126U, 96U, 0U};
    unsigned k;
    for (k = 0U; k < 6U; ++k) {
        struct scenario s = scenario_make("GEN rejected configuration", 0U, 3U,
                                          1U, 32U, 0U);
        begin_checks();
        if (aragatz_prepare_idle() != 0) return 1;
        program_generator(&s);
        gen_wr(offsets[k], values[k]);
        gen_wr(GEN_COMMAND, GEN_CMD_START);
        usleep(200U);
        expect32("GEN config_error only", gen_rd(GEN_STATUS), 4U);
        expect32("invalid GEN emitted no words", gen_rd(GEN_WORDS_SENT), 0U);
        expect32("invalid GEN emitted no events", gen_rd(GEN_ALG_COUNT), 0U);
        gen_wr(GEN_COMMAND, GEN_CMD_CLEAR_STATS);
        usleep(20U);
        expect32("GEN clear sticky error", gen_rd(GEN_STATUS), 0U);
        if (finish_checks(s.name, 0) != 0) return 1;
    }
    return 0;
}

static int runtime_error_recovery(void)
{
    struct scenario s = scenario_make("short frame error/recovery", 4U, 4U,
                                      1U, 128U, 0U);
    const uint32_t used = 128U + 8U * 768U + ARAGATZ_GUARD_BYTES;
    uint32_t old_last[4], i;
    begin_checks();
    xil_printf("\r\n[RUN] runtime short frame, then CLEAR_ERROR\r\n");
    if (aragatz_check_buffer() != 0 || aragatz_prepare_idle() != 0) return 1;
    for (i = 0U; i < 4U; ++i)
        old_last[i] = cap_rd(CAP_LAST_ADDR + i * 4U);
    if (fill_ddr(used) != 0) return 1;
    s.num = 1U; s.den = 1000U;
    program_generator(&s);
    gen_wr(GEN_FRAME_WORDS, 95U);
    gen_wr(GEN_TRIG_MODE, 0U);
    program_capture(&s);
    if (start_capture() != 0) return abort_case("runtime error setup failed");
    gen_wr(GEN_COMMAND, GEN_CMD_START);
    if (wait_state(CAP_STATE_ERROR, 10U, TIMEOUT_US) != 0)
        return abort_case("short frame did not enter ERROR");
    generator_stop();
    expect32("short frame format error", cap_rd(CAP_ERRORS), 8U);
    expect32("no writer transaction on pretrigger error",
             cap_rd(CAP_STATUS) & STATUS_AXI_BUSY, 0U);
    expect32("no fragment on pretrigger error", cap_rd(CAP_RUN_FRAGMENTS), 0U);
    expect32("no trigger on pretrigger error", cap_rd(CAP_TRIG_ACCEPTED), 0U);
    for (i = 0U; i < 4U; ++i)
        expect32("pretrigger error preserves LAST metadata",
                 cap_rd(CAP_LAST_ADDR + i * 4U), old_last[i]);
    verify_untouched(used);
    if (errors != 0U) return finish_checks(s.name, 0);
    /* No XFER was started; GEN STOP delay allowed the synchronous FIFO
     * cleanup to finish before this command (no software allow-clear bit). */
    cap_wr(CAP_COMMAND, CAP_CMD_CLEAR_ERROR);
    usleep(200U);
    if (wait_state(CAP_STATE_IDLE, 10U, 10000U) != 0)
        return abort_case("CLEAR_ERROR did not return IDLE");
    expect32("runtime flags cleared", cap_rd(CAP_ERRORS), 0U);
    return finish_checks(s.name, 0);
}

int aragatz_test_recovery(void)
{
    struct scenario s = scenario_make("capture after error recovery", 4U, 4U,
                                      2U, 128U, 128U);
    if (aragatz_check_buffer() != 0) return 1;
    if (rejected_capture_config() != 0) return 1;
    if (rejected_generator_config() != 0) return 1;
    if (run_scenario(&s, 0) != 0) return 1;
    if (runtime_error_recovery() != 0) return 1;
    return run_scenario(&s, 0);
}

int aragatz_test_long(void)
{
    struct scenario s = scenario_make("long contiguous capture", 64U, 960U,
                                      1U, 128U, 0U);
    if (run_scenario(&s, 0) != 0) return 1;
    s = scenario_make("128 sequential fragments", 4U, 4U, 128U, 128U, 128U);
    return run_scenario(&s, 0);
}

static int repeated_runs(uint32_t count, int endurance)
{
    uint32_t k;
    uint64_t bytes_verified = 0U;
    if (count == 0U) {
        xil_printf("FAIL: repeat count must be nonzero\r\n");
        return 1;
    }
    xil_printf("\r\n=== %s: %d runs, full payload check each run ===\r\n",
               endurance ? "ENDURANCE" : "RESTART LOOP", (unsigned)count);
    for (k = 0U; k < count; ++k) {
        uint32_t prev = endurance ? 64U :
                        (k % 3U == 0U ? 0U : (k % 3U == 1U ? 4U : 64U));
        struct scenario s = scenario_make(endurance ? "endurance" : "restart loop",
                                          prev, endurance ? 960U : 3U,
                                          endurance ? 1U : 2U, 128U,
                                          endurance ? 0U : (prev == 64U ? 512U : 128U));
        s.word = k % 3U == 0U ? 0U : (k % 3U == 1U ? 47U : 95U);
        if (run_scenario(&s, 1) != 0) {
            xil_printf("FAIL at run %d/%d; first failure preserved\r\n",
                       (unsigned)(k + 1U), (unsigned)count);
            return 1;
        }
        bytes_verified += (128U + (uint64_t)(s.prev + s.post) * 768U) * s.fragments;
        if ((k + 1U) % 10U == 0U || k + 1U == count)
            xil_printf("Progress: %d/%d PASS, payload MiB=%d\r\n",
                       (unsigned)(k + 1U), (unsigned)count,
                       (unsigned)(bytes_verified >> 20));
    }
    xil_printf("[%s] %s: %d completed runs; total bytes=0x%08x%08x\r\n",
               "PASS", endurance ? "endurance" : "restart loop", (unsigned)count,
               (unsigned)(bytes_verified >> 32), (unsigned)bytes_verified);
    return 0;
}

int aragatz_test_repeat(void)
{
    return repeated_runs(ARAGATZ_REPEAT_RUNS, 0);
}

int aragatz_test_endurance(void)
{
    return repeated_runs(ARAGATZ_ENDURANCE_RUNS, 1);
}

int aragatz_test_rate_sweep(void)
{
    static const uint32_t nums[] = {12U, 24U, 32U, 40U, 48U, 64U, 80U, 100U, 125U};
    uint32_t last_pass = 0U;
    unsigned k;
    xil_printf("\r\n=== RATE CHARACTERIZATION (separate from required regression) ===\r\n");
    xil_printf("Configured source rate assumes PL=%d Hz; this is not a DDR timer.\r\n",
               (unsigned)ARAGATZ_PL_CLOCK_HZ);
    for (k = 0U; k < sizeof(nums) / sizeof(nums[0]); ++k) {
        struct scenario s = scenario_make("rate sweep / 1024 frames", 64U, 960U,
                                          1U, 128U, 0U);
        uint32_t rate;
        s.num = nums[k];
        rate = (uint32_t)((uint64_t)ARAGATZ_PL_CLOCK_HZ * s.num * 8U / s.den);
        xil_printf("\r\nPROFILE %d/%d: configured bytes/s=%d\r\n",
                   (unsigned)s.num, (unsigned)s.den, (unsigned)rate);
        if (run_scenario(&s, 0) != 0) {
            xil_printf("SWEEP STOP: profile %d/125 did not pass data integrity.\r\n",
                       (unsigned)s.num);
            if (last_pass != 0U)
                xil_printf("Last passing profile in this sweep: %d/125.\r\n",
                           (unsigned)last_pass);
            xil_printf("Inspect errors/log before deciding overload versus a defect.\r\n");
            return 1;
        }
        last_pass = s.num;
    }
    xil_printf("SWEEP PASS: all listed short capture profiles passed.\r\n");
    xil_printf("This does not establish sustained throughput at the highest rate.\r\n");
    return 0;
}

int aragatz_test_all(void)
{
    static int (*const groups[])(void) = {
        aragatz_test_registers, aragatz_test_smoke, aragatz_test_pretrigger,
        aragatz_test_fragments, aragatz_test_boundaries, aragatz_test_stop_restart,
        aragatz_test_recovery, aragatz_test_long, aragatz_test_ddr,
        aragatz_test_repeat
    };
    unsigned k;
    xil_printf("\r\n=== ARAGATZ V3 FUNCTIONAL REGRESSION ===\r\n");
    for (k = 0U; k < sizeof(groups) / sizeof(groups[0]); ++k)
        if (groups[k]() != 0) {
            xil_printf("=== V3 REGRESSION FAIL: group %d, later groups not run ===\r\n",
                       k + 1U);
            return 1;
        }
    xil_printf("=== V3 REGRESSION PASS (10 groups) ===\r\n");
    return 0;
}
