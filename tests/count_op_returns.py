"""Count OP_RETURN outputs in a serialized coinbase tx read as hex on stdin.

Used by test_solo_regtest.sh to learn how many commitments the enforcer's
template carries, so the suite can assert the mined block preserved exactly
that many rather than hardcoding a number that sidechain activity changes.
"""
import sys


def rd_varint(b, off):
    n = b[off]; off += 1
    if n < 0xfd:  return n, off
    if n == 0xfd: return int.from_bytes(b[off:off+2], 'little'), off + 2
    if n == 0xfe: return int.from_bytes(b[off:off+4], 'little'), off + 4
    return int.from_bytes(b[off:off+8], 'little'), off + 8


tx = bytes.fromhex(sys.stdin.read().strip())
off = 4                                     # version
if tx[off] == 0x00 and tx[off+1] != 0x00:   # segwit marker+flag
    off += 2
vin, off = rd_varint(tx, off)
for _ in range(vin):
    off += 36                               # prevout
    ss, off = rd_varint(tx, off)
    off += ss + 4                           # scriptSig + sequence
vout, off = rd_varint(tx, off)
n = 0
for _ in range(vout):
    off += 8                                # value
    spk_len, off = rd_varint(tx, off)
    if spk_len >= 1 and tx[off] == 0x6a:
        n += 1
    off += spk_len
print(n)
