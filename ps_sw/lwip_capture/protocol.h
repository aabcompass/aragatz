#ifndef ARAGATZ_PROTOCOL_H
#define ARAGATZ_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

/* Published lwip-capture-v1 format: all fields are big-endian u32.
 * Payload is an unchanged little-endian byte image of the reserved DDR. */
#define AR_PROTOCOL_VERSION 1U
#define AR_COMMAND_BYTES 32U
#define AR_HEADER_BYTES 64U
enum ar_opcode { AR_START=1, AR_FETCH=2, AR_ACK=3, AR_STATUS=4 };
/* These message names are internal; no message-type field is sent on wire. */
enum ar_message { AR_DATA=1, AR_STATUS_REPLY=2, AR_ERROR=3, AR_ACKED=4 };
enum ar_code {
    AR_OK=0, AR_BAD_COMMAND=1, AR_BAD_CONFIG=1,
    AR_BUSY=2, AR_NOT_READY=2, AR_CAPTURE_FAILED=3,
    AR_NO_CAPTURE=4, AR_ID_MISMATCH=4, AR_VERIFY_FAILED=4
};
struct ar_command { uint32_t opcode, id, arg[3]; };
struct ar_capture_metadata;

/* Same incremental convention as zlib.crc32(): begin with zero. */
uint32_t ar_crc32_update(uint32_t crc, const uint8_t *p, size_t n);
int ar_decode_command(struct ar_command *out, const uint8_t wire[AR_COMMAND_BYTES]);
void ar_encode_header(uint8_t out[AR_HEADER_BYTES], uint16_t type, uint16_t code,
    const struct ar_capture_metadata *m, unsigned state, uint32_t link_mbps,
    uint64_t last_tx_us, uint64_t last_tx_acked);
#endif
