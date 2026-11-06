#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并启动人脸识别任务
 *
 * 该函数创建多个FreeRTOS任务来处理摄像头、LCD显示、AI人脸检测与识别、以及按键检测
 * 任务间通过队列进行帧数据的传递和同步
 */
void app_human_face_recognition(void);

/**
 * @brief 人脸头像缩略图规格
 *
 * 注册成功时从摄像头帧裁剪人脸区域，缩放到 FACE_AVATAR_W × FACE_AVATAR_H，
 * 以摄像头原始字节序的 RGB565 存在 vfs FAT 分区（/vfs/avatars/face_<id>.bin）。
 */
#define FACE_AVATAR_W 32
#define FACE_AVATAR_H 32
#define FACE_AVATAR_PIXELS (FACE_AVATAR_W * FACE_AVATAR_H)

/**
 * @brief 读取当前人脸识别相似度阈值
 *
 * @return 相似度百分比（50~95，默认 80）
 */
int human_face_get_threshold(void);

/**
 * @brief 设置人脸识别相似度阈值，并写入 NVS 掉电保存
 *
 * @param percent 相似度百分比，超出 50~95 会被钳位
 * @note  识别器尚未创建时只记录数值，创建后会使用该值；可在任意任务中调用
 */
void human_face_set_threshold(int percent);

/**
 * @brief 查询“模板自学习”开关
 *
 * @return 1 表示开启（识别成功后会把当前特征拟合进人脸模板），0 表示关闭
 */
int human_face_get_self_learn(void);

/**
 * @brief 设置“模板自学习”开关，并写入 NVS 掉电保存
 *
 * @param enable 非 0 开启、0 关闭
 * @note  开启后仅当相似度不低于 max(用户阈值, 90%) 时才学习，
 *        每个模板最多学习 10 次，达到上限后仍按固定小权重更新（新数据替代旧数据）
 */
void human_face_set_self_learn(int enable);

/**
 * @brief 获取已录入人脸的 ID 与姓名列表（线程安全，可在任意任务中调用）
 *
 * @param ids        输出人脸 ID（用于拼接头像文件名），可为 NULL
 * @param names      输出缓冲，每项至少 32 字节
 * @param max_count  names 最多容纳多少项
 * @param total      输出已录入人脸总数（可为 NULL）
 * @return 实际写入 names 的条目数（<= max_count；0 表示未录入或识别器未就绪）
 */
int human_face_list_enrolled(int *ids, char names[][32], int max_count, int *total);

/**
 * @brief 读取指定人脸的头像缩略图（线程安全）
 *
 * @param id  人脸 ID（human_face_list_enrolled 返回的 ids）
 * @param buf 输出缓冲，至少 FACE_AVATAR_PIXELS 个 uint16_t
 * @return 1 读取成功；0 失败（未挂载、无文件或文件长度不符）
 * @note  输出为摄像头原始字节序的 RGB565，LVGL(16_SWAP) 可直接显示
 */
int human_face_avatar_load(int id, uint16_t *buf);

/**
 * @brief 识别历史记录规格
 *
 * 每次人脸识别成功都会保存一张人脸照片（RGB565，3:2），最多保留最近
 * FACE_HISTORY_MAX 条（对应管理员界面“历史记录”页的 3 行 3 列），
 * 超出上限时自动覆盖最旧的一条。
 * 照片保存在 PSRAM 中供界面快速显示，同时写入 vfs(FAT) 分区
 * （/vfs/history/<槽位>.bin），掉电后重启会自动恢复。
 */
#define FACE_HISTORY_MAX 9                          /* 最多保留的历史条数 */
#define FACE_HISTORY_W   180                        /* 保存照片宽度（像素） */
#define FACE_HISTORY_H   120                        /* 保存照片高度（像素） */
#define FACE_HISTORY_PIXELS (FACE_HISTORY_W * FACE_HISTORY_H) /* 单张照片像素数 */

/**
 * @brief 查询当前可查看的历史记录条数（线程安全）
 *
 * @return 0~FACE_HISTORY_MAX（0 表示暂无记录或历史功能未就绪）
 */
int human_face_history_count(void);

/**
 * @brief 读取一条历史记录照片，并缩放到调用者指定的大小（线程安全）
 *
 * @param index 记录索引，0 表示最新一条
 * @param dst   输出缓冲，至少 dst_w × dst_h 个 uint16_t
 * @param dst_w 输出宽度（像素）
 * @param dst_h 输出高度（像素）
 * @return 1 读取成功；0 失败（索引越界、未就绪或缓冲无效）
 * @note  输出为摄像头原始字节序的 RGB565，LVGL(16_SWAP) 可直接显示
 */
int human_face_history_image(int index, uint16_t *dst, int dst_w, int dst_h);

/**
 * @brief 历史记录元数据（供云端上传任务使用）
 */
typedef struct
{
    uint32_t seq;       /* 记录序号（全局递增，越大越新） */
    uint32_t epoch;     /* 识别时间（UNIX 秒，0 表示当时尚未校时） */
    int sim;            /* 相似度百分比 */
    int uploaded;       /* 1 表示已上传到巴法云 */
    char name[32];      /* 识别到的姓名 */
} human_face_history_meta_t;

/**
 * @brief 读取一条历史记录的元数据（线程安全）
 *
 * @param index 记录索引，0 表示最新一条
 * @param meta  输出元数据
 * @return 1 成功；0 失败（索引越界或未就绪）
 */
int human_face_history_get_meta(int index, human_face_history_meta_t *meta);

/**
 * @brief 按记录序号读取照片并缩放到指定大小（线程安全）
 *
 * @param seq   记录序号（human_face_history_meta_t.seq）
 * @param dst   输出缓冲，至少 dst_w × dst_h 个 uint16_t
 * @param dst_w 输出宽度
 * @param dst_h 输出高度
 * @return 1 成功；0 失败（记录已被覆盖或未就绪）
 * @note  输出为摄像头原始字节序的 RGB565（大端）
 */
int human_face_history_read_seq(uint32_t seq, uint16_t *dst, int dst_w, int dst_h);

/**
 * @brief 把一条历史记录标记为“已上传”并写入文件头（线程安全）
 *
 * @param seq 记录序号
 * @return 1 成功（或该记录不存在/已经标记）；0 写入失败
 */
int human_face_history_mark_uploaded(uint32_t seq);

/**
 * @brief 暂停摄像头采集（写 Flash / 初始化 Wi-Fi 等操作前调用）
 * @note  内部会停止采集并清空流水线中残留的帧，避免 PSRAM 摄像头 DMA 与
 *        MSPI（Flash/NVS）冲突；必须与 human_face_capture_resume() 成对使用
 */
void human_face_capture_pause(void);

/**
 * @brief 恢复摄像头采集
 */
void human_face_capture_resume(void);

/**
 * @brief 允许摄像头开始采集（开机默认不取帧，Wi-Fi 初始化完成后再调用）
 * @note  避免 Wi-Fi 初始化读写 NVS/PHY 时与 PSRAM 摄像头 DMA 冲突
 */
void human_face_camera_start(void);

/**
 * @brief 删除指定人脸（特征 + 头像），并写入 Flash 持久化
 *
 * @param id 人脸 ID（human_face_list_enrolled 返回的 ids）
 * @return 1 成功；0 失败（未就绪、识别器忙或超时）
 * @note  内部由 AI 任务执行：暂停采集 → 改库 → 写 Flash → 恢复采集，
 *        调用会阻塞到操作完成（正常约需 200ms）
 */
int human_face_db_delete(int id);

/**
 * @brief 清空所有人脸（特征 + 头像），并写入 Flash 持久化
 *
 * @return 1 成功；0 失败（未就绪、识别器忙或超时）
 * @note  与 human_face_db_delete 相同的执行与阻塞方式
 */
int human_face_db_clear(void);

#ifdef __cplusplus
}
#endif
