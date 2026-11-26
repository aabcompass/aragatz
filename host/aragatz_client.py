#!/usr/bin/env python3
"""Aragatz reconstructed TCP v1, standard library only."""
import argparse
import json
import pathlib
import socket
import struct
import time
import zlib

START, FETCH, ACK, STATUS = 1, 2, 3, 4
KEYS = "magic version header_bytes result state id frames prev fragments length crc first period cap_status cap_errors header_crc".split()

def crc32(data):
    return zlib.crc32(data) & 0xffffffff

def request(op, ident=0, frames=0, prev=0, fragments=0):
    raw = struct.pack(">7I", 0x41524743, 1, op, ident, frames, prev, fragments)
    return raw + struct.pack(">I", crc32(raw))

def read_exact(sock, count):
    result = bytearray()
    while len(result) < count:
        block = sock.recv(min(65536, count-len(result)))
        if not block:
            raise EOFError("TCP closed before complete reply")
        result.extend(block)
    return bytes(result)

def header(raw):
    h = dict(zip(KEYS, struct.unpack(">16I", raw)))
    if (h["magic"], h["version"], h["header_bytes"]) != (0x4152475a, 1, 64):
        raise ValueError("Unexpected protocol")
    if crc32(raw[:60]) != h["header_crc"]:
        raise ValueError("Header CRC mismatch")
    if h["length"] > 1048576:
        raise ValueError("Payload exceeds DDR reserve")
    return h

def exchange(sock, op, *args):
    sock.sendall(request(op, *args))
    h = header(read_exact(sock, 64))
    if h["result"]:
        raise RuntimeError(str(h))
    return h

def verify(data, h):
    if len(data) != h["length"] or crc32(data) != h["crc"]:
        raise ValueError("Payload length or CRC mismatch")
    f, p, n = (h[k] for k in ("frames", "prev", "fragments"))
    stride = 128 + 768*f
    if not (2 <= f and 0 <= p <= 64 and p < f and n > 0 and stride*n == len(data)):
        raise ValueError("Invalid capture geometry")
    if h["state"] != 2 or h["cap_errors"] or h["cap_status"] & 15 != 6 or h["cap_status"] & 256:
        raise ValueError("Capture not clean END")
    if h["first"] != 128 or h["period"] != max(512, f+128):
        raise ValueError("Unexpected trigger schedule")
    last_ts = -1
    for frag in range(n):
        base = frag*stride
        trigger = h["first"]+frag*h["period"]
        words = struct.unpack_from("<16Q", data, base)
        expected = ((frag<<32)|0xc0dec035, (0x54524752<<32)|trigger, 0x5566778811223344)
        if (words[0], words[2], words[3]) != expected or words[1] <= last_ts:
            raise ValueError(f"Fragment {frag}: preamble mismatch")
        if words[4:7] != (0,)*3 or words[7:] != (0x1234567812345678,)*9:
            raise ValueError(f"Fragment {frag}: AUX/padding mismatch")
        last_ts = words[1]
        for i, (actual,) in enumerate(struct.iter_unpack("<Q", data[base+128:base+stride])):
            frame, word = divmod(i, 96)
            expected_word = ((trigger-p+frame)<<32)|word
            if actual != expected_word:
                raise ValueError(f"Fragment {frag}, frame {frame}, word {word}: {actual:016x} != {expected_word:016x}")
    return n*f*96

def capture(sock, args, ident=None):
    t0 = time.perf_counter()
    h = exchange(sock, FETCH, ident) if ident is not None else exchange(sock, START, 0, args.frames, args.prev, args.fragments)
    t1 = time.perf_counter()
    if not h["length"]:
        raise ValueError("Empty capture")
    data = read_exact(sock, h["length"])
    t2 = time.perf_counter()
    count = verify(data, h)
    output = pathlib.Path(args.output)
    output.write_bytes(data)
    meta = output.with_suffix(output.suffix+".json")
    meta.write_text(json.dumps(h, indent=2)+"\n")
    verify(output.read_bytes(), json.loads(meta.read_text()))
    exchange(sock, ACK, h["id"], h["length"], h["crc"])
    t3 = time.perf_counter()
    return dict(bytes=len(data), words=count, payload_seconds=t2-t1,
                request_seconds=t2-t0, verified_seconds=t3-t0)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    for action in ("status", "capture", "fetch", "bench", "echo"):
        p = sub.add_parser(action)
        p.add_argument("host")
        p.add_argument("--port", type=int, default=7 if action=="echo" else 5001)
        p.add_argument("--timeout", type=float, default=20)
        if action in ("capture", "fetch", "bench"):
            p.add_argument("--output", default="capture.bin")
            p.add_argument("--frames", type=int, default=64)
            p.add_argument("--prev", type=int, default=16)
            p.add_argument("--fragments", type=int, default=16)
        if action=="fetch":
            p.add_argument("id", type=int)
        if action=="bench":
            p.add_argument("--repeats", type=int, default=100)
            p.add_argument("--report", default="bench.json")
    p = sub.add_parser("verify")
    p.add_argument("file")
    args = parser.parse_args()
    if args.action=="verify":
        path=pathlib.Path(args.file)
        print("Verified words:", verify(path.read_bytes(), json.loads(path.with_suffix(path.suffix+".json").read_text())))
        return
    with socket.create_connection((args.host,args.port),args.timeout) as sock:
        sock.settimeout(args.timeout)
        if args.action=="echo":
            data=bytes(range(256))*4
            sock.sendall(data)
            if read_exact(sock,len(data))!=data:
                raise ValueError("Echo mismatch")
            print("Echo PASS")
        elif args.action=="status":
            print(json.dumps(exchange(sock,STATUS),indent=2))
        elif args.action=="bench":
            if args.repeats<1: parser.error("repeats must be positive")
            results=[]
            for i in range(args.repeats):
                r=capture(sock,args)
                results.append(r)
                print(i+1,r,flush=True)
                pathlib.Path(args.report).write_text(json.dumps(results,indent=2)+"\n")
            for key in ("payload_seconds","request_seconds","verified_seconds"):
                print(key, sum(r["bytes"] for r in results)/sum(r[key] for r in results)/1e6, "MB/s")
        else:
            print(capture(sock,args,args.id if args.action=="fetch" else None))
if __name__=="__main__":
    main()
