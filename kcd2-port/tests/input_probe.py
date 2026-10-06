"""Send a Minecraft test command through the SkyCraft input ring."""

import argparse
import ctypes
import math
import struct
import time


MAPPING = "Local\\SkyCraft_v1"
FILE_MAP_ALL_ACCESS = 0x000F001F
INPUT_RING = 0x1000
INPUT_DATA = INPUT_RING + 0x80
INPUT_ENTRIES = 4096

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
kernel.OpenFileMappingW.restype = ctypes.c_void_p
kernel.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                 ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t]
kernel.MapViewOfFile.restype = ctypes.c_void_p
kernel.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
kernel.CloseHandle.argtypes = [ctypes.c_void_p]


def read(view, offset, fmt):
    return struct.unpack(fmt, ctypes.string_at(view + offset, struct.calcsize(fmt)))


def write(view, offset, fmt, *values):
    data = struct.pack(fmt, *values)
    ctypes.memmove(view + offset, data, len(data))


def push(view, kind, code=0, a=0, b=0, c=0):
    head = read(view, INPUT_RING, "<Q")[0]
    tail = read(view, INPUT_RING + 0x40, "<Q")[0]
    if head < tail or head - tail >= INPUT_ENTRIES:
        raise RuntimeError("SkyCraft input ring is full")
    write(view, INPUT_DATA + (head % INPUT_ENTRIES) * 16,
          "<HHiii", kind, code, a, b, c)
    write(view, INPUT_RING, "<Q", head + 1)


def key(view, scancode):
    push(view, 1, scancode, 1)
    push(view, 1, scancode, 0)


def screen_open(view):
    return bool(read(view, 0x204, "<I")[0] & 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--command", help="Minecraft command; defaults to a grass test block")
    args = parser.parse_args()
    handle = kernel.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, MAPPING)
    if not handle:
        raise RuntimeError(f"SkyCraft mapping is not open: {ctypes.get_last_error()}")
    view = kernel.MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0)
    if not view:
        kernel.CloseHandle(handle)
        raise RuntimeError(f"MapViewOfFile failed: {ctypes.get_last_error()}")
    try:
        magic, version, host_pid, mc_pid = read(view, 0, "<IIII")
        if (magic, version) != (0x43594B53, 11) or not host_pid or not mc_pid:
            raise RuntimeError("Minecraft is not linked to the KCD2 host")
        if not read(view, 0x204, "<I")[0] & 1:
            raise RuntimeError("Minecraft has not entered its mirror world")
        if screen_open(view):
            raise RuntimeError("Close the current Minecraft screen before this probe")
        px, py, pz, yaw = read(view, 0x208, "<dddf")
        radians = math.radians(yaw)
        x = math.floor(px - math.sin(radians) * 4)
        y = math.floor(py + 1)
        z = math.floor(pz + math.cos(radians) * 4)
        command = args.command or f"/setblock {x} {y} {z} minecraft:grass_block"
        if not command.startswith("/"):
            raise ValueError("Command must begin with /")

        key(view, 23)  # T opens chat.
        deadline = time.monotonic() + 3
        while not screen_open(view) and time.monotonic() < deadline:
            time.sleep(0.05)
        if not screen_open(view):
            raise RuntimeError("Minecraft did not open chat from the input ring")
        for character in command:
            push(view, 5, 0, ord(character))
        time.sleep(0.3)
        key(view, 40)  # Enter submits chat.
        print(f"Sent to Minecraft: {command}", flush=True)
    finally:
        kernel.UnmapViewOfFile(view)
        kernel.CloseHandle(handle)


if __name__ == "__main__":
    main()
