"""数控电源v1 测试台上位机。

分层（与固件 src/{lib/proto,app/psu_link} 一一对应）：

    protocol.py   帧编解码 + 命令/事件常量 + 单位换算（纯逻辑，可单测）
    transport.py  串口打开/关闭/枚举（pyserial）
    link.py       后台读线程：字节流 → 帧/日志文本；发送加锁
    device.py     设备会话：握手、命令+ACK 重试、keepalive、看门狗、遥测缓存、CSV
    dummy.py      纯 Python 设备模拟器（没有板子也能跑界面与 CLI）
    cli.py        命令行
    gui.py        Tkinter 界面
"""

__version__ = "0.1.0"
