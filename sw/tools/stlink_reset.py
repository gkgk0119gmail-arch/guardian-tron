#!/usr/bin/env python3
"""Revive a hung ST-LINK V3 on USB (Mac + libusb 'device not responding') in software.
exit 0 = responding, 1 = still unresponsive after reset, 2 = no device"""
import sys, time, glob
import usb.core, usb.util, usb.backend.libusb1
libs = glob.glob('/opt/homebrew/lib/libusb-1.0*.dylib') + glob.glob('/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macosaarch64*/tools/bin/libusb-1.0*.dylib')
be = usb.backend.libusb1.get_backend(find_library=lambda x: libs[0]) if libs else usb.backend.libusb1.get_backend()
def find(): return usb.core.find(idVendor=0x0483, idProduct=0x3754, backend=be)
def ping(dev):
    try:
        usb.util.claim_interface(dev, 0)
        dev.write(0x01, bytes([0xF1, 0x80] + [0]*14), timeout=1500)   # ST-LINK GET_VERSION
        dev.read(0x81, 6, timeout=1500); return True
    except Exception: return False
    finally:
        try: usb.util.release_interface(dev, 0)
        except Exception: pass
dev = find()
if dev is None: print("ST-LINK not found"); sys.exit(2)
if ping(dev): print("ST-LINK OK"); sys.exit(0)
print("ST-LINK not responding -> USB reset");
try: dev.reset()
except Exception as e: print("reset failed:", e)
time.sleep(3); dev = find()
if dev is not None and ping(dev): print("ST-LINK recovered"); sys.exit(0)
print("recovery failed - unplug and replug the cable"); sys.exit(1)
