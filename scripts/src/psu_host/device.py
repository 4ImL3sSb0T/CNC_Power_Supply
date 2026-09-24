"""设备会话：握手、命令 + ACK 重试、keepalive、看门狗、遥测缓存、CSV 记录。

协议层（protocol.py）不认识"会话"，这里才是有状态的一层：
    - 每条命令等 ACK（默认 250 ms 超时、重试 3 次）
    - 主动上报的 TELEM/EVENT/ITEM/STATUS 在这里解成结构化对象并回调出去
    - keepalive 线程按周期发心跳，顺便测 RTT；设备侧看门狗靠它续命

安全约定：`disconnect(safe=True)`（默认）会先急停回安全态再退出远程。
拔线这种情况发不出命令，由设备侧看门狗兜底 —— 见 README 的上板手册。
"""

from __future__ import annotations

import csv
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Iterable

from . import protocol as P
from .link import Link
from .protocol import Ack, Cmd, Frame, Info, ItemInfo, ProtocolError, Rsp, Telemetry, Unit
from .transport import BAUD_DEFAULT, SerialTransport, Transport

LogCallback = Callable[[str], None]
TelemetryCallback = Callable[[Telemetry], None]
EventCallback = Callable[[int, bytes], None]
ItemCallback = Callable[[ItemInfo], None]
StateCallback = Callable[[str], None]

CSV_HEADER = [
    "host_time",
    "tick_ms",
    "run_state",
    "ce_on",
    "manual",
    "remote",
    "wd_armed",
    "pwm_permille",
    "vset_mv",
    "ipwm_permille",
    "ilim_ma",
    "pg_mv",
    "pg_state",
    "vout_mv",
]


class DeviceError(Exception):
    """会话层错误（发送失败、超时、应答异常）。"""


class CommandTimeout(DeviceError):
    """命令重试用尽仍没有收到应答。"""


class CommandRejected(DeviceError):
    """设备明确拒绝（ACK 里带非 0 的 exit_code_t）。"""

    def __init__(self, cmd: int, ack: Ack) -> None:
        try:
            name = P.Cmd(cmd).name
        except ValueError:
            name = f"cmd({cmd:#04x})"
        super().__init__(f"{name} 被拒绝：{P.exit_code_text(ack.code)}")
        self.cmd = cmd
        self.ack = ack


@dataclass
class ItemState:
    """界面上"测试项结果表"的一行。"""

    index: int
    name: str = ""
    result: int = P.TestResult.PENDING
    elapsed_ms: int = 0
    detail: str = ""

    def update(self, item: ItemInfo) -> None:
        self.index = item.index
        if item.name:
            self.name = item.name
        self.result = item.result
        self.elapsed_ms = item.elapsed_ms
        self.detail = item.detail

    @property
    def result_text(self) -> str:
        return P.RESULT_TEXT.get(self.result, "?")


class _Waiter:
    """等一条指定 seq 的响应。"""

    __slots__ = ("event", "frame")

    def __init__(self) -> None:
        self.event = threading.Event()
        self.frame: Frame | None = None

    def feed(self, frame: Frame) -> None:
        self.frame = frame
        self.event.set()


class PsuDevice:
    """一台设备的会话（真机或 dummy 模拟器共用）。"""

    def __init__(
        self,
        link: Link,
        on_log: LogCallback | None = None,
        on_telemetry: TelemetryCallback | None = None,
        on_event: EventCallback | None = None,
        on_item: ItemCallback | None = None,
        on_state: StateCallback | None = None,
    ) -> None:
        self.link = link
        self.on_log = on_log
        self.on_telemetry = on_telemetry
        self.on_event = on_event
        self.on_item = on_item
        self.on_state = on_state

        self.info: Info | None = None
        self.telemetry: Telemetry | None = None
        self.items: list[ItemState] = [ItemState(i) for i in range(7)]
        self.connected = True
        self.last_rtt_ms: float | None = None
        self.protocol_version: int | None = None

        self._waiters: dict[int, _Waiter] = {}
        self._waiters_lock = threading.Lock()
        self._seq = 0
        self._ka_stop = threading.Event()
        self._ka_thread: threading.Thread | None = None
        self._csv_file: Any = None
        self._csv_writer: Any = None
        self._csv_path: Path | None = None

        link.on_frame = self.handle_frame
        link.on_closed = self._handle_closed

    # ---------------- 握手与命令 ----------------
    def handshake(self, timeout: float = 0.5, retries: int = 3) -> Info:
        """PING → INFO → STATUS。设备侧刚上电或刚插上时多给几次重试。"""
        nonce = int(time.monotonic() * 1000) & 0xFFFFFFFF
        pong = self.request(Cmd.PING, P.pack_u32(nonce), expect={Rsp.PONG}, timeout=timeout, retries=retries)
        if len(pong.payload) >= 5:
            self.protocol_version = pong.payload[4]
        info_frame = self.request(Cmd.GET_INFO, expect={Rsp.INFO}, timeout=timeout, retries=retries)
        self.info = Info.parse(info_frame.payload)
        self.get_status()
        return self.info

    def request(
        self,
        cmd: int,
        payload: bytes = b"",
        expect: Iterable[int] | None = None,
        timeout: float = 0.25,
        retries: int = 3,
    ) -> Frame:
        """发一条命令并等指定的响应（按 seq 配对），超时就重发。"""
        wanted = set(expect) if expect is not None else None
        if not self.connected:
            raise DeviceError("链路已断开")

        last_error: Exception = CommandTimeout(f"{cmd:#04x} 无应答")
        for _ in range(max(1, retries)):
            seq = self._next_seq()
            waiter = _Waiter()
            with self._waiters_lock:
                self._waiters[seq] = waiter
            try:
                self.link.send(cmd, seq, payload)
            except Exception as exc:
                self._drop_waiter(seq)
                raise DeviceError(f"发送失败：{exc}") from exc

            if waiter.event.wait(timeout):
                frame = waiter.frame
                assert frame is not None
                if wanted is not None and frame.cmd not in wanted:
                    raise ProtocolError(f"等 {wanted}，收到 {frame.cmd:#04x}")
                return frame
            self._drop_waiter(seq)
            last_error = CommandTimeout(f"{cmd:#04x} 等 {timeout * 1000:.0f}ms 无应答")

        raise last_error

    def command(
        self,
        cmd: int,
        payload: bytes = b"",
        timeout: float = 0.25,
        retries: int = 3,
        check: bool = True,
    ) -> Ack:
        """发一条命令并解析 ACK。check=True 时非 0 错误码抛 CommandRejected。"""
        ack = Ack.parse(self.request(cmd, payload, expect={Rsp.ACK}, timeout=timeout, retries=retries).payload)
        if check and not ack.ok:
            raise CommandRejected(ack.acked_cmd, ack)
        return ack

    def _next_seq(self) -> int:
        """1..255 循环（0 留给设备主动上报，避免和响应混淆）。"""
        self._seq = self._seq % 255 + 1
        return self._seq

    def _drop_waiter(self, seq: int) -> None:
        with self._waiters_lock:
            self._waiters.pop(seq, None)

    # ---------------- 具体命令 ----------------
    def set_vout(self, value: int, unit: Unit = Unit.MV, **kw: Any) -> Ack:
        """设定输出电压。unit=MV 时 value 是 mV，unit=PERMILLE 时是 0–1000‰。"""
        return self.command(Cmd.SET_VOUT, P.payload_setpoint(unit, value), **kw)

    def set_ilim(self, value: int, unit: Unit = Unit.MV, **kw: Any) -> Ack:
        """设定限流。unit=MV 时 value 是 mA，unit=PERMILLE 时是 0–1000‰。"""
        return self.command(Cmd.SET_ILIM, P.payload_setpoint(unit, value), **kw)

    def set_output(self, on: bool, **kw: Any) -> Ack:
        return self.command(Cmd.SET_OUTPUT, bytes((1 if on else 0,)), **kw)

    def safe_state(self, **kw: Any) -> Ack:
        """急停：CE# 关断 + 双 PWM 0%。设备侧会同时退出远程模式。"""
        return self.command(Cmd.SAFE_STATE, **kw)

    def run_test(self, **kw: Any) -> Ack:
        return self.command(Cmd.RUN_TEST, **kw)

    def stop_test(self, **kw: Any) -> Ack:
        return self.command(Cmd.STOP_TEST, **kw)

    def set_telem_period(self, period_ms: int, **kw: Any) -> Ack:
        """遥测周期，0 = 停止上报（设备侧允许 20–1000 ms）。"""
        return self.command(Cmd.SET_TELEM, P.pack_u16(period_ms), **kw)

    def set_remote(self, enable: bool, timeout_ms: int = 0, **kw: Any) -> Ack:
        """进/出远程模式。enable=True 且 timeout_ms>0 时武装设备侧看门狗。"""
        return self.command(Cmd.SET_REMOTE, P.payload_set_remote(enable, timeout_ms), **kw)

    def get_status(self, **kw: Any) -> None:
        """拉一次完整快照（含 7 项测试结果），结果通过回调派发。"""
        self.request(Cmd.GET_STATUS, expect={Rsp.STATUS}, **kw)

    def arm_watchdog(self, timeout_ms: int = 3000, keepalive_ms: int | None = None) -> None:
        """进远程模式 + 武装看门狗 + 起 keepalive 线程（默认周期是超时的 1/3）。"""
        self.set_remote(True, timeout_ms)
        self.start_keepalive(keepalive_ms if keepalive_ms is not None else max(200, timeout_ms // 3))

    # ---------------- keepalive ----------------
    def start_keepalive(self, period_ms: int = 1000) -> None:
        self.stop_keepalive()
        self._ka_stop = threading.Event()
        period = max(0.1, period_ms / 1000.0)

        def loop() -> None:
            while True:
                started = time.perf_counter()
                try:
                    self.command(
                        Cmd.KEEPALIVE,
                        P.pack_u32(int(time.monotonic() * 1000) & 0xFFFFFFFF),
                        timeout=0.3,
                        retries=1,
                    )
                    self.last_rtt_ms = (time.perf_counter() - started) * 1000.0
                except Exception as exc:
                    self.last_rtt_ms = None
                    self._log(f"[keepalive] 无应答：{exc}")
                if self._ka_stop.wait(period):
                    return

        self._ka_thread = threading.Thread(target=loop, name="psu-keepalive", daemon=True)
        self._ka_thread.start()

    def stop_keepalive(self) -> None:
        self._ka_stop.set()
        if self._ka_thread is not None:
            self._ka_thread.join(timeout=1.0)
            self._ka_thread = None

    # ---------------- 帧派发 ----------------
    def handle_frame(self, frame: Frame) -> None:
        """读线程回调入口：先喂给等响应的 waiter，再刷新状态缓存。

        注意"先喂再解析"而不是"喂了就返回"：GET_STATUS 的响应是 STATUS + 一串 ITEM，
        它们都会先命中 waiter（seq 相同），但同时也必须刷新遥测/测试项缓存，
        否则重连后拿到快照却不上屏。
        """
        with self._waiters_lock:
            waiter = self._waiters.pop(frame.seq, None)
        if waiter is not None:
            waiter.feed(frame)

        try:
            if frame.cmd == Rsp.TELEM:
                self._on_telemetry(Telemetry.parse(frame.payload))
            elif frame.cmd == Rsp.STATUS:
                count = self.info.item_count if self.info else 7
                telemetry, results = P.parse_status_items(frame.payload, count)
                for index, result in enumerate(results):
                    if self.items[index].result != result:
                        self.items[index].result = result
                self._on_telemetry(telemetry)
            elif frame.cmd == Rsp.EVENT:
                event, data = P.parse_event(frame.payload)
                self._log(self._describe_event(event, data))
                if self.on_event is not None:
                    self.on_event(event, data)
            elif frame.cmd == Rsp.ITEM:
                item = ItemInfo.parse(frame.payload)
                if item.index < len(self.items):
                    self.items[item.index].update(item)
                if self.on_item is not None:
                    self.on_item(item)
            elif frame.cmd == Rsp.ACK and waiter is None:
                ack = Ack.parse(frame.payload)
                self._log(f"设备主动 ACK：{P.exit_code_text(ack.code)}")
            elif frame.cmd not in (Rsp.ACK, Rsp.PONG, Rsp.INFO):
                self._log(f"未知响应 {frame.cmd:#04x}（{len(frame.payload)} 字节）")
        except ProtocolError as exc:
            self._log(f"响应解析失败：{exc}")

    def _on_telemetry(self, telemetry: Telemetry) -> None:
        self.telemetry = telemetry
        self._write_csv_row(telemetry)
        if self.on_telemetry is not None:
            self.on_telemetry(telemetry)

    @staticmethod
    def _describe_event(event: int, data: bytes) -> str:
        try:
            name = P.Event(event).name
        except ValueError:
            name = f"event({event})"
        if event == P.Event.TEST_START and data:
            return f"[事件] 测试序列开始（{data[0]} 项）"
        if event == P.Event.TEST_END and data:
            return f"[事件] 测试序列结束（{P.RUN_STATE_TEXT.get(data[0], '?')}）"
        if event == P.Event.REMOTE and data:
            return f"[事件] 远程模式{'开启' if data[0] else '关闭'}"
        if event == P.Event.WATCHDOG and len(data) >= 2:
            return f"[事件] 看门狗超时（{int.from_bytes(data[:2], 'little')} ms 无心跳）→ 设备已回安全态"
        return f"[事件] {name} {data.hex(' ')}"

    def _handle_closed(self, reason: str) -> None:
        self.connected = False
        self.stop_keepalive()
        self._log(f"[链路] 断开：{reason}")
        if self.on_state is not None:
            self.on_state("closed")

    def _log(self, text: str) -> None:
        if self.on_log is not None:
            self.on_log(text)

    # ---------------- CSV ----------------
    def start_csv(self, path: str | Path) -> Path:
        """开始记录遥测。已开着会先关掉旧的。"""
        self.stop_csv()
        self._csv_path = Path(path)
        if self._csv_path.parent != Path(""):
            self._csv_path.parent.mkdir(parents=True, exist_ok=True)
        is_new = not self._csv_path.exists() or self._csv_path.stat().st_size == 0
        self._csv_file = self._csv_path.open("a", newline="", encoding="utf-8")
        self._csv_writer = csv.writer(self._csv_file)
        if is_new:
            self._csv_writer.writerow(CSV_HEADER)
        return self._csv_path

    def stop_csv(self) -> None:
        if self._csv_file is not None:
            try:
                self._csv_file.close()
            finally:
                self._csv_file = None
                self._csv_writer = None

    @property
    def csv_path(self) -> Path | None:
        return self._csv_path

    def _write_csv_row(self, telemetry: Telemetry) -> None:
        if self._csv_writer is None:
            return
        vset = telemetry.vout_setpoint_mv(self.info)
        ilim = telemetry.ilim_setpoint_ma(self.info)
        self._csv_writer.writerow(
            [
                datetime.now().isoformat(timespec="milliseconds"),
                telemetry.tick_ms,
                P.RUN_STATE_TEXT.get(telemetry.run_state, "?"),
                int(telemetry.ce_on),
                int(telemetry.manual),
                int(telemetry.remote),
                int(telemetry.wd_armed),
                telemetry.pwm_permille,
                "" if vset is None else vset,
                telemetry.ipwm_permille,
                "" if ilim is None else ilim,
                telemetry.pg_mv,
                P.PG_STATE_TEXT.get(telemetry.pg_state, "?"),
                telemetry.vout_mv,
            ]
        )
        self._csv_file.flush()

    # ---------------- 收尾 ----------------
    def disconnect(self, safe: bool = True) -> None:
        """断开链路。safe=True（默认）先急停回安全态再退出远程模式。"""
        self.stop_keepalive()
        if safe and self.connected:
            try:
                self.safe_state(retries=1, timeout=0.3)
            except CommandRejected as exc:
                self._log(f"[断开] 急停被拒：{exc}")
            except DeviceError as exc:
                self._log(f"[断开] 急停无应答：{exc}")
            try:
                self.set_remote(False, 0, retries=1, timeout=0.3)
            except DeviceError:
                pass
        self.connected = False
        self.link.stop()
        self.stop_csv()


def open_device(
    port: str | None,
    *,
    dummy: bool = False,
    baudrate: int = BAUD_DEFAULT,
    **callbacks: Any,
) -> PsuDevice:
    """建传输 + 链路 + 会话（dummy=True 时用进程内模拟器，不需要板子）。"""
    transport: Transport
    if dummy:
        from .dummy import DummyTransport

        transport = DummyTransport()
    else:
        if not port:
            raise DeviceError("没有指定串口；用 psu-host ports 看有哪些")
        transport = SerialTransport(port, baudrate)

    link = Link(
        transport,
        on_text=callbacks.get("on_log"),
        on_frame=None,          # 构造完 PsuDevice 后由它接管
    )
    device = PsuDevice(link, **callbacks)
    device.link.start()
    return device
