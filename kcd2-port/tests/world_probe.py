"""Publish a textured cube through the real SkyCraft render ring for a short test."""

import ctypes
import math
import os
import struct
import sys
import time


MAPPING = "Local\\SkyCraft_v1"
MAGIC = 0x43594B53
VERSION = 11
FILE_MAP_ALL_ACCESS = 0x000F001F
SLOT_BYTES = 3840 * 2160 * 4
RENDER_RING = 0x20000 + (32 << 20) + 3 * SLOT_BYTES
RING_DATA = RENDER_RING + 0x80
RING_CAPACITY = (64 << 20) - 0x80

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


def publish(view, kind, payload):
    head = read(view, RENDER_RING, "<Q")[0]
    tail = read(view, RENDER_RING + 0x40, "<Q")[0]
    required = (8 + len(payload) + 7) & ~7
    if head < tail or head - tail + required + 8 >= RING_CAPACITY:
        raise RuntimeError("SkyCraft render ring is full")
    position = head % RING_CAPACITY
    if position + required > RING_CAPACITY:
        write(view, RING_DATA + position, "<II", 0, 0)
        head += RING_CAPACITY - position
        position = 0
    write(view, RING_DATA + position, "<II", kind, len(payload))
    ctypes.memmove(view + RING_DATA + position + 8, payload, len(payload))
    write(view, RENDER_RING, "<Q", head + required)


def vertex(point, origin, uv):
    return struct.pack("<fffffIII", point[0] - origin[0],
                       point[1] - origin[1], point[2] - origin[2],
                       uv[0], uv[1], 0xFFFFFFFF, 0x0F0F, 0)


def cube_mesh(center, section):
    x, y, z = center
    origin = tuple(value * 16 for value in section)
    corners = [
        (x - 0.5, y - 0.5, z - 0.5),
        (x + 0.5, y - 0.5, z - 0.5),
        (x + 0.5, y + 0.5, z - 0.5),
        (x - 0.5, y + 0.5, z - 0.5),
        (x - 0.5, y - 0.5, z + 0.5),
        (x + 0.5, y - 0.5, z + 0.5),
        (x + 0.5, y + 0.5, z + 0.5),
        (x - 0.5, y + 0.5, z + 0.5),
    ]
    faces = [(0, 1, 2, 3), (5, 4, 7, 6), (4, 0, 3, 7),
             (1, 5, 6, 2), (3, 2, 6, 7), (4, 5, 1, 0)]
    uv = [(0, 0), (1, 0), (1, 1), (0, 1)]
    data = bytearray()
    for face in faces:
        for index in (0, 1, 2, 0, 2, 3):
            data += vertex(corners[face[index]], origin, uv[index])
    return struct.pack("<iiiI", *section, 36) + data


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
            raise RuntimeError("Host missing, protocol mismatch, or Minecraft linked")
        _, flags = read(view, 0x100, "<II")
        if not flags & 1:
            raise RuntimeError("Load a save before running the world probe")
        px, py, pz, yaw, pitch = read(view, 0x110, "<dddff")
        radians = math.radians(yaw)
        center = (px - math.sin(radians) * 3.5, py + 1.3,
                  pz + math.cos(radians) * 3.5)
        section = tuple(math.floor(value / 16) for value in center)

        pixels = bytearray()
        for y in range(16):
            for x in range(16):
                pixels += bytes((68, 175, 64, 255) if (x // 4 + y // 4) % 2
                                else (36, 109, 48, 255))
        publish(view, 3, b"")
        publish(view, 1, struct.pack("<II", 16, 16) + pixels)
        publish(view, 2, cube_mesh(center, section))
        write(view, 12, "<I", os.getpid())
        print(f"Published cube at {center} to game pid {host_pid}", flush=True)

        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            write(view, 24, "<Q", kernel.GetTickCount64())
            heartbeat = read(view, 16, "<Q")[0]
            if kernel.GetTickCount64() - heartbeat > 3000:
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
