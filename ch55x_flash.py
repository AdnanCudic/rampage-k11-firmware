# ch55xtool (pip install ch55xtool) with the fixes the K11's LZ901/CH555
# bootloader needs. See README.md, "Flashing quirks of this chip".
#
# The bootloader answers on bulk IN endpoint 0x83, and a full erase takes
# longer than ch55xtool's 5 s reply timeout.
# CH55X_FULL_UID=1 sums all 8 UID bytes for the XOR key instead of the
# first 4 that ch55xtool uses for sub-ID 0x11 chips.
import os
import sys
import ch55xtool.ch55xtool as tool

tool.EP_IN_ADDR = 0x83
tool.USB_MAX_TIMEOUT = 30000
if os.environ.get("CH55X_FULL_UID") == "1":
    setattr(tool, "__chip_uid_chk_sum", lambda subid, uid: sum(uid) & 0xFF)
# CH55X_ZERO_KEY=1 sends a fixed 30-byte zero key base (as wchisp does)
# instead of a random-length random one, so the XOR key is deterministic.
if os.environ.get("CH55X_ZERO_KEY") == "1":

    def zero_key(chip_id, chksum):
        key = bytearray([chksum] * 8)
        key[7] = (chip_id + key[0]) & 0xFF
        return key, bytearray(30)

    setattr(tool, "__gen_key_values", zero_key)

# CH55X_WCHUNK / CH55X_VCHUNK set the data bytes per write / verify packet
# (multiple of 8, max 56). Same packet format as ch55xtool's own loop.
WCHUNK = int(os.environ.get("CH55X_WCHUNK", "56"))
VCHUNK = int(os.environ.get("CH55X_VCHUNK", "56"))


def write_verify(dev, key, data, func="FlashWrite"):
    chunk = VCHUNK if func == "FlashVerify" else WCHUNK
    # CH55X_VMAP=1: verify every chunk and print good/bad ranges instead of
    # stopping at the first mismatch.
    vmap = func == "FlashVerify" and os.environ.get("CH55X_VMAP") == "1"
    results = []
    addr = 0
    while addr < len(data):
        buf = bytearray(data[addr : addr + chunk])
        if len(buf) & 7:
            buf += b"\x00" * (8 - (len(buf) & 7))
        for i in range(len(buf)):
            buf[i] ^= key[i & 7]
        payload = addr.to_bytes(4, "little") + bytes([tool.random.randint(0, 255)]) + bytes(buf)
        ret, ret_pl = tool.cmd_exec(dev, func, payload)
        ok = ret is not None and ret_pl[0] == 0x00
        if vmap:
            results.append((addr, len(buf), ok))
        elif not ok:
            return False, addr
        addr += len(buf)
    if vmap:
        print()
        start, state = 0, results[0][2]
        for a, n, ok in results[1:] + [(addr, 0, None)]:
            if ok != state:
                print("  0x%04X-0x%04X %s" % (start, a - 1, "good" if state else "BAD"))
                start, state = a, ok
    return True, min(addr, len(data))


setattr(tool, "__flash_ops_write_verify", write_verify)

# The bootloader's first reply can come back garbled (seen once after the
# firmware jumped into it): retry detection before declaring the chip unknown.
_detect = getattr(tool, "__detect_ch5xx")


def detect_retry(dev):
    for _ in range(3):
        ret, chip_id, chip_subid = _detect(dev)
        if chip_id == 0x55:
            break
    return ret, chip_id, chip_subid


setattr(tool, "__detect_ch5xx", detect_retry)
sys.argv[0] = "ch55xtool"
sys.exit(tool.main())
