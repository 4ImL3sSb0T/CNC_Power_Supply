"""串口传输层：pyserial 的薄封装 + 端口枚举。

link.py 只依赖 Transport 的 read/write/close 三个方法，所以 dummy.py 里的
进程内模拟器可以无缝替换成串口，界面和 CLI 的代码不用分叉。
"""

from __future__ import annotations

from dataclasses import dataclass

import serial
from serial.tools import list_ports

PICO_VID = 0x2E8A          # Raspberry Pi（Pico / Pico 2）
PICO_PID_CDC = 0x000A      # stdio_usb 的 CDC 接口
BAUD_DEFAULT = 115200      # USB CDC 忽略这个值，pyserial 需要一个合法数
READ_TIMEOUT = 0.05        # 读等待上限，决定读线程被唤醒的粒度


@dataclass(frozen=True)
class PortInfo:
    """一个候选串口。"""

    device: str
    description: str = ""
    vid: int | None = None
    pid: int | None = None

    @property
    def is_pico(self) -> bool:
        return self.vid == PICO_VID

    def label(self) -> str:
        parts = [self.device]
        if self.description:
            parts.append(self.description)
        if self.is_pico:
            parts.append("Pico")
        return "  —  ".join(parts)


def list_serial_ports() -> list[PortInfo]:
    """列出串口，Pico 排前面（固件 CDC 的 VID 是 0x2E8A）。"""
    ports = [
        PortInfo(p.device, p.description or "", p.vid, p.pid)
        for p in list_ports.comports()
    ]
    ports.sort(key=lambda p: (not p.is_pico, p.device))
    return ports


class Transport:
    """传输接口。"""

    name: str = "?"

    def read(self, size: int, timeout: float = READ_TIMEOUT) -> bytes:  # pragma: no cover
        raise NotImplementedError

    def write(self, data: bytes) -> int:  # pragma: no cover
        raise NotImplementedError

    def close(self) -> None:
        pass

    @property
    def is_open(self) -> bool:
        return True


class SerialTransport(Transport):
    """真实串口。"""

    def __init__(self, port: str, baudrate: int = BAUD_DEFAULT) -> None:
        self.name = port
        self._ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            timeout=READ_TIMEOUT,
            write_timeout=1.0,
        )
        # 固件用 tud_cdc_connected()（即 DTR）判断主机在位，DTR 不拉起来设备一个字都不发。
        # pyserial 打开时一般已经置位，这里显式来一遍，免得依赖具体平台的默认行为。
        try:
            self._ser.dtr = True
        except (OSError, serial.SerialException):
            pass

    def read(self, size: int, timeout: float = READ_TIMEOUT) -> bytes:
        self._ser.timeout = timeout
        return self._ser.read(size)

    def write(self, data: bytes) -> int:
        written = self._ser.write(data)
        self._ser.flush()
        return written or 0

    def close(self) -> None:
        try:
            self._ser.close()
        except OSError:
            pass

    @property
    def is_open(self) -> bool:
        return bool(self._ser.is_open)
