#!/usr/bin/env python3
"""Serve the iOS root image to the phone as its root disk over USB (patch 0957).

  pip install pyusb libusb-package
  python tools/usb-disk-serve.py ios16-root.img

The phone boots with rd=usbdisk0 and shows up as USB 1209:0002 (WinUSB on
Windows). It sends requests on bulk IN 0x81 (struct udk_req, plus the data
of a write); this answers on bulk OUT 0x01 (the data of a read, then struct
udk_status). Writes go into the image file. Keep this running while iOS runs.
"""
import os, struct, sys, time

import usb.core
import usb.util
try:
    import libusb_package
    backend = libusb_package.get_libusb1_backend()
except ImportError:
    backend = None

VID, PID = 0x1209, 0x0002
REQ = struct.Struct('<IIIIQQ')     # magic, op, seq, length, offset, reserved
STS = struct.Struct('<IIIIQQ')     # magic, seq, status, reserved, size, reserved2
REQ_MAGIC, STS_MAGIC = 0x44545341, 0x53545341
HELLO, READ, WRITE, SYNC = 0, 1, 2, 3
MAX = 1024 * 1024


def main():
    path = sys.argv[1]
    size = os.path.getsize(path)
    f = open(path, 'r+b')
    log = open(path + '.requests.log', 'w')
    print(f'{path}: {size >> 20} MiB; waiting for the phone ({VID:04x}:{PID:04x})...')
    dev = None
    while dev is None:
        dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
        if dev is None:
            time.sleep(0.5)
    dev.set_configuration(1)
    usb.util.claim_interface(dev, 0)
    print('phone connected; serving blocks (Ctrl+C stops iOS\'s disk)')
    reads = writes = last_seq = 0
    t0 = time.time()
    while True:
        try:
            pkt = bytes(dev.read(0x81, REQ.size + MAX + 512, timeout=5000))
        except usb.core.USBTimeoutError:
            print(f'  idle 5 s: the phone sends no requests (last seq {last_seq}, read {reads >> 20} MiB)')
            continue
        except usb.core.USBError as e:
            print(f'USB read of the next request failed: {e} (after {reads >> 10} KiB read, {writes >> 10} KiB written)')
            return
        magic, op, seq, length, offset, _ = REQ.unpack_from(pkt)
        last_seq = seq
        if magic != REQ_MAGIC:
            print(f'bad request magic {magic:#x}')
            continue
        status = 0
        print(f'  req seq {seq} op {op} offset {offset:#x} length {length} (got {len(pkt)} bytes)') if seq < 20 else None
        # every request, for matching against file extents (tools/find-extents.py)
        log.write(f'{seq} {op} {offset:#x} {length}\n')
        log.flush()
        if op == READ:
            f.seek(offset)
            data = f.read(length).ljust(length, b'\0')
            try:
                dev.write(0x01, data, timeout=20000)
            except usb.core.USBError as e:
                print(f'USB write of {length} bytes (seq {seq}) failed: {e}')
                return
            reads += length
        elif op == WRITE:
            f.seek(offset)
            f.write(pkt[REQ.size:REQ.size + length])
            writes += length
        elif op == SYNC:
            f.flush()
            os.fsync(f.fileno())
        elif op != HELLO:
            status = 1
        dev.write(0x01, STS.pack(STS_MAGIC, seq, status, 0, size, 0), timeout=20000)
        if op == HELLO:
            print('phone asked for the disk: iOS is mounting its root')
        if time.time() - t0 > 10:
            print(f'  read {reads >> 20} MiB, written {writes >> 20} MiB')
            t0 = time.time()


if __name__ == '__main__':
    main()
