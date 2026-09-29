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


# ---------------------------------------------------------------- 持久化配置
def test_config_round_trip_covers_all_fields(session):
    device, _events = session
    assert device.info is not None
    assert device.info.caps & P.CAP_CFG and device.info.caps & P.CAP_STORE

    assert device.get_config().boot_count >= 1       # 上电就落过一条 BOOT 记录

    assert device.set_config_field(P.CfgField.TELEM_PERIOD, 200).arg == 200
    assert device.set_config_field(P.CfgField.WD_TIMEOUT, 2500).arg == 2500
    ack = device.set_config_field(P.CfgField.VOUT, 12350)
    # ACK 回的是换算后的占空比‰：1200×12350 − 200×23400 再除以 23400 = 433
    assert ack.arg == 433

    cfg = device.get_config()
    assert cfg.telem_period_ms == 200 and cfg.wd_timeout_ms == 2500
    assert cfg.vout_permille == ack.arg
    assert cfg.loaded and not cfg.using_defaults

    # 异步落盘：dirty 位最终要清零
    assert wait_for(lambda: not device.get_config().dirty)

    # 遥测周期是唯一会自动应用的非安全项，设备侧真换了
    simulator = device.link.transport
    assert simulator.state.telem_period_ms == 200
    assert simulator.state.cfg_ilim_permille == 0


def test_config_rejects_bad_field_or_range(session):
    device, _events = session
    for field, value in ((P.CfgField.TELEM_PERIOD, 5),
                         (P.CfgField.WD_TIMEOUT, 61000),
                         (P.CfgField.VOUT, 25000),       # 超出 3.9–23.4 V
                         (99, 1)):                       # 未知字段
        with pytest.raises(CommandRejected):
            device.set_config_field(field, value)


def test_config_changes_do_not_touch_hardware(session):
    """预设 ≠ 实时设定值：改配置不该动 PWM/CE#（上电必须停在安全态）。"""
    device, _events = session
    device.set_config_field(P.CfgField.VOUT, 12350)
    device.set_config_field(P.CfgField.ILIM, 3000)

    simulator = device.link.transport
    assert simulator.state.pwm_permille == 0
    assert simulator.state.ipwm_permille == 0
    assert simulator.state.ce_on is False


def test_config_reset_keeps_boot_count(session):
    device, _events = session
    device.set_config_field(P.CfgField.VOUT, 12350)
    before = device.get_config().boot_count

    device.reset_config()
    cfg = device.get_config()
    assert cfg.vout_permille == 0
    assert cfg.telem_period_ms == 100                  # 回到默认周期
    assert cfg.boot_count == before


def test_restore_config_applies_presets_but_leaves_output_off(session):
    device, _events = session
    device.set_config_field(P.CfgField.VOUT, 12350)
    device.set_config_field(P.CfgField.ILIM, 3000)

    cfg = device.restore_config()
    assert cfg.vout_permille > 0 and cfg.ilim_permille > 0

    simulator = device.link.transport
    assert simulator.state.pwm_permille == cfg.vout_permille
    assert simulator.state.ipwm_permille == cfg.ilim_permille
    assert simulator.state.ce_on is False            # 恢复预设不等于开输出


# ---------------------------------------------------------------- 日志
def test_log_starts_with_boot_record(session):
    device, _events = session
    info = device.log_info()
    assert info.rec_size == P.LOG_REC_SIZE
    assert info.total >= 1

    records = device.read_log()
    assert records[0].type == P.LogType.BOOT
    assert records[0].arg >= 1                       # 启动序号
    assert "启动序号" in records[0].describe()


def test_log_records_host_and_watchdog_safe_events(session):
    device, events = session
    baseline = len(device.read_log())

    device.set_output(True)
    device.safe_state()                              # 原因：上位机急停
    assert wait_for(lambda: len(device.read_log()) >= baseline + 1)

    device.set_output(True)
    device.set_remote(True, 400)                     # 武装 400ms 看门狗，不起 keepalive
    assert wait_for(lambda: any(event == P.Event.WATCHDOG for event, _ in events), timeout=2.0)

    records = device.read_log()
    safe = [record for record in records if record.type == P.LogType.SAFE]
    assert [record.a for record in safe] == [P.SafeReason.HOST, P.SafeReason.WATCHDOG]
    assert safe[1].arg == 400                        # 看门狗那条带上窗口值
    assert "看门狗超时" in safe[1].describe()


def test_log_clear_empties_device(session):
    device, _events = session
    assert device.log_info().total >= 1

    device.clear_log()
    assert wait_for(lambda: device.log_info().total == 0)
    assert device.read_log() == []
