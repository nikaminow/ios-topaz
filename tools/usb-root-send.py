#!/usr/bin/env python3
"""Send the iOS root image to the phone over USB (asteroids, patch 0956).

  pip install pyusb libusb-package
  python tools/usb-root-send.py ios16-root.img

Boot the phone with usbroot=1 rd=md0 and a payload built with
mkpayload.py --ramdisk-size <at least the image size>. The kernel shows up
as USB 1209:0002 (one vendor interface, bulk OUT 0x01) and writes what it
receives straight into the RAM disk; a short last packet or the reserved
size ends the transfer. On Windows the device needs the WinUSB driver
(Zadig, or Device Manager > WinUsb Device as for fastboot).
"""
import os, sys, time

import usb.core
import usb.util
try:
    import libusb_package
    backend = libusb_package.get_libusb1_backend()
except ImportError:
    backend = None

VID, PID = 0x1209, 0x0002
CHUNK = 4 * 1024 * 1024


def main():
    path = sys.argv[1]
    size = os.path.getsize(path)
    print(f'{path}: {size >> 20} MiB; waiting for {VID:04x}:{PID:04x}...')
    dev = None
    while dev is None:
        dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
        if dev is None:
            time.sleep(0.5)
    dev.set_configuration(1)
    usb.util.claim_interface(dev, 0)
    sent, t0 = 0, time.time()
    with open(path, 'rb') as f:
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            dev.write(0x01, block, timeout=20000)
            sent += len(block)
            if sent % (64 << 20) < CHUNK or sent == size:
                rate = sent / max(time.time() - t0, 0.001) / 1e6
                print(f'  {sent >> 20} / {size >> 20} MiB  {rate:.0f} MB/s')
    # a zero-length packet ends the transfer if the size was a multiple of 512
    if size % 512 == 0:
        try:
            dev.write(0x01, b'', timeout=5000)
        except usb.core.USBError:
            pass    # the image filled the reserved size: the phone already stopped
    print('done: the phone mounts the root and starts iOS')


if __name__ == '__main__':
    main()
