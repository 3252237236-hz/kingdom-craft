"""Save the latest published Minecraft HUD frame from SkyCraft shared memory."""

import ctypes
import struct
from pathlib import Path

from PIL import Image


MAPPING = "Local\\SkyCraft_v1"
FILE_MAP_READ = 0x0004
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


def main():
    handle = kernel.OpenFileMappingW(FILE_MAP_READ, False, MAPPING)
    if not handle:
        raise RuntimeError(f"OpenFileMappingW: {ctypes.get_last_error()}")
    view = kernel.MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0)
    if not view:
        kernel.CloseHandle(handle)
        raise RuntimeError(f"MapViewOfFile: {ctypes.get_last_error()}")
    try:
        headers = []
        for slot in range(3):
            header = ctypes.string_at(view + SLOT_HEADERS + slot * 0x40, 24)
            width, height, flags, _, frame_id = struct.unpack("<IIIIQ", header)
            if 0 < width <= 3840 and 0 < height <= 2160:
                headers.append((frame_id, slot, width, height, flags))
        if not headers:
            raise RuntimeError("No published overlay frame")
        frame_id, slot, width, height, flags = max(headers)
        pixels = ctypes.string_at(view + OVERLAY_PIXELS + slot * SLOT_BYTES,
                                  width * height * 4)
        image = Image.frombytes("RGBA", (width, height), pixels)
        if flags & 1:
            image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        output = Path(__file__).with_name("overlay-capture.png")
        image.save(output)
        print(f"frame={frame_id} slot={slot} size={width}x{height} flags={flags} output={output}")
    finally:
        kernel.UnmapViewOfFile(view)
        kernel.CloseHandle(handle)


if __name__ == "__main__":
    main()
