import sys, struct

path = sys.argv[1]
with open(path, 'rb') as f:
    d = f.read(4096)

print(f"File size (first read): {len(d)} bytes")
print(f"First 16 bytes hex: {d[:16].hex()}")
print(f"First 16 bytes repr: {repr(d[:16])}")

# Try 4-byte LE hlen
hlen_le = struct.unpack_from('<I', d, 0)[0]
print(f"\n4-byte LE hlen = {hlen_le}")
print(f"  body_off would be = {4 + hlen_le}")
print(f"  hbuf[0:4] = {d[4:8].hex()}")

# Try 4-byte BE hlen
hlen_be = struct.unpack_from('>I', d, 0)[0]
print(f"4-byte BE hlen = {hlen_be}")

# Show raw bytes around offset 4
print(f"\nBytes 0..64: {d[:64].hex()}")

# Try to find '<' which starts the XML
for i in range(min(64, len(d))):
    if d[i:i+2] == b'<\x00':
        print(f"Found UTF-16LE '<' at offset {i}")
        break
    if d[i:i+1] == b'<':
        print(f"Found UTF-8 '<' at offset {i}")
        break

# Try decoding header as UTF-16LE from offset 4
if hlen_le < len(d) - 4:
    hbuf = d[4:4+hlen_le]
    try:
        txt = hbuf.decode('utf-16-le', errors='replace')
        print(f"\nUTF-16LE header decoded: {txt[:400]}")
    except Exception as e:
        print(f"UTF-16LE decode failed: {e}")
    try:
        txt = hbuf.decode('utf-8', errors='replace')
        print(f"UTF-8 header decoded: {txt[:400]}")
    except Exception as e:
        print(f"UTF-8 decode failed: {e}")
