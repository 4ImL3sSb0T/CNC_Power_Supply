/**
 * @file        board_config.h
 * @brief       测试台接线 + 数控电源v1（SC8701 验证板）板级常量
 *
 * H1 6P 排针网表（嘉立创EDA「数控电源v1 / 验证板」，2026-09-23 回读）：
 *   1=AGND  2=IPWM  3=PWM  4=PG节点  5=CE#  6=3V3
 *
 * PG 节点拓扑：3V3(H1.6) → R5(10K) → H1.4 → LED3 → PG(U1.4 开漏，故障拉低)
 *   故障时 LED3 导通，节点被压到 ≈2.1V（LED 压降）；释放时被 R5 拉到 3.3V。
 *   所以 H1.4 用 ADC 读（数字阈值灰区问题见测试项 PG-LEVELS）。
 *
 * SC8701 控制约定（数据手册 + 评审记录）：
 *   /CE   低有效，内部 1M 下拉，悬空即上电使能 —— MCU 必须主动驱动。
 *   PWM   20–100kHz，占空比 D 把输出设定缩放 1/6…1 倍。
 *   IPWM  20–100kHz，IOUT 限值 = ILIM2_SET × D，ITUNE 选中 ILIM2 时严禁悬空。
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* ---------------------------------------------------------------- */
/* 测试台 GPIO ↔ H1                                                  */
/* ---------------------------------------------------------------- */
#define PCB_PIN_PWM             4       /* H1.3  SC8701 PWM  电压设定（PWM2A） */
#define PCB_PIN_IPWM            5       /* H1.2  SC8701 IPWM 电流限（PWM2B，严禁悬空） */
#define PCB_PIN_CE_N            6       /* H1.5  SC8701 CE#  低=使能（悬空=使能！） */
#define PCB_ADC_PG_GPIO         26      /* H1.4  PG 节点模拟电压 → ADC0 */
#define PCB_ADC_PG_CH           0
#define PCB_ADC_VOUT_GPIO       27      /* 可选：外接分压实测 VOUT → ADC1 */
#define PCB_ADC_VOUT_CH         1

/* 测试台外设（ATK-DNRP2350AM：屏/按键在各自 BSP 头文件里） */
#define IMU_SDA_GPIO            16      /* IMU 模块 I2C0 SDA */
#define IMU_SCL_GPIO            17      /* IMU 模块 I2C0 SCL */

/* ---------------------------------------------------------------- */
/* SC8701 板级常量（R19/R20 = 182K/10K，VFB_REF = 1.22V）             */
/* ---------------------------------------------------------------- */
#define PCB_PWM_FREQ_HZ         50000u  /* SC8701 接受 20–100kHz，取中值 */
#define PCB_VOUT_SET_MV         23400u  /* D=100% 时的设定电压（R19/R20 决定） */
/* D=0 时设定缩到 1/6，即 3.90V：VSET(D) = 23400 × (1 + 5D) / 6 */
#define PCB_ILIM2_FULL_MA       5040u   /* (1.21/24K)×(1K/10mΩ) ≈ 5.04A，IPWM D=100% */
#define PCB_IIN_LIM_MA          4200u   /* (1.21/24K)×(1K/12mΩ) ≈ 4.2A，固定不可调 */

/* 测试时序 */
#define PCB_PG_TIMEOUT_MS       3000u   /* 使能后等 PG 的超时 */
#define PCB_PG_OFF_TIMEOUT_MS   1000u   /* 关断后等 PG 撤销的超时 */
#define PCB_SWEEP_SETTLE_MS     1500u   /* PWM 扫描每步稳定时间 */
#define PCB_IPWM_SETTLE_MS      2000u   /* IPWM 扫描每步停留时间（人工核对用） */

/* ---------------------------------------------------------------- */
/* PG 节点电平判定（good=3.3V，fault≈2.1V，取中值带滞回）             */
/* ---------------------------------------------------------------- */
#define PCB_PG_GOOD_MIN_MV      2700u
#define PCB_PG_FAULT_MAX_MV     2400u
/* RP2350 数字输入阈值约 VIL≤0.3×IOVDD、VIH≥0.65×IOVDD（以数据手册为准），
 * 用来量化 LED3 压降对数字读的影响 */
#define PCB_MCU_VIL_MAX_MV      990u
#define PCB_MCU_VIH_MIN_MV      2145u

/* ---------------------------------------------------------------- */
/* 可选：外接电阻分压实测真实 VOUT（默认关闭，接线后置 1）            */
/*   接线：VOUT → 9.1K → GP27(ADC1)，GP27 → 1K → AGND                */
/* ---------------------------------------------------------------- */
#define PCB_TEST_VOUT_SENSE_ENABLE      0
#define PCB_VOUT_SENSE_DIV_X1000        10100u  /* (9.1K+1K)/1K = 10.1 倍 */
#define PCB_VOUT_SENSE_TOLERANCE_PCT    5u      /* 允差 ±5% */

#endif
