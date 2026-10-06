"""Publish a brief diagnostic HUD frame to the running SkyCraft KCD2 host."""

import ctypes
import os
import struct
import sys
import time


MAPPING = "Local\\SkyCraft_v1"
MAGIC = 0x43594B53
VERSION = 11
FILE_MAP_ALL_ACCESS = 0x000F001F
OVERLAY_CTL = 0x300
SLOT_HEADERS = 0x340
OVERLAY_PIXELS = 0x20000 + (32 << 20)
SLOT_BYTES = 3840 * 2160 * 4

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
kernel.OpenFileMappingW.restype = ctypes.c_void_p
kernel.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                 ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t]
kernel.MapViewOfFile.restype = ctypes.c_void_p
kernel.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
kernel.CloseHandle.argtypes = [ctypes.c_void_p]
kernel.GetTickCount64.restype = ctypes.c_uint64
def read(view, offset, fmt):
    return struct.unpack(fmt, ctypes.string_at(view + offset, struct.calcsize(fmt)))


def write(view, offset, fmt, *values):
    data = struct.pack(fmt, *values)
    ctypes.memmove(view + offset, data, len(data))


def fill_rect(pixels, width, left, top, right, bottom, color):
    row = bytes(color) * (right - left)
    for y in range(top, bottom):
        start = (y * width + left) * 4
        pixels[start:start + len(row)] = row


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    handle = kernel.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, MAPPING)
    if not handle:
        raise RuntimeError(f"SkyCraft mapping is not open: {ctypes.get_last_error()}")
    view = kernel.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0)
    if not view:
        kernel.CloseHandle(handle)
        raise RuntimeError(f"MapViewOfFile failed: {ctypes.get_last_error()}")
    try:
        magic, version, host_pid, mc_pid = read(view, 0, "<IIII")
        if (magic, version) != (MAGIC, VERSION) or not host_pid or mc_pid:
            raise RuntimeError("Host missing, protocol mismatch, or Minecraft already linked")
        _, flags, _, _, width, height = read(view, 0x100, "<IIII36xII")
        if not flags & 1 or not (0 < width <= 3840 and 0 < height <= 2160):
            raise RuntimeError("Load a save before running the overlay probe")

        pixels = bytearray(width * height * 4)
        fill_rect(pixels, width, 24, 24, 96, 96, (255, 48, 48, 230))
        fill_rect(pixels, width, 24, height - 96, 96, height - 24,
                  (48, 230, 90, 230))
        fill_rect(pixels, width, width // 2 - 60, height // 2 - 8,
                  width // 2 + 60, height // 2 + 8, (50, 220, 230, 220))

        slot = 1
        ctypes.memmove(view + OVERLAY_PIXELS + slot * SLOT_BYTES,
                       bytes(pixels), len(pixels))
        write(view, SLOT_HEADERS + slot * 0x40, "<IIIIQ", width, height, 0, 0, 1)
        # The aligned 32-bit store publishes this one-shot frame atomically on x64.
        ctypes.c_uint32.from_address(view + OVERLAY_CTL).value = slot | 4
        write(view, OVERLAY_CTL + 8, "<Q", 1)
        write(view, 12, "<I", os.getpid())
        print(f"Published {width}x{height} test frame to game pid {host_pid}", flush=True)

        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            write(view, 24, "<Q", kernel.GetTickCount64())
            host_heartbeat = read(view, 16, "<Q")[0]
            if kernel.GetTickCount64() - host_heartbeat > 3000:
                break
            time.sleep(0.05)
    finally:
        if read(view, 12, "<I")[0] == os.getpid():
            write(view, 24, "<Q", 0)
            write(view, 12, "<I", 0)
        kernel.UnmapViewOfFile(view)
        kernel.CloseHandle(handle)


if __name__ == "__main__":
    main()
