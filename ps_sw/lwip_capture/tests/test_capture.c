/* Software contract tests only: no ARM cache, PHY, GEM or real PL is tested.
 * Production capture.c is linked against a controlled MMIO/cache model.
 * One process per scenario deliberately preserves the sticky FAULT contract. */
#define _GNU_SOURCE
#include "capture.h"
#include "protocol.h"
#include "aragatz_hw.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define BUSY (1U << 4)
#define AXI_BUSY (1U << 8)
#define FIFO_EMPTY (1U << 9)
#define C(off) cap[(off) / 4U]
#define G(off) gen[(off) / 4U]
static uint32_t cap[64], gen[32];
static uint32_t writes, cap_starts, cap_stops, gen_starts, gen_stops;
static uint32_t flush_calls, invalidate_calls, flushed, invalidated;
static uint32_t barriers;
static int stale_start, reject_buffer;
static uint64_t now_us;
static uint64_t invalidate_cost_us, crc_cost_us;
static uint8_t *memory;

/* GNU ld --wrap only in the host test: execute the real CRC and advance
 * the mock clock by its assigned cost while the production poll is busy. */
uint32_t __real_ar_crc32_update(uint32_t crc, const uint8_t *p, size_t n);
uint32_t __wrap_ar_crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    uint32_t result = __real_ar_crc32_update(crc, p, n);
    now_us += crc_cost_us;
    return result;
}

static uint32_t current_state(void) { return C(CAP_STATUS) & 15U; }

uint32_t Xil_In32(UINTPTR address)
{
    assert(!(address & 3U));
    if (address >= CAP_BASE && address < CAP_BASE + sizeof(cap))
        return C(address - CAP_BASE);
    if (address >= GEN_BASE && address < GEN_BASE + sizeof(gen))
        return G(address - GEN_BASE);
    assert(!"Unexpected MMIO read");
    return 0;
}

void Xil_Out32(UINTPTR address, uint32_t value)
{
    uint32_t off;
    assert(!(address & 3U));
    ++writes;
    if (address >= CAP_BASE && address < CAP_BASE + sizeof(cap)) {
        off = (uint32_t)(address - CAP_BASE);
        if (off == CAP_COMMAND) {
            if (value == CAP_CMD_START) {
                ++cap_starts;
                assert(current_state() == CAP_STATE_IDLE);
                assert(flushed == C(CAP_BUFFER_BYTES) + ARAGATZ_GUARD_BYTES);
                assert(C(CAP_BUFFER_BYTES) == ar_capture_meta()->payload_bytes);
                C(CAP_STATUS) = stale_start ? CAP_STATE_END | FIFO_EMPTY : CAP_STATE_FILL | BUSY;
                C(CAP_RUN_FRAGMENTS) = 0;
            } else if (value == CAP_CMD_STOP) {
                ++cap_stops;
                /* AFTER cannot consume STOP: reject mistaken software order. */
                assert(current_state() != CAP_STATE_AFTER);
                if (current_state() == CAP_STATE_END) C(CAP_STATUS) = CAP_STATE_IDLE | FIFO_EMPTY;
                /* An active acquisition drains only when the test says so. */
            } else if (value == CAP_CMD_CLEAR_STATS) {
                assert(current_state() == CAP_STATE_IDLE || current_state() == CAP_STATE_END);
                C(CAP_FRAGMENTS_TOTAL) = C(CAP_TRIG_ACCEPTED) = C(CAP_TRIG_ACCEPTED_HI) = 0;
                C(CAP_TRIG_MISSED) = C(CAP_TRIG_MISSED_HI) = 0;
                C(CAP_FIFO_OVERFLOWS) = C(CAP_AXI_ERRORS) = 0;
            } else assert(!"Unexpected/combined capture command");
        } else if (!(reject_buffer && off == CAP_BUFFER_BYTES)) C(off) = value;
        return;
    }
    if (address >= GEN_BASE && address < GEN_BASE + sizeof(gen)) {
        off = (uint32_t)(address - GEN_BASE);
        if (off == GEN_COMMAND) {
            if (value == GEN_CMD_START) {
                ++gen_starts;
                assert(current_state() == CAP_STATE_FILL);
                assert(C(CAP_STATUS) & BUSY);
                G(GEN_STATUS) = 1U;
                C(CAP_STATUS) = CAP_STATE_XFER | BUSY | AXI_BUSY;
            } else if (value == GEN_CMD_STOP) {
                ++gen_stops;
                assert(!(C(CAP_STATUS) & AXI_BUSY));
                assert(current_state() == CAP_STATE_IDLE || current_state() == CAP_STATE_END ||
                       current_state() == CAP_STATE_ERROR);
                G(GEN_STATUS) = 0;
            } else if (value == GEN_CMD_CLEAR_STATS) {
                assert(!(G(GEN_STATUS) & 1U));
                G(GEN_STATUS) = 0;
            } else assert(!"Unexpected/combined generator command");
        } else G(off) = value;
        return;
    }
    assert(!"Unexpected MMIO write");
}

void Xil_DCacheFlushRange(INTPTR address, uint32_t length)
{
    assert(current_state() == CAP_STATE_IDLE && !(C(CAP_STATUS) & (BUSY | AXI_BUSY)));
    assert(!((UINTPTR)address & 31U) && !(length & 31U));
    assert(length && length <= 4096U);
    assert((UINTPTR)address == ARAGATZ_TEST_DDR_BASE + flushed);
    assert(flushed + length <= ARAGATZ_TEST_DDR_SIZE);
    ++flush_calls;
    flushed += length;
}

void Xil_DCacheInvalidateRange(INTPTR address, uint32_t length)
{
    assert(current_state() == CAP_STATE_END && !(C(CAP_STATUS) & (BUSY | AXI_BUSY)));
    assert(G(GEN_STATUS) == 0);
    assert(!((UINTPTR)address & 31U) && !(length & 31U));
    assert(length && length <= 4096U);
    assert((UINTPTR)address == ARAGATZ_TEST_DDR_BASE + invalidated);
    assert(invalidated + length <= ARAGATZ_TEST_DDR_SIZE);
    ++invalidate_calls;
    invalidated += length;
    now_us += invalidate_cost_us;
}

void test_capture_dsb(void) { ++barriers; }

/* Independent bitwise reference, deliberately different from production's
 * table implementation and the incremental scheduling in capture.c. */
static uint32_t reference_crc(const uint8_t *p, size_t bytes)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    unsigned bit;
    for (i = 0; i < bytes; ++i) {
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static void tick(void) { now_us += 100U; ar_capture_poll(now_us); }

static void until_generator(void)
{
    unsigned i;
    for (i = 0; i < 1024U && !(G(GEN_STATUS) & 1U); ++i) {
        assert(ar_capture_state() != AR_CAPTURE_FAULT);
        tick();
    }
    assert(G(GEN_STATUS) & 1U);
    assert(ar_capture_state() == AR_CAPTURE_CAPTURING);
    assert(invalidate_calls == 0);
}

static void until_state(enum ar_capture_state want)
{
    unsigned i;
    for (i = 0; i < 2048U && ar_capture_state() != want; ++i) {
        if (want != AR_CAPTURE_FAULT) assert(ar_capture_state() != AR_CAPTURE_FAULT);
        tick();
    }
    assert(ar_capture_state() == want);
}

static void setup(void)
{
    void *wanted = (void *)(UINTPTR)ARAGATZ_TEST_DDR_BASE;
    memory = mmap(wanted, ARAGATZ_TEST_DDR_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (memory == MAP_FAILED) { perror("mmap test reserve"); exit(2); }
    assert(memory == wanted);
    memset(memory, 0xCC, ARAGATZ_TEST_DDR_SIZE);
    C(CAP_VERSION) = G(GEN_VERSION) = ARAGATZ_IP_VERSION;
    C(CAP_STATUS) = CAP_STATE_IDLE | FIFO_EMPTY;
}

static void begin_normal(void)
{
    assert(ar_capture_init() == 0);
    assert(ar_capture_begin(UINT64_C(0x123456789ABC), 64U, 16U, 2U, now_us) == 0);
    until_generator();
}

static void complete_hardware(void)
{
    const struct ar_capture_metadata *m = ar_capture_meta();
    uint32_t f, frame, word, one = 128U + m->frames * 768U;
    assert(current_state() == CAP_STATE_XFER);
    assert(G(GEN_STATUS) & 1U);
    for (f = 0; f < m->fragments; ++f) {
        uint32_t *p = (uint32_t *)(memory + f * one);
        uint64_t *q = (uint64_t *)p;
        uint32_t trigger = m->gen_first + f * m->gen_period;
        p[0] = ARAGATZ_HEADER_ID;
        p[1] = f;
        q[1] = UINT64_C(10000000) + f * UINT64_C(500000);
        q[2] = (UINT64_C(0x54524752) << 32) | trigger;
        p[6] = ARAGATZ_AUX0; p[7] = ARAGATZ_AUX1;
        memset(p + 8, 0, 6 * sizeof(uint32_t));
        for (word = 14; word < 32; ++word) p[word] = 0x12345678U;
        q += 16;
        for (frame = 0; frame < m->frames; ++frame)
            for (word = 0; word < 96U; ++word)
                *q++ = ((uint64_t)(trigger - m->prev + frame) << 32) | word;
    }
    C(CAP_STATUS) = CAP_STATE_END | FIFO_EMPTY; /* bit7 and FRAMES_CAPTURED are ZERO */
    C(CAP_FRAMES_CAPTURED) = 0;
    C(CAP_FRAMES_COMMITTED) = m->frames;
    C(CAP_RUN_FRAGMENTS) = C(CAP_FRAGMENTS_TOTAL) = C(CAP_TRIG_ACCEPTED) = m->fragments;
    C(CAP_LAST_FRAMES) = m->frames;
    C(CAP_LAST_BYTES) = one;
    C(CAP_LAST_STATUS) = 1;
    C(CAP_LAST_ADDR) = ARAGATZ_TEST_DDR_BASE + one * (m->fragments - 1U);
    C(CAP_CURRENT_ADDR) = ARAGATZ_TEST_DDR_BASE + (uint32_t)m->payload_bytes;
    C(CAP_LAST_TRIG_SOURCE) = 1;
    C(CAP_LAST_TRIG_DATA_HI) = ARAGATZ_TRIGGER_MAGIC;
    C(CAP_LAST_TRIG_DATA_LO) = m->gen_first + (m->fragments - 1U) * m->gen_period;
}

static void test_success(void)
{
    const struct ar_capture_metadata *m;
    struct ar_capture_metadata saved;
    uint32_t before;
    assert(ar_capture_init() == 0);
    before = writes;
    assert(ar_capture_begin(0, 64, 16, 2, 0) == -1);
    assert(ar_capture_begin(1, 1, 0, 1, 0) == -1);
    assert(ar_capture_begin(1, 64, 64, 1, 0) == -1);
    assert(ar_capture_begin(1, 128, 65, 1, 0) == -1);
    assert(ar_capture_begin(1, 64, 16, 17, 0) == -1);
    assert(ar_capture_begin(1, UINT32_MAX, 0, UINT32_MAX, 0) == -1);
    assert(ar_capture_begin(1, 131072, 0, 1, 0) == -1);
    assert(writes == before && ar_capture_state() == AR_CAPTURE_IDLE);
    assert(ar_capture_begin(UINT64_C(0x123456789ABC), 64, 16, 2, now_us) == 0);
    until_generator();
    assert(cap_starts == 1 && gen_starts == 1);
    assert(ar_capture_begin(2, 64, 16, 2, now_us) == -1);
    C(CAP_STATUS) |= (1U << 7); /* Input complete while writer still has BRESPs. */
    tick();
    assert(ar_capture_state() == AR_CAPTURE_CAPTURING && invalidate_calls == 0);
    complete_hardware();
    until_state(AR_CAPTURE_READY);
    m = ar_capture_meta();
    assert(m->payload_bytes == 2U * (128U + 64U * 768U));
    assert(m->guard_errors == 0 && m->last_frames == 64U);
    assert(flushed == m->payload_bytes + ARAGATZ_GUARD_BYTES && flushed == invalidated);
    assert(flush_calls > 1 && invalidate_calls > 1 && barriers >= 2);
    assert(m->payload_crc32 == reference_crc(m->data, (size_t)m->payload_bytes));
    assert(reference_crc((const uint8_t *)"123456789", 9) == 0xCBF43926U);
    assert(ar_crc32_update(0, (const uint8_t *)"123456789", 9) == 0xCBF43926U);
    saved = *m;
    before = writes;
    now_us += UINT64_C(86400000000); /* Socket/peer absence never frees READY. */
    ar_capture_poll(now_us);
    assert(ar_capture_state() == AR_CAPTURE_READY && writes == before);
    assert(memcmp(&saved, m, sizeof(saved)) == 0);
    assert(ar_capture_verified(m->id + 1U, m->payload_crc32, (uint32_t)m->payload_bytes) == -1);
    assert(ar_capture_verified(m->id, m->payload_crc32 ^ 1U, (uint32_t)m->payload_bytes) == -1);
    assert(ar_capture_verified(m->id, m->payload_crc32, (uint32_t)m->payload_bytes - 1U) == -1);
    assert(ar_capture_begin(3, 64, 16, 2, now_us) == -1);
    assert(ar_capture_verified(m->id, m->payload_crc32, (uint32_t)m->payload_bytes) == 0);
    assert(ar_capture_verified(m->id, m->payload_crc32, (uint32_t)m->payload_bytes) == 0);
    assert(ar_capture_state() == AR_CAPTURE_VERIFIED);
    assert(ar_capture_begin(saved.id, 64, 16, 2, now_us) == -1);
    flushed = invalidated = flush_calls = invalidate_calls = 0;
    assert(ar_capture_begin(saved.id + 1U, 64, 16, 2, now_us) == 0);
    assert(current_state() == CAP_STATE_IDLE && cap_stops == 1U); /* old END explicitly left */
    until_generator();
    complete_hardware();
    until_state(AR_CAPTURE_READY);
    saved = *ar_capture_meta();
    before = writes;
    assert(ar_capture_release() == -1); /* No application verification yet. */
    assert(ar_capture_verified(saved.id, saved.payload_crc32,
                               (uint32_t)saved.payload_bytes) == 0);
    assert(ar_capture_release() == 0 && ar_capture_state() == AR_CAPTURE_IDLE);
    assert(ar_capture_meta()->id == saved.id && ar_capture_meta()->payload_bytes == 0);
    assert(ar_capture_meta()->frames == 0 && ar_capture_meta()->payload_crc32 == 0);
    assert(writes == before); /* ACK changes ownership metadata, never DDR/MMIO. */
    assert(ar_capture_release() == -1);
}

static void test_tiny_timing(void)
{
    uint64_t cache_start, crc_start;
    const struct ar_capture_metadata *m;
    assert(ar_capture_init() == 0);
    assert(ar_capture_begin(0, 2U, 0U, 1U, now_us) == -1 && writes == 0);
    assert(ar_capture_begin(1, 2U, 0U, 1U, now_us) == 0);
    until_generator();
    complete_hardware();
    m = ar_capture_meta();
    assert(m->payload_bytes == 1664U && m->payload_bytes + ARAGATZ_GUARD_BYTES <= 4096U);
    invalidate_cost_us = 700U;
    crc_cost_us = 900U;
    until_state(AR_CAPTURE_CACHE);
    cache_start = now_us;
    tick(); /* one and only invalidate chunk consumes 700 us after timestamp */
    assert(invalidate_calls == 1 && ar_capture_state() == AR_CAPTURE_CACHE);
    assert(m->cache_us == 0 && now_us == cache_start + 800U);
    tick(); /* completion observes the final chunk's cost */
    assert(ar_capture_state() == AR_CAPTURE_CRC && m->cache_us == 900U);
    crc_start = now_us;
    tick(); /* one CRC chunk consumes 900 us after timestamp */
    assert(ar_capture_state() == AR_CAPTURE_CRC && m->crc_us == 0);
    assert(now_us == crc_start + 1000U);
    tick();
    assert(ar_capture_state() == AR_CAPTURE_READY && m->crc_us == 1100U);
    assert(m->cache_us >= invalidate_cost_us && m->crc_us >= crc_cost_us);
    assert(m->payload_crc32 == reference_crc(m->data, (size_t)m->payload_bytes));
}

static void test_fault_case(const char *which)
{
    uint32_t before, saved_status;
    if (!strcmp(which, "init_active")) {
        C(CAP_STATUS) = CAP_STATE_XFER | BUSY | AXI_BUSY;
        G(GEN_STATUS) = 1;
        assert(ar_capture_init() == -1);
        ar_capture_poll(UINT64_C(100000000));
        assert(writes == 0 && G(GEN_STATUS) == 1);
    } else if (!strcmp(which, "version")) {
        C(CAP_VERSION) ^= 1;
        assert(ar_capture_init() == -1 && writes == 0);
    } else if (!strcmp(which, "stale_end")) {
        C(CAP_STATUS) = CAP_STATE_END | FIFO_EMPTY;
        stale_start = 1;
        assert(ar_capture_init() == 0);
        assert(ar_capture_begin(1, 64, 16, 2, now_us) == 0);
        until_state(AR_CAPTURE_FAULT);
        assert(cap_stops == 1 && cap_starts == 1 && gen_starts == 0 && invalidate_calls == 0);
    } else if (!strcmp(which, "readback")) {
        reject_buffer = 1;
        assert(ar_capture_init() == 0);
        assert(ar_capture_begin(1, 64, 16, 2, now_us) == 0);
        until_state(AR_CAPTURE_FAULT);
        assert(cap_starts == 0 && gen_starts == 0 && invalidate_calls == 0);
    } else {
        begin_normal();
        if (!strcmp(which, "error_bit12")) {
            C(CAP_ERRORS) = 1U << 12;
            before = gen_stops;
            tick();
            assert(cap_stops == 1 && gen_stops == before && G(GEN_STATUS) == 1);
            C(CAP_STATUS) = CAP_STATE_IDLE | FIFO_EMPTY;
            C(CAP_ERRORS) = 0;
            tick();
            assert(gen_stops == before + 1U && ar_capture_meta()->cap_errors == (1U << 12));
            assert(invalidate_calls == 0);
        } else if (!strcmp(which, "partial_stop")) {
            C(CAP_STATUS) = CAP_STATE_IDLE | FIFO_EMPTY;
            C(CAP_LAST_STATUS) = 2;
            tick();
            assert(invalidate_calls == 0 && ar_capture_meta()->last_status == 2);
        } else if (!strcmp(which, "timeout_abort") || !strcmp(which, "abort_after")) {
            before = gen_stops;
            if (!strcmp(which, "abort_after")) C(CAP_STATUS) = CAP_STATE_AFTER | BUSY;
            saved_status = C(CAP_STATUS);
            now_us += UINT64_C(6000000);
            ar_capture_poll(now_us);
            assert(ar_capture_state() == AR_CAPTURE_FAULT && gen_stops == before);
            if (!strcmp(which, "abort_after")) {
                assert(cap_stops == 0);
                C(CAP_STATUS) = CAP_STATE_FILL | BUSY;
                tick();
                assert(cap_stops == 1);
            } else assert(cap_stops == 1);
            tick();
            assert(cap_stops == 1 && gen_stops == before); /* no repeated STOP */
            C(CAP_STATUS) = CAP_STATE_AFTER | BUSY;
            tick();
            assert(gen_stops == before);
            C(CAP_STATUS) = CAP_STATE_IDLE | FIFO_EMPTY;
            tick();
            assert(gen_stops == before + 1U && ar_capture_meta()->cap_status == saved_status);
            assert(invalidate_calls == 0);
        } else {
            complete_hardware();
            if (!strcmp(which, "bad_end")) C(CAP_LAST_BYTES) -= 128U;
            else if (!strcmp(which, "missed_trigger")) C(CAP_TRIG_MISSED) = 1;
            else if (!strcmp(which, "guard")) memory[ar_capture_meta()->payload_bytes] ^= 1U;
            else if (!strcmp(which, "restart_cache")) {
                until_state(AR_CAPTURE_CACHE);
                C(CAP_STATUS) = CAP_STATE_XFER | BUSY | AXI_BUSY;
            } else if (!strcmp(which, "restart_ready")) {
                until_state(AR_CAPTURE_READY);
                C(CAP_STATUS) = CAP_STATE_XFER | BUSY | AXI_BUSY;
            } else assert(!"Unknown test case");
            until_state(AR_CAPTURE_FAULT);
            if (!strcmp(which, "guard")) assert(ar_capture_meta()->guard_errors == 1);
            if (!strcmp(which, "bad_end") || !strcmp(which, "missed_trigger"))
                assert(invalidate_calls == 0);
        }
    }
    assert(ar_capture_state() == AR_CAPTURE_FAULT);
    assert(strlen(ar_capture_fault_reason()) > 0);
    before = writes;
    assert(ar_capture_init() == -1); /* re-init is not an implicit recovery */
    assert(ar_capture_begin(999, 64, 16, 2, now_us) == -1);
    assert(writes == before && ar_capture_state() == AR_CAPTURE_FAULT);
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    setup();
    if (!strcmp(argv[1], "success")) test_success();
    else if (!strcmp(argv[1], "tiny_timing")) test_tiny_timing();
    else test_fault_case(argv[1]);
    assert(munmap(memory, ARAGATZ_TEST_DDR_SIZE) == 0);
    printf("PASS capture host contract: %s\n", argv[1]);
    return 0;
}
