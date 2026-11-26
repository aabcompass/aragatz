#!/usr/bin/env python3
"""Receive and verify finite Aragatz captures over raw TCP v1 (stdlib only)."""

import argparse
import datetime
import errno
import json
import math
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import time
import zlib

START, FETCH, ACK, STATUS = 1, 2, 3, 4
OK, INVALID, BUSY, HWFAULT, NOTFOUND = range(5)
EMPTY, ACQUIRING, RETAINED, FAULT = range(4)
VERSION, HEADER_BYTES, REQUEST_BYTES = 1, 64, 32
CMD_MAGIC, RESP_MAGIC = 0x41524743, 0x4152475A
DDR_BYTES, GUARD_BYTES = 1048576, 256
MAX_PAYLOAD = DDR_BYTES - GUARD_BYTES
MAX_FRAGMENTS = 16
KEYS = ("magic version header_bytes result state id frames prev fragments "
        "length crc first period cap_status cap_errors header_crc").split()
IDENTITY_KEYS = "id frames prev fragments length crc first period".split()
RESULT_NAMES = {OK: "OK", INVALID: "invalid request", BUSY: "busy",
                HWFAULT: "hardware fault", NOTFOUND: "not found"}


class BoardError(RuntimeError):
    def __init__(self, response):
        self.response = response
        super().__init__(f"Board: {RESULT_NAMES[response['result']]} "
                         f"(state={response['state']}, id={response['id']}, "
                         f"CAP_STATUS=0x{response['cap_status']:08x}, "
                         f"CAP_ERRORS=0x{response['cap_errors']:08x})")


class UnacknowledgedCapture(RuntimeError):
    """The client deliberately did not send an application ACK."""


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def _u32(value, name):
    if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
        raise ValueError(f"{name} must be an unsigned 32-bit integer")
    return value


def geometry(frames, prev, fragments):
    for value, name in ((frames, "frames"), (prev, "prev"),
                        (fragments, "fragments")):
        _u32(value, name)
    length = (128 + 768 * frames) * fragments
    if not (2 <= frames <= 131072 and prev <= 64 and prev < frames and
            1 <= fragments <= MAX_FRAGMENTS and length <= MAX_PAYLOAD):
        raise ValueError("Invalid capture geometry: frames >= 2, prev <= 64 "
                         "and prev < frames, 1..16 fragments, payload plus "
                         "256-byte guard must fit 1 MiB")
    return length


def request(op, ident=0, frames=0, prev=0, fragments=0):
    values = (op, ident, frames, prev, fragments)
    for value, name in zip(values, ("opcode", "id", "frames", "prev", "fragments")):
        _u32(value, name)
    if op == START:
        if ident:
            raise ValueError("START requires id=0")
        geometry(frames, prev, fragments)
    elif op == FETCH:
        if not ident or frames or prev or fragments:
            raise ValueError("FETCH requires a nonzero id and zero arguments")
    elif op == ACK:
        if not ident or not 0 < frames <= MAX_PAYLOAD or fragments:
            raise ValueError("ACK requires id, payload length, CRC and fragments=0")
    elif op == STATUS:
        if any(values[1:]):
            raise ValueError("STATUS requires zero arguments")
    else:
        raise ValueError("Unknown opcode")
    raw = struct.pack(">7I", CMD_MAGIC, VERSION, *values)
    return raw + struct.pack(">I", crc32(raw))


def read_exact(sock, count):
    if type(count) is not int or not 0 <= count <= DDR_BYTES:
        raise ValueError("Invalid receive size")
    result = bytearray()
    while len(result) < count:
        block = sock.recv(min(65536, count - len(result)))
        if not block:
            raise EOFError(f"TCP closed after {len(result)} of {count} bytes")
        result.extend(block)
    return bytes(result)


def header(raw):
    if len(raw) != HEADER_BYTES:
        raise ValueError("Expected a 64-byte response header")
    h = dict(zip(KEYS, struct.unpack(">16I", raw)))
    if (h["magic"], h["version"], h["header_bytes"]) != (RESP_MAGIC, VERSION, HEADER_BYTES):
        raise ValueError("Unexpected protocol (expected ARAGATZ v1, 64-byte header)")
    if crc32(raw[:60]) != h["header_crc"]:
        raise ValueError("Header CRC mismatch")
    if h["result"] not in RESULT_NAMES or h["state"] not in range(4):
        raise ValueError("Unknown response result or capture state")
    if h["length"] > MAX_PAYLOAD:
        raise ValueError("Payload exceeds the reserved DDR range minus its guard")
    return h


def metadata_header(metadata):
    """Recheck the wire header CRC in saved JSON, including legacy flat JSON."""
    try:
        raw = struct.pack(">16I", *(_u32(metadata[k], k) for k in KEYS))
    except (KeyError, TypeError, struct.error) as exc:
        raise ValueError("Invalid saved metadata") from exc
    return header(raw)


def exchange(sock, op, *args):
    sock.sendall(request(op, *args))
    h = header(read_exact(sock, HEADER_BYTES))
    if h["result"] != OK:
        raise BoardError(h)
    return h


def validate_capture_header(h, ident=None, expected=None):
    if h["result"] != OK or not h["id"] or h["state"] != RETAINED:
        raise ValueError("Reply does not describe a retained successful capture")
    length = geometry(h["frames"], h["prev"], h["fragments"])
    if h["length"] != length:
        raise ValueError("Capture length does not match its geometry")
    if h["cap_errors"] or h["cap_status"] & 15 != 6 or h["cap_status"] & (16 | 256):
        raise ValueError("Capture is not in clean END with capture and AXI idle")
    if h["first"] != 128 or h["period"] != max(512, h["frames"] + 128):
        raise ValueError("Unexpected trigger schedule")
    if ident is not None and h["id"] != ident:
        raise ValueError(f"Capture ID mismatch: {h['id']} != {ident}")
    if expected is not None:
        for key in IDENTITY_KEYS:
            if h[key] != expected[key]:
                raise ValueError(f"Retained capture metadata changed: {key}")


def verify(data, h):
    validate_capture_header(h)
    if len(data) != h["length"] or crc32(data) != h["crc"]:
        raise ValueError("Payload length or CRC mismatch")
    frames, prev, fragments = (h[k] for k in ("frames", "prev", "fragments"))
    stride = 128 + 768 * frames
    view = memoryview(data)
    last_ts = -1
    for frag in range(fragments):
        base = frag * stride
        trigger = h["first"] + frag * h["period"]
        words = struct.unpack_from("<16Q", view, base)
        expected = ((frag << 32) | 0xC0DEC035,
                    (0x54524752 << 32) | trigger, 0x5566778811223344)
        if (words[0], words[2], words[3]) != expected or words[1] <= last_ts:
            raise ValueError(f"Fragment {frag}: preamble or timestamp mismatch")
        if words[4:7] != (0,) * 3 or words[7:] != (0x1234567812345678,) * 9:
            raise ValueError(f"Fragment {frag}: AUX/padding mismatch")
        last_ts = words[1]
        for i, (actual,) in enumerate(struct.iter_unpack("<Q", view[base + 128:base + stride])):
            frame, word = divmod(i, 96)
            expected_word = ((trigger - prev + frame) << 32) | word
            if actual != expected_word:
                raise ValueError(f"Fragment {frag}, frame {frame}, word {word}: "
                                 f"{actual:016x} != {expected_word:016x}")
    return fragments * frames * 96


def rates(byte_count, seconds):
    if seconds <= 0:
        return {"MB_per_s": None, "MiB_per_s": None, "Mbit_per_s": None}
    value = byte_count / seconds
    return {"MB_per_s": value / 1e6, "MiB_per_s": value / (1024 * 1024),
            "Mbit_per_s": value * 8 / 1e6}


def _interval_metrics(byte_count, **intervals):
    result = {"bytes": byte_count, **intervals}
    result["rates"] = {name: rates(byte_count, value) for name, value in intervals.items()
                       if name in ("payload_seconds", "request_seconds", "verified_seconds")}
    return result


def receive_capture(sock, frames=None, prev=None, fragments=None, ident=None, expected=None):
    start = time.perf_counter()
    if ident is None:
        h = exchange(sock, START, 0, frames, prev, fragments)
    else:
        h = exchange(sock, FETCH, ident)
    header_done = time.perf_counter()
    validate_capture_header(h, ident, expected)
    if ident is None and (h["frames"], h["prev"], h["fragments"]) != (frames, prev, fragments):
        raise ValueError("START reply geometry differs from the request")
    data = read_exact(sock, h["length"])
    data_done = time.perf_counter()
    try:
        words = verify(data, h)
    except ValueError as exc:
        raise UnacknowledgedCapture(f"Capture {h['id']} failed verification; no ACK sent: {exc}") from exc
    verified = time.perf_counter()
    metrics = _interval_metrics(len(data), payload_seconds=data_done - header_done,
                               request_seconds=data_done - start,
                               verification_seconds=verified - data_done,
                               verified_seconds=verified - start)
    metrics.update(id=h["id"], words=words)
    return h, data, metrics


def metadata_path(output):
    path = Path(output)
    return path.with_suffix(path.suffix + ".json")


def preflight_paths(paths, overwrite=False):
    normalized = [Path(p).resolve() for p in paths]
    if len(set(normalized)) != len(normalized):
        raise ValueError("Output and report paths must be different")
    for path in normalized:
        if path.exists() and not overwrite:
            raise FileExistsError(f"Refusing to overwrite {path}; choose a new name or --overwrite")
        if path.is_dir():
            raise IsADirectoryError(str(path))
        path.parent.mkdir(parents=True, exist_ok=True)


def _fsync_parent(path):
    if os.name == "nt":
        return  # File data is fsynced; Python cannot portably fsync a Windows directory.
    fd = os.open(str(path.parent), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def atomic_write(path, data, overwrite=False):
    """Publish a complete fsynced file; fail atomically if no-clobber output exists.

    The no-clobber path uses a same-directory hard link (Linux/NTFS). An unsupported
    filesystem fails before capture ACK; choose a local filesystem in that case.
    """
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix="." + path.name + ".", suffix=".tmp",
                                         dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if overwrite:
            os.replace(temporary, path)
            temporary = None
        else:
            try:
                os.link(temporary, path)
            except OSError as exc:
                if exc.errno in (errno.EPERM, errno.ENOTSUP, errno.EOPNOTSUPP):
                    raise OSError("Output filesystem does not support atomic no-clobber "
                                  "publication; use a local Linux/NTFS filesystem") from exc
                raise
        _fsync_parent(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_json(path, value, overwrite=False):
    atomic_write(path, (json.dumps(value, indent=2, allow_nan=False) + "\n").encode("utf-8"), overwrite)


def load_saved(path):
    path = Path(path)
    meta = metadata_path(path)
    if path.stat().st_size > MAX_PAYLOAD or meta.stat().st_size > 65536:
        raise ValueError("Saved capture or metadata is too large")
    h = metadata_header(json.loads(meta.read_text(encoding="utf-8")))
    data = path.read_bytes()
    return h, data, verify(data, h)


def save_capture(output, h, data, metrics, overwrite=False):
    output = Path(output)
    meta = dict(h)
    meta["capture_metadata"] = {
        "schema": "aragatz.capture.v1",
        "saved_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "metrics": metrics,
    }
    atomic_write(output, data, overwrite)
    write_json(metadata_path(output), meta, overwrite)
    saved_h, saved_data, count = load_saved(output)
    if saved_h != h or saved_data != data:
        raise ValueError("Saved capture changed during disk readback")
    return count


def acknowledge(sock, h):
    # The server requires successful DATA delivery on this same connection.
    response = exchange(sock, ACK, h["id"], h["length"], h["crc"])
    if response["id"] != h["id"] or response["state"] != EMPTY or any(
            response[key] for key in ("frames", "prev", "fragments", "length", "crc", "period")):
        raise ValueError("Unexpected ACK response; query STATUS before another capture")
    return response


def capture(sock, args, ident=None):
    overwrite = getattr(args, "overwrite", False)
    preflight_paths([args.output, metadata_path(args.output)], overwrite)
    start = time.perf_counter()
    h, data, metrics = receive_capture(sock, args.frames, args.prev, args.fragments, ident)
    try:
        saved = time.perf_counter()
        save_capture(args.output, h, data, metrics, overwrite)
        metrics["save_and_readback_seconds"] = time.perf_counter() - saved
    except (OSError, ValueError) as exc:
        raise UnacknowledgedCapture(f"Capture {h['id']} was received but not saved/verified; "
                                    f"no ACK sent: {exc}") from exc
    # If ACK transmission/response fails, its acceptance is unknown. Do not label
    # that case 'retained': STATUS after reconnect is the authoritative answer.
    acknowledge(sock, h)
    metrics["cycle_seconds"] = time.perf_counter() - start
    metrics["cycle_rate"] = rates(len(data), metrics["cycle_seconds"])
    metrics["acknowledged"] = True
    return metrics


def bench(sock, args):
    """Capture once, persist/verify once, measure N verified immutable FETCH replies."""
    preflight_paths([args.output, metadata_path(args.output), args.report], args.overwrite)
    start = time.perf_counter()
    h, data, initial = receive_capture(sock, args.frames, args.prev, args.fragments)
    report = {"schema": "aragatz.benchmark.v1", "mode": "retained_fetch",
              "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "host": args.host, "port": args.port, "capture": h,
              "initial_capture": initial, "repeats": args.repeats,
              "samples": [], "acknowledgement": "not_attempted"}
    try:
        save_capture(args.output, h, data, initial, args.overwrite)
        measured_start = time.perf_counter()
        for index in range(args.repeats):
            replay_h, replay, metrics = receive_capture(sock, ident=h["id"], expected=h)
            if replay != data:
                raise ValueError("Retained capture bytes changed between FETCH replies")
            metrics["iteration"] = index + 1
            report["samples"].append(metrics)
            print(f"FETCH {index + 1}/{args.repeats}: "
                  f"{metrics['rates']['request_seconds']['MB_per_s']:.3f} MB/s "
                  "request-to-last-byte; CRC32 and COUNTER PASS", flush=True)
        measured_seconds = time.perf_counter() - measured_start
        byte_count = len(data) * args.repeats
        intervals = {name: sum(item[name] for item in report["samples"])
                     for name in ("payload_seconds", "request_seconds", "verified_seconds")}
        report["aggregate"] = _interval_metrics(byte_count, **intervals)
        report["aggregate"].update(wall_seconds=measured_seconds,
                                   wall_rate=rates(byte_count, measured_seconds))
        # Persist measurements as well as the verified seed before releasing DDR.
        write_json(args.report, report, args.overwrite)
        saved_h, saved_data, _ = load_saved(args.output)
        if saved_h != h or saved_data != data:
            raise ValueError("Saved seed capture changed during benchmark")
    except (OSError, ValueError, EOFError, BoardError, UnacknowledgedCapture) as exc:
        raise UnacknowledgedCapture(f"Benchmark stopped with capture {h['id']}; "
                                    f"no ACK sent: {exc}") from exc
    acknowledge(sock, h)
    report["acknowledgement"] = "accepted"
    report["cycle_seconds"] = time.perf_counter() - start
    # This file was created by this invocation; replace its final ACK status.
    write_json(args.report, report, overwrite=True)
    return report


def _positive_int(value):
    n = int(value, 0)
    if n < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return n


def _timeout(value):
    n = float(value)
    if not math.isfinite(n) or n <= 0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return n


def make_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    for action in ("status", "capture", "fetch", "bench", "echo"):
        p = sub.add_parser(action)
        p.add_argument("host")
        p.add_argument("--port", type=_positive_int, default=7 if action == "echo" else 5001)
        p.add_argument("--timeout", type=_timeout, default=30,
                       help="TCP inactivity timeout in seconds (default 30)")
        if action in ("capture", "fetch", "bench"):
            p.add_argument("--output", default="capture.bin")
            p.add_argument("--overwrite", action="store_true")
        if action in ("capture", "bench"):
            p.add_argument("--frames", type=_positive_int, default=64)
            p.add_argument("--prev", type=int, default=16)
            p.add_argument("--fragments", type=_positive_int, default=16)
        elif action == "fetch":
            p.set_defaults(frames=None, prev=None, fragments=None)
            p.add_argument("id", type=_positive_int)
        if action == "bench":
            p.add_argument("--repeats", type=_positive_int, default=100,
                           help="number of FETCH transfers after one initial capture")
            p.add_argument("--report", default="bench.json")
        if action == "echo":
            p.add_argument("--bytes", type=_positive_int, default=1024)
            p.add_argument("--rounds", type=_positive_int, default=16)
    p = sub.add_parser("verify")
    p.add_argument("file")
    return parser


def main(argv=None):
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        if args.action == "verify":
            h, _, count = load_saved(args.file)
            print(f"Capture {h['id']}: CRC32 and COUNTER PASS; {count} words")
            return 0
        if args.port > 65535:
            raise ValueError("TCP port must be in 1..65535")
        if args.action in ("capture", "bench"):
            geometry(args.frames, args.prev, args.fragments)
        if args.action in ("capture", "fetch", "bench"):
            paths = [args.output, metadata_path(args.output)]
            if args.action == "bench":
                paths.append(args.report)
            preflight_paths(paths, args.overwrite)
        if args.action == "fetch":
            _u32(args.id, "capture id")
        if args.action == "echo" and args.bytes > DDR_BYTES:
            raise ValueError("Echo --bytes must not exceed 1 MiB")
        with socket.create_connection((args.host, args.port), args.timeout) as sock:
            sock.settimeout(args.timeout)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            if args.action == "echo":
                data = (bytes(range(256)) * ((args.bytes + 255) // 256))[:args.bytes]
                for _ in range(args.rounds):
                    sock.sendall(data)
                    if read_exact(sock, len(data)) != data:
                        raise ValueError("Echo data mismatch")
                print(f"Echo PASS: {args.rounds} rounds of {len(data)} bytes")
            elif args.action == "status":
                print(json.dumps(exchange(sock, STATUS), indent=2))
            elif args.action == "bench":
                report = bench(sock, args)
                print(json.dumps(report["aggregate"], indent=2))
                print(f"Report: {args.report}; capture {report['capture']['id']} acknowledged")
            else:
                result = capture(sock, args, args.id if args.action == "fetch" else None)
                print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, EOFError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        print("After a transport/ACK failure, reconnect and query STATUS. "
              "For RETAINED state, FETCH its id to a new output file; "
              "do not reset the board if the buffer is needed.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
