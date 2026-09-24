# 数控电源v1 上位机（psu-host）

USB CDC 上的二进制协议上位机：设电压 / 设限流 / 开输出 / 急停 / 遥测 / 跑测试序列 /
实时曲线 / CSV 记录。设备侧实现在固件的 `src/lib/proto/`（帧编解码）与 `src/app/psu_link/`
（传输、命令、看门狗、遥测）。

---

## 一、快速上手

```bash
cd scripts

uv sync                       # 建 .venv 并装 pyserial（首次会自动下 Python 3.12）

# 没有板子也能先玩：内置模拟器实现同一张命令表
uv run psu-host --dummy info
uv run psu-host --dummy monitor
uv run psu-host-gui --dummy           # 界面（见下方"界面"）

# 有板子
uv run psu-host ports                # 列出串口，Pico（VID 0x2E8A）排最前
uv run psu-host --port COM7 info     # 握手 + 打印设备信息与测试项结果
uv run psu-host --port COM7 monitor --csv log.csv
```

| 命令 | 作用 |
| --- | --- |
| `ports` | 列出串口 |
| `info` | 握手（PING/INFO/STATUS）并打印设备信息、各项测试结果 |
| `monitor [--csv 文件] [--seconds N]` | 持续打印遥测，可落 CSV |
| `set-v 12.35` / `set-v 433 --permille` | 设电压（默认按 V，`--permille` 按占空比‰） |
| `set-ilim 3.2` / `set-ilim 635 --permille` | 设限流（默认按 A） |
| `on` / `off` | 开/关输出（`on` 会先把界面设定值发下去，避免带着 0‰ 限流开出去） |
| `safe` | 急停：CE# 关断 + 双 PWM 0%（设备同时退出远程模式） |
| `test [--watch]` | 跑 PCB 测试序列，`--watch` 跟着打印每一步 |
| `sweep --start 3.9 --stop 23.4 --step 1 --dwell 0.5 [--csv 文件]` | 电压扫描（万用表逐点核对用） |

通用选项：`--port/-p`、`--dummy`、`--wd MS`（看门狗超时，默认 3000，`0` = 不武装）、
`--telem-ms MS`（遥测周期，默认 100）、`--no-remote`、`--no-safe`（退出时不急停）、`-v`。

退出时默认先 `SAFE_STATE` 再退出远程；`--no-safe` 可以关掉这个行为。

### 界面

```bash
uv run psu-host-gui                  # 或者
uv run python -m psu_host.gui
```

端口下拉框里选串口，或选「[无硬件] 使用内置模拟器」。界面上有：

- 设定：电压/限流滑块（`物理量 V/A` 与 `占空比 ‰` 两种单位可切）+ 应用按钮
- 输出 ON / OFF / 急停 SAFE（急停后按界面设置自动重新武装看门狗）
- 遥测面板：运行状态、CE/手动/远程/看门狗标志、设定值与实际占空比、PG 节点电压与判定、
  实测 VOUT（板子没接分压时会显示"没接"）、设备 tick、链路 RTT 与帧统计
- 测试序列：跑测试 / 停止 + 7 项结果表（名称/结果/耗时/详情，带颜色）
- 实时曲线：PG 节点电压与设定电压，最近 60 秒
- 日志窗：设备 printf 文本 + 协议事件；CSV 记录开关

---

## 二、协议（v1）

链路上：`0x00 + COBS(帧内容) + 0x00`（前后各一个分隔符，接收端跳过空块）。帧内容（小端）：

```
+--------+--------+--------+------------------+------------------+
|  cmd   |  seq   |  len   |     payload      |      crc16       |
| 1 字节 | 1 字节 | 1 字节 |     len 字节     |      2 字节      |
+--------+--------+--------+------------------+------------------+
         |<------- CRC-16/CCITT-FALSE 覆盖范围 --------->|
```

- `cmd` 的 bit7 表示方向：`0` = 上位机→设备，`1` = 设备→上位机
- `seq`：响应原样回填；设备主动上报（遥测/事件/测试项）填 `0`
- CRC-16/CCITT-FALSE：poly `0x1021`、初值 `0xFFFF`、不反射、不异或输出
- payload 上限 48 字节 → 帧内容最长 53、COBS 后最长 54、加前后分隔符 56 字节。
  56 < 64 = CDC 的 TX FIFO，所以一帧一次 `tud_cdc_write` 写完，不会被别的任务的
  printf 插进中间

**为什么要前置分隔符**：设备的 printf 日志也走这条 CDC，而文本里不会出现 `0x00`。
只有尾部一个分隔符时，"一行日志 + 紧跟着的帧"会粘成同一个块、整块 CRC 失败，**连带把
那一帧丢掉**；加了前置分隔符，日志在帧之前就被切成独立的文本块，帧本身完好。
分帧用 COBS 则保证编码结果里不含 `0x00`，错位的块只会 CRC 失败被丢，重同步自动完成。
上位机因此把"解不出合法帧的块"直接当文本显示在日志窗里 —— 一根 USB 线同时看协议和日志。

### 命令（上位机 → 设备）

| cmd | 名称 | payload | 响应 |
| --- | --- | --- | --- |
| 0x01 | PING | u32 nonce | PONG |
| 0x02 | GET_INFO | — | INFO |
| 0x03 | SET_VOUT | u8 unit, u16 val | ACK（arg = 生效占空比‰） |
| 0x04 | SET_ILIM | u8 unit, u16 val | ACK（arg = 生效占空比‰） |
| 0x05 | SET_OUTPUT | u8 on | ACK |
| 0x06 | SAFE_STATE | — | ACK |
| 0x07 | RUN_TEST | — | ACK |
| 0x08 | STOP_TEST | — | ACK |
| 0x09 | SET_TELEM | u16 period_ms（0 = 停） | ACK |
| 0x0A | SET_REMOTE | u8 enable, u16 timeout_ms | ACK |
| 0x0B | GET_STATUS | — | STATUS + 若干 ITEM |
| 0x0C | KEEPALIVE | u32 host_ms | ACK |

`unit`：`0` = 原始占空比 ‰（0–1000，精确扫描用），`1` = SET_VOUT 用 mV、SET_ILIM 用 mA。
**mV/mA → ‰ 的换算在固件侧做**（用 `board_config.h` 的常量），越界返回
`EXIT_INVALID_PARAM`，所以上位机不写死任何板级数字。

### 响应与上报（设备 → 上位机）

| cmd | 名称 | payload |
| --- | --- | --- |
| 0x81 | PONG | u32 nonce, u8 proto_ver, u8 role, u16 reserved |
| 0x82 | INFO | u8 proto_ver, u8 fw_major, u8 fw_minor, u8 item_count, u16 vout_full_mv, u16 vout_min_mv, u16 ilim_full_ma, u16 iin_lim_ma, u16 pwm_freq_khz, u8 caps, u8 reserved |
| 0x83 | TELEM | 见下（16 字节，周期上报） |
| 0x84 | ACK | u8 acked_cmd, i8 code（`exit_code_t`）, u16 arg |
| 0x85 | EVENT | u8 event + 事件数据 |
| 0x86 | ITEM | u8 phase, u8 index, u8 result, u8 reserved, u32 elapsed_ms, char name[12], char detail[22] |
| 0x8B | STATUS | TELEM 的 16 字节 + u8 item_results[7] + u8 reserved |

TELEM（16 字节）：

| 偏移 | 字段 |
| --- | --- |
| 0 | u8 flags（bit0 CE 已使能、bit1 手动、bit2 远程、bit3 看门狗已武装、bit4 VOUT 实测有效） |
| 1 | u8 run_state（0 IDLE / 1 RUNNING / 2 DONE / 3 ABORTED） |
| 2 | u8 cur_index |
| 3 | u8 pg_state（0 FAULT / 1 ? / 2 GOOD） |
| 4 | u16 pwm_permille（电压设定‰） |
| 6 | u16 ipwm_permille（限流设定‰） |
| 8 | u16 pg_mv（PG 节点电压） |
| 10 | u16 vout_mv（实测 VOUT，未接分压为 0） |
| 12 | u32 tick_ms（设备运行时间） |

INFO 里的板级常量是**唯一事实来源**：上位机用 `vout_full_mv` 等算显示值
（`VSET(D) = FULL × (1 + 5D) / 6`，D = ‰/1000），不自己编数字。

EVENT：`0x01 TEST_START`（u8 项数）、`0x02 TEST_END`（u8 run_state）、
`0x03 REMOTE`（u8 enable）、`0x04 WATCHDOG`（u16 超时 ms）。

### 远程模式与看门狗

- `SET_REMOTE{1, timeout}` 进远程模式并武装看门狗（0 = 只进远程、不武装）
- 远程模式下**设备本地按键只保留长按急停**，屏上显示 `RMT`；单击/双击被忽略
- 主机每 `timeout/3` 发一次 `KEEPALIVE`；`timeout` 内收不到任何合法帧 →
  设备急停回安全态（CE# 关断 + 双 PWM 0%）+ 发 `EV_WATCHDOG` + 自动退出远程
- 本地长按急停会退出远程模式（上位机收到 `EV_REMOTE{0}`，可以选择重新武装）

### 命令的拒绝

- 测试序列运行中：`SET_VOUT` / `SET_ILIM` / `SET_OUTPUT` / `RUN_TEST` 返回 `EXIT_BUSY`
- 急停 `SAFE_STATE` 与 `STOP_TEST` 任何时候都被接受（会打断正在执行的测试项）
- 未知命令返回 `EXIT_NOT_SUPPORTED`；参数越界返回 `EXIT_INVALID_PARAM`

---

## 三、排错

**列不到串口 / 连不上**

1. 板子要烧的是本仓库的固件（`ninja -C build && picotool load build/CNC_Power_Supply.uf2`），
   USB CDC 才会以 VID `0x2E8A` 出现
2. 用了别的 CDC 设备时端口可能排后面，用 `--port` 显式指定
3. 设备一个字都不发：固件用 `tud_cdc_connected()`（DTR）判断主机在位，
   上位机打开端口时会显式置 DTR；第三方串口工具要确认它拉起了 DTR
4. 握手超时：`--no-remote` 不影响握手；先 `uv run psu-host --port COMx info -v` 看细节

**日志窗里出现乱码/半截帧**

正常现象：设备 printf 和协议帧共用一条 CDC，帧是有前后分隔符的，日志则在下一个分隔符处
被切成文本块；只有真正损坏的块（被打断/误码）会以文本形式出现，并在帧统计里计入
`错帧`。周期性遥测丢一帧无所谓，重同步是自动的。

**设定值被拒绝（EXIT_BUSY）**

设备正在跑测试序列。`STOP_TEST` 或等它结束再设。

**万用表读数与设定值不一致**

先看有没有开输出（`CE ON`）以及限流是不是 0‰（限流 0 时输出会塌）。
PG 节点电压只是 power-good，不是输出电压；真实 VOUT 需要板上的 9.1K/1K 分压
（`board_config.h` 的 `PCB_TEST_VOUT_SENSE_ENABLE`）。

**开发自测（不需要硬件）**

```bash
uv run pytest -q                 # 44 项：协议编解码 + 会话层（握手/ACK 拒绝/看门狗/keepalive/CSV）
uv run psu-host --dummy info
uv run psu-host --dummy monitor --seconds 3
uv run psu-host --dummy sweep --start 3.9 --stop 23.4 --step 5 --dwell 0.2
uv run psu-host --dummy test --watch
uv run psu-host-gui --dummy
```

`--dummy` 走的是同一个 `protocol.py`、同一个 `device.py`，只是把串口换成了进程内模拟器，
所以命令、ACK 配对、keepalive、看门狗、CSV 这些路径都能在没板子时验证。
看门狗那两条用例（`tests/test_device_session.py`）就是靠它验证的：不起 keepalive 时
500 ms 后设备确实回安全态并上报 `EV_WATCHDOG`，起了 keepalive 则不会。

---

## 四、上板手册（真机验证顺序）

1. `ninja -C build`，用 `picotool` 把 `.uf2` 烧进去（或拖到 BOOTSEL 出来的盘符）
2. 插 USB，`uv run psu-host ports` 应该能看到 Pico 的端口
3. `uv run psu-host --port COMx info` —— 握手成功、打印设备信息（没成功先看上面的排错）
4. 开输出、设 12 V，用万用表核对：
   `uv run psu-host --port COMx set-ilim 5.04 && uv run psu-host --port COMx set-v 12.35`
   `uv run psu-host --port COMx on`
   屏上 `VSET` 与万用表读数应一致（±1‰ 量化）。核对完 `uv run psu-host --port COMx safe`
5. 跑测试序列：`uv run psu-host --port COMx test --watch`（或界面点"跑测试"），
   7 项结果与屏幕一致
6. **看门狗验证**：进远程并武装看门狗后，直接拔掉 USB 线（模拟上位机崩溃），
   设备应在超时后回安全态：屏上 `RMT` 消失、CE 关断、双 PWM 归零。
   用界面时保持看门狗勾选、遥测开启即可，1 秒一次 keepalive 在跑
7. 本地急停仍然有效：远程模式下长按 KEY0，输出应立即关断并退出远程

---

## 五、协议自检（C ↔ Python 逐字节一致）

两边的编解码各写一份（固件 C / 上位机 Python），靠"同一组输入 → 同一串字节"钉住：

```bash
cd scripts

# 1) 黄金向量：把 4 条代表性帧的字节序列钉死在单测里
uv run pytest -q -k golden

# 2) C 与 Python 直接 diff（本仓库开发时跑过，6 条向量全部一致）
clang -I.. -I../src -o /tmp/codec_vectors.exe tools/codec_vectors.c ../src/lib/proto/psu_proto.c
/tmp/codec_vectors.exe > /tmp/c.txt
uv run python tools/gen_codec_vectors.py > /tmp/py.txt
diff /tmp/c.txt /tmp/py.txt && echo "两边字节一致"
```

`tools/codec_vectors.c` 里照抄了 `psu_link.c::link_send` 的组帧方式（前置 0x00 +
`psu_proto_encode` + 尾部 0x00），因为真正的 `link_send` 依赖 TinyUSB、没法在主机端编译。

改动协议时（新增命令、调整布局）请**两边同时改**，然后跑上面两步，并更新
`tests/test_protocol.py::test_golden_vectors_pin_the_wire_format` 里的 hex。

---

## 六、目录结构

```
scripts/
├─ pyproject.toml          uv 工程（依赖 pyserial；dev 组 pytest）
├─ uv.lock                 锁定版本，建议入库
├─ src/psu_host/
│  ├─ protocol.py          帧编解码 + 命令/事件常量 + 单位换算（与固件逐字节对齐）
│  ├─ transport.py         串口枚举与读写（pyserial）
│  ├─ link.py              读线程：字节流 → 帧 / 日志文本；发送加锁
│  ├─ device.py            会话：握手、命令+ACK 重试、keepalive、看门狗、CSV
│  ├─ dummy.py             内置模拟器（同一张命令表，含测试序列与看门狗行为）
│  ├─ cli.py               命令行
│  └─ gui.py               Tkinter 界面
├─ tests/
│  ├─ test_protocol.py       协议编解码（CRC/COBS/帧/分流/换算/模拟器语义）
│  └─ test_device_session.py 会话层端到端（握手/拒绝/看门狗/keepalive/CSV）
└─ tools/
   ├─ codec_vectors.c        C 侧黄金向量输出程序（与固件共用 psu_proto.c）
   └─ gen_codec_vectors.py   同样向量的 Python 版，直接 diff 两边
```
