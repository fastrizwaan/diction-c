import sys, struct

path = sys.argv[1]
with open(path, 'rb') as f:
    d = f.read(8192)

hlen_be = struct.unpack_from('>I', d, 0)[0]
print(f'BE hlen = {hlen_be}')

hbuf = d[4:4+hlen_be]
txt = hbuf.decode('utf-16-le', errors='replace')
print(f'Full header XML:\n{txt}')
