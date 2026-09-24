"""纯 Python 设备模拟器：实现固件同一张命令表，不需要真板子。

用途：
    1. 没有硬件的场合跑通界面 / CLI（`psu-host --dummy monitor`）
    2. 作为端到端自测：主机侧的握手、命令+ACK、keepalive、看门狗、CSV 全流程
       都能对着它验证 —— 它按固件语义回帧（含 EXIT_BUSY / EXIT_INVALID_PARAM 分支）

刻意模仿固件行为的地方：
    - SET_VOUT/SET_ILIM 的单位换算与越界拒绝（psu_link.c 的 vout_to_permille）
    - 遥测只发占空比‰，mV/mA 由上位机用 INFO 常量换算
    - 看门狗超时 → 急停回安全态 + 退出远程 + EV_WATCHDOG
    - 测试序列按时间推进，逐项上报 ITEM 帧（含 EVENT TEST_START/END）
    - device → host 的 printf 日志文本也照发，用来验证主机的文本/帧分流
"""

from __future__ import annotations

import struct
import threading
import time
from dataclasses import dataclass, field

from . import protocol as P
from .protocol import Cmd, Frame, FrameSplitter, Info, Rsp, Unit
from .transport import Transport

# 与 board_config.h 一致的板级常量（模拟器就是"另一台设备"，这些值该它自己定）
VOUT_FULL_MV = 23400
VOUT_MIN_MV = VOUT_FULL_MV // 6        # D=0 时设定缩到 1/6
ILIM_FULL_MA = 5040
IIN_LIM_MA = 4200
PWM_FREQ_KHZ = 50
PG_GOOD_MV = 3312                      # 建立后：3V3 经 R5，开漏释放
PG_FAULT_MV = 2104                     # 故障/关断：被 LED3 压降钳住

TEST_ITEM_COUNT = 7
TICK_MS = 20                           # 模拟器内部节拍


@dataclass
class _SimItem:
    """测试序列的一项：时长 + 结果 + 详情。"""

    name: str
    duration_ms: int
    result: int
    detail: str
    ce_on: bool = False
    pwm: int = 0
    ipwm: int = 0


# 一次跑完约 2 秒，够界面动起来看
SEQUENCE: list[_SimItem] = [
    _SimItem("SELF-IMU", 200, P.TestResult.PASS, "0x23 ok"),
    _SimItem("SAFE-STATE", 150, P.TestResult.PASS, "PG=2.10V"),
    _SimItem("PG-ENABLE", 400, P.TestResult.PASS, "t=120ms H=3.31V", ce_on=True, ipwm=1000),
    _SimItem("PG-DISABLE", 200, P.TestResult.PASS, "t=8ms L=2.10V"),
    _SimItem("PG-LEVELS", 150, P.TestResult.WARN, "H=3.31 L=2.10"),
    _SimItem("PWM-SWEEP", 500, P.TestResult.MANUAL, "5 pts 3.9-23.4V", ce_on=True, ipwm=1000, pwm=1000),
    _SimItem("IPWM-SWEEP", 500, P.TestResult.MANUAL, "5 pts 0-5.04A", ce_on=True, ipwm=1000, pwm=0),
]


@dataclass
class _SimState:
    ce_on: bool = False
    manual: bool = False
    remote: bool = False
    wd_timeout_ms: int = 0
    wd_last: float = 0.0
    pwm_permille: int = 0
    ipwm_permille: int = 0
    run_state: int = P.RunState.IDLE
    cur_index: int = 0
    telem_period_ms: int = 100
    telem_next: float = 0.0
    items: list[dict] = field(default_factory=list)


class DummyTransport(Transport):
    """进程内模拟器，接口与 SerialTransport 相同，直接喂给 Link。"""

    name = "dummy"

    def __init__(self) -> None:
        self._lock = threading.Condition()
        self._out = bytearray()              # 设备 → 主机：帧 + 日志文本
        self._splitter = FrameSplitter()
        self._closed = False
        self._started = time.monotonic()

        self.info = Info(
            proto_ver=P.PROTO_VERSION,
            fw_major=0,
            fw_minor=1,
            item_count=TEST_ITEM_COUNT,
            vout_full_mv=VOUT_FULL_MV,
            vout_min_mv=VOUT_MIN_MV,
            ilim_full_ma=ILIM_FULL_MA,
            iin_lim_ma=IIN_LIM_MA,
            pwm_freq_khz=PWM_FREQ_KHZ,
            caps=P.CAP_WATCHDOG | P.CAP_ITEM_EVENTS | P.CAP_REMOTE | P.CAP_TELEM_PERIOD,
        )
        self.state = _SimState()
        self.state.wd_last = time.monotonic()
        self.state.telem_next = time.monotonic() + self.state.telem_period_ms / 1000.0
        self.state.items = [
            {"result": P.TestResult.PENDING, "elapsed_ms": 0, "detail": ""}
            for _ in range(TEST_ITEM_COUNT)
        ]
        self._seq_item = -1
        self._seq_item_started = 0.0
        self._thread = threading.Thread(target=self._tick_loop, name="psu-dummy", daemon=True)
        self._thread.start()

    # ---------------- Transport 接口 ----------------
    def read(self, size: int, timeout: float = 0.05) -> bytes:
        with self._lock:
            deadline = time.monotonic() + max(timeout, 0.0)
            while not self._out and not self._closed:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return b""
                self._lock.wait(remaining)
            if not self._out:
                if self._closed:
                    raise OSError("模拟器已关闭")
                return b""
            data = bytes(self._out[:size])
            del self._out[:size]
            return data

    def write(self, data: bytes) -> int:
        for kind, value in self._splitter.feed(data):
            if kind == "frame":
                with self._lock:
                    self.state.wd_last = time.monotonic()   # 任何有效帧都算"主机还在"
                self._handle(value)                        # type: ignore[arg-type]
            else:
                self._text(f"[dummy] 收到非协议字节：{value.strip()!r}")
        return len(data)

    def close(self) -> None:
        with self._lock:
            self._closed = True
            self._lock.notify_all()

    @property
    def is_open(self) -> bool:
        return not self._closed

    # ---------------- 设备 → 主机 输出 ----------------
    def _frame(self, cmd: int, seq: int, payload: bytes = b"") -> None:
        with self._lock:
            if self._closed:
                return
            self._out += P.encode_frame(cmd, seq, payload)
            self._lock.notify_all()

    def _text(self, line: str) -> None:
        """模仿固件 printf：文本不带 0x00，靠下一条帧的 0x00 才被分出来。"""
        with self._lock:
            if self._closed:
                return
            self._out += (line + "\n").encode("utf-8")
            self._lock.notify_all()

    def _ack(self, seq: int, cmd: int, code: int = 0, arg: int = 0) -> None:
        self._frame(Rsp.ACK, seq, struct.pack("<BbH", cmd, code, arg))

    def _event(self, event: int, data: bytes = b"") -> None:
        self._frame(Rsp.EVENT, 0, bytes((event,)) + data)

    def _item_frame(self, index: int, phase: int) -> None:
        item = self.state.items[index]
        name = SEQUENCE[index].name[: P.ITEM_NAME_LEN - 1].encode("ascii", "replace")
        detail = item["detail"][: P.ITEM_DETAIL_LEN - 1].encode("utf-8", "replace")
        payload = (
            struct.pack("<BBBBI", phase, index, item["result"], 0, item["elapsed_ms"])
            + name.ljust(P.ITEM_NAME_LEN, b"\x00")
            + detail.ljust(P.ITEM_DETAIL_LEN, b"\x00")
        )
        self._frame(Rsp.ITEM, 0, payload)

    def _live_block(self) -> bytes:
        state = self.state
        flags = 0
        if state.ce_on:
            flags |= P.FLAG_CE_ON
        if state.manual:
            flags |= P.FLAG_MANUAL
        if state.remote:
            flags |= P.FLAG_REMOTE
        if state.wd_timeout_ms:
            flags |= P.FLAG_WD_ARMED
        pg_mv = PG_GOOD_MV if state.ce_on else PG_FAULT_MV
        pg_state = P.PgState.GOOD if state.ce_on else P.PgState.FAULT
        tick_ms = int((time.monotonic() - self._started) * 1000) & 0xFFFFFFFF
        return struct.pack(
            "<BBBBHHHHI",
            flags,
            state.run_state,
            state.cur_index,
            pg_state,
            state.pwm_permille,
            state.ipwm_permille,
            pg_mv,
            0,                      # 板上没有实测 VOUT 分压
            tick_ms,
        )

    def _send_telem(self) -> None:
        self._frame(Rsp.TELEM, 0, self._live_block())

    def _send_status(self, seq: int) -> None:
        results = bytes(item["result"] for item in self.state.items)
        self._frame(Rsp.STATUS, seq, self._live_block() + results + b"\x00")
        # 7 项全发（含 PENDING）：主机靠 ITEM 帧拿到测试项名字，与固件行为一致
        for index in range(TEST_ITEM_COUNT):
            self._item_frame(index, P.ItemPhase.END)

    # ---------------- 主机命令 ----------------
    def _handle(self, frame: Frame) -> None:
        cmd = frame.cmd
        payload = frame.payload
        seq = frame.seq

        if cmd == Cmd.PING:
            nonce = payload[:4] if len(payload) >= 4 else b"\x00" * 4
            self._frame(Rsp.PONG, seq, nonce + bytes((P.PROTO_VERSION, 0)) + struct.pack("<H", 0))
        elif cmd == Cmd.GET_INFO:
            info = self.info
            self._frame(
                Rsp.INFO,
                seq,
                struct.pack(
                    "<BBBBHHHHHBB",
                    info.proto_ver,
                    info.fw_major,
                    info.fw_minor,
                    info.item_count,
                    info.vout_full_mv,
                    info.vout_min_mv,
                    info.ilim_full_ma,
                    info.iin_lim_ma,
                    info.pwm_freq_khz,
                    info.caps,
                    0,
                ),
            )
        elif cmd == Cmd.GET_STATUS:
            self._send_status(seq)
        elif cmd in (Cmd.SET_VOUT, Cmd.SET_ILIM):
            self._setpoint(seq, cmd, payload)
        elif cmd == Cmd.SET_OUTPUT:
            if len(payload) != 1 or payload[0] > 1:
                self._ack(seq, cmd, -3)
            elif self.state.run_state == P.RunState.RUNNING:
                self._ack(seq, cmd, -6)                     # EXIT_BUSY
            else:
                self.state.ce_on = payload[0] == 1
                self.state.manual = True
                self._ack(seq, cmd, 0, payload[0])
                self._text(f"[SET] CE={'ON' if self.state.ce_on else 'OFF'}")
        elif cmd == Cmd.SAFE_STATE:
            self._safe_state()
            self._ack(seq, cmd)
        elif cmd == Cmd.RUN_TEST:
            if self.state.run_state == P.RunState.RUNNING:
                self._ack(seq, cmd, -6)
            else:
                self._start_sequence()
                self._ack(seq, cmd)
        elif cmd == Cmd.STOP_TEST:
            if self.state.run_state == P.RunState.RUNNING:
                self._finish_sequence(P.RunState.ABORTED)
            self._ack(seq, cmd)
        elif cmd == Cmd.SET_TELEM:
            period = struct.unpack_from("<H", payload, 0)[0] if len(payload) == 2 else -1
            if period == 0:
                self.state.telem_period_ms = 0
                self._ack(seq, cmd, 0, 0)
            elif 20 <= period <= 1000:
                self.state.telem_period_ms = period
                self.state.telem_next = time.monotonic() + period / 1000.0
                self._ack(seq, cmd, 0, period)
            else:
                self._ack(seq, cmd, -3, max(period, 0))
        elif cmd == Cmd.SET_REMOTE:
            enable = payload[0] if len(payload) == 3 else 9
            timeout = struct.unpack_from("<H", payload, 1)[0] if len(payload) == 3 else 0
            if enable > 1 or (enable and timeout and not (200 <= timeout <= 60000)):
                self._ack(seq, cmd, -3, timeout)
            else:
                changed = self.state.remote != bool(enable)
                self.state.remote = bool(enable)
                self.state.wd_timeout_ms = timeout if enable else 0
                self.state.wd_last = time.monotonic()
                self._ack(seq, cmd, 0, timeout)
                if changed:                 # 与固件一致：只在状态真的变了才上报
                    self._event(P.Event.REMOTE, bytes((1 if enable else 0,)))
                self._text(
                    f"[LINK] 远程模式 {'开启' if enable else '关闭'}（看门狗 {self.state.wd_timeout_ms}ms）"
                )
        elif cmd == Cmd.KEEPALIVE:
            self._ack(seq, cmd)
        else:
            self._ack(seq, cmd, -4)                          # EXIT_NOT_SUPPORTED

    def _setpoint(self, seq: int, cmd: int, payload: bytes) -> None:
        if len(payload) != 3:
            self._ack(seq, cmd, -3)
            return
        unit = payload[0]
        value = struct.unpack_from("<H", payload, 1)[0]
        permille = self._to_permille(cmd, unit, value)
        if permille is None:
            self._ack(seq, cmd, -3, 0)
            return
        if self.state.run_state == P.RunState.RUNNING:
            self._ack(seq, cmd, -6, permille)               # EXIT_BUSY
            return
        if cmd == Cmd.SET_VOUT:
            self.state.pwm_permille = permille
            self._text(f"[SET] VSET={self.vout_setpoint_mv()}mV (D={permille // 10}%)")
        else:
            self.state.ipwm_permille = permille
            self._text(f"[SET] ILIM={self.ilim_setpoint_ma()}mA (D={permille // 10}%)")
        self.state.manual = True
        self._ack(seq, cmd, 0, permille)

    def _to_permille(self, cmd: int, unit: int, value: int) -> int | None:
        """与固件 psu_link.c 的换算/拒绝规则一致。"""
        if unit == Unit.PERMILLE:
            return value if value <= 1000 else None
        if unit != Unit.MV:
            return None
        if cmd == Cmd.SET_VOUT:
            if not (VOUT_MIN_MV <= value <= VOUT_FULL_MV):
                return None
            num = 1200 * value - 200 * VOUT_FULL_MV
            return (num + VOUT_FULL_MV // 2) // VOUT_FULL_MV
        if not (0 <= value <= ILIM_FULL_MA):
            return None
        return (value * 1000 + ILIM_FULL_MA // 2) // ILIM_FULL_MA

    # ---------------- 板级换算（给日志文本用） ----------------
    def vout_setpoint_mv(self) -> int:
        """VSET(D) = FULL × (1 + 5D) / 6，与固件 pcb_ctrl_vout_setpoint_mv 一致。"""
        return (VOUT_FULL_MV * (1000 + 5 * self.state.pwm_permille)) // 6000

    def ilim_setpoint_ma(self) -> int:
        return (ILIM_FULL_MA * self.state.ipwm_permille) // 1000

    # ---------------- 安全态 / 序列 ----------------
    def _safe_state(self) -> None:
        state = self.state
        state.ce_on = False
        state.manual = False
        state.pwm_permille = 0
        state.ipwm_permille = 0
        if state.remote:
            state.remote = False
            state.wd_timeout_ms = 0
            self._event(P.Event.REMOTE, b"\x00")
        if state.run_state == P.RunState.RUNNING:
            self._finish_sequence(P.RunState.ABORTED)
        self._text("[SAFE] 安全态：CE# 关断、PWM 0%、IPWM 0%")

    def _start_sequence(self) -> None:
        state = self.state
        state.run_state = P.RunState.RUNNING
        state.cur_index = 0
        state.manual = False
        for item in state.items:
            item.update(result=P.TestResult.PENDING, elapsed_ms=0, detail="")
        for index in range(TEST_ITEM_COUNT):
            self._item_frame(index, P.ItemPhase.END)
        self._event(P.Event.TEST_START, bytes((TEST_ITEM_COUNT,)))
        self._text("==== PCB 测试开始 ====")
        self._seq_item = -1
        self._seq_item_started = time.monotonic()
        self._seq_apply_current()

    def _seq_apply_current(self) -> None:
        """把当前测试项对应的控制脚状态搬进模拟状态。"""
        index = self._seq_item
        if index < 0 or index >= TEST_ITEM_COUNT:
            return
        spec = SEQUENCE[index]
        state = self.state
        state.cur_index = index
        state.ce_on = spec.ce_on
        state.pwm_permille = spec.pwm
        state.ipwm_permille = spec.ipwm
        state.items[index]["result"] = P.TestResult.RUNNING
        self._item_frame(index, P.ItemPhase.START)
        self._text(f"[{index + 1}/{TEST_ITEM_COUNT}] {spec.name} ...")

    def _finish_sequence(self, run_state: int) -> None:
        state = self.state
        state.run_state = run_state
        state.ce_on = False
        state.pwm_permille = 0
        state.ipwm_permille = 0
        state.manual = False
        self._event(P.Event.TEST_END, bytes((run_state,)))
        self._text(f"==== PCB 测试结束 ({'完成' if run_state == P.RunState.DONE else '已急停'}) ====")

    def _advance_sequence(self, now: float) -> None:
        state = self.state
        if state.run_state != P.RunState.RUNNING:
            return
        elapsed_ms = (now - self._seq_item_started) * 1000.0
        if self._seq_item >= 0:
            spec = SEQUENCE[self._seq_item]
            state.items[self._seq_item]["elapsed_ms"] = int(elapsed_ms)
            if elapsed_ms < spec.duration_ms:
                return
            state.items[self._seq_item].update(
                result=spec.result, elapsed_ms=int(elapsed_ms), detail=spec.detail
            )
            self._item_frame(self._seq_item, P.ItemPhase.END)
            self._text(
                f"[{self._seq_item + 1}/{TEST_ITEM_COUNT}] {spec.name} -> "
                f"{P.RESULT_TEXT.get(spec.result, '?')} {spec.detail} ({int(elapsed_ms)}ms)"
            )
        self._seq_item += 1
        self._seq_item_started = now
        if self._seq_item >= TEST_ITEM_COUNT:
            self._finish_sequence(P.RunState.DONE)
            return
        self._seq_apply_current()

    # ---------------- 内部节拍 ----------------
    def _tick_loop(self) -> None:
        while True:
            now = time.monotonic()
            with self._lock:
                if self._closed:
                    return
            self._advance_sequence(now)

            state = self.state
            if state.telem_period_ms and now >= state.telem_next:
                state.telem_next = now + state.telem_period_ms / 1000.0
                self._send_telem()

            if state.wd_timeout_ms and (now - state.wd_last) * 1000.0 > state.wd_timeout_ms:
                timeout = state.wd_timeout_ms
                state.wd_timeout_ms = 0
                self._safe_state()
                self._event(P.Event.WATCHDOG, struct.pack("<H", timeout))
                self._text(f"[LINK] 看门狗超时：已回安全态并退出远程")

            time.sleep(TICK_MS / 1000.0)
