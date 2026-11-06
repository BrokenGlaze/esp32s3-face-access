/**
 * @file    bemfa_uploader.h
 * @brief   巴法云照片上传：识别成功后把历史照片（JPEG）上传到图片主题
 *
 * @note    - 上传在独立低优先级任务中进行，不阻塞人脸识别与界面；
 *          - 未联网/未校时时记录留在队列里等待，联网后自动补传；
 *          - 上传失败的记录保留“未上传”标记，下次开机会自动重试；
 *          - 依赖识别模块的历史记录接口（照片 + 识别时间 + 序号）。
 */

#ifndef __BEMFA_UPLOADER_H__
#define __BEMFA_UPLOADER_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动照片上传任务（在 Wi-Fi 初始化完成后调用一次）
 *
 * 内部会创建上传队列与 PSRAM 照片缓冲，并在联网后自动补传开机前
 * 尚未上传的历史记录。
 */
void bemfa_uploader_start(void);

/**
 * @brief 把一条历史记录加入上传队列（识别成功时由 AI 任务调用）
 *
 * @param seq   历史记录序号（human_face_history_meta_t.seq）
 * @param epoch 识别时间（UNIX 秒，0 表示未校时）
 * @param name  识别到的姓名
 * @param sim   相似度百分比
 * @return true 已入队；false 未启动或队列已满（该记录保持“未上传”，下次开机会补传）
 */
bool bemfa_uploader_enqueue(uint32_t seq, uint32_t epoch, const char *name, int sim);

#ifdef __cplusplus
}
#endif

#endif
