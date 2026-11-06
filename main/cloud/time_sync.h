/**
 * @file    time_sync.h
 * @brief   联网后的时间同步（SNTP）：给人脸历史记录提供本地时间戳
 *
 * @note    设备无 RTC，断电后必须联网校时；未校时前时间无效（1970 年），
 *          调用者应先用 time_sync_is_valid() 判断。
 */

#ifndef __TIME_SYNC_H__
#define __TIME_SYNC_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化时间同步（必须在 Wi-Fi 事件循环创建之后调用）
 *
 * 连接 Wi-Fi 获取到 IP 后自动启动 SNTP；时区固定为 UTC+8。
 */
void time_sync_init(void);

/**
 * @brief 查询是否已完成校时
 */
bool time_sync_is_valid(void);

/**
 * @brief 获取当前 UNIX 时间戳（秒，UTC）
 * @return 未校时返回 0
 */
int64_t time_sync_epoch(void);

/**
 * @brief 格式化当前本地时间
 * @param buf 输出缓冲
 * @param len 缓冲长度
 * @param fmt strftime 格式，如 "%Y-%m-%d %H:%M:%S"、"%-Y%m%d"
 * @return true 成功（已校时且格式化成功）
 */
bool time_sync_format(char *buf, size_t len, const char *fmt);

#ifdef __cplusplus
}
#endif

#endif
