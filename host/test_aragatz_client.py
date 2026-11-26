import unittest
import struct
import aragatz_client as a

class Segmented:
    def __init__(self, data):
        self.data=data
    def recv(self,n):
        block=self.data[:min(n,3)]
        self.data=self.data[len(block):]
        return block

def sample():
    f,p,n=4,1,2
    data=bytearray()
    for i in range(n):
        t=128+i*512
        data+=struct.pack("<16Q",(i<<32)|0xc0dec035,100+i,
            (0x54524752<<32)|t,0x5566778811223344,
            *([0]*3),*([0x1234567812345678]*9))
        for j in range(f):
            for w in range(96):
                data+=struct.pack("<Q",((t-p+j)<<32)|w)
    raw=struct.pack(">15I",0x4152475a,1,64,0,2,1,f,p,n,
                    len(data),a.crc32(data),128,512,6,0)
    return bytes(data),raw+struct.pack(">I",a.crc32(raw))

class Tests(unittest.TestCase):
    def test_crc(self):
        self.assertEqual(a.crc32(b"123456789"),0xcbf43926)
    def test_command(self):
        b=a.request(a.START,0,64,16,16)
        self.assertEqual(len(b),32)
        self.assertEqual(struct.unpack(">8I",b),
            (0x41524743,1,1,0,64,16,16,a.crc32(b[:28])))
    def test_segmented(self):
        data,h=sample()
        s=Segmented(h+data)
        self.assertEqual(a.verify(a.read_exact(s,len(h)+len(data))[64:],a.header(h)),768)
    def test_eof(self):
        with self.assertRaises(EOFError):
            a.read_exact(Segmented(b"abc"),4)
    def test_header_corruption(self):
        _,h=sample()
        with self.assertRaises(ValueError):
            a.header(h[:20]+bytes([h[20]^1])+h[21:])
    def test_crc_corruption(self):
        d,r=sample()
        with self.assertRaises(ValueError):
            a.verify(d[:-1]+bytes([d[-1]^1]),a.header(r))
    def test_counter_even_with_matching_crc(self):
        d,r=sample()
        h=a.header(r)
        d=d[:128]+bytes([d[128]^1])+d[129:]
        h["crc"]=a.crc32(d)
        with self.assertRaisesRegex(ValueError,"word"):
            a.verify(d,h)
    def test_geometry(self):
        d,r=sample()
        h=a.header(r)
        h["frames"]=100
        with self.assertRaises(ValueError):
            a.verify(d,h)
    def test_bad_end(self):
        d,r=sample()
        h=a.header(r)
        h["cap_status"]=5
        with self.assertRaises(ValueError):
            a.verify(d,h)
    def test_padding(self):
        d,r=sample()
        h=a.header(r)
        d=d[:56]+b"\x00"+d[57:]
        h["crc"]=a.crc32(d)
        with self.assertRaisesRegex(ValueError,"padding"):
            a.verify(d,h)
# Protocol/persistence and real loopback TCP tests. These exercise only host code
# and a model server; their timing is not a measurement of the Zynq Ethernet path.
import argparse
import contextlib
import io
import json
import pathlib
import shutil
import socket
import subprocess
import tempfile
import threading
from unittest import mock


def encoded(h, **changes):
    h = {**h, **changes}
    raw = struct.pack('>15I', *(h[k] for k in a.KEYS[:-1]))
    return raw + struct.pack('>I', a.crc32(raw))


class Board:
    def __init__(self, data=None, reply_change=None, corrupt=False, bad_ack=False,
                 retained=False, on_ack=None, replay_change=False, on_fetch=None):
        default_data, raw = sample()
        self.data = default_data if data is None else data
        self.h = a.header(raw)
        self.reply_change = reply_change or {}
        self.corrupt, self.bad_ack = corrupt, bad_ack
        self.retained, self.on_ack = retained, on_ack
        self.on_fetch = on_fetch
        self.replay_change = replay_change
        self.commands, self.errors = [], []
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.address = self.listener.getsockname()
        self.listener.listen(2)
        self.listener.settimeout(0.1)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def connect(self):
        s = socket.create_connection(self.address, timeout=3)
        s.settimeout(3)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return s

    @staticmethod
    def send(s, data):
        # Fragment across fields and across the header/payload boundary. TCP may
        # coalesce adjacent sends, so neither endpoint assumes message boundaries.
        off = 0
        for count in (1, 3, 17, 47, 251, len(data)):
            block = data[off:off+count]
            if not block:
                break
            s.sendall(block)
            off += len(block)

    def run_connection(self, s):
        s.settimeout(3)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        delivered = False
        while not self.stop.is_set():
            try:
                raw = a.read_exact(s, 32)
            except EOFError:
                return
            words = struct.unpack('>8I', raw)
            assert words[0:2] == (a.CMD_MAGIC, a.VERSION)
            assert words[7] == a.crc32(raw[:28])
            op, ident, frames, prev, fragments = words[2:7]
            self.commands.append((op, ident, frames, prev, fragments))
            if op in (a.START, a.FETCH):
                if op == a.START:
                    assert (ident, frames, prev, fragments) == (0, 4, 1, 2)
                    assert not self.retained
                    self.retained = True
                else:
                    assert self.retained and ident == self.h['id']
                    if self.on_fetch:
                        self.on_fetch()
                payload = self.data
                changes = dict(self.reply_change)
                if self.corrupt:
                    payload = payload[:-1] + bytes([payload[-1] ^ 1])
                if self.replay_change and op == a.FETCH:
                    payload = payload[:8] + struct.pack('<Q', 90) + payload[16:]
                    changes['crc'] = a.crc32(payload)
                self.send(s, encoded(self.h, **changes) + payload)
                delivered = True
            elif op == a.ACK:
                assert delivered and self.retained
                assert (ident, frames, prev, fragments) == (self.h['id'], len(self.data), a.crc32(self.data), 0)
                if self.on_ack:
                    self.on_ack()
                self.retained = False
                self.send(s, encoded(self.h, state=a.EMPTY,
                          id=self.h['id'] + int(self.bad_ack), frames=0, prev=0,
                          fragments=0, length=0, crc=0, period=0))
            elif op == a.STATUS:
                if self.retained:
                    self.send(s, encoded(self.h))
                else:
                    self.send(s, encoded(self.h, state=a.EMPTY, frames=0, prev=0,
                                         fragments=0, length=0, crc=0, period=0))
            else:
                raise AssertionError('Unexpected request')

    def run(self):
        while not self.stop.is_set():
            try:
                s, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with s:
                try:
                    self.run_connection(s)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # A malformed response must cause the client to close.
                except Exception as exc:
                    self.errors.append(exc)

    def __exit__(self, kind, value, traceback):
        self.stop.set()
        self.listener.close()
        self.thread.join(4)
        if self.thread.is_alive():
            raise AssertionError('Model server did not terminate')
        if self.errors:
            raise AssertionError(self.errors)


def options(root, **changes):
    values = dict(host='127.0.0.1', port=5001, frames=4, prev=1, fragments=2,
                  output=str(root / 'capture.bin'), report=str(root / 'bench.json'),
                  repeats=3, overwrite=False)
    values.update(changes)
    return argparse.Namespace(**values)


class ValidationTests(unittest.TestCase):
    def test_header_rejects_unknown_version_and_size(self):
        _, raw = sample()
        h = a.header(raw)
        for changes in ({'version': 2}, {'header_bytes': 192}, {'magic': 0}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                a.header(encoded(h, **changes))

    def test_header_rejects_unknown_results_states_and_oversize(self):
        _, raw = sample()
        h = a.header(raw)
        for changes in ({'state': 4}, {'result': 99}, {'length': a.MAX_PAYLOAD+1}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                a.header(encoded(h, **changes))

    def test_header_exact_length(self):
        for n in (0, 63, 65, 192):
            with self.subTest(n=n), self.assertRaises(ValueError):
                a.header(bytes(n))

    def test_requests_reject_overflow_invalid_arguments_and_out_of_envelope(self):
        for args in ((a.START, 1, 4, 1, 2), (a.START, 0, 4, 4, 2),
                     (a.START, 0, 4, 1, 17), (a.START, 0, 100000, 1, 1),
                     (a.FETCH, 0), (a.FETCH, 1, 2), (a.STATUS, 1),
                     (a.ACK, 1, 0, 0), (a.ACK, 1, 5, 0, 1),
                     (a.FETCH, 1 << 32), (a.FETCH, -1), (99,)):
            with self.subTest(args=args), self.assertRaises(ValueError):
                a.request(*args)

    def test_bad_schedule_id_status_and_axi_busy(self):
        data, raw = sample()
        h = a.header(raw)
        for changes in ({'first': 127}, {'period': 511}, {'id': 0},
                        {'state': a.ACQUIRING}, {'cap_errors': 1},
                        {'cap_status': 22}, {'cap_status': 262}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                a.verify(data, {**h, **changes})

    def test_all_preamble_fields_checked_even_with_recomputed_crc(self):
        data, raw = sample()
        h = a.header(raw)
        for off in (0, 4, 16, 20, 24, 28, 32, 40, 48, 56, 120):
            broken = data[:off] + bytes([data[off] ^ 1]) + data[off+1:]
            with self.subTest(offset=off), self.assertRaises(ValueError):
                a.verify(broken, {**h, 'crc': a.crc32(broken)})

    def test_timestamp_must_increase_across_fragments(self):
        data, raw = sample()
        offset = 128 + 768*4 + 8
        broken = data[:offset] + struct.pack('<Q', 99) + data[offset+8:]
        with self.assertRaisesRegex(ValueError, 'timestamp'):
            a.verify(broken, {**a.header(raw), 'crc': a.crc32(broken)})

    def test_rates_use_explicit_units(self):
        self.assertEqual(a.rates(1000000, 1),
                         {'MB_per_s': 1, 'MiB_per_s': 1000000/1048576, 'Mbit_per_s': 8})
        self.assertIsNone(a.rates(1, 0)['MB_per_s'])

    def test_saved_metadata_crc_is_rechecked(self):
        _, raw = sample()
        h = a.header(raw)
        self.assertEqual(a.metadata_header(h), h)
        with self.assertRaisesRegex(ValueError, 'CRC'):
            a.metadata_header({**h, 'id': 2})
        for value in (True, -1, 1 << 32, '1'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                a.metadata_header({**h, 'id': value})

    def test_atomic_no_clobber_and_explicit_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / 'capture.bin'
            a.atomic_write(path, b'first')
            with self.assertRaises(FileExistsError):
                a.atomic_write(path, b'second')
            self.assertEqual(path.read_bytes(), b'first')
            a.atomic_write(path, b'third', overwrite=True)
            self.assertEqual(path.read_bytes(), b'third')
            self.assertEqual(list(path.parent.iterdir()), [path])

    def test_fsync_failure_does_not_publish_file(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp) / 'capture.bin'
            with mock.patch.object(a.os, 'fsync', side_effect=OSError('disk error')):
                with self.assertRaises(OSError):
                    a.atomic_write(path, b'bad')
            self.assertFalse(path.exists())
            self.assertEqual(list(path.parent.iterdir()), [])

    def test_preflight_rejects_aliasing_report_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp)
            with self.assertRaises(ValueError):
                a.preflight_paths([path / 'x.bin', path / 'dir' / '..' / 'x.bin'], True)

    def test_capture_preflight_prevents_any_request(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp)
            args = options(path)
            pathlib.Path(args.output).write_bytes(b'valuable')
            s = mock.Mock()
            with self.assertRaises(FileExistsError):
                a.capture(s, args)
            s.sendall.assert_not_called()

    def test_parser_rejects_zero_repeat_and_nonfinite_timeout(self):
        parser = a.make_parser()
        for tail in (['--repeats', '0'], ['--timeout', 'nan'], ['--timeout', 'inf']):
            with self.subTest(tail=tail), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                parser.parse_args(['bench', '127.0.0.1', *tail])

    @unittest.skipUnless(shutil.which('gcc'), 'C fixture requires gcc')
    def test_c_firmware_header_and_payload_interoperate(self):
        root = pathlib.Path(__file__).resolve().parents[1]
        ps = root / 'ps_sw' / 'lwip_capture'
        with tempfile.TemporaryDirectory() as temp:
            exe, fixture = pathlib.Path(temp) / 'server_test', pathlib.Path(temp) / 'fixture.bin'
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I', str(ps / 'tests' / 'server_stubs'), '-I', str(ps),
                            str(ps / 'tests' / 'test_server.c'), str(ps / 'protocol.c'),
                            '-o', str(exe)], check=True, capture_output=True, text=True)
            subprocess.run([str(exe), str(fixture)], check=True, capture_output=True, text=True)
            wire = fixture.read_bytes()
            h = a.header(wire[:64])
            self.assertEqual(a.verify(wire[64:], h), h['frames']*h['fragments']*96)
            self.assertEqual(h['length'], len(wire)-64)


class LoopbackTests(unittest.TestCase):
    def test_capture_is_saved_and_read_back_before_ack(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            seen = []
            def at_ack():
                seen.append(a.load_saved(args.output)[2])
            with Board(on_ack=at_ack) as board, board.connect() as s:
                result = a.capture(s, args)
                self.assertTrue(result['acknowledged'])
                self.assertGreater(result['cycle_seconds'], result['request_seconds'])
            self.assertEqual(seen, [768])
            self.assertEqual([x[0] for x in board.commands], [a.START, a.ACK])
            self.assertFalse(board.retained)

    def test_corrupt_payload_does_not_ack_or_save(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            with Board(corrupt=True) as board, board.connect() as s:
                with self.assertRaisesRegex(a.UnacknowledgedCapture, 'no ACK'):
                    a.capture(s, args)
            self.assertEqual([x[0] for x in board.commands], [a.START])
            self.assertTrue(board.retained)
            self.assertFalse(pathlib.Path(args.output).exists())

    def test_file_error_does_not_ack(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            with Board() as board, board.connect() as s, mock.patch.object(
                    a, 'atomic_write', side_effect=OSError('disk full')):
                with self.assertRaisesRegex(a.UnacknowledgedCapture, 'no ACK'):
                    a.capture(s, args)
            self.assertEqual([x[0] for x in board.commands], [a.START])
            self.assertTrue(board.retained)

    def test_failed_disk_readback_does_not_ack(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            with Board() as board, board.connect() as s, mock.patch.object(
                    a, 'load_saved', side_effect=ValueError('readback mismatch')):
                with self.assertRaisesRegex(a.UnacknowledgedCapture, 'no ACK'):
                    a.capture(s, args)
            self.assertEqual([x[0] for x in board.commands], [a.START])
            self.assertTrue(board.retained)

    def test_fetch_rejects_unexpected_capture_id(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            with Board(retained=True, reply_change={'id': 2}) as board, board.connect() as s:
                with self.assertRaisesRegex(ValueError, 'ID mismatch'):
                    a.capture(s, args, ident=1)
            self.assertEqual([x[0] for x in board.commands], [a.FETCH])
            self.assertTrue(board.retained)

    def test_start_rejects_wrong_geometry(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            # Still valid geometry, but different from the requested prev=1.
            with Board(reply_change={'prev': 0}) as board, board.connect() as s:
                with self.assertRaisesRegex(ValueError, 'differs'):
                    a.capture(s, args)
            self.assertEqual([x[0] for x in board.commands], [a.START])

    def test_bad_ack_response_is_not_success(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            with Board(bad_ack=True) as board, board.connect() as s:
                with self.assertRaisesRegex(ValueError, 'ACK response'):
                    a.capture(s, args)
            self.assertEqual([x[0] for x in board.commands], [a.START, a.ACK])
            self.assertTrue(pathlib.Path(args.output).exists())

    def test_reconnect_fetch_preserves_then_releases_capture(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            with Board() as board:
                with board.connect() as first:
                    h, _, _ = a.receive_capture(first, 4, 1, 2)
                with board.connect() as second:
                    status = a.exchange(second, a.STATUS)
                    self.assertEqual(status['id'], h['id'])
                    result = a.capture(second, args, ident=h['id'])
                    self.assertTrue(result['acknowledged'])
            self.assertEqual([x[0] for x in board.commands], [a.START, a.STATUS, a.FETCH, a.ACK])

    def test_benchmark_replays_one_immutable_capture_then_acks(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            def at_ack():
                report = json.loads(pathlib.Path(args.report).read_text())
                self.assertEqual(len(report['samples']), 3)
                self.assertEqual(report['acknowledgement'], 'not_attempted')
            with Board(on_ack=at_ack) as board, board.connect() as s, contextlib.redirect_stdout(io.StringIO()):
                report = a.bench(s, args)
            self.assertEqual([x[0] for x in board.commands], [a.START, a.FETCH, a.FETCH, a.FETCH, a.ACK])
            self.assertFalse(board.retained)
            saved = json.loads(pathlib.Path(args.report).read_text())
            self.assertEqual(saved['acknowledgement'], 'accepted')
            self.assertEqual(saved['aggregate']['bytes'], len(board.data)*3)
            self.assertEqual(saved['aggregate']['request_seconds'],
                             sum(x['request_seconds'] for x in saved['samples']))
            self.assertEqual(saved, report)

    def test_benchmark_rejects_changed_retained_buffer(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            with Board(replay_change=True) as board, board.connect() as s, contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(a.UnacknowledgedCapture, 'metadata changed'):
                    a.bench(s, args)
            self.assertEqual([x[0] for x in board.commands], [a.START, a.FETCH])
            self.assertTrue(board.retained)

    def test_benchmark_rejects_replaced_valid_saved_capture_before_ack(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            replaced = []
            def replace_saved_pair():
                if not replaced:
                    other_header = a.header(encoded(board.h, id=2))
                    # Simulate another process publishing a complete, valid
                    # capture pair while FETCH replay is in progress.
                    a.save_capture(args.output, other_header, board.data, {}, overwrite=True)
                    replaced.append(True)
            with Board(on_fetch=replace_saved_pair) as board, board.connect() as s, contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(a.UnacknowledgedCapture, 'Saved seed capture changed'):
                    a.bench(s, args)
            self.assertEqual(a.load_saved(args.output)[0]['id'], 2)
            self.assertEqual([x[0] for x in board.commands], [a.START, a.FETCH, a.FETCH, a.FETCH])
            self.assertTrue(board.retained)

    def test_saved_capture_can_be_verified_offline(self):
        with tempfile.TemporaryDirectory() as temp:
            args = options(pathlib.Path(temp))
            with Board() as board, board.connect() as s:
                a.capture(s, args)
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(a.main(['verify', args.output]), 0)
            self.assertIn('CRC32 and COUNTER PASS', out.getvalue())
            data = pathlib.Path(args.output).read_bytes()
            pathlib.Path(args.output).write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(a.main(['verify', args.output]), 1)


if __name__ == '__main__':
    unittest.main()
