import sys
src, dst, target = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
b = bytearray(open(src, 'rb').read())
assert len(b) <= 0xFFFE
b += b'\xFF' * (0xFFFE - len(b))
s = sum((b[i] << 8) | b[i + 1] for i in range(0x200, 0xFFFE, 2)) & 0xFFFF
b += ((target - s) & 0xFFFF).to_bytes(2, 'big')
assert len(b) == 0x10000 and b[0x100:0x104] == b'SEGA' and b[0x18E:0x190] == b'\0\0' and b[0x1000] == 0xAA
assert sum((b[i] << 8) | b[i + 1] for i in range(0x200, 0x10000, 2)) & 0xFFFF == target
open(dst, 'wb').write(b)
print(dst, 'ok, real checksum $%04X' % target)
