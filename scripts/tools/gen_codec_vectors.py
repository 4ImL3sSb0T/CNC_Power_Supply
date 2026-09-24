"""输出与 tools/codec_vectors.c 完全一致的黄金向量，用于 diff 两边实现。

    uv run python tools/gen_codec_vectors.py > py.txt
    diff c.txt py.txt && echo "两边字节一致"
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

from psu_host import protocol as P  # noqa: E402


def dump(name: str, cmd: int, seq: int, payload: bytes) -> None:
    wire = P.encode_frame(cmd, seq, payload)
    print(f"{name:<9} len={len(wire):3d} {wire.hex()}")


def main() -> int:
    dump("PING", P.Cmd.PING, 7, struct.pack("<I", 0x11223344))
    dump("SET_VOUT", P.Cmd.SET_VOUT, 1, bytes((P.Unit.MV,)) + P.pack_u16(12350))
    dump("SAFE", P.Cmd.SAFE_STATE, 255, b"")
    dump("TELEM16", P.Rsp.TELEM, 0, bytes(range(16)))
    dump("ACK", P.Rsp.ACK, 9, struct.pack("<BbH", P.Cmd.SET_VOUT, -6, 433))
    dump("ITEM", P.Rsp.ITEM, 0, bytes(P.ITEM_PAYLOAD_LEN))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
