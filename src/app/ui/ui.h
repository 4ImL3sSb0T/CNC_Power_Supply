/**
 * @file        ui.h
 * @brief       测试台屏幕界面：测试结果 + 实时设定/PG 状态可视化
 */

#ifndef UI_H
#define UI_H

#include "hagl.h"

void ui_task(void *pvParameters);   /* pvParameters = hagl_backend_t* */

#endif
