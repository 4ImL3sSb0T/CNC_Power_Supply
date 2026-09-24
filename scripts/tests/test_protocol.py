"""协议层单测：这是"上位机 ↔ 固件"唯一的自动化契约。

固件侧同一套算法在 src/lib/proto/psu_proto.c，改协议必须两边一起改：
下面的黄金向量（hex）是两边共同遵守的字节级约定，也和 clang 编译出的
C 实现逐字节比对过（见 README「协议自检」）。
"""

from __future__ import annotations

import struct

import pytest

from psu_host import protocol as P
from psu_host.dummy import DummyTransport, VOUT_FULL_MV, VOUT_MIN_MV, ILIM_FULL_MA


# ---------------------------------------------------------------- CRC
def test_crc16_standard_check_value():
    # CRC-16/CCITT-FALSE 的标准校验值（poly 0x1021、初值 0xFFFF、不反射）
    assert P.crc16(b"123456789") == 0x29B1
    assert P.crc16(b"") == 0xFFFF


def test_crc16_catches_single_bit_error():
    frame = bytes((P.Cmd.SET_VOUT, 1, 3, P.Unit.MV, 0x3C, 0x30))
    good = P.crc16(frame)
    for bit in range(len(frame) * 8):
        broken = bytearray(frame)
        broken[bit // 8] ^= 1 << (bit % 8)
        assert P.crc16(bytes(broken)) != good


# ---------------------------------------------------------------- COBS
@pytest.mark.parametrize(
    "raw,encoded",
    [
        (b"", b"\x01"),
        (b"\x00", b"\x01\x01"),
        (b"\x11\x22\x00\x33", b"\x03\x11\x22\x02\x33"),
        (b"\x00\x00\x00\x00\x00", b"\x01\x01\x01\x01\x01\x01"),
        (b"abcde", b"\x06abcde"),
        (bytes(range(1, 6)), b"\x06" + bytes(range(1, 6))),
    ],
)
def test_cobs_known_vectors(raw, encoded):
    assert P.cobs_encode(raw) == encoded
    assert P.cobs_decode(encoded) == raw


@pytest.mark.parametrize("size", [1, 2, 3, 15, 63, 64, 65, 200, 253, 254, 255, 256, 1000])
def test_cobs_round_trip_all_zero_bytes(size):
    raw = b"\x00" * size
    encoded = P.cobs_encode(raw)
    assert b"\x00" not in encoded
    assert P.cobs_decode(encoded) == raw


def test_cobs_round_trip_random():
    import random

    rng = random.Random(20260924)
    for _ in range(200):
        size = rng.randrange(0, 300)
        raw = bytes(rng.randrange(256) for _ in range(size))
        encoded = P.cobs_encode(raw)
        assert b"\x00" not in encoded
        assert P.cobs_decode(encoded) == raw


def test_cobs_decode_rejects_malformed():
    with pytest.raises(P.ProtocolError):
        P.cobs_decode(b"\x03\x11")               # 块被截断
    with pytest.raises(P.ProtocolError):
        P.cobs_decode(b"\x02\x00\x11")           # 块内出现 0x00


# ---------------------------------------------------------------- 帧
def test_frame_round_trip_all_commands():
    cases = [
        (P.Cmd.PING, P.pack_u32(0x11223344)),
        (P.Cmd.GET_INFO, b""),
        (P.Cmd.SET_VOUT, P.payload_setpoint(P.Unit.MV, 12350)),
        (P.Cmd.SET_ILIM, P.payload_setpoint(P.Unit.PERMILLE, 635)),
        (P.Cmd.SET_OUTPUT, b"\x01"),
        (P.Cmd.SAFE_STATE, b""),
        (P.Cmd.RUN_TEST, b""),
        (P.Cmd.STOP_TEST, b""),
        (P.Cmd.SET_TELEM, P.pack_u16(100)),
        (P.Cmd.SET_REMOTE, P.payload_set_remote(True, 3000)),
        (P.Cmd.GET_STATUS, b""),
        (P.Cmd.KEEPALIVE, P.pack_u32(12345678)),
        (P.Rsp.TELEM, bytes(16)),
        (P.Rsp.ACK, struct.pack("<BbH", P.Cmd.SET_VOUT, -6, 433)),
        (P.Rsp.ITEM, bytes(P.ITEM_PAYLOAD_LEN)),
    ]
    for seq, (cmd, payload) in enumerate(cases, start=1):
        on_wire = P.encode_frame(cmd, seq, payload)
        assert on_wire.startswith(b"\x00") and on_wire.endswith(b"\x00")
        assert len(on_wire) <= P.MAX_WIRE
        frame = P.decode_block(on_wire[1:-1])
        assert (frame.cmd, frame.seq, frame.payload) == (cmd, seq, payload)


def test_golden_vectors_pin_the_wire_format():
    """字节级黄金向量：固件侧 psu_proto.c 必须产出同样的字节。

    这几条是与 C 实现（clang 编出来的本地小程序）逐字节比对过的 —— 见 README
    「协议自检」。改动协议时两边一起改，并更新这里的 hex。
    """
    assert P.encode_frame(P.Cmd.PING, 7, P.pack_u32(0x11223344)).hex() == "000a01070444332211e45200"
    assert P.encode_frame(P.Cmd.SET_VOUT, 1, P.payload_setpoint(P.Unit.MV, 12350)).hex() == "0009030103013e3084d600"
    assert P.encode_frame(P.Cmd.SAFE_STATE, 255, b"").hex() == "000306ff03c37d00"
    assert P.encode_frame(P.Rsp.TELEM, 0, bytes(range(16))).hex() == (
        "0002830210120102030405060708090a0b0c0d0e0f941c00"
    )


def test_decode_rejects_bad_length_and_crc():
    body = bytes((P.Cmd.PING, 1, 4, 1, 2, 3, 4))
    good = P.cobs_encode(body + struct.pack("<H", P.crc16(body)))
    assert P.decode_block(good).payload == bytes((1, 2, 3, 4))

    # 长度字段被改：结构校验挡下来
    bad_len = bytearray(body)
    bad_len[2] = 3
    encoded = P.cobs_encode(bytes(bad_len) + struct.pack("<H", P.crc16(bytes(bad_len))))
    with pytest.raises(P.ProtocolError):
        P.decode_block(encoded)

    # payload 被改但 CRC 还是老值：CRC 挡下来
    bad_crc = bytearray(body)
    bad_crc[3] ^= 0xFF
    encoded = P.cobs_encode(bytes(bad_crc) + struct.pack("<H", P.crc16(body)))
    with pytest.raises(P.CrcError):
        P.decode_block(encoded)

    # 空块 / 超长块
    with pytest.raises(P.ProtocolError):
        P.decode_block(b"\x01")
    with pytest.raises(P.ProtocolError):
        P.decode_block(b"\x01" * (P.MAX_BLOCK + 1))


# ---------------------------------------------------------------- 分流
def test_splitter_separates_frames_and_printf_text():
    """设备 printf 的文本和协议帧混在一条 CDC 上：帧要解出来，文本要原样交出去。"""
    splitter = P.FrameSplitter()
    frame1 = P.encode_frame(P.Rsp.TELEM, 0, bytes(range(16)))
    frame2 = P.encode_frame(P.Rsp.ACK, 9, struct.pack("<BbH", P.Cmd.PING, 0, 0))
    stream = b"==== PCB \xe6\xb5\x8b\xe8\xaf\x95\xe5\xbc\x80\xe5\xa7\x8b ====\n" + frame1
    stream += b"[1/7] SELF-IMU ...\n" + frame2

    out = list(splitter.feed(stream))
    kinds = [kind for kind, _ in out]
    assert kinds == ["text", "frame", "text", "frame"]
    assert "PCB" in out[0][1]
    assert out[1][1].cmd == P.Rsp.TELEM
    assert out[3][1].cmd == P.Rsp.ACK
    assert splitter.stats.frames == 2
    assert splitter.stats.crc_errors == 0


def test_splitter_handles_text_split_across_chunks():
    """日志行被任意切分（USB 每包 64 字节）也要能拼回来。"""
    splitter = P.FrameSplitter()
    frame = P.encode_frame(P.Rsp.TELEM, 0, bytes(16))
    stream = "中文日志".encode("utf-8") + frame
    collected = []
    for index in range(0, len(stream), 3):
        collected.extend(splitter.feed(stream[index : index + 3]))
    texts = [value for kind, value in collected if kind == "text"]
    frames = [value for kind, value in collected if kind == "frame"]
    assert len(frames) == 1
    assert "中文日志" in "".join(texts)


def test_splitter_counts_corrupted_frame_as_crc_error():
    splitter = P.FrameSplitter()
    body = bytes((P.Rsp.TELEM, 5, 3, 1, 2, 3))
    on_wire = b"\x00" + P.cobs_encode(body + struct.pack("<H", P.crc16(body) ^ 0xFFFF)) + b"\x00"
    out = list(splitter.feed(on_wire))
    assert [kind for kind, _ in out] == ["text"]
    assert splitter.stats.crc_errors == 1
    assert splitter.stats.frames == 0


def test_splitter_flushes_long_text_without_delimiter():
    """遥测停发时（设备只打日志），文本不该被无限期憋在缓冲里。"""
    splitter = P.FrameSplitter(text_flush_limit=32)
    out = list(splitter.feed(b"A" * 40))
    assert len(out) == 1 and out[0][0] == "text"


# ---------------------------------------------------------------- 载荷
def test_info_and_telemetry_layout_matches_firmware_structs():
    payload = struct.pack("<BBBBHHHHHBB", 1, 0, 1, 7, 23400, 3900, 5040, 4200, 50, 0x1F, 0)
    info = P.Info.parse(payload)
    assert (info.proto_ver, info.item_count, info.vout_full_mv) == (1, 7, 23400)
    assert info.supports.count("看门狗") == 1

    telemetry_payload = struct.pack("<BBBBHHHHI", 0x0F, 1, 2, 2, 433, 1000, 3312, 0, 123456)
    telemetry = P.Telemetry.parse(telemetry_payload)
    assert telemetry.ce_on and telemetry.remote and telemetry.wd_armed
    assert telemetry.pg_mv == 3312 and telemetry.tick_ms == 123456

    ack = P.Ack.parse(struct.pack("<BbH", P.Cmd.SET_VOUT, -6, 433))
    assert (ack.acked_cmd, ack.code, ack.arg, ack.ok) == (P.Cmd.SET_VOUT, -6, 433, False)


def test_item_frame_and_status_parse():
    name = b"PG-ENABLE".ljust(12, b"\x00")
    detail = "t=120ms H=3.31V".encode("utf-8").ljust(22, b"\x00")
    payload = struct.pack("<BBBBI", P.ItemPhase.END, 2, P.TestResult.PASS, 0, 120) + name + detail
    item = P.ItemInfo.parse(payload)
    assert item.name == "PG-ENABLE" and item.detail == "t=120ms H=3.31V"
    assert item.result == P.TestResult.PASS and item.elapsed_ms == 120

    status = struct.pack("<BBBBHHHHI", 0x01, 2, 6, 2, 1000, 1000, 3312, 0, 42) + bytes(
        [P.TestResult.PASS] * 6 + [P.TestResult.MANUAL]
    ) + b"\x00"
    telemetry, results = P.parse_status_items(status, 7)
    assert telemetry.run_state == P.RunState.DONE
    assert results == [P.TestResult.PASS] * 6 + [P.TestResult.MANUAL]


def test_event_parse():
    event, data = P.parse_event(bytes((P.Event.WATCHDOG,)) + struct.pack("<H", 3000))
    assert event == P.Event.WATCHDOG and int.from_bytes(data, "little") == 3000


# ---------------------------------------------------------------- 单位换算
def test_setpoint_conversion_matches_firmware_formula():
    info = P.Info.parse(struct.pack("<BBBBHHHHHBB", 1, 0, 1, 7, 23400, 3900, 5040, 4200, 50, 0x1F, 0))
    # 端点
    assert P.vout_mv_from_permille(0, info) == 3900
    assert P.vout_mv_from_permille(1000, info) == 23400
    assert P.ilim_ma_from_permille(0, info) == 0
    assert P.ilim_ma_from_permille(1000, info) == 5040
    # 单调不回退，且 mV → ‰ → mV 往返误差不超过一个 ‰ 的量化步长
    previous = -1
    for permille in range(1001):
        mv = P.vout_mv_from_permille(permille, info)
        assert mv > previous
        previous = mv
        back = P.vout_permille_from_mv(mv, info)
        assert abs(P.vout_mv_from_permille(back, info) - mv) <= 20      # 20 mV ≈ 1‰
    # 典型点：12.35 V 附近
    assert P.vout_permille_from_mv(12350, info) == 433
    assert P.ilim_permille_from_ma(3200, info) == 635
    # 越界夹紧
    assert P.vout_permille_from_mv(1000, info) == 0
    assert P.vout_permille_from_mv(30000, info) == 1000
    assert P.ilim_permille_from_ma(-5, info) == 0
    assert P.ilim_permille_from_ma(99999, info) == 1000


# ---------------------------------------------------------------- 模拟器=固件语义
def test_dummy_device_matches_firmware_semantics():
    """dummy 是"另一台设备"，它的换算与拒绝规则必须和固件一致。"""
    sim = DummyTransport()
    try:
        assert (sim.info.vout_full_mv, sim.info.vout_min_mv) == (VOUT_FULL_MV, VOUT_MIN_MV)
        assert sim.info.ilim_full_ma == ILIM_FULL_MA
        # mV → ‰ 与 protocol 的换算一致（两者都对应固件 psu_link.c 的公式）
        for mv in (3900, 5000, 12350, 20000, 23400):
            assert sim._to_permille(P.Cmd.SET_VOUT, P.Unit.MV, mv) == P.vout_permille_from_mv(
                mv, sim.info
            )
        for ma in (0, 1, 3200, 5040):
            assert sim._to_permille(P.Cmd.SET_ILIM, P.Unit.MV, ma) == P.ilim_permille_from_ma(
                ma, sim.info
            )
        # 越界/坏单位一律拒绝（返回 None → ACK 里带 EXIT_INVALID_PARAM）
        assert sim._to_permille(P.Cmd.SET_VOUT, P.Unit.MV, 24000) is None
        assert sim._to_permille(P.Cmd.SET_VOUT, P.Unit.PERMILLE, 1001) is None
        assert sim._to_permille(P.Cmd.SET_ILIM, P.Unit.MV, 6000) is None
        assert sim._to_permille(P.Cmd.SET_VOUT, 7, 1000) is None
    finally:
        sim.close()


def test_dummy_answers_ping_and_reports_telemetry():
    """过一遍最小端到端：PING → PONG、SET_TELEM → 遥测帧。"""
    import time

    sim = DummyTransport()
    try:
        sim.write(P.encode_frame(P.Cmd.PING, 3, P.pack_u32(0xDEADBEEF)))
        sim.write(P.encode_frame(P.Cmd.SET_TELEM, 4, P.pack_u16(20)))
        splitter = P.FrameSplitter()

        frames = []
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            data = sim.read(256, timeout=0.05)
            if data:
                frames.extend(value for kind, value in splitter.feed(data) if kind == "frame")
            if len(frames) >= 3:                 # PONG + ACK + 第一条遥测
                break
        cmds = [frame.cmd for frame in frames]
        assert P.Rsp.PONG in cmds
        assert P.Rsp.ACK in cmds
        assert P.Rsp.TELEM in cmds
        pong = next(frame for frame in frames if frame.cmd == P.Rsp.PONG)
        assert pong.seq == 3 and P.unpack_u32(pong.payload[:4]) == 0xDEADBEEF
    finally:
        sim.close()
