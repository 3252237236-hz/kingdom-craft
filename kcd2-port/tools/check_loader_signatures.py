"""Check the loader's WHGame.dll patterns against the installed game binary."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path


SCAN = re.compile(
    r"(?P<name>[A-Za-z_][A-Za-z_0-9]*)\s*=\s*kcd2_address::scan\(\s*"
    r'"(?P<pattern>(?:[0-9A-Fa-f?]{1,2} ?)+)"',
    re.MULTILINE,
)


def locate(data: bytes, tokens: list[str]) -> list[int]:
    values = [None if "?" in token else int(token, 16) for token in tokens]
    runs = []
    start = None
    for index, value in enumerate(values + [None]):
        if value is not None and start is None:
            start = index
        if value is None and start is not None:
            runs.append((start, bytes(values[start:index])))
            start = None
    if not runs:
        raise ValueError("Pattern has no fixed bytes")
    anchor_offset, anchor = max(runs, key=lambda run: len(run[1]))
    positions = []
    offset = 0
    while (found := data.find(anchor, offset)) != -1:
        candidate = found - anchor_offset
        if candidate >= 0 and candidate + len(values) <= len(data):
            if all(value is None or data[candidate + i] == value for i, value in enumerate(values)):
                positions.append(candidate)
        offset = found + 1
    return positions


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("loader_source", type=Path)
    parser.add_argument("game_binary", type=Path)
    args = parser.parse_args()

    source = args.loader_source.read_text(encoding="utf-8")
    begin = source.index("auto scan_addresses_and_set_ptr =")
    end = source.index("scan_addresses_and_set_ptr();", begin)
    patterns = list(SCAN.finditer(source[begin:end]))
    if not patterns:
        raise RuntimeError("No loader patterns found")

    data = args.game_binary.read_bytes()
    print(f"WHGame.dll SHA256 {hashlib.sha256(data).hexdigest()}")
    missing = 0
    for match in patterns:
        positions = locate(data, match["pattern"].split())
        if not positions:
            missing += 1
        location = ", ".join(f"0x{position:X}" for position in positions[:3]) or "MISSING"
        print(f"{match['name']}: {location} ({len(positions)} match(es))")
    print(f"{len(patterns) - missing}/{len(patterns)} patterns found")
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
