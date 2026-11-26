#ifndef ARAGATZ_CAPTURE_H
#define ARAGATZ_CAPTURE_H

#include <stdint.h>

enum ar_capture_state {
    AR_CAPTURE_IDLE = 0, AR_CAPTURE_PREPARING = 1,
    AR_CAPTURE_CAPTURING = 2, AR_CAPTURE_CACHE = 3,
    AR_CAPTURE_CRC = 4, AR_CAPTURE_READY = 5,
    AR_CAPTURE_VERIFIED = 6, AR_CAPTURE_FAULT = 7
};

struct ar_capture_metadata {
    uint64_t id, payload_bytes, capture_us, cache_us, crc_us, trigger_missed;
    uint32_t payload_crc32, cap_status, cap_errors;
    uint32_t fragments, frames, prev, gen_first, gen_period, gen_word;
    uint32_t rate_num, rate_den, cap_version, gen_version;
    uint32_t completed_fragments, last_frames, last_bytes, last_status;
    uint32_t guard_errors;
    const uint8_t *data;
};

/* All calls are from the single standalone main loop, never an ISR.
 * poll() does bounded work and never waits for hardware or network activity.
 * now_us must be monotonic; it may start at zero. */
int ar_capture_init(void);
int ar_capture_begin(uint64_t id, uint32_t frames, uint32_t prev,
                     uint32_t fragments, uint64_t now_us);
void ar_capture_poll(uint64_t now_us);
enum ar_capture_state ar_capture_state(void);
const struct ar_capture_metadata *ar_capture_meta(void);

/* Verification and release are separate internal steps. server.c accepts
 * the matching application ACK only after that connection fully TCP-ACKed
 * DATA, then calls both. Wire v1 ACK consumes the snapshot; a duplicate ACK
 * after release is rejected. Socket close/timeouts never release a capture. */
int ar_capture_verified(uint64_t id, uint32_t crc, uint32_t bytes);
int ar_capture_release(void);

/* Empty string outside FAULT. FAULT is sticky until a board/software reset;
 * init() is not a recovery API and does not discard an existing snapshot. */
const char *ar_capture_fault_reason(void);

#endif
