# Minimal Python probe for the station (pip install hidapi). Used during
# reverse engineering; works on Windows and Linux. Usage: probe.py [record maxmin devinfo ...]
import hid, sys, time

VID, PID = 0x1130, 0x0829


def cks(b):
    return sum(b) & 0xFF


def exchange(dev, req):
    frame = bytes(req) + b'\0'
    frame = frame[:-1] + bytes([cks(frame[:-1])])
    assert frame[0] == len(frame)
    # drain
    dev.set_nonblocking(1)
    while dev.read(64):
        pass
    dev.set_nonblocking(0)
    dev.write(b'\x00' + frame + b'\0' * (64 - len(frame)))
    r = bytes(dev.read(64, timeout_ms=1200))
    if not r:
        raise IOError('timeout')
    if r[0] > 0x40:
        r += bytes(dev.read(64, timeout_ms=1200))
    n = r[0]
    r = r[:n]
    if cks(r[:-1]) != r[-1]:
        raise IOError('checksum ' + r.hex())
    if r[1] != req[1]:
        raise IOError('cmd mismatch ' + r.hex())
    return r


cmds = {
    'devinfo': [0x03, 0x01],
    'record': [0x03, 0x04],
    'maxmin': [0x03, 0x05],
    'param3': [0x04, 0x08, 0x03],
    'fat': [0x04, 0x02, 0x00],
    'hisinfo': [0x04, 0x02, 0x03],
}
dev = hid.device()
dev.open(VID, PID)
for name in (sys.argv[1:] or cmds):
    try:
        r = exchange(dev, cmds[name])
        print(f'{name:8s} len={len(r):3d} {r.hex(" ")}')
    except Exception as e:
        print(name, 'ERR', e)
dev.close()
