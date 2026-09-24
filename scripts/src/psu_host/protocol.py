"""上位机 ↔ 测试台 通信协议（与固件 src/lib/proto/psu_proto.{h,c} 逐字节对齐）。

链路上的一帧 = **0x00 + COBS(帧内容) + 0x00**（前后各一个分隔符；接收端跳过空块），
帧内容（小端）：

    +--------+--------+--------+------------------+------------------+
    |  cmd   |  seq   |  len   |     payload      |      crc16       |
    | 1 字节 | 1 字节 | 1 字节 |     len 字节     |      2 字节      |
    +--------+--------+--------+------------------+------------------+
             |<------- CRC-16/CCITT-FALSE 覆盖范围 --------->|

cmd 的 bit7 表示方向：0 = 上位机→设备，1 = 设备→上位机。
seq 用于请求-响应配对：设备原样回填；设备主动上报（遥测/事件）填 0。

前置分隔符为什么必要：设备的 printf 日志文本也走这条 CDC，而文本里没有 0x00。
没有前置分隔符时，"一行日志 + 紧跟的帧"会粘成一个块，整块 CRC 失败 → 帧被丢掉。
有了前置分隔符，日志在帧之前就被切成一个独立的文本块交出去，帧本身完好。

分帧用 COBS（编码结果不含 0x00）：日志混进协议流时错位的块只会在 CRC 上失败被丢弃，
重同步是自动的。本模块因此既吐出"帧"，也把解不出来的块当"日志文本"交给上层显示。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Iterator

# ---------------------------------------------------------------- 帧格式常量
PROTO_VERSION = 1
MAX_PAYLOAD = 48           # payload 上限（保证整帧 ≤ 55 字节）
MAX_BODY = 3 + MAX_PAYLOAD + 2      # 帧内容最长 53
MAX_BLOCK = MAX_BODY + 1            # COBS 最坏开销 1 字节
MAX_ENCODED = MAX_BLOCK + 1         # 含尾部 0x00 分隔符 = 55
MAX_WIRE = MAX_ENCODED + 1          # 再加前置分隔符 = 56（仍 < CDC TX FIFO 的 64）
MIN_BODY = 5                        # cmd+seq+len+crc16


class ProtocolError(Exception):
    """帧结构不合法（长度字段不符、COBS 块损坏、块内出现 0x00）。"""


class CrcError(ProtocolError):
    """CRC 校验失败 —— 最常见的原因是被日志文本劈开或误码。"""


# ---------------------------------------------------------------- 命令字
class Cmd(IntEnum):
    """上位机 → 设备（bit7 = 0）。"""

    PING = 0x01         # payload: u32 nonce        → PONG
    GET_INFO = 0x02     # 无                        → INFO
    SET_VOUT = 0x03     # u8 unit, u16 val          → ACK(arg = 生效占空比‰)
    SET_ILIM = 0x04     # u8 unit, u16 val          → ACK(arg = 生效占空比‰)
    SET_OUTPUT = 0x05   # u8 on(0/1)                → ACK
    SAFE_STATE = 0x06   # 无（急停，永远接受）      → ACK
    RUN_TEST = 0x07     # 无                        → ACK
    STOP_TEST = 0x08    # 无                        → ACK
    SET_TELEM = 0x09    # u16 period_ms(0 = 停)     → ACK
    SET_REMOTE = 0x0A   # u8 enable, u16 timeout_ms → ACK
    GET_STATUS = 0x0B   # 无                        → STATUS + 若干 ITEM
    KEEPALIVE = 0x0C    # u32 host_ms               → ACK


class Rsp(IntEnum):
    """设备 → 上位机（bit7 = 1）。"""

    PONG = 0x81         # u32 nonce, u8 proto_ver, u8 role, u16 reserved
    INFO = 0x82         # 16 字节，见 Info
    TELEM = 0x83        # 16 字节，周期上报，见 Telemetry
    ACK = 0x84          # u8 acked_cmd, i8 code, u16 arg
    EVENT = 0x85        # u8 event + 事件数据
    ITEM = 0x86         # 42 字节测试项状态
    STATUS = 0x8B       # TELEM 16 字节 + u8 item_results[7] + u8 reserved = 24


class Unit(IntEnum):
    """SET_VOUT / SET_ILIM 的取值单位。"""

    PERMILLE = 0        # 原始占空比 0–1000‰（精确扫描用）
    MV = 1              # SET_VOUT 用 mV，SET_ILIM 用 mA


# ITEM 载荷里的定长字符串（含结尾 0，与固件 PSU_ITEM_*_LEN 一致）
ITEM_NAME_LEN = 12
ITEM_DETAIL_LEN = 22
ITEM_PAYLOAD_LEN = 4 + 4 + ITEM_NAME_LEN + ITEM_DETAIL_LEN      # 42


# INFO.caps 位
CAP_WATCHDOG = 0x01
CAP_ITEM_EVENTS = 0x02
CAP_VOUT_SENSE = 0x04
CAP_REMOTE = 0x08
CAP_TELEM_PERIOD = 0x10

# TELEM.flags 位
FLAG_CE_ON = 0x01
FLAG_MANUAL = 0x02
FLAG_REMOTE = 0x04
FLAG_WD_ARMED = 0x08
FLAG_VOUT_VALID = 0x10


class Event(IntEnum):
    TEST_START = 0x01   # 数据：u8 item_count
    TEST_END = 0x02     # 数据：u8 run_state
    REMOTE = 0x03       # 数据：u8 enable
    WATCHDOG = 0x04     # 数据：u16 timeout_ms（看门狗超时，设备已回安全态）


class ItemPhase(IntEnum):
    START = 1
    END = 2


class RunState(IntEnum):
    IDLE = 0
    RUNNING = 1
    DONE = 2
    ABORTED = 3


RUN_STATE_TEXT = {0: "IDLE", 1: "RUN", 2: "DONE", 3: "STOP"}


class PgState(IntEnum):
    FAULT = 0
    UNKNOWN = 1
    GOOD = 2


PG_STATE_TEXT = {0: "FAULT", 1: "?", 2: "GOOD"}


class TestResult(IntEnum):
    PENDING = 0
    RUNNING = 1
    PASS = 2
    FAIL = 3
    WARN = 4
    SKIP = 5
    MANUAL = 6      # 设定正确，需万用表/电子负载人工核对


RESULT_TEXT = {
    0: "-",
    1: "RUN",
    2: "PASS",
    3: "FAIL",
    4: "WARN",
    5: "SKIP",
    6: "MANL",
}


# ---------------------------------------------------------------- 编解码
def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE：poly 0x1021，初值 0xFFFF，不反射、不异或输出。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    """标准 COBS 编码（不含 0x00 分隔符）。"""
    out = bytearray()
    out.append(0)               # 预留给第一个 code 字节
    code_idx = 0
    code = 1
    for byte in data:
        if byte == 0x00:
            out[code_idx] = code
            code = 1
            code_idx = len(out)
            out.append(0)
        else:
            out.append(byte)
            code += 1
            if code == 0xFF:
                out[code_idx] = 0xFF
                code = 1
                code_idx = len(out)
                out.append(0)
    out[code_idx] = code
    return bytes(out)


def cobs_decode(block: bytes) -> bytes:
    """解码一段不含分隔符的 COBS 块；结构与固件 psu_cobs_decode 一致。"""
    out = bytearray()
    read = 0
    total = len(block)
    while read < total:
        code = block[read]
        read += 1
        if code == 0x00:
            raise ProtocolError("COBS 块内出现 0x00（0x00 只能做分隔符）")
        end = read + code - 1
        if end > total:
            raise ProtocolError("COBS 块被截断")
        out += block[read:end]
        read = end
        if code < 0xFF and read < total:
            out.append(0x00)
    return bytes(out)


@dataclass(frozen=True)
class Frame:
    """一条解出来的帧。"""

    cmd: int
    seq: int
    payload: bytes = b""

    @property
    def is_error(self) -> bool:
        return bool(self.cmd & 0x80)


def encode_frame(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    """组帧，返回链路上的完整字节（前置 + COBS + 尾部分隔符）。"""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload 超长：{len(payload)} > {MAX_PAYLOAD}")
    body = bytes((cmd & 0xFF, seq & 0xFF, len(payload))) + payload
    body += struct.pack("<H", crc16(body))
    return b"\x00" + cobs_encode(body) + b"\x00"


def decode_block(block: bytes) -> Frame:
    """解析一段不含分隔符的块，校验长度字段与 CRC。"""
    if len(block) < 2 or len(block) > MAX_BLOCK:
        raise ProtocolError(f"块长度 {len(block)} 不在 [2, {MAX_BLOCK}] 内")
    body = cobs_decode(block)
    if len(body) < MIN_BODY:
        raise ProtocolError(f"帧内容只有 {len(body)} 字节，短于最小长度 {MIN_BODY}")
    length = body[2]
    if length > MAX_PAYLOAD:
        raise ProtocolError(f"长度字段 {length} 超过 {MAX_PAYLOAD}")
    if len(body) != length + MIN_BODY:
        raise ProtocolError(f"长度字段 {length} 与实际 {len(body) - MIN_BODY} 不符")
    expect = struct.unpack_from("<H", body, 3 + length)[0]
    actual = crc16(body[: 3 + length])
    if expect != actual:
        raise CrcError(f"CRC 不符：帧内 0x{expect:04X}，算出 0x{actual:04X}")
    return Frame(cmd=body[0], seq=body[1], payload=body[3 : 3 + length])


@dataclass
class SplitterStats:
    """分流统计，给界面显示用。

    texts 包含设备日志文本，也包含"被日志或杂音打断的帧"——两者在链路上长得一样
    （都是一段解不出合法帧的字节），所以区分不了；crc_errors 给出其中 CRC 失败的数量，
    作为链路质量参考。
    """

    frames: int = 0
    texts: int = 0
    crc_errors: int = 0


@dataclass
class FrameSplitter:
    """增量分流：喂字节流，产出 ("frame", Frame) 或 ("text", str)。

    与固件的接收端对称：0x00 切块，块内解不出合法帧就当日志文本交出去
    （设备 printf 的日志没有 0x00，所以整行日志会攒到下一个分隔符才吐出来）。
    """

    # 攒到这么多字节还没见到分隔符就按文本立即吐出（遥测停发、设备狂打日志时别憋着）
    text_flush_limit: int = 256

    _buf: bytearray = field(default_factory=bytearray, init=False)
    stats: SplitterStats = field(default_factory=SplitterStats, init=False)

    def feed(self, data: bytes) -> Iterator[tuple[str, object]]:
        for byte in data:
            if byte == 0x00:
                yield from self._flush_block()
                continue
            self._buf.append(byte)
            if len(self._buf) >= self.text_flush_limit:
                self.stats.texts += 1
                yield ("text", self._as_text(bytes(self._buf)))
                self._buf.clear()

    def reset(self) -> None:
        """断开/重连时清掉半截数据，避免把旧字节和新帧拼在一起。"""
        self._buf.clear()

    def _flush_block(self) -> Iterator[tuple[str, object]]:
        if not self._buf:
            return
        block = bytes(self._buf)
        self._buf.clear()
        try:
            frame = decode_block(block)
        except CrcError:
            # 帧被打断/误码：整块当文本交出去（内容可能是日志 + 半截帧）
            self.stats.crc_errors += 1
            self.stats.texts += 1
            yield ("text", self._as_text(block))
            return
        except ProtocolError:
            # 结构不合法：设备 printf 的日志行就走这条路径（文本里没有 0x00，
            # 攒到下一个分隔符时被当成一个"块"，但它的首字节不是合法 COBS code）
            self.stats.texts += 1
            yield ("text", self._as_text(block))
            return
        self.stats.frames += 1
        yield ("frame", frame)

    @staticmethod
    def _as_text(block: bytes) -> str:
        return block.decode("utf-8", errors="replace")


# ---------------------------------------------------------------- 载荷解析
@dataclass(frozen=True)
class Info:
    """INFO（16 字节）：板级常量由设备下发，上位机不重复写死任何换算常数。"""

    proto_ver: int
    fw_major: int
    fw_minor: int
    item_count: int
    vout_full_mv: int       # 占空比 100% 时的电压设定
    vout_min_mv: int        # 占空比 0% 时的电压设定
    ilim_full_ma: int       # 占空比 100% 时的限流设定
    iin_lim_ma: int         # 固定输入限流（仅显示）
    pwm_freq_khz: int
    caps: int

    @classmethod
    def parse(cls, payload: bytes) -> "Info":
        if len(payload) != 16:
            raise ProtocolError(f"INFO 应为 16 字节，实为 {len(payload)}")
        (proto, major, minor, items,
         vfull, vmin, ifull, iin, freq, caps, _res) = struct.unpack("<BBBBHHHHHBB", payload)
        return cls(proto, major, minor, items, vfull, vmin, ifull, iin, freq, caps)

    @property
    def supports(self) -> str:
        bits = [
            (CAP_WATCHDOG, "看门狗"),
            (CAP_ITEM_EVENTS, "测试项事件"),
            (CAP_VOUT_SENSE, "VOUT 实测"),
            (CAP_REMOTE, "远程模式"),
            (CAP_TELEM_PERIOD, "遥测周期"),
        ]
        return " ".join(name for bit, name in bits if self.caps & bit) or "无"


@dataclass(frozen=True)
class Telemetry:
    """TELEM（16 字节），也是 STATUS 的前 16 字节。"""

    flags: int
    run_state: int
    cur_index: int
    pg_state: int
    pwm_permille: int
    ipwm_permille: int
    pg_mv: int
    vout_mv: int
    tick_ms: int

    @classmethod
    def parse(cls, payload: bytes) -> "Telemetry":
        if len(payload) < 16:
            raise ProtocolError(f"TELEM 至少 16 字节，实为 {len(payload)}")
        flags, run_state, cur_index, pg_state, pwm, ipwm, pg_mv, vout_mv, tick = struct.unpack_from(
            "<BBBBHHHHI", payload, 0
        )
        return cls(flags, run_state, cur_index, pg_state, pwm, ipwm, pg_mv, vout_mv, tick)

    @property
    def ce_on(self) -> bool:
        return bool(self.flags & FLAG_CE_ON)

    @property
    def manual(self) -> bool:
        return bool(self.flags & FLAG_MANUAL)

    @property
    def remote(self) -> bool:
        return bool(self.flags & FLAG_REMOTE)

    @property
    def wd_armed(self) -> bool:
        return bool(self.flags & FLAG_WD_ARMED)

    @property
    def vout_valid(self) -> bool:
        return bool(self.flags & FLAG_VOUT_VALID)

    def vout_setpoint_mv(self, info: Info | None) -> int | None:
        """由占空比换算设定电压；常量来自 INFO，没有 INFO 时不猜。"""
        if info is None:
            return None
        return vout_mv_from_permille(self.pwm_permille, info)

    def ilim_setpoint_ma(self, info: Info | None) -> int | None:
        if info is None:
            return None
        return ilim_ma_from_permille(self.ipwm_permille, info)


@dataclass(frozen=True)
class Ack:
    """ACK（4 字节）：被应答的命令、exit_code_t、附加数值。"""

    acked_cmd: int
    code: int
    arg: int

    @classmethod
    def parse(cls, payload: bytes) -> "Ack":
        if len(payload) != 4:
            raise ProtocolError(f"ACK 应为 4 字节，实为 {len(payload)}")
        acked, code, arg = struct.unpack("<BbH", payload)
        return cls(acked, code, arg)

    @property
    def ok(self) -> bool:
        return self.code == 0


@dataclass(frozen=True)
class ItemInfo:
    """ITEM（42 字节）：某个测试项开始或出结果。"""

    phase: int
    index: int
    result: int
    elapsed_ms: int
    name: str
    detail: str

    @classmethod
    def parse(cls, payload: bytes) -> "ItemInfo":
        if len(payload) != ITEM_PAYLOAD_LEN:
            raise ProtocolError(f"ITEM 应为 {ITEM_PAYLOAD_LEN} 字节，实为 {len(payload)}")
        phase, index, result, _res, elapsed = struct.unpack_from("<BBBBI", payload, 0)
        name = payload[8:20].split(b"\x00", 1)[0].decode("ascii", errors="replace")
        detail = payload[20:42].split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        return cls(phase, index, result, elapsed, name, detail)


def parse_event(payload: bytes) -> tuple[int, bytes]:
    """事件 → (事件号, 事件数据)。"""
    if not payload:
        raise ProtocolError("EVENT 至少 1 字节")
    return payload[0], payload[1:]


def parse_status_items(payload: bytes, item_count: int) -> tuple[Telemetry, list[int]]:
    """STATUS → (遥测块, 7 项结果码)。"""
    telemetry = Telemetry.parse(payload)
    results = list(payload[16 : 16 + item_count])
    if len(results) < item_count:
        raise ProtocolError(f"STATUS 里只有 {len(results)} 项结果，应有 {item_count} 项")
    return telemetry, results


# ---------------------------------------------------------------- 单位换算
# 与固件 psu_link.c 的 vout_to_permille / ilim_to_permille 互为逆运算。
# 常量一律来自 INFO 帧，上位机不写死任何板级数字。
def vout_mv_from_permille(permille: int, info: Info) -> int:
    """VSET(D) = FULL × (1 + 5D) / 6，D = permille/1000。"""
    return (info.vout_full_mv * (1000 + 5 * permille)) // 6000


def vout_permille_from_mv(mv: int, info: Info) -> int:
    """D = (6·VSET/FULL − 1) / 5，四舍五入到 ‰，并夹在合法范围内。"""
    if mv <= info.vout_min_mv:
        return 0
    if mv >= info.vout_full_mv:
        return 1000
    num = 1200 * mv - 200 * info.vout_full_mv
    return (num + info.vout_full_mv // 2) // info.vout_full_mv


def ilim_ma_from_permille(permille: int, info: Info) -> int:
    return (info.ilim_full_ma * permille) // 1000


def ilim_permille_from_ma(ma: int, info: Info) -> int:
    if ma <= 0:
        return 0
    if ma >= info.ilim_full_ma:
        return 1000
    return (ma * 1000 + info.ilim_full_ma // 2) // info.ilim_full_ma


# ---------------------------------------------------------------- 载荷构造
def pack_u16(value: int) -> bytes:
    return struct.pack("<H", value & 0xFFFF)


def pack_u32(value: int) -> bytes:
    return struct.pack("<I", value & 0xFFFFFFFF)


def unpack_u16(data: bytes) -> int:
    return struct.unpack_from("<H", data, 0)[0]


def unpack_u32(data: bytes) -> int:
    return struct.unpack_from("<I", data, 0)[0]


def payload_setpoint(unit: Unit, value: int) -> bytes:
    return bytes((int(unit),)) + pack_u16(value)


def payload_set_remote(enable: bool, timeout_ms: int) -> bytes:
    return bytes((1 if enable else 0,)) + pack_u16(timeout_ms)


# ---------------------------------------------------------------- 错误码
# 与固件 lib/tools/common_def.h 的 exit_code_t 对齐
EXIT_CODE_TEXT = {
    1: "EXIT_SKIP",
    0: "EXIT_OK",
    -1: "EXIT_FAIL",
    -2: "EXIT_TIMEOUT",
    -3: "EXIT_INVALID_PARAM",
    -4: "EXIT_NOT_SUPPORTED",
    -5: "EXIT_NO_MEMORY",
    -6: "EXIT_BUSY",
    -7: "EXIT_NO_RESOURCE",
    -8: "EXIT_ALREADY_EXISTS",
    -9: "EXIT_DOES_NOT_EXIST",
    -10: "EXIT_NOT_INITIALIZED",
    -11: "EXIT_ALREADY_INITIALIZED",
    -12: "EXIT_CRC_MISMATCH",
    -13: "EXIT_HW_FAILURE",
    -14: "EXIT_UNKNOWN",
    2: "EXIT_IN_PROGRESS",
}


def exit_code_text(code: int) -> str:
    return EXIT_CODE_TEXT.get(code, f"code({code})")
