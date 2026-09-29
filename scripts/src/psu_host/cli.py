"""命令行上位机：脚本化/自动化用，也是没有硬件时的自测入口。

    uv run psu-host --dummy monitor           # 对着内置模拟器看遥测
    uv run psu-host ports                     # 列出串口（Pico 排最前）
    uv run psu-host --port COM7 info          # 握手 + 打印设备信息
    uv run psu-host --port COM7 set-v 12.35   # 设定 12.35 V（单位 mV 发下去，固件换算）
    uv run psu-host --port COM7 on            # 先应用当前设定再开输出
    uv run psu-host --port COM7 sweep --start 3.9 --stop 23.4 --step 1 --csv sweep.csv
    uv run psu-host --port COM7 cfg-get       # 读设备上的持久化配置（含启动序号）
    uv run psu-host --port COM7 cfg-set --telem 200 --vout 12.0
    uv run psu-host --port COM7 cfg-restore   # 把预设值下发到硬件（CE# 仍关断）
    uv run psu-host --port COM7 log-dump      # 下载设备记录的事件日志
"""

from __future__ import annotations

import argparse
import csv
import sys
import time
from datetime import datetime
from pathlib import Path

from . import protocol as P
from .device import CommandRejected, DeviceError, PsuDevice, open_device
from .protocol import Info, Telemetry, Unit
from .transport import list_serial_ports

TTY = sys.stdout.isatty()


def _add_global_options(parser: argparse.ArgumentParser, *, as_parent: bool) -> None:
    """全局选项。as_parent=True 时用 SUPPRESS 默认值，好让子命令后面也能重复给（否则
    子解析器的默认值会把子命令前面给过的值覆盖掉）。"""
    default = argparse.SUPPRESS if as_parent else None
    false = argparse.SUPPRESS if as_parent else False

    parser.add_argument("--port", "-p", default=default, help="串口，如 COM7；不指定时配合 --dummy 使用")
    parser.add_argument("--dummy", action="store_true", default=false, help="用内置模拟器，不需要真板子")
    parser.add_argument("--baud", type=int, default=argparse.SUPPRESS if as_parent else 115200,
                        help="波特率（USB CDC 忽略，默认 115200）")
    parser.add_argument("--wd", type=int, default=argparse.SUPPRESS if as_parent else 3000,
                        help="看门狗超时 ms（0 = 不武装），默认 3000")
    parser.add_argument("--telem-ms", type=int, default=default, help="遥测周期 ms（0 = 停止上报）")
    parser.add_argument("--no-remote", action="store_true", default=false,
                        help="不进远程模式（本地按键照常生效）")
    parser.add_argument("--no-safe", action="store_true", default=false, help="退出时不自动急停（默认会急停）")
    parser.add_argument("--quiet", action="store_true", default=false, help="不回显设备日志文本")
    parser.add_argument("-v", "--verbose", action="store_true", default=false, help="回显协议层细节")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="psu-host",
        description="数控电源v1 测试台上位机（USB CDC 二进制协议：COBS + CRC16）",
    )
    _add_global_options(parser, as_parent=False)

    # 子命令后面也能再给一遍全局选项（argparse 的惯例是全局选项放前面，这里松开一点）
    common = argparse.ArgumentParser(add_help=False)
    _add_global_options(common, as_parent=True)

    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("ports", help="列出串口")
    sub.add_parser("info", parents=[common], help="握手并打印设备信息")

    monitor = sub.add_parser("monitor", parents=[common], help="持续打印遥测")
    monitor.add_argument("--csv", type=Path, help="同时记录到 CSV")
    monitor.add_argument("--seconds", type=float, default=0.0, help="跑多少秒后退出（0 = 直到 Ctrl+C）")

    set_v = sub.add_parser("set-v", parents=[common], help="设定输出电压")
    set_v.add_argument("value", type=float, help="物理量单位是 V（如 12.35），--permille 时是 ‰")
    set_v.add_argument("--permille", action="store_true", help="按原始占空比 ‰ 设定")

    set_i = sub.add_parser("set-ilim", parents=[common], help="设定限流")
    set_i.add_argument("value", type=float, help="物理量单位是 A（如 3.2），--permille 时是 ‰")
    set_i.add_argument("--permille", action="store_true", help="按原始占空比 ‰ 设定")

    sub.add_parser("on", parents=[common], help="开输出（按当前设定值）")
    sub.add_parser("off", parents=[common], help="关输出")
    sub.add_parser("safe", parents=[common], help="急停：CE# 关断 + 双 PWM 0%")

    test = sub.add_parser("test", parents=[common], help="跑 PCB 测试序列")
    test.add_argument("--watch", action="store_true", help="跟着打印每一项的进展")
    test.add_argument("--timeout", type=float, default=60.0, help="等序列结束的上限秒数")

    sweep = sub.add_parser("sweep", parents=[common], help="电压设定扫描（逐点下发，便于万用表核对）")
    sweep.add_argument("--start", type=float, default=3.9, help="起点 V")
    sweep.add_argument("--stop", type=float, default=23.4, help="终点 V")
    sweep.add_argument("--step", type=float, default=1.0, help="步长 V")
    sweep.add_argument("--dwell", type=float, default=0.5, help="每点停留秒数")
    sweep.add_argument("--ilim", type=float, default=None, help="扫描前先设定限流 A")
    sweep.add_argument("--csv", type=Path, help="把每点读数记到 CSV")

    sub.add_parser("cfg-get", parents=[common], help="读设备上的持久化配置")
    # 选项名刻意避开全局的 --wd / --telem-ms（argparse 的父解析器不允许重名）
    cfg_set = sub.add_parser("cfg-set", parents=[common], help="改持久化配置（可同时给多项）")
    cfg_set.add_argument("--telem-period", type=int, dest="telem_period",
                         help="遥测周期 ms（0 / 20–1000），改完立即生效")
    cfg_set.add_argument("--wd-timeout", type=int, dest="wd_timeout",
                         help="看门狗建议超时 ms（0 / 200–60000），不会自动武装")
    cfg_set.add_argument("--vout", type=float, help="预设电压 V")
    cfg_set.add_argument("--ilim", type=float, help="预设限流 A")
    sub.add_parser("cfg-reset", parents=[common], help="恢复配置默认值（启动序号不重置）")
    sub.add_parser("cfg-restore", parents=[common],
                   help="把预设值下发到硬件（设备上电不会自动恢复，要在这里做）")

    sub.add_parser("log-info", parents=[common], help="设备日志的容量与范围")
    log_dump = sub.add_parser("log-dump", parents=[common], help="下载并打印设备日志")
    log_dump.add_argument("--since", type=int, help="从第几条开始（缺省从最旧一条起）")
    log_dump.add_argument("--csv", type=Path, help="同时写成 CSV")
    sub.add_parser("log-clear", parents=[common], help="清空设备日志")

    return parser


# ---------------------------------------------------------------- 打印助手
def _log_text(text: str) -> None:
    """设备侧文本块：逐行打。带固件前缀的是真日志（剥掉前缀），其余原样保留
    —— 半截帧乱码有时正是要看的线索。"""
    for line in text.splitlines():
        line = line.rstrip("\r")
        if not line:
            continue
        if line.startswith(P.LOG_LINE_PREFIX):
            line = line[len(P.LOG_LINE_PREFIX):]
        print(f"  · {line}", file=sys.stdout, flush=True)


def print_info(info: Info, dev: PsuDevice) -> None:
    print("设备信息")
    print(f"  协议版本    {info.proto_ver}（本机实现 {P.PROTO_VERSION}）")
    print(f"  固件版本    {info.fw_major}.{info.fw_minor}")
    print(f"  测试项数    {info.item_count}")
    print(f"  电压设定    {info.vout_min_mv / 1000:.2f} – {info.vout_full_mv / 1000:.2f} V（占空比 0–100%）")
    print(f"  限流设定    0 – {info.ilim_full_ma / 1000:.2f} A")
    print(f"  输入限流    {info.iin_lim_ma / 1000:.2f} A（固定，仅显示）")
    print(f"  PWM 频率    {info.pwm_freq_khz} kHz")
    print(f"  能力        {info.supports}")
    rtt = f"{dev.last_rtt_ms:.0f} ms" if dev.last_rtt_ms else "未测"
    print(f"  链路        {dev.link.transport.name}，RTT {rtt}")


def telemetry_line(dev: PsuDevice, telemetry: Telemetry) -> str:
    info = dev.info
    vset = telemetry.vout_setpoint_mv(info)
    ilim = telemetry.ilim_setpoint_ma(info)
    flags = " ".join(
        name
        for name, on in (
            ("CE", telemetry.ce_on),
            ("MANU", telemetry.manual),
            ("RMT", telemetry.remote),
            ("WD", telemetry.wd_armed),
        )
        if on
    )
    return (
        f"t={telemetry.tick_ms / 1000:8.3f}s {P.RUN_STATE_TEXT.get(telemetry.run_state, '?'):>4} "
        f"[{flags:<16}] "
        f"VSET {(vset or 0) / 1000:6.3f}V({telemetry.pwm_permille / 10:5.1f}%) "
        f"ILIM {(ilim or 0) / 1000:5.3f}A({telemetry.ipwm_permille / 10:5.1f}%) "
        f"PG {telemetry.pg_mv / 1000:5.3f}V {P.PG_STATE_TEXT.get(telemetry.pg_state, '?'):<5}"
    )


# ---------------------------------------------------------------- 子命令
def cmd_ports(_args: argparse.Namespace) -> int:
    ports = list_serial_ports()
    if not ports:
        print("没有发现串口。板子插上了吗？驱动装了吗？（设备管理器里应该出现 USB 串行设备）")
        return 0
    for port in ports:
        mark = "  ← Pico（推荐）" if port.is_pico else ""
        print(f"  {port.label()}{mark}")
    return 0


def cmd_info(args: argparse.Namespace, dev: PsuDevice) -> int:
    assert dev.info is not None
    print_info(dev.info, dev)
    dev.get_status()
    time.sleep(0.05)
    print("  测试项结果")
    for item in dev.items:
        print(f"    {item.index + 1} {item.name or '<名称未知>':<12} {item.result_text:<5} "
              f"{item.elapsed_ms:>6} ms  {item.detail}")
    return 0


def cmd_monitor(args: argparse.Namespace, dev: PsuDevice) -> int:
    if args.csv:
        path = dev.start_csv(args.csv)
        print(f"正在记录 CSV：{path}")
    deadline = time.monotonic() + args.seconds if args.seconds else None
    print("遥测中（Ctrl+C 退出）")
    last = ""
    try:
        while True:
            if deadline is not None and time.monotonic() >= deadline:
                return 0
            time.sleep(0.05)
            telemetry = dev.telemetry
            if telemetry is None:
                continue
            line = telemetry_line(dev, telemetry)
            if line != last:
                last = line
                print(("\r" + line) if TTY else line, end="" if TTY else "\n", flush=True)
    except KeyboardInterrupt:
        if TTY:
            print()
        return 0


def _resolve_setpoint(dev: PsuDevice, raw: float, permille_mode: bool, is_vout: bool) -> tuple[int, Unit]:
    info = dev.info
    assert info is not None
    if permille_mode:
        return int(round(raw)), Unit.PERMILLE
    if is_vout:
        mv = int(round(raw * 1000))
        if not (info.vout_min_mv <= mv <= info.vout_full_mv):
            raise DeviceError(f"电压要落在 {info.vout_min_mv / 1000:.2f}–{info.vout_full_mv / 1000:.2f} V 内")
        return mv, Unit.MV
    ma = int(round(raw * 1000))
    if not (0 <= ma <= info.ilim_full_ma):
        raise DeviceError(f"限流要落在 0–{info.ilim_full_ma / 1000:.2f} A 内")
    return ma, Unit.MV


def cmd_set_v(args: argparse.Namespace, dev: PsuDevice) -> int:
    value, unit = _resolve_setpoint(dev, args.value, args.permille, is_vout=True)
    ack = dev.set_vout(value, unit)
    info = dev.info
    assert info is not None
    permille = ack.arg
    print(f"已设定电压：{P.vout_mv_from_permille(permille, info) / 1000:.3f} V"
          f"（占空比 {permille}‰，{P.exit_code_text(ack.code)}）")
    return 0


def cmd_set_ilim(args: argparse.Namespace, dev: PsuDevice) -> int:
    value, unit = _resolve_setpoint(dev, args.value, args.permille, is_vout=False)
    ack = dev.set_ilim(value, unit)
    info = dev.info
    assert info is not None
    permille = ack.arg
    print(f"已设定限流：{P.ilim_ma_from_permille(permille, info) / 1000:.3f} A"
          f"（占空比 {permille}‰，{P.exit_code_text(ack.code)}）")
    return 0


def cmd_output(args: argparse.Namespace, dev: PsuDevice) -> int:
    on = args.command == "on"
    ack = dev.set_output(on)
    print(f"输出 {'ON' if on else 'OFF'}（{P.exit_code_text(ack.code)}）")
    if on:
        print("提示：限流为 0‰ 时输出会塌掉；用 set-ilim 先给个限流值")
    return 0


def cmd_safe(_args: argparse.Namespace, dev: PsuDevice) -> int:
    dev.safe_state()
    print("已急停：CE# 关断 + PWM 0% + IPWM 0%（设备同时退出远程模式）")
    return 0


def cmd_test(args: argparse.Namespace, dev: PsuDevice) -> int:
    dev.run_test()
    print("测试序列已启动")
    if not args.watch:
        return 0
    deadline = time.monotonic() + args.timeout
    last: dict[int, int] = {}
    while time.monotonic() < deadline:
        time.sleep(0.05)
        telemetry = dev.telemetry
        for item in dev.items:
            if last.get(item.index) == item.result:      # 只在状态变化时打一行
                continue
            last[item.index] = item.result
            if item.result == P.TestResult.PENDING:
                continue
            if item.result == P.TestResult.RUNNING:
                print(f"  {item.index + 1}/{len(dev.items)} {item.name:<12} ...")
            else:
                print(f"  {item.index + 1}/{len(dev.items)} {item.name:<12} "
                      f"{item.result_text:<5} {item.elapsed_ms:>6} ms  {item.detail}")
        if telemetry is not None and telemetry.run_state in (P.RunState.DONE, P.RunState.ABORTED):
            print(f"序列结束：{P.RUN_STATE_TEXT.get(telemetry.run_state, '?')}")
            return 0
    print("等待超时（序列可能还在跑，用 monitor 看状态）")
    return 1


def cmd_sweep(args: argparse.Namespace, dev: PsuDevice) -> int:
    info = dev.info
    assert info is not None
    if args.step <= 0:
        raise DeviceError("步长要大于 0")
    if args.ilim is not None:
        ack = dev.set_ilim(int(round(args.ilim * 1000)), Unit.MV)
        print(f"限流 {P.ilim_ma_from_permille(ack.arg, info) / 1000:.3f} A")

    points: list[float] = []
    value = args.start
    while value <= args.stop + 1e-9:
        points.append(round(value, 3))
        value += args.step

    csv_file = None
    writer = None
    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        csv_file = args.csv.open("w", newline="", encoding="utf-8")
        writer = csv.writer(csv_file)
        writer.writerow(["host_time", "set_v", "set_mv", "pwm_permille", "pg_mv", "pg_state", "vout_mv"])

    print(f"扫描 {len(points)} 点：{points[0]} … {points[-1]} V")
    dev.set_output(True)
    try:
        for target in points:
            mv = int(round(target * 1000))
            if not (info.vout_min_mv <= mv <= info.vout_full_mv):
                print(f"  {target:6.2f} V 超出范围，跳过")
                continue
            ack = dev.set_vout(mv, Unit.MV)
            time.sleep(args.dwell)
            telemetry = dev.telemetry
            actual = P.vout_mv_from_permille(ack.arg, info)
            if telemetry is None:
                print(f"  {target:6.2f} V → 占空比 {ack.arg:>4}‰（还没收到遥测）")
                continue
            print(
                f"  {target:6.2f} V → 设定 {actual / 1000:6.3f} V（{ack.arg:>4}‰）"
                f"  PG {telemetry.pg_mv / 1000:5.3f} V {P.PG_STATE_TEXT.get(telemetry.pg_state, '?')}"
            )
            if writer is not None:
                writer.writerow([
                    datetime.now().isoformat(timespec="milliseconds"),
                    f"{target:.3f}",
                    actual,
                    ack.arg,
                    telemetry.pg_mv,
                    P.PG_STATE_TEXT.get(telemetry.pg_state, "?"),
                    telemetry.vout_mv,
                ])
                csv_file.flush()
    finally:
        if csv_file is not None:
            csv_file.close()
    dev.set_vout(0, Unit.PERMILLE)
    dev.set_output(False)
    print("扫描结束，已回到 0‰ 并关输出")
    return 0


# ---------------------------------------------------------------- 持久化配置 / 日志
def print_cfg(cfg: P.CfgState, info: Info | None) -> None:
    vout = P.vout_mv_from_permille(cfg.vout_permille, info) if info else None
    ilim = P.ilim_ma_from_permille(cfg.ilim_permille, info) if info else None
    print("持久化配置")
    print(f"  遥测周期(ms)      {cfg.telem_period_ms}   ← 上电自动应用")
    print(f"  看门狗超时(ms)    {cfg.wd_timeout_ms}   ← 只是 SET_REMOTE 的建议值，上电不武装")
    print(f"  预设电压          {cfg.vout_permille:>4}‰"
          + (f"  = {vout / 1000:.3f} V" if vout is not None else ""))
    print(f"  预设限流          {cfg.ilim_permille:>4}‰"
          + (f"  = {ilim / 1000:.3f} A" if ilim is not None else ""))
    print(f"  启动序号          {cfg.boot_count}")
    marks = []
    if cfg.using_defaults:
        marks.append("正在用默认值")
    if cfg.dirty:
        marks.append("有改动还没落盘")
    source = "Flash" if cfg.loaded else "默认值"
    print(f"  来源              {source}" + (f"（{'，'.join(marks)}）" if marks else ""))
    print("  说明              预设值上电**不会**自动下发（上电必须停在安全态）；"
          "要用 cfg-restore 或 set-v/set-ilim")


def _wait_cfg_clean(dev: PsuDevice, timeout: float = 1.0) -> P.CfgState:
    """等异步落盘完成（dirty 位清零）；超时就返回当前状态，交给调用方显示。"""
    deadline = time.monotonic() + timeout
    cfg = dev.get_config()
    while cfg.dirty and time.monotonic() < deadline:
        time.sleep(0.05)
        cfg = dev.get_config()
    return cfg


def cmd_cfg_get(_args: argparse.Namespace, dev: PsuDevice) -> int:
    print_cfg(dev.get_config(), dev.info)
    return 0


def cmd_cfg_set(args: argparse.Namespace, dev: PsuDevice) -> int:
    info = dev.info
    assert info is not None

    # 先在本地按同一套范围挡一道：设备侧也会拒，但这里报错更清楚（顺带避免把
    # 超过 u16 的数字静默截断成另一个合法值）
    if args.telem_period is not None and args.telem_period != 0 \
            and not (P.TELEM_PERIOD_MIN <= args.telem_period <= P.TELEM_PERIOD_MAX):
        raise DeviceError(f"遥测周期要落在 {P.TELEM_PERIOD_MIN}–{P.TELEM_PERIOD_MAX} ms 内（0 = 停止上报）")
    if args.wd_timeout is not None and args.wd_timeout != 0 \
            and not (P.WD_TIMEOUT_MIN <= args.wd_timeout <= P.WD_TIMEOUT_MAX):
        raise DeviceError(f"看门狗超时要落在 {P.WD_TIMEOUT_MIN}–{P.WD_TIMEOUT_MAX} ms 内（0 = 不用）")
    if args.vout is not None:
        mv = int(round(args.vout * 1000))
        if not (info.vout_min_mv <= mv <= info.vout_full_mv):
            raise DeviceError(f"预设电压要落在 {info.vout_min_mv / 1000:.2f}–"
                              f"{info.vout_full_mv / 1000:.2f} V 内")
    if args.ilim is not None:
        ma = int(round(args.ilim * 1000))
        if not (0 <= ma <= info.ilim_full_ma):
            raise DeviceError(f"预设限流要落在 0–{info.ilim_full_ma / 1000:.2f} A 内")

    todo: list[tuple[P.CfgField, int, str]] = []
    if args.telem_period is not None:
        todo.append((P.CfgField.TELEM_PERIOD, args.telem_period, f"{args.telem_period} ms"))
    if args.wd_timeout is not None:
        todo.append((P.CfgField.WD_TIMEOUT, args.wd_timeout, f"{args.wd_timeout} ms"))
    if args.vout is not None:
        todo.append((P.CfgField.VOUT, int(round(args.vout * 1000)), f"{args.vout:.3f} V"))
    if args.ilim is not None:
        todo.append((P.CfgField.ILIM, int(round(args.ilim * 1000)), f"{args.ilim:.3f} A"))
    if not todo:
        raise DeviceError("至少给一项：--telem-period / --wd-timeout / --vout / --ilim")

    for field, value, text in todo:
        ack = dev.set_config_field(field, value)
        tail = f"（生效占空比 {ack.arg}‰）" if field in (P.CfgField.VOUT, P.CfgField.ILIM) else ""
        print(f"  {P.CFG_FIELD_TEXT[field]} → {text}{tail}")

    cfg = _wait_cfg_clean(dev)
    if cfg.dirty:
        print("  警告：设备还没把改动落盘（dirty 位仍为 1），再 cfg-get 看一次")
    else:
        print("  已落盘")
    return 0


def cmd_cfg_reset(_args: argparse.Namespace, dev: PsuDevice) -> int:
    dev.reset_config()
    print_cfg(_wait_cfg_clean(dev), dev.info)
    return 0


def cmd_cfg_restore(_args: argparse.Namespace, dev: PsuDevice) -> int:
    cfg = dev.restore_config()
    print(f"已按预设下发：遥测 {cfg.telem_period_ms} ms，"
          f"VOUT {cfg.vout_permille}‰，ILIM {cfg.ilim_permille}‰")
    print("CE# 仍是关断（上电安全态），要开输出再执行 on")
    return 0


def cmd_log_info(_args: argparse.Namespace, dev: PsuDevice) -> int:
    info = dev.log_info()
    print("设备日志")
    print(f"  记录 {info.rec_size} B × {info.total} 条"
          f"（段 {info.seg_count} × {info.seg_size} B = {info.seg_count * info.seg_size // 1024} KB）")
    print(f"  最旧一条的编号    {info.first_index}")
    return 0


def cmd_log_dump(args: argparse.Namespace, dev: PsuDevice) -> int:
    records = dev.read_log(since=args.since)
    if not records:
        print("设备上没有日志记录")
        return 0

    print(f"共 {len(records)} 条")
    for rec in records:
        print(f"  {rec.describe(dev.info)}")

    if args.csv:
        with args.csv.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(["tick_ms", "type", "a", "b", "pwm_permille", "ipwm_permille",
                             "arg", "描述"])
            for rec in records:
                writer.writerow([rec.tick_ms, rec.type, rec.a, rec.b, rec.pwm_permille,
                                 rec.ipwm_permille, rec.arg, rec.describe(dev.info)])
        print(f"已写入 {args.csv}")
    return 0


def cmd_log_clear(_args: argparse.Namespace, dev: PsuDevice) -> int:
    dev.clear_log()
    deadline = time.monotonic() + 1.0
    info = dev.log_info()
    while info.total and time.monotonic() < deadline:
        time.sleep(0.05)
        info = dev.log_info()
    print(f"清空完成：当前 {info.total} 条")
    return 0


DISPATCH = {
    "info": cmd_info,
    "monitor": cmd_monitor,
    "set-v": cmd_set_v,
    "set-ilim": cmd_set_ilim,
    "on": cmd_output,
    "off": cmd_output,
    "safe": cmd_safe,
    "test": cmd_test,
    "sweep": cmd_sweep,
    "cfg-get": cmd_cfg_get,
    "cfg-set": cmd_cfg_set,
    "cfg-reset": cmd_cfg_reset,
    "cfg-restore": cmd_cfg_restore,
    "log-info": cmd_log_info,
    "log-dump": cmd_log_dump,
    "log-clear": cmd_log_clear,
}


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.command == "ports":
        return cmd_ports(args)

    # 全局选项可能给在子命令前或后，没给出时属性会缺失（SUPPRESS），所以统一 getattr
    port = getattr(args, "port", None)
    dummy = getattr(args, "dummy", False)
    baud = getattr(args, "baud", 115200)
    wd = getattr(args, "wd", 3000)
    telem_ms = getattr(args, "telem_ms", None)
    no_remote = getattr(args, "no_remote", False)
    no_safe = getattr(args, "no_safe", False)
    quiet = getattr(args, "quiet", False)
    verbose = getattr(args, "verbose", False)

    dev: PsuDevice | None = None
    try:
        dev = open_device(
            port,
            dummy=dummy,
            baudrate=baud,
            on_log=None if quiet else _log_text,
        )
        info = dev.handshake()
        if verbose:
            print_info(info, dev)
        if telem_ms is not None:
            dev.set_telem_period(telem_ms)
        if not no_remote:
            if wd:
                dev.arm_watchdog(wd)
            else:
                dev.set_remote(True, 0)
        return DISPATCH[args.command](args, dev)
    except CommandRejected as exc:
        print(f"设备拒绝：{exc}", file=sys.stderr)
        return 2
    except (DeviceError, OSError) as exc:
        print(f"出错了：{exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if dev is not None:
            dev.disconnect(safe=not no_safe)


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
