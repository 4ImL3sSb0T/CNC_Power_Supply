/**
 * @file        psu_link.h
 * @brief       上位机通信链路：USB 侧收帧 → 命令解释 → 转成测试台命令；遥测/事件上报
 *
 * 结构分工：
 *   lib/proto/psu_proto.c   纯帧编解码（COBS + CRC16），无 RTOS/硬件依赖
 *   app/psu_link/psu_link.c 传输（TinyUSB CDC 原生读写）+ 命令语义 + 看门狗 + 遥测
 *
 * 线程约定：
 *   - 收、发、解帧、上报全部只在 psu_link_task 上下文完成（不需要互斥）
 *   - 对 SC8701 控制脚的写操作不在这里做：命令统一 post 给 pcb_test_task
 *     （pcb_ctrl 的单一所有者），本模块只读 pcb_test_get_status() 快照
 *   - 紧急情况（远程急停、看门狗超时）走的是 pcb_test 的急停通路，与长按按键等效
 *
 * 链路复用：USB CDC 上同时跑协议帧和 printf 日志文本。分帧用 COBS（不会产生
 * 0x00），日志文本也不会含 0x00，所以文本只会让它夹住的那一帧 CRC 失败被丢弃，
 * 重同步是自动的。因此这里可以放心用 printf 打少量状态日志。
 */

#ifndef PSU_LINK_H
#define PSU_LINK_H

#include "lib/tools/common_def.h"

/* 固件版本：与 CMakeLists.txt 的 pico_set_program_version 保持一致 */
#define PSU_LINK_FW_MAJOR           0u
#define PSU_LINK_FW_MINOR           1u

/* 任务节拍：5ms 轮询一次 CDC 与状态快照（遥测最快 20ms 一帧，抖动 ≤5ms） */
#define PSU_LINK_POLL_MS            5u

/* 遥测周期（SET_TELEM 可改；0 = 停止上报） */
#define PSU_LINK_TELEM_PERIOD_MS    100u
#define PSU_LINK_TELEM_PERIOD_MIN   20u
#define PSU_LINK_TELEM_PERIOD_MAX   1000u

/* 主机失联看门狗（SET_REMOTE 武装；0 = 不武装） */
#define PSU_LINK_WD_TIMEOUT_MIN_MS  200u
#define PSU_LINK_WD_TIMEOUT_MAX_MS  60000u

/* 发送一帧最多等 FIFO 腾出空间的时间（正常情况 FIFO 是空的，等不到） */
#define PSU_LINK_TX_WAIT_MS         20u

/* 复位链路状态。必须在调度器启动前调用一次（不建任务） */
exit_code_t psu_link_init(void);

/* 协议任务：由 main.c 用 xTaskCreate 建，优先级高于 UI/测试任务以保证命令响应 */
void psu_link_task(void *pvParameters);

#endif /* PSU_LINK_H */
