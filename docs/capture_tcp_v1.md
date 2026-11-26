# Capture TCP v1 (reconstruction, not hardware validated)

All protocol integers are unsigned 32-bit big-endian. Each request is eight words:
magic 0x41524743 (ARGC), version 1, opcode, capture ID, frames, prev, fragments,
CRC32 of the preceding 28 bytes. Opcodes START=1, FETCH=2, ACK=3, STATUS=4.
START requires ID=0; ACK uses frames=payload length, prev=payload CRC, fragments=0.
FETCH and STATUS unused parameters must be zero.

Every reply is sixteen words: magic 0x4152475a (ARGZ), version=1, header bytes=64,
result (0 OK, 1 invalid, 2 busy, 3 hardware fault, 4 not found), state
(0 empty, 1 acquiring, 2 retained, 3 fault), capture ID, frames, prev, fragments,
payload length, payload CRC, first trigger frame, trigger period, CAP status,
CAP errors, header CRC32 over preceding 60 bytes.
Only successful START/FETCH replies carry payload immediately after header.
STATUS and ACK never carry payload (length describes retained capture).
CRC32 is standard reflected IEEE (zlib.crc32). Payload is raw little-endian DDR.

One connection, one outstanding request; close and reconnect after protocol
or transport failure. A disconnected capture continues; use STATUS then FETCH
with retained ID. ACK must match ID, length and CRC; only then can START reuse
DDR. IDs are session-local (reset invalidates them). No authentication:
use only an isolated trusted bench network.

This reconstructed protocol is intentionally 64 bytes, not the lost draft's
192 bytes; clients and firmware must come from the same revision.
