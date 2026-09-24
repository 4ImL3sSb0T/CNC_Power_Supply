"""链路层：一个后台读线程 + 加锁写 + 断线通知。

职责边界：
    - 只做字节流 ↔ 帧/日志文本的搬运，不理解命令语义（那是 device.py 的事）
    - 设备 printf 出来的日志文本也在这里分流出来，交给日志窗显示

线程模型：读线程调用 on_frame/on_text/on_closed 回调（**不在** Tk 线程里），
界面必须自己塞进队列再由主线程取出 —— 见 gui.py 的 _pump()。
"""

from __future__ import annotations

import threading
import time
from typing import Callable

from .protocol import Frame, FrameSplitter, SplitterStats, encode_frame
from .transport import Transport

FrameCallback = Callable[[Frame], None]
TextCallback = Callable[[str], None]
ClosedCallback = Callable[[str], None]


class Link:
    """一条到设备的链路。"""

    def __init__(
        self,
        transport: Transport,
        on_frame: FrameCallback | None = None,
        on_text: TextCallback | None = None,
        on_closed: ClosedCallback | None = None,
    ) -> None:
        self.transport = transport
        self.on_frame = on_frame
        self.on_text = on_text
        self.on_closed = on_closed

        self._splitter = FrameSplitter()
        self._tx_lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_rx = 0.0

    # ---------------- 生命周期 ----------------
    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run, name="psu-link-rx", daemon=True)
        self._thread.start()

    def stop(self, reason: str = "") -> None:
        """停读线程并关传输。可以重复调用。"""
        already = self._stop.is_set()
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
            self._thread = None
        self.transport.close()
        if not already and reason and self.on_closed is not None:
            self.on_closed(reason)

    # ---------------- 发送 ----------------
    def send(self, cmd: int, seq: int, payload: bytes = b"") -> None:
        """发一帧。加锁保证多线程（GUI + keepalive）写不会交错。"""
        with self._tx_lock:
            self.transport.write(encode_frame(cmd, seq, payload))

    # ---------------- 状态 ----------------
    @property
    def stats(self) -> SplitterStats:
        return self._splitter.stats

    @property
    def last_rx_age(self) -> float:
        """距离上次收到数据过了多少秒（判断设备是否还在说话）。"""
        if self._last_rx == 0.0:
            return float("inf")
        return time.monotonic() - self._last_rx

    # ---------------- 读线程 ----------------
    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                data = self.transport.read(64)
            except Exception as exc:                    # 拔线/端口消失
                self._stop.set()
                self.transport.close()
                if self.on_closed is not None:
                    self.on_closed(f"{type(exc).__name__}: {exc}")
                return

            if not data:
                continue

            self._last_rx = time.monotonic()
            for kind, value in self._splitter.feed(data):
                if kind == "frame":
                    if self.on_frame is not None:
                        self.on_frame(value)            # type: ignore[arg-type]
                elif self.on_text is not None:
                    self.on_text(value)                 # type: ignore[arg-type]
