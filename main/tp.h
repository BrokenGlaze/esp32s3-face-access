#ifndef __TP_H__
#define __TP_H__
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft6x36.h" // 自定义 Touchscreen 头文件，提供 tp_init 函数声明
#include <stdbool.h>
#include <stdint.h>

/* ==================== 触摸坐标映射位置 ====================
 * 1：由 esp_lcd_touch 驱动完成“竖屏原始坐标 -> 横屏显示坐标”的 swap/mirror，
 *    tp_get_handle() 返回的坐标已经是 LCD_WIDTH x LCD_HEIGHT，LVGL 端口可直接使用；
 * 0：驱动输出原始竖屏坐标，由上层 touch_to_display() 手动映射（旧逻辑，可回退）。
 */
#ifndef TP_NATIVE_TO_DISPLAY
#define TP_NATIVE_TO_DISPLAY 1
#endif

/* extern "C" 防止 C++ 编译时函数名被修饰，导致与 tp.c 中的 C 实现链接失败 */
#ifdef __cplusplus
extern "C" {
#endif

extern esp_err_t tp_init(void);
extern esp_lcd_touch_handle_t tp_get_handle(void);

/**
 * @brief 读取触摸快照：当前是否被按下及其坐标
 *
 * 快照由 tp_init() 内部启动的独立采样任务（20ms 周期、带超时直读）维护，
 * 调用本身只读内存，无 I2C 事务、不会阻塞，可在任意任务/中断上下文使用。
 *
 * @param x 输出当前坐标 X（可为 NULL）
 * @param y 输出当前坐标 Y（可为 NULL）
 * @return true 表示当前有手指按下
 */
extern bool tp_snapshot_pressed(uint16_t *x, uint16_t *y);

/**
 * @brief 取走一次“粘滞点击”
 *
 * 采样任务检测到“按下 → 抬起”时置位；本函数取走并清除。
 * 用于在 LVGL 采样被刷屏挤掉、错过按下瞬间时补发一次按下-抬起，
 * 保证一次性点击不丢失。
 *
 * @param x 输出点击坐标 X（可为 NULL）
 * @param y 输出点击坐标 Y（可为 NULL）
 * @return true 表示取到一次尚未消费的点击
 */
extern bool tp_click_take(uint16_t *x, uint16_t *y);

/**
 * @brief 记录一次“粘滞点击被取走”的延迟（诊断用）
 * @param latency_ms 点击置位到被消费的毫秒数
 */
extern void tp_note_click_latency(uint32_t latency_ms);

/**
 * @brief 读取最近一次粘滞点击的置位时刻（us）
 */
extern uint32_t tp_click_latch_us(void);

#ifdef __cplusplus
}
#endif

#endif
