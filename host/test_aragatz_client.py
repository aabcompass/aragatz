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
if __name__=="__main__":
    unittest.main()
