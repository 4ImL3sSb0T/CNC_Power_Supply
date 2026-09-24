"""会话层端到端测试：对着内置模拟器跑真机的命令路径。

覆盖 device.py 的行为：握手、ACK 配对与重试、EXIT_BUSY/EXIT_INVALID_PARAM 拒绝、
keepalive 与看门狗的相互作用、CSV 记录、断开时急停。
不需要硬件，也不碰串口。
"""

from __future__ import annotations

import tempfile
import time
from pathlib import Path

import pytest

from psu_host import protocol as P
from psu_host.device import CommandRejected, DeviceError, PsuDevice, open_device


@pytest.fixture()
def session():
    """连上模拟器，返回 (device, events)；结束时断开。"""
    events: list[tuple[int, bytes]] = []
    device = open_device(
        None,
        dummy=True,
        on_event=lambda event, data: events.append((event, data)),
    )
    device.handshake()
    try:
        yield device, events
    finally:
        device.disconnect(safe=False)


def wait_for(predicate, timeout: float = 2.0, interval: float = 0.02) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def test_handshake_gives_info_and_item_names(session):
    device, _events = session
    info = device.info
    assert info is not None
    assert info.proto_ver == P.PROTO_VERSION
    assert info.item_count == 7
    assert info.vout_min_mv == 3900 and info.vout_full_mv == 23400
    # 项名来自设备发的 ITEM 帧（握手时就发全 7 项）
    assert wait_for(lambda: all(item.name for item in device.items))
    assert device.items[0].name == "SELF-IMU"
    assert device.items[6].name == "IPWM-SWEEP"


def test_setpoint_round_trip_and_rejection(session):
    device, _events = session
    ack = device.set_vout(12350)
    assert ack.ok and ack.arg == 433
    assert device.telemetry is not None
    assert wait_for(lambda: device.telemetry is not None and device.telemetry.pwm_permille == 433)

    # 越界 → 设备明确拒绝（EXIT_INVALID_PARAM）
    with pytest.raises(CommandRejected) as excinfo:
        device.set_vout(30000)
    assert excinfo.value.ack.code == -3

    ack = device.set_ilim(3200)
    assert ack.ok and ack.arg == 635
    with pytest.raises(CommandRejected):
        device.set_ilim(6000)


def test_busy_while_test_sequence_runs(session):
    device, events = session
    device.run_test()
    # 序列跑起来之后：设定值/输出开关/再次跑序列都被拒
    assert wait_for(lambda: device.telemetry is not None and device.telemetry.run_state == P.RunState.RUNNING)
    for call in (
        lambda: device.set_vout(10000),
        lambda: device.set_ilim(3000),
        lambda: device.set_output(True),
        lambda: device.run_test(),
    ):
        with pytest.raises(CommandRejected) as excinfo:
            call()
        assert excinfo.value.ack.code == -6          # EXIT_BUSY

    # 急停永远接受，并且能把序列打断
    device.safe_state()
    assert wait_for(lambda: any(event == P.Event.TEST_END for event, _ in events))
    assert wait_for(lambda: device.telemetry is not None and device.telemetry.run_state != P.RunState.RUNNING)
    assert device.telemetry is not None and not device.telemetry.ce_on


def test_watchdog_fires_when_host_goes_silent(session):
    device, events = session
    device.set_output(True)
    device.set_remote(True, 500)                     # 武装 500ms 看门狗，故意不起 keepalive
    assert wait_for(lambda: device.telemetry is not None and device.telemetry.ce_on)

    # 主机不再发任何帧 → 设备应自己回安全态并上报 EV_WATCHDOG
    assert wait_for(lambda: any(event == P.Event.WATCHDOG for event, _ in events), timeout=2.0)
    assert wait_for(lambda: device.telemetry is not None and not device.telemetry.ce_on)
    assert device.telemetry is not None and not device.telemetry.remote
    timeout = next(data for event, data in events if event == P.Event.WATCHDOG)
    assert int.from_bytes(timeout, "little") == 500


def test_keepalive_holds_watchdog_off(session):
    device, _events = session
    device.set_output(True)
    device.arm_watchdog(600)                         # 顺带起 keepalive（周期 200ms）
    assert wait_for(lambda: device.telemetry is not None and device.telemetry.ce_on and device.telemetry.wd_armed)

    time.sleep(1.5)                                  # 远超过 600ms 超时
    assert device.telemetry is not None
    assert device.telemetry.ce_on, "keepalive 在跑，设备不该回安全态"
    assert device.last_rtt_ms is not None and device.last_rtt_ms >= 0


def test_csv_records_telemetry(session):
    device, _events = session
    # 用自建临时目录，不依赖 pytest 的 tmp_path（它的共享 basetemp 可能不可访问）
    with tempfile.TemporaryDirectory(prefix="psu_csv_") as tmp:
        path = Path(tmp) / "log.csv"
        device.start_csv(path)
        assert wait_for(lambda: path.exists() and path.stat().st_size > 0)
        time.sleep(0.35)                             # 攒几行遥测
        device.stop_csv()

        lines = path.read_text(encoding="utf-8").strip().splitlines()
        assert lines[0].startswith("host_time,tick_ms,run_state")
        assert len(lines) >= 3
        columns = lines[1].split(",")
        assert columns[2] in {"IDLE", "RUN", "DONE", "STOP"}


def test_disconnect_safe_leaves_device_in_safe_state(session):
    device, _events = session
    device.set_output(True)
    device.set_remote(True, 0)
    assert wait_for(lambda: device.telemetry is not None and device.telemetry.ce_on)

    sim = device.link.transport
    device.disconnect(safe=True)
    assert sim.state.ce_on is False
    assert sim.state.pwm_permille == 0 and sim.state.ipwm_permille == 0
    assert sim.state.remote is False


def test_unsupported_command_is_reported(session):
    device, _events = session
    with pytest.raises(DeviceError):
        device.command(0x7F, b"")                    # 不存在的命令字 → EXIT_NOT_SUPPORTED
