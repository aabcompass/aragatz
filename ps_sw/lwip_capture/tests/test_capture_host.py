#!/usr/bin/env python3
"""Build/run the actual capture controller against Linux host-only mocks.

This tests software transitions/cache-call ordering and buffer ownership.
It does not execute ARM cache instructions or claim a board/PHY/link PASS.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CASES = (
    "success", "init_active", "version", "stale_end", "readback",
    "bad_end", "missed_trigger", "partial_stop", "error_bit12", "guard",
    "timeout_abort", "abort_after", "restart_cache", "restart_ready", "tiny_timing",
)


def main():
    with tempfile.TemporaryDirectory(prefix="aragatz-capture-test-") as tmp:
        exe = Path(tmp) / "test_capture"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra",
            "-Werror", "-Wconversion", "-Wshadow", "-fno-pie", "-no-pie",
            "-Wl,--defsym,__aragatz_app_ddr_end=0x10000000",
            "-Wl,--defsym,__aragatz_capture_start=0x10000000",
            "-Wl,--defsym,__aragatz_capture_end=0x10100000",
            "-Wl,--wrap=ar_crc32_update",
            "-I", str(ROOT / "ps_sw/lwip_capture/tests/capture_stubs"),
            "-I", str(ROOT / "ps_sw/baremetal/include"),
            "-I", str(ROOT / "ps_sw/lwip_capture"),
            str(ROOT / "ps_sw/lwip_capture/tests/test_capture.c"),
            str(ROOT / "ps_sw/lwip_capture/capture.c"),
            str(ROOT / "ps_sw/lwip_capture/protocol.c"),
            "-o", str(exe),
        ], check=True)
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=10)
    print(f"{len(CASES)} host capture scenarios passed; no target hardware was exercised.")


if __name__ == "__main__":
    main()
