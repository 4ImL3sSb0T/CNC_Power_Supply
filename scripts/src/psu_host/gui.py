"""Tkinter 上位机：端口连接、设定值、输出开关、急停、遥测、测试项表、实时曲线、CSV、日志。

线程模型（重要）：
    设备的回调都发生在 link.py 的读线程/keepalive 线程里，**不能**直接碰 Tk 控件。
    所有回调一律塞进 self.queue，由主线程的 _pump()（after 40ms）取出后再更新界面。
    反过来，发命令（可能等 ACK 最多 ~0.75s）放在工作线程里，避免冻住界面。
"""

from __future__ import annotations

import argparse
import queue
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from . import protocol as P
from .device import DeviceError, PsuDevice, open_device
from .protocol import Info, Telemetry, Unit
from .transport import list_serial_ports

DUMMY_LABEL = "[无硬件] 使用内置模拟器"

# 曲线窗口：600 点 × 100ms ≈ 60 秒
CURVE_POINTS = 600
PG_FULL_V = 3.4
RESULT_COLORS = {
    P.TestResult.PENDING: "#888888",
    P.TestResult.RUNNING: "#0077cc",
    P.TestResult.PASS: "#118811",
    P.TestResult.FAIL: "#cc1111",
    P.TestResult.WARN: "#bb7700",
    P.TestResult.SKIP: "#888888",
    P.TestResult.MANUAL: "#0077aa",
}


@dataclass(frozen=True)
class GuiSettings:
    """界面上设定值的快照。

    Tk 变量只能在主线程读（别的线程读会抛 "main thread is not in main loop"），
    所以按钮回调先在主线程把值读成这个快照，再交给工作线程去发命令。
    """

    vout: tuple[int, Unit]
    ilim: tuple[int, Unit]
    watchdog: bool
    watchdog_ms: int
    telem_ms: int


def _int_or(text: str, fallback: int) -> int:
    try:
        value = int(float(text))
    except (TypeError, ValueError):
        return fallback
    return max(0, value)


class PsuGui:
    def __init__(self, root: tk.Tk, port: str | None = None, dummy: bool = False, baud: int = 115200) -> None:
        self.root = root
        self.baud = baud
        self.device: PsuDevice | None = None
        self.info: Info | None = None
        self.queue: queue.Queue[tuple] = queue.Queue()
        self.series_pg: deque[tuple[float, float]] = deque(maxlen=CURVE_POINTS)
        self.series_vset: deque[tuple[float, float]] = deque(maxlen=CURVE_POINTS)
        self._last_telemetry: Telemetry | None = None
        self._csv_enabled = False

        root.title("CNC PSU 上位机 — 数控电源v1 测试台")
        root.geometry("900x820")
        root.minsize(820, 700)

        self._build_connect(port, dummy)
        self._build_setpoints()
        self._build_telemetry()
        self._build_test()
        self._build_curve()
        self._build_log()
        self._build_status()

        self._refresh_ports()
        self._set_connected(False)
        root.protocol("WM_DELETE_WINDOW", self._on_close)
        root.after(40, self._pump)

    # ================================================================ 界面
    def _build_connect(self, port: str | None, dummy: bool) -> None:
        frame = ttk.LabelFrame(self.root, text="链路")
        frame.pack(fill="x", padx=8, pady=(8, 4))

        self.var_port = tk.StringVar(value=DUMMY_LABEL if dummy else (port or ""))
        self.combo_port = ttk.Combobox(frame, textvariable=self.var_port, width=42, state="readonly")
        self.combo_port.grid(row=0, column=0, padx=6, pady=6, sticky="w")

        ttk.Button(frame, text="刷新", command=self._refresh_ports, width=6).grid(row=0, column=1, padx=2)
        self.btn_connect = ttk.Button(frame, text="连接", command=self._toggle_connect, width=10)
        self.btn_connect.grid(row=0, column=2, padx=6)

        self.lbl_link = ttk.Label(frame, text="未连接", foreground="#888888")
        self.lbl_link.grid(row=0, column=3, padx=6, sticky="w")

        self.var_wd = tk.BooleanVar(value=True)
        self.var_wd_ms = tk.StringVar(value="3000")
        self.var_telem_ms = tk.StringVar(value="100")
        options = ttk.Frame(frame)
        options.grid(row=1, column=0, columnspan=4, sticky="w", padx=6, pady=(0, 6))
        ttk.Checkbutton(options, text="看门狗", variable=self.var_wd).pack(side="left")
        ttk.Entry(options, textvariable=self.var_wd_ms, width=7).pack(side="left", padx=4)
        ttk.Label(options, text="ms 内无心跳 → 设备自动回安全态").pack(side="left", padx=(0, 18))
        ttk.Label(options, text="遥测周期").pack(side="left")
        ttk.Entry(options, textvariable=self.var_telem_ms, width=7).pack(side="left", padx=4)
        ttk.Label(options, text="ms").pack(side="left")

    def _build_setpoints(self) -> None:
        frame = ttk.LabelFrame(self.root, text="设定（远程模式下生效）")
        frame.pack(fill="x", padx=8, pady=4)

        # 单位：物理量（V/A）或原始占空比（‰）
        self.var_unit = tk.StringVar(value="phys")
        bar = ttk.Frame(frame)
        bar.grid(row=0, column=0, columnspan=4, sticky="w", padx=6, pady=(4, 0))
        ttk.Radiobutton(bar, text="物理量 V / A", value="phys", variable=self.var_unit,
                        command=self._on_unit_change).pack(side="left")
        ttk.Radiobutton(bar, text="占空比 ‰", value="permille", variable=self.var_unit,
                        command=self._on_unit_change).pack(side="left", padx=8)

        self.var_vset = tk.DoubleVar(value=3.9)
        self.var_ilim = tk.DoubleVar(value=5.04)
        self.scale_vset = tk.Scale(
            frame, from_=3.9, to=23.4, resolution=0.01, orient="horizontal", length=520,
            variable=self.var_vset, label="输出电压", command=lambda _v: self._update_setpoint_labels(),
        )
        self.scale_vset.grid(row=1, column=0, columnspan=2, padx=6, sticky="w")

        self.scale_ilim = tk.Scale(
            frame, from_=0.0, to=5.04, resolution=0.01, orient="horizontal", length=520,
            variable=self.var_ilim, label="电流限值", command=lambda _v: self._update_setpoint_labels(),
        )
        self.scale_ilim.grid(row=2, column=0, columnspan=2, padx=6, sticky="w")

        ttk.Button(frame, text="应用电压", command=self._apply_vout, width=12).grid(row=1, column=2, padx=6)
        ttk.Button(frame, text="应用限流", command=self._apply_ilim, width=12).grid(row=2, column=2, padx=6)

        buttons = ttk.Frame(frame)
        buttons.grid(row=3, column=0, columnspan=3, sticky="w", padx=6, pady=6)
        self.btn_on = ttk.Button(buttons, text="输出 ON", command=lambda: self._on_output(True), width=12)
        self.btn_on.pack(side="left")
        self.btn_off = ttk.Button(buttons, text="输出 OFF", command=lambda: self._on_output(False), width=12)
        self.btn_off.pack(side="left", padx=6)
        self.btn_safe = ttk.Button(buttons, text="急停 SAFE", command=self._on_safe, width=12)
        self.btn_safe.pack(side="left", padx=6)
        ttk.Label(buttons, text="（急停会同时退出远程模式）").pack(side="left", padx=6)

    def _build_telemetry(self) -> None:
        frame = ttk.LabelFrame(self.root, text="遥测")
        frame.pack(fill="x", padx=8, pady=4)

        self.lbl_state = tk.Label(frame, text="IDLE", font=("Segoe UI", 16, "bold"), fg="#555555")
        self.lbl_state.grid(row=0, column=0, rowspan=2, padx=10, pady=4, sticky="w")

        self.lbl_vset = ttk.Label(frame, text="设定电压  —", font=("Consolas", 11))
        self.lbl_vset.grid(row=0, column=1, padx=10, sticky="w")
        self.lbl_ilim = ttk.Label(frame, text="设定限流  —", font=("Consolas", 11))
        self.lbl_ilim.grid(row=1, column=1, padx=10, sticky="w")
        self.lbl_pg = ttk.Label(frame, text="PG 节点  —", font=("Consolas", 11))
        self.lbl_pg.grid(row=0, column=2, padx=10, sticky="w")
        self.lbl_vout = ttk.Label(frame, text="实测 VOUT  —", font=("Consolas", 11))
        self.lbl_vout.grid(row=1, column=2, padx=10, sticky="w")
        self.lbl_misc = ttk.Label(frame, text="tick —   RTT —", font=("Consolas", 10))
        self.lbl_misc.grid(row=0, column=3, rowspan=2, padx=10, sticky="w")

    def _build_test(self) -> None:
        frame = ttk.LabelFrame(self.root, text="PCB 测试序列")
        frame.pack(fill="both", expand=True, padx=8, pady=4)

        bar = ttk.Frame(frame)
        bar.pack(fill="x", padx=6, pady=4)
        self.btn_run = ttk.Button(bar, text="跑测试", command=lambda: self._async(self._do_run_test), width=12)
        self.btn_run.pack(side="left")
        self.btn_stop = ttk.Button(bar, text="停止", command=lambda: self._async(self._do_stop_test), width=12)
        self.btn_stop.pack(side="left", padx=6)

        columns = ("name", "result", "elapsed", "detail")
        self.tree = ttk.Treeview(frame, columns=columns, show="headings", height=7)
        for key, title, width in (
            ("name", "测试项", 110),
            ("result", "结果", 70),
            ("elapsed", "耗时", 80),
            ("detail", "详情", 380),
        ):
            self.tree.heading(key, text=title)
            self.tree.column(key, width=width, anchor="w")
        self.tree.pack(fill="both", expand=True, padx=6, pady=(0, 6))
        for index in range(7):
            self.tree.insert("", "end", iid=str(index), values=("—", "-", "", ""))
        for result, color in RESULT_COLORS.items():
            self.tree.tag_configure(f"r{result}", foreground=color)

    def _build_curve(self) -> None:
        frame = ttk.LabelFrame(self.root, text="实时曲线（最近 60 秒）")
        frame.pack(fill="both", expand=True, padx=8, pady=4)
        self.canvas = tk.Canvas(frame, height=180, background="#101418", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=6, pady=6)

    def _build_log(self) -> None:
        frame = ttk.LabelFrame(self.root, text="日志（设备 printf 文本 + 协议事件）")
        frame.pack(fill="both", expand=True, padx=8, pady=4)

        bar = ttk.Frame(frame)
        bar.pack(fill="x", padx=6, pady=(4, 0))
        ttk.Button(bar, text="清空", command=self._clear_log, width=8).pack(side="left")
        self.var_csv = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="记录 CSV 到", variable=self.var_csv, command=self._toggle_csv).pack(side="left", padx=8)
        self.lbl_csv = ttk.Label(bar, text="（未开始）", foreground="#666666")
        self.lbl_csv.pack(side="left")

        self.log_text = ScrolledText(frame, height=9, font=("Consolas", 9), background="#0f1114", foreground="#dddddd")
        self.log_text.pack(fill="both", expand=True, padx=6, pady=6)
        self.log_text.tag_configure("device", foreground="#cccccc")
        self.log_text.tag_configure("event", foreground="#33bbff")
        self.log_text.tag_configure("error", foreground="#ff5555")
        self.log_text.configure(state="disabled")

    def _build_status(self) -> None:
        self.lbl_status = ttk.Label(self.root, text="就绪", foreground="#444444", anchor="w")
        self.lbl_status.pack(fill="x", padx=10, pady=(0, 6))

    # ================================================================ 连接
    def _refresh_ports(self) -> None:
        values = [DUMMY_LABEL]
        for port in list_serial_ports():
            values.append(port.label())
        self.combo_port.configure(values=values)
        current = self.var_port.get()
        if not current or (current not in values and current != DUMMY_LABEL):
            self.var_port.set(values[1] if len(values) > 1 else DUMMY_LABEL)

    def _selected_port(self) -> str | None:
        label = self.var_port.get()
        if not label or label == DUMMY_LABEL:
            return None
        return label.split("  —  ")[0].strip()

    def _toggle_connect(self) -> None:
        if self.device is None:
            self._async(
                self._connect,
                self._read_settings(),
                self._selected_port(),
                self.var_port.get() == DUMMY_LABEL,
            )
        else:
            self._async(self._disconnect, True)

    def _read_settings(self) -> GuiSettings:
        """在主线程把界面上的值读成快照（Tk 变量不能在工作线程里读）。"""
        return GuiSettings(
            vout=self._read_scale(self.var_vset.get(), is_vout=True),
            ilim=self._read_scale(self.var_ilim.get(), is_vout=False),
            watchdog=bool(self.var_wd.get()),
            watchdog_ms=_int_or(self.var_wd_ms.get(), 3000),
            telem_ms=_int_or(self.var_telem_ms.get(), 100),
        )

    def _connect(self, settings: GuiSettings, port: str | None, dummy: bool) -> None:
        if not dummy and not port:
            self._post("log", "先选一个串口，或选“内置模拟器”", "error")
            return
        self._post("log", f"正在连接 {port or '内置模拟器'} …", "device")
        device = open_device(
            port,
            dummy=dummy,
            baudrate=self.baud,
            on_log=lambda text: self._post("log", text, "device"),
            on_telemetry=lambda telemetry: self._post("telemetry", telemetry),
            on_item=lambda item: self._post("item", item),
            on_event=lambda event, data: self._post("event", event, data),
            on_state=lambda state: self._post("state", state),
        )
        self.device = device
        try:
            info = device.handshake()
        except DeviceError as exc:
            self._post("log", f"握手失败：{exc}", "error")
            device.disconnect(safe=False)
            self.device = None
            return

        self.info = info
        device.set_telem_period(settings.telem_ms)
        if settings.watchdog:
            device.arm_watchdog(settings.watchdog_ms)
        else:
            device.set_remote(True, 0)
        self._post("log", f"已连接：{info.fw_major}.{info.fw_minor} 固件，协议 v{info.proto_ver}，"
                          f"能力：{info.supports}", "event")
        self._post("state", "connected")
        self._post("info", info)

    def _disconnect(self, safe: bool = True) -> None:
        device = self.device
        self.device = None
        if device is not None:
            device.disconnect(safe=safe)
        self._post("log", "已断开" + ("（已先急停回安全态）" if safe else ""), "device")
        self._post("state", "closed")

    def _set_connected(self, connected: bool, info: Info | None = None) -> None:
        state = "normal" if connected else "disabled"
        for widget in (self.btn_on, self.btn_off, self.btn_safe, self.btn_run, self.btn_stop):
            widget.configure(state=state)
        for widget in (self.scale_vset, self.scale_ilim):
            widget.configure(state=state)
        self.btn_connect.configure(text="断开" if connected else "连接")
        if connected and info is not None:
            self.lbl_link.configure(text=f"已连接  {info.fw_major}.{info.fw_minor} 协议 v{info.proto_ver}",
                                    foreground="#118811")
        elif not connected:
            self.lbl_link.configure(text="未连接", foreground="#888888")
            self.lbl_state.configure(text="—", fg="#555555")

    # ================================================================ 动作
    def _apply_vout(self) -> None:
        self._async(self._do_setpoint, *self._read_settings().vout, True)

    def _apply_ilim(self) -> None:
        self._async(self._do_setpoint, *self._read_settings().ilim, False)

    def _do_setpoint(self, value: int, unit: Unit, is_vout: bool) -> None:
        device, _info = self._require()
        self._send_setpoint(device, value, unit, is_vout)

    def _send_setpoint(self, device: PsuDevice, value: int, unit: Unit, is_vout: bool) -> None:
        ack = device.set_vout(value, unit) if is_vout else device.set_ilim(value, unit)
        info = self.info
        if unit is Unit.MV and info is not None:
            if is_vout:
                actual = P.vout_mv_from_permille(ack.arg, info)
                self._post("log", f"电压设定 {value / 1000:.3f} V → 占空比 {ack.arg}‰"
                                  f"（实际 {actual / 1000:.3f} V）", "event")
            else:
                actual = P.ilim_ma_from_permille(ack.arg, info)
                self._post("log", f"限流设定 {value / 1000:.3f} A → 占空比 {ack.arg}‰"
                                  f"（实际 {actual / 1000:.3f} A）", "event")
        else:
            label = "电压" if is_vout else "限流"
            self._post("log", f"{label}设定 {ack.arg}‰", "event")

    def _on_output(self, on: bool) -> None:
        self._async(self._do_output, on, self._read_settings())

    def _do_output(self, on: bool, settings: GuiSettings) -> None:
        device, _info = self._require()
        if on:
            # 先把界面上当前的设定值发下去，再开输出 —— 免得带着 0‰ 限流开出去
            self._send_setpoint(device, *settings.vout, True)
            self._send_setpoint(device, *settings.ilim, False)
        ack = device.set_output(on)
        self._post("log", f"输出 {'ON' if on else 'OFF'}（{P.exit_code_text(ack.code)}）", "event")

    def _on_safe(self) -> None:
        self._async(self._do_safe, self._read_settings())

    def _do_safe(self, settings: GuiSettings) -> None:
        device, _info = self._require()
        try:
            device.safe_state()
            self._post("log", "已急停：CE# 关断 + 双 PWM 0%（设备已退出远程模式）", "error")
            # 急停会让设备退出远程，看门狗也被撤掉；这里按界面设置重新武装
            if settings.watchdog:
                device.arm_watchdog(settings.watchdog_ms)
            else:
                device.set_remote(True, 0)
        except DeviceError as exc:
            self._post("log", f"急停失败：{exc}", "error")

    def _do_run_test(self) -> None:
        device, _info = self._require()
        device.run_test()
        self._post("log", "测试序列已启动", "event")

    def _do_stop_test(self) -> None:
        device, _info = self._require()
        device.stop_test()
        self._post("log", "已请求停止测试序列", "event")

    def _require(self) -> tuple[PsuDevice, Info | None]:
        if self.device is None:
            raise DeviceError("还没连接")
        return self.device, self.info

    def _read_scale(self, raw: float, is_vout: bool) -> tuple[int, Unit]:
        """把界面读数换成 (值, 单位)：物理量模式下 V→mV、A→mA。"""
        if self.var_unit.get() == "permille":
            return round(raw), Unit.PERMILLE
        return round(raw * 1000), Unit.MV

    def _on_unit_change(self) -> None:
        if self.info is not None:
            self._set_scale_ranges(self.info)
        else:
            self._update_setpoint_labels()

    def _set_scale_ranges(self, info: Info) -> None:
        if self.var_unit.get() == "permille":
            self.scale_vset.configure(from_=0, to=1000, resolution=1)
            self.scale_ilim.configure(from_=0, to=1000, resolution=1)
            self.var_vset.set(0)
            self.var_ilim.set(1000)
        else:
            self.scale_vset.configure(from_=info.vout_min_mv / 1000, to=info.vout_full_mv / 1000,
                                      resolution=0.01)
            self.scale_ilim.configure(from_=0.0, to=info.ilim_full_ma / 1000, resolution=0.01)
            self.var_vset.set(info.vout_min_mv / 1000)
            self.var_ilim.set(info.ilim_full_ma / 1000)
        self._update_setpoint_labels()

    def _update_setpoint_labels(self) -> None:
        info = self.info
        if info is None:
            return
        if self.var_unit.get() == "permille":
            self.scale_vset.configure(label=f"输出电压  {self.var_vset.get():.0f}‰")
            self.scale_ilim.configure(label=f"电流限值  {self.var_ilim.get():.0f}‰")
        else:
            self.scale_vset.configure(label=f"输出电压  {self.var_vset.get():.2f} V")
            self.scale_ilim.configure(label=f"电流限值  {self.var_ilim.get():.2f} A")

    # ================================================================ CSV / 日志
    def _toggle_csv(self) -> None:
        if self.device is None:
            self.var_csv.set(False)
            self._post("log", "先连接设备再开 CSV 记录", "error")
            return
        if self.var_csv.get():
            default = Path.cwd() / f"psu_log_{datetime.now():%Y%m%d_%H%M%S}.csv"
            path = filedialog.asksaveasfilename(
                title="遥测记录保存到", defaultextension=".csv", initialfile=default.name,
                initialdir=str(default.parent),
            )
            if not path:
                self.var_csv.set(False)
                return
            self.device.start_csv(path)
            self.lbl_csv.configure(text=path, foreground="#118811")
            self._post("log", f"开始记录 CSV：{path}", "event")
        else:
            self.device.stop_csv()
            self.lbl_csv.configure(text="（未开始）", foreground="#666666")
            self._post("log", "已停止 CSV 记录", "event")

    def _clear_log(self) -> None:
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")

    def _append_log(self, text: str, tag: str) -> None:
        self.log_text.configure(state="normal")
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.log_text.insert("end", f"{stamp}  {text}\n", tag)
        # 只留最近 2000 行，长时间跑不至于把内存吃光
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > 2000:
            self.log_text.delete("1.0", f"{lines - 2000}.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    # ================================================================ 线程桥
    def _post(self, kind: str, *payload: object) -> None:
        """所有回调走这里进队列（可能在任何线程里被调用）。"""
        self.queue.put((kind, *payload))

    def _async(self, fn, *args, **kwargs) -> None:
        """发命令放到工作线程，别冻住界面（等 ACK 最长 ~0.75s）。"""

        def run() -> None:
            try:
                fn(*args, **kwargs)
            except DeviceError as exc:
                self._post("log", f"{exc}", "error")
            except Exception as exc:                # 兜底：不让工作线程静默死掉
                self._post("log", f"{type(exc).__name__}: {exc}", "error")

        threading.Thread(target=run, daemon=True).start()

    def _pump(self) -> None:
        try:
            while True:
                message = self.queue.get_nowait()
                self._handle_message(message)
        except queue.Empty:
            pass
        self.root.after(40, self._pump)

    def _handle_message(self, message: tuple) -> None:
        kind = message[0]
        if kind == "log":
            self._append_log(str(message[1]), str(message[2]))
        elif kind == "telemetry":
            self._update_telemetry(message[1])
        elif kind == "item":
            self._update_item(message[1])
        elif kind == "event":
            self._append_log(str(message[1]), "event")
        elif kind == "info":
            self._set_connected(True, message[1])
            self._set_scale_ranges(message[1])
        elif kind == "state":
            self._set_connected(message[1] == "connected", self.info)

    def _update_telemetry(self, telemetry: Telemetry) -> None:
        self._last_telemetry = telemetry
        info = self.info
        now = time.monotonic()

        state_text = P.RUN_STATE_TEXT.get(telemetry.run_state, "?")
        tags = []
        if telemetry.remote:
            tags.append("RMT")
        if telemetry.wd_armed:
            tags.append("WD")
        if telemetry.manual:
            tags.append("MANU")
        if telemetry.ce_on and telemetry.pg_state == P.PgState.FAULT:
            tags.append("CE 已开但 PG=FAULT")
        self.lbl_state.configure(
            text=f"{state_text}  {' '.join(tags)}",
            fg="#bb7700" if telemetry.ce_on and telemetry.pg_state != P.PgState.GOOD else "#118811",
        )

        vset = telemetry.vout_setpoint_mv(info)
        ilim = telemetry.ilim_setpoint_ma(info)
        self.lbl_vset.configure(
            text=f"设定电压  {'—' if vset is None else f'{vset / 1000:.3f} V'} ({telemetry.pwm_permille / 10:.1f}%)"
        )
        self.lbl_ilim.configure(
            text=f"设定限流  {'—' if ilim is None else f'{ilim / 1000:.3f} A'} ({telemetry.ipwm_permille / 10:.1f}%)"
        )
        pg_text = P.PG_STATE_TEXT.get(telemetry.pg_state, "?")
        self.lbl_pg.configure(
            text=f"PG 节点  {telemetry.pg_mv / 1000:.3f} V {pg_text}"
            + ("" if telemetry.ce_on else "（CE 关断）"),
            foreground="#118811" if telemetry.pg_state == P.PgState.GOOD else "#886600",
        )
        self.lbl_vout.configure(
            text="实测 VOUT  —（板子没接分压）" if not telemetry.vout_valid
            else f"实测 VOUT  {telemetry.vout_mv / 1000:.3f} V"
        )
        rtt = self.device.last_rtt_ms if self.device else None
        stats = self.device.link.stats if self.device else None
        extra = f"tick {telemetry.tick_ms / 1000:.1f}s"
        if rtt:
            extra += f"   RTT {rtt:.0f}ms"
        if stats is not None:
            extra += f"   帧 {stats.frames} 文本 {stats.texts} 错帧 {stats.crc_errors}"
        self.lbl_misc.configure(text=extra)

        self.series_pg.append((now, telemetry.pg_mv / 1000))
        self.series_vset.append((now, (vset or 0) / 1000))
        self._redraw_curves()

    def _update_item(self, item) -> None:
        self.tree.item(
            str(item.index),
            values=(item.name, P.RESULT_TEXT.get(item.result, "?"),
                    f"{item.elapsed_ms} ms", item.detail),
            tags=(f"r{item.result}",),
        )

    # ================================================================ 曲线
    def _redraw_curves(self) -> None:
        canvas = self.canvas
        canvas.delete("all")
        width = max(canvas.winfo_width(), 200)
        height = max(canvas.winfo_height(), 120)
        if not self.series_pg:
            return
        half = height // 2
        self._draw_series(self.series_pg, PG_FULL_V, 0, half, width, "#33dd88", f"PG 节点 0–{PG_FULL_V:g} V")
        vmax = (self.info.vout_full_mv / 1000) if self.info else 24.0
        self._draw_series(self.series_vset, vmax, half, height, width, "#44aaff", f"设定电压 0–{vmax:g} V")

    def _draw_series(self, series, vmax: float, y0: int, y1: int, width: int, color: str, title: str) -> None:
        canvas = self.canvas
        pad = 4
        top, bottom = y0 + pad, y1 - pad
        canvas.create_rectangle(0, y0, width, y1, outline="#26313c", fill="#0c1014")
        canvas.create_text(6, y0 + 8, text=title, fill=color, anchor="w", font=("Consolas", 8))
        for frac in (0.25, 0.5, 0.75):
            y = top + (bottom - top) * frac
            canvas.create_line(0, y, width, y, fill="#1a2430")

        t_end = series[-1][0]
        t_start = t_end - 60.0
        points: list[float] = []
        for stamp, value in series:
            if stamp < t_start:
                continue
            x = (stamp - t_start) / 60.0 * width
            ratio = min(max(value / vmax, 0.0), 1.0)
            y = bottom - ratio * (bottom - top)
            points.extend((x, y))
        if len(points) >= 4:
            canvas.create_line(*points, fill=color, width=2)

    # ================================================================ 收尾
    def _on_close(self) -> None:
        if self.device is not None:
            if not messagebox.askokcancel("退出", "退出前先急停回安全态并断开？"):
                return
            try:
                self.device.disconnect(safe=True)
            except Exception:
                pass
        self.root.destroy()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="psu-host-gui", description="数控电源v1 上位机（Tkinter 界面）"
    )
    parser.add_argument("--port", "-p", help="串口，如 COM7（也可以在界面上选）")
    parser.add_argument("--dummy", action="store_true", help="默认选中内置模拟器")
    parser.add_argument("--baud", type=int, default=115200, help="波特率（USB CDC 忽略）")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = tk.Tk()
    PsuGui(root, port=args.port, dummy=args.dummy, baud=args.baud)
    root.mainloop()
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
