/**
 * @file        fs_test.h
 * @brief       LittleFS 冒烟测试：挂载后每次启动给 /log/bootcount.txt 打点
 */

#ifndef FS_TEST_H
#define FS_TEST_H

void fs_test_task(void *pvParameters);

#endif
