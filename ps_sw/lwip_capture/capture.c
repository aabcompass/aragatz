/* Capture -> all BRESP/END -> cache invalidate -> immutable DDR -> CRC.
 * Standalone/raw-lwIP integration must call poll() from its regular loop.
 * No network callbacks, allocation, sleep, or polling loops live here. */
#include "capture.h"
#include "protocol.h"
#include "aragatz_hw.h"
#include "xil_cache.h"
#include "xpseudo_asm.h"
#include <stddef.h>
#include <string.h>

#define AR_WORK_BYTES          4096U
#define AR_PREP_TIMEOUT_US     UINT64_C(10000000)
#define AR_RUN_TIMEOUT_US      UINT64_C(5000000)
#define AR_ABORT_TIMEOUT_US    UINT64_C(5000000)
#define STATUS_BUSY           (1U << 4)
#define STATUS_AXI_BUSY       (1U << 8)
#define STATUS_FIFO_EMPTY     (1U << 9)
#define GEN_RUNNING           1U

/* Added by the application linker reservation script; an exclusive bound.
 * All app sections, all stacks, heap, lwIP pools and GEM descriptors must
 * use the linker DDR region that ends here, below the capture reservation. */
extern char __aragatz_capture_start[];
extern char __aragatz_capture_end[];
extern char __aragatz_app_ddr_end[];

enum phase {
    P_NONE, P_WAIT_IDLE, P_WAIT_CLEAR, P_FILL_MEMORY, P_PROGRAM,
    P_WAIT_FILL, P_RUN, P_WAIT_GEN_STOP,
    P_INVALIDATE, P_INVALIDATE_FINISH, P_CRC, P_CRC_FINISH
};
enum abort_phase { A_NONE, A_WAIT_LEGAL, A_DRAIN, A_FINISHED, A_STUCK };

static struct ar_capture_metadata meta;
static enum ar_capture_state state = AR_CAPTURE_IDLE;
static enum phase phase;
static enum abort_phase abort_phase;
static int initialized;
static int registers_available;
static uint32_t cursor, used_bytes, fragment_bytes;
static uint64_t phase_started, capture_started, abort_started;
static const char *fault_reason = "";

static uint32_t hw_state(uint32_t status) { return status & 15U; }

static int hw_quiescent(uint32_t status)
{
    uint32_t s = hw_state(status);
    return (s == CAP_STATE_IDLE || s == CAP_STATE_END) &&
           !(status & (STATUS_BUSY | STATUS_AXI_BUSY));
}

static int stop_legal(uint32_t status)
{
    uint32_t s = hw_state(status);
    return s == CAP_STATE_FILL || s == CAP_STATE_WAIT || s == CAP_STATE_XFER;
}

static uint64_t read_stat64(uint32_t lo, uint32_t hi)
{
    uint32_t low = cap_rd(lo); /* LO atomically latches the corresponding HI. */
    uint32_t high = cap_rd(hi);
    return ((uint64_t)high << 32) | low;
}

static void snapshot(void)
{
    if (!registers_available) return;
    meta.cap_status = cap_rd(CAP_STATUS);
    meta.cap_errors = cap_rd(CAP_ERRORS);
    meta.completed_fragments = cap_rd(CAP_RUN_FRAGMENTS);
    meta.last_frames = cap_rd(CAP_LAST_FRAMES);
    meta.last_bytes = cap_rd(CAP_LAST_BYTES);
    meta.last_status = cap_rd(CAP_LAST_STATUS);
    meta.trigger_missed = read_stat64(CAP_TRIG_MISSED, CAP_TRIG_MISSED_HI);
}

static void fail(const char *reason, uint64_t now_us, int may_abort)
{
    if (state == AR_CAPTURE_FAULT) return;
    snapshot();
    fault_reason = reason;
    state = AR_CAPTURE_FAULT;
    phase = P_NONE;
    abort_phase = A_NONE;
    if (!may_abort || !registers_available) return;

    /* Do not stop the camera source before capture has its final TLAST.
     * A failed or timed-out drain never permits a new capture. */
    if (hw_quiescent(meta.cap_status) ||
        (hw_state(meta.cap_status) == CAP_STATE_ERROR &&
         !(meta.cap_status & STATUS_AXI_BUSY))) {
        gen_wr(GEN_COMMAND, GEN_CMD_STOP);
        abort_phase = A_FINISHED;
    } else {
        abort_started = now_us;
        if (stop_legal(meta.cap_status)) {
            gen_wr(GEN_PAUSE, 0U);
            cap_wr(CAP_COMMAND, CAP_CMD_STOP);
            abort_phase = A_DRAIN;
        } else {
            /* AFTER ignores STOP. Wait for its next FILL or quiescent END
             * instead of losing the only abort command in FIFO cleanup. */
            abort_phase = A_WAIT_LEGAL;
        }
    }
}

static void poll_abort(uint64_t now_us)
{
    uint32_t status;
    if (abort_phase != A_DRAIN && abort_phase != A_WAIT_LEGAL) return;
    status = cap_rd(CAP_STATUS);
    if (hw_quiescent(status) ||
        (hw_state(status) == CAP_STATE_ERROR && !(status & STATUS_AXI_BUSY))) {
        gen_wr(GEN_COMMAND, GEN_CMD_STOP);
        abort_phase = A_FINISHED;
    } else if (now_us - abort_started >= AR_ABORT_TIMEOUT_US) {
        /* Keep the first failure's metadata and never force buffer reuse.
         * Hardware reset is needed if accepted AXI requests cannot drain. */
        abort_phase = A_STUCK;
    } else if (abort_phase == A_WAIT_LEGAL && stop_legal(status)) {
        gen_wr(GEN_PAUSE, 0U);
        cap_wr(CAP_COMMAND, CAP_CMD_STOP);
        abort_phase = A_DRAIN;
    }
}

static int check_reservation(void)
{
    uint64_t base = (uint64_t)ARAGATZ_TEST_DDR_BASE;
    uint64_t size = (uint64_t)ARAGATZ_TEST_DDR_SIZE;
    uint64_t end = base + size;
    uint64_t app_end = (uint64_t)(UINTPTR)__aragatz_app_ddr_end;
    volatile uint32_t stack_probe = 0;
    uint64_t sp = (uint64_t)(UINTPTR)&stack_probe;
    uint64_t mp = (uint64_t)(UINTPTR)&meta;

    if (size < 4096U || size > UINT32_MAX || base > UINT32_MAX ||
        end > (UINT64_C(1) << 32) || end <= base ||
        (UINTPTR)__aragatz_capture_start != base ||
        (UINTPTR)__aragatz_capture_end != end ||
        base < XPAR_PS7_DDR_0_S_AXI_BASEADDR ||
        end - 1U > XPAR_PS7_DDR_0_S_AXI_HIGHADDR ||
        end - 1U > ARAGATZ_HP0_HIGHADDR ||
        (base & 127U) || (size & 127U) ||
        app_end <= XPAR_PS7_DDR_0_S_AXI_BASEADDR || app_end > base ||
        (sp >= base && sp < end) || (mp >= base && mp < end))
        return -1;
    return 0;
}

int ar_capture_init(void)
{
    if (initialized) return state == AR_CAPTURE_FAULT ? -1 : 0;
    initialized = 1;
    memset(&meta, 0, sizeof(meta));
    meta.data = (const uint8_t *)(UINTPTR)ARAGATZ_TEST_DDR_BASE;
    if (check_reservation() != 0) {
        fail("DDR bounds/linker reservation mismatch", 0, 0);
        return -1;
    }
    if ((uint64_t)XPAR_CAMERA_CAPTURE_BD_0_HIGHADDR <
        (uint64_t)CAP_BASE + CAP_BUFFER_BYTES + 3U ||
        (uint64_t)XPAR_CAMERA_TEST_GEN_BD_0_HIGHADDR <
        (uint64_t)GEN_BASE + GEN_VERSION + 3U) {
        fail("BSP register aperture too small", 0, 0);
        return -1;
    }
    registers_available = 1;
    meta.cap_version = cap_rd(CAP_VERSION);
    meta.gen_version = gen_rd(GEN_VERSION);
    snapshot();
    if (meta.cap_version != ARAGATZ_IP_VERSION ||
        meta.gen_version != ARAGATZ_IP_VERSION) {
        fail("Unexpected capture/generator IP version", 0, 0);
        return -1;
    }
    if (!hw_quiescent(meta.cap_status) || meta.cap_errors) {
        /* Startup must not take over an in-flight run belonging to the
         * hardware-regression application or another software session. */
        fail("Startup requires clean IDLE or END; reset/recover first", 0, 0);
        return -1;
    }
    state = AR_CAPTURE_IDLE;
    return 0;
}

int ar_capture_begin(uint64_t id, uint32_t frames, uint32_t prev,
                     uint32_t fragments, uint64_t now_us)
{
    uint64_t one, total;
    uint32_t status, cap_version, gen_version;
    if (!initialized || (state != AR_CAPTURE_IDLE && state != AR_CAPTURE_VERIFIED) ||
        !id || id == meta.id || frames < 2U || frames > 131072U ||
        prev > 64U || prev >= frames || fragments == 0U || fragments > 16U)
        return -1;
    one = 128U + (uint64_t)frames * 768U;
    total = one * fragments;
    if (total + ARAGATZ_GUARD_BYTES > ARAGATZ_TEST_DDR_SIZE ||
        total > UINT32_MAX || (total & 127U))
        return -1;
    status = cap_rd(CAP_STATUS);
    if (!hw_quiescent(status) || cap_rd(CAP_ERRORS)) {
        fail("Hardware changed before START", now_us, 0);
        return -1;
    }

    cap_version = meta.cap_version;
    gen_version = meta.gen_version;
    memset(&meta, 0, sizeof(meta));
    meta.id = id;
    meta.payload_bytes = total;
    meta.frames = frames;
    meta.prev = prev;
    meta.fragments = fragments;
    meta.gen_first = 128U; /* > max PRE; skip sync and history filling. */
    meta.gen_period = frames + 128U;
    if (meta.gen_period < 512U) meta.gen_period = 512U;
    meta.gen_word = 47U;
    meta.rate_num = 24U;
    meta.rate_den = 125U;
    meta.cap_version = cap_version;
    meta.gen_version = gen_version;
    meta.data = (const uint8_t *)(UINTPTR)ARAGATZ_TEST_DDR_BASE;
    fragment_bytes = (uint32_t)one;
    used_bytes = (uint32_t)total + ARAGATZ_GUARD_BYTES;
    cursor = 0;
    phase_started = now_us;
    abort_phase = A_NONE;
    fault_reason = "";
    state = AR_CAPTURE_PREPARING;
    phase = P_WAIT_IDLE;

    /* Safe because both allowed entry states have no capture DMA activity. */
    gen_wr(GEN_COMMAND, GEN_CMD_STOP);
    gen_wr(GEN_PAUSE, 0U);
    if (hw_state(status) == CAP_STATE_END)
        cap_wr(CAP_COMMAND, CAP_CMD_STOP);
    snapshot();
    return 0;
}

static void program(void)
{
    unsigned i;
    gen_wr(GEN_PAUSE, 0U);
    gen_wr(GEN_MODE, 0U);
    gen_wr(GEN_FRAME_WORDS, ARAGATZ_FRAME_WORDS);
    gen_wr(GEN_RATE_NUM, meta.rate_num);
    gen_wr(GEN_RATE_DEN, meta.rate_den);
    gen_wr(GEN_CONSTANT_LO, 0U);
    gen_wr(GEN_CONSTANT_HI, 0U);
    gen_wr(GEN_TRIG_MODE, 1U);
    gen_wr(GEN_TRIG_FIRST, meta.gen_first);
    gen_wr(GEN_TRIG_PERIOD, meta.gen_period);
    gen_wr(GEN_TRIG_WORD, meta.gen_word);
    gen_wr(GEN_EXT_WIDTH, 4U);
    cap_wr(CAP_TRIGGER_ENABLE, 1U);
    cap_wr(CAP_N_FRAMES, meta.frames);
    cap_wr(CAP_N_PREV, meta.prev);
    cap_wr(CAP_N_FRAGMENTS, meta.fragments);
    cap_wr(CAP_MEM_ADDR, ARAGATZ_TEST_DDR_BASE);
    cap_wr(CAP_HEADER_ID, ARAGATZ_HEADER_ID);
    cap_wr(CAP_AUX0, ARAGATZ_AUX0);
    cap_wr(CAP_AUX0 + 4U, ARAGATZ_AUX1);
    for (i = 2; i < 8; ++i) cap_wr(CAP_AUX0 + 4U * i, 0U);
    cap_wr(CAP_INT_PERIOD, 0U);
    cap_wr(CAP_INT_CTRL, 0U);
    cap_wr(CAP_BUFFER_BYTES, (uint32_t)meta.payload_bytes);
}

static int config_matches(void)
{
    unsigned i;
    if (cap_rd(CAP_TRIGGER_ENABLE) != 1U || cap_rd(CAP_N_FRAMES) != meta.frames ||
        cap_rd(CAP_N_PREV) != meta.prev || cap_rd(CAP_N_FRAGMENTS) != meta.fragments ||
        cap_rd(CAP_MEM_ADDR) != ARAGATZ_TEST_DDR_BASE ||
        cap_rd(CAP_BUFFER_BYTES) != meta.payload_bytes ||
        cap_rd(CAP_HEADER_ID) != ARAGATZ_HEADER_ID ||
        cap_rd(CAP_AUX0) != ARAGATZ_AUX0 ||
        cap_rd(CAP_AUX0 + 4U) != ARAGATZ_AUX1 ||
        cap_rd(CAP_INT_PERIOD) || cap_rd(CAP_INT_CTRL) ||
        gen_rd(GEN_PAUSE) || gen_rd(GEN_MODE) ||
        gen_rd(GEN_FRAME_WORDS) != ARAGATZ_FRAME_WORDS ||
        gen_rd(GEN_RATE_NUM) != meta.rate_num || gen_rd(GEN_RATE_DEN) != meta.rate_den ||
        gen_rd(GEN_TRIG_MODE) != 1U || gen_rd(GEN_TRIG_FIRST) != meta.gen_first ||
        gen_rd(GEN_TRIG_PERIOD) != meta.gen_period || gen_rd(GEN_TRIG_WORD) != meta.gen_word ||
        gen_rd(GEN_EXT_WIDTH) != 4U || gen_rd(GEN_STATUS))
        return 0;
    for (i = 2; i < 8; ++i)
        if (cap_rd(CAP_AUX0 + 4U * i)) return 0;
    return 1;
}

static int complete_matches(void)
{
    uint64_t total = meta.payload_bytes;
    snapshot();
    return hw_state(meta.cap_status) == CAP_STATE_END &&
           !(meta.cap_status & (STATUS_BUSY | STATUS_AXI_BUSY)) &&
           (meta.cap_status & STATUS_FIFO_EMPTY) && meta.cap_errors == 0 &&
           meta.completed_fragments == meta.fragments &&
           meta.last_status == 1U && meta.last_frames == meta.frames &&
           meta.last_bytes == fragment_bytes && meta.trigger_missed == 0 &&
           cap_rd(CAP_LAST_ADDR) == ARAGATZ_TEST_DDR_BASE + total - fragment_bytes &&
           cap_rd(CAP_CURRENT_ADDR) == ARAGATZ_TEST_DDR_BASE + total &&
           cap_rd(CAP_FRAMES_COMMITTED) == meta.frames &&
           cap_rd(CAP_FRAGMENTS_TOTAL) == meta.fragments &&
           read_stat64(CAP_TRIG_ACCEPTED, CAP_TRIG_ACCEPTED_HI) == meta.fragments &&
           cap_rd(CAP_FIFO_OVERFLOWS) == 0 && cap_rd(CAP_AXI_ERRORS) == 0 &&
           cap_rd(CAP_LAST_TRIG_SOURCE) == 1U &&
           cap_rd(CAP_LAST_TRIG_DATA_HI) == ARAGATZ_TRIGGER_MAGIC &&
           cap_rd(CAP_LAST_TRIG_DATA_LO) ==
               meta.gen_first + (meta.fragments - 1U) * meta.gen_period;
}

static int frozen(void)
{
    uint32_t status = cap_rd(CAP_STATUS);
    return hw_state(status) == CAP_STATE_END &&
           !(status & (STATUS_BUSY | STATUS_AXI_BUSY)) && cap_rd(CAP_ERRORS) == 0;
}

void ar_capture_poll(uint64_t now_us)
{
    uint32_t status, n, i;
    volatile uint32_t *ddr = (volatile uint32_t *)(UINTPTR)ARAGATZ_TEST_DDR_BASE;
    if (!initialized) return;
    if (state == AR_CAPTURE_FAULT) { poll_abort(now_us); return; }
    if (state == AR_CAPTURE_IDLE) return;
    if (state == AR_CAPTURE_READY || state == AR_CAPTURE_VERIFIED) {
        if (!frozen() || gen_rd(GEN_STATUS))
            fail("Hardware changed while immutable capture was retained", now_us, 1);
        return;
    }

    if (state == AR_CAPTURE_PREPARING && now_us - phase_started >= AR_PREP_TIMEOUT_US) {
        fail("Preparation timeout", now_us, 1);
        return;
    }
    if (state == AR_CAPTURE_CAPTURING && now_us - capture_started >= AR_RUN_TIMEOUT_US) {
        fail("Capture/END timeout", now_us, 1);
        return;
    }
    status = cap_rd(CAP_STATUS);
    if (cap_rd(CAP_ERRORS) || hw_state(status) == CAP_STATE_ERROR) {
        fail("Capture hardware error", now_us, 1);
        return;
    }

    switch (phase) {
    case P_WAIT_IDLE:
        if (hw_state(status) != CAP_STATE_IDLE ||
            (status & (STATUS_BUSY | STATUS_AXI_BUSY)) || (gen_rd(GEN_STATUS) & GEN_RUNNING))
            return;
        cap_wr(CAP_COMMAND, CAP_CMD_CLEAR_STATS);
        gen_wr(GEN_COMMAND, GEN_CMD_CLEAR_STATS);
        phase = P_WAIT_CLEAR;
        return;
    case P_WAIT_CLEAR:
        if (gen_rd(GEN_STATUS) || cap_rd(CAP_FRAGMENTS_TOTAL) ||
            read_stat64(CAP_TRIG_ACCEPTED, CAP_TRIG_ACCEPTED_HI) ||
            read_stat64(CAP_TRIG_MISSED, CAP_TRIG_MISSED_HI) ||
            cap_rd(CAP_FIFO_OVERFLOWS) || cap_rd(CAP_AXI_ERRORS)) return;
        phase = P_FILL_MEMORY;
        return;
    case P_FILL_MEMORY:
        if (hw_state(status) != CAP_STATE_IDLE || (status & (STATUS_BUSY | STATUS_AXI_BUSY))) {
            fail("Capture became active during DDR preparation", now_us, 1);
            return;
        }
        n = used_bytes - cursor;
        if (n > AR_WORK_BYTES) n = AR_WORK_BYTES;
        for (i = 0; i < n / 4U; ++i)
            ddr[cursor / 4U + i] = ARAGATZ_SENTINEL(cursor / 4U + i);
        /* Every span is isolated and 32-byte cache-line aligned. This is
         * the only flush, performed BEFORE capture can write the buffer. */
        Xil_DCacheFlushRange((INTPTR)(ARAGATZ_TEST_DDR_BASE + cursor), n);
        cursor += n;
        if (cursor == used_bytes) phase = P_PROGRAM;
        return;
    case P_PROGRAM:
        if (!hw_quiescent(status) || hw_state(status) != CAP_STATE_IDLE) {
            fail("Expected IDLE before configuration", now_us, 1);
            return;
        }
        program();
        if (!config_matches()) {
            fail("Configuration/CAP_BUFFER_BYTES readback mismatch", now_us, 1);
            return;
        }
        dsb();
        cap_wr(CAP_COMMAND, CAP_CMD_START);
        capture_started = now_us;
        state = AR_CAPTURE_CAPTURING;
        phase = P_WAIT_FILL;
        return;
    case P_WAIT_FILL:
        if (hw_state(status) == CAP_STATE_IDLE) return; /* START validation */
        if (hw_state(status) != CAP_STATE_FILL || !(status & STATUS_BUSY)) {
            fail("START did not enter FILL; stale END rejected", now_us, 1);
            return;
        }
        gen_wr(GEN_COMMAND, GEN_CMD_START);
        phase = P_RUN;
        return;
    case P_RUN:
        if (hw_state(status) == CAP_STATE_IDLE) {
            fail("Capture stopped before complete END", now_us, 1);
            return;
        }
        if (gen_rd(GEN_STATUS) & 12U) {
            fail("Generator configuration/start error", now_us, 1);
            return;
        }
        if (hw_state(status) != CAP_STATE_END) return;
        meta.capture_us = now_us - capture_started;
        if (!complete_matches()) {
            fail("END metadata/counts/guards/trigger schedule mismatch", now_us, 1);
            return;
        }
        gen_wr(GEN_COMMAND, GEN_CMD_STOP);
        phase = P_WAIT_GEN_STOP;
        return;
    case P_WAIT_GEN_STOP:
        if (!frozen()) {
            fail("Capture changed after END", now_us, 1);
            return;
        }
        if (gen_rd(GEN_STATUS) & GEN_RUNNING) return;
        if (gen_rd(GEN_STATUS)) {
            fail("Generator stopped with sticky error", now_us, 1);
            return;
        }
        state = AR_CAPTURE_CACHE;
        phase = P_INVALIDATE;
        phase_started = now_us;
        cursor = 0;
        return;
    case P_INVALIDATE:
        if (!frozen()) {
            fail("DDR writer restarted during cache maintenance", now_us, 1);
            return;
        }
        n = used_bytes - cursor;
        if (n > AR_WORK_BYTES) n = AR_WORK_BYTES;
        Xil_DCacheInvalidateRange((INTPTR)(ARAGATZ_TEST_DDR_BASE + cursor), n);
        cursor += n;
        if (cursor != used_bytes) return;
        dsb();
        for (i = (uint32_t)meta.payload_bytes / 4U; i < used_bytes / 4U; ++i)
            if (ddr[i] != ARAGATZ_SENTINEL(i)) ++meta.guard_errors;
        if (meta.guard_errors) {
            fail("DDR suffix guard overwritten", now_us, 1);
            return;
        }
        /* now_us was sampled BEFORE this poll's work. Finish on the next
         * poll so the final invalidate/guard cost belongs to cache_us. */
        phase = P_INVALIDATE_FINISH;
        return;
    case P_INVALIDATE_FINISH:
        if (!frozen()) {
            fail("DDR writer restarted after cache maintenance", now_us, 1);
            return;
        }
        meta.cache_us = now_us - phase_started;
        cursor = 0;
        meta.payload_crc32 = 0;
        phase_started = now_us;
        state = AR_CAPTURE_CRC;
        phase = P_CRC;
        return;
    case P_CRC:
        if (!frozen()) {
            fail("DDR writer restarted during CRC", now_us, 1);
            return;
        }
        n = (uint32_t)meta.payload_bytes - cursor;
        if (n > AR_WORK_BYTES) n = AR_WORK_BYTES;
        meta.payload_crc32 = ar_crc32_update(meta.payload_crc32, meta.data + cursor, n);
        cursor += n;
        if (cursor == meta.payload_bytes)
            phase = P_CRC_FINISH;
        return;
    case P_CRC_FINISH:
        if (!frozen()) {
            fail("DDR writer restarted after CRC", now_us, 1);
            return;
        }
        /* Include the last chunk's execution time in this elapsed metric. */
        meta.crc_us = now_us - phase_started;
        state = AR_CAPTURE_READY;
        phase = P_NONE;
        return;
    default:
        fail("Internal capture state mismatch", now_us, 1);
        return;
    }
}

enum ar_capture_state ar_capture_state(void) { return state; }
const struct ar_capture_metadata *ar_capture_meta(void) { return &meta; }
const char *ar_capture_fault_reason(void) { return fault_reason; }

int ar_capture_verified(uint64_t id, uint32_t crc, uint32_t bytes)
{
    if ((state != AR_CAPTURE_READY && state != AR_CAPTURE_VERIFIED) ||
        id != meta.id || crc != meta.payload_crc32 || bytes != meta.payload_bytes)
        return -1;
    state = AR_CAPTURE_VERIFIED;
    return 0;
}

/* Wire v1 ACK consumes the retained snapshot. It deliberately preserves the
 * last identifier/status for an EMPTY status reply, never the buffer geometry.
 * Called by server.c only after this connection fully TCP-ACKed the payload. */
int ar_capture_release(void)
{
    uint64_t id;
    uint32_t cap_version, gen_version, status, errors;
    if (state != AR_CAPTURE_VERIFIED) return -1;
    id = meta.id; cap_version = meta.cap_version; gen_version = meta.gen_version;
    status = meta.cap_status; errors = meta.cap_errors;
    memset(&meta, 0, sizeof(meta));
    meta.id = id; meta.cap_version = cap_version; meta.gen_version = gen_version;
    meta.cap_status = status; meta.cap_errors = errors;
    meta.data = (const uint8_t *)(UINTPTR)ARAGATZ_TEST_DDR_BASE;
    phase = P_NONE;
    state = AR_CAPTURE_IDLE;
    return 0;
}
