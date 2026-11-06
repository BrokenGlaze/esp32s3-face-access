/**
 * @file    camera.h
 * @brief   DVP 摄像头（GC0308）驱动模块头文件
 *
 * @note    使用说明：
 *          - 硬件接口：DVP 8 位并行数据（D0~D7）+ 同步信号（XCLK/PCLK/VSYNC/HREF），
 *            SCCB 控制线复用 I2C0 总线（SIOD=GPIO1 / SIOC=GPIO2）
 *          - 摄像头电源（DVP_PWDN）由 PCA9557 扩展 IO 第 2 位控制，拉低上电
 *          - 前置条件：必须先调用 i2c_init()（新版 I2C 驱动）和 pca9557_init()，
 *            再调用 lcd_init()，最后调用 camera_init() -> camera_lcd_start()
 *          - 调用流程：camera_init() 初始化 esp_camera（RGB565/QVGA 双缓冲），
 *            camera_lcd_start() 创建采集任务（Core1）与显示任务（Core0）循环显示
 *          - 显示策略：屏幕方向由 lcd.h 的 LCD_ORIENTATION 宏决定（横屏 480x320 /
 *            竖屏 320x480），camera_lcd_start() 预览模式从 QVGA(320x240) 帧中央裁剪
 *            LCD_WIDTH x LCD_HEIGHT 区域显示，画面不变形（如需改变摄像头分辨率，
 *            修改 camera.c 中 camera_config_t 的 frame_size 宏即可）
 *          - 人脸识别流程由 AI 模块（who_human_face_recognition）自行采集/显示整帧，
 *            不再调用 camera_lcd_start()
 *          - 构建：idf.py build flash monitor
 */

#ifndef __CAMERA_H__
#define __CAMERA_H__

#include "esp_err.h"

/* ====================== 摄像头引脚定义 ======================= */
#define CAMERA_PIN_PWDN  -1   /* 电源引脚：不使用，由 PCA9557 的 DVP_PWDN 控制 */
#define CAMERA_PIN_RESET -1   /* 复位引脚：不使用 */

#define CAMERA_PIN_XCLK  5    /* XCLK 时钟输出（ESP32-S3 由 LCD_CAM 外设生成） */
#define CAMERA_PIN_SIOD  1    /* SCCB 数据线（复用 I2C0 SDA） */
#define CAMERA_PIN_SIOC  2    /* SCCB 时钟线（复用 I2C0 SCL） */

#define CAMERA_PIN_D7    9    /* 数据位 7 */
#define CAMERA_PIN_D6    4    /* 数据位 6 */
#define CAMERA_PIN_D5    6    /* 数据位 5 */
#define CAMERA_PIN_D4    15   /* 数据位 4 */
#define CAMERA_PIN_D3    17   /* 数据位 3 */
#define CAMERA_PIN_D2    8    /* 数据位 2 */
#define CAMERA_PIN_D1    18   /* 数据位 1 */
#define CAMERA_PIN_D0    16   /* 数据位 0 */
#define CAMERA_PIN_VSYNC 3    /* 垂直同步信号 */
#define CAMERA_PIN_HREF  46   /* 水平参考信号 */
#define CAMERA_PIN_PCLK  7    /* 像素时钟输入 */

#define CAMERA_XCLK_FREQ_HZ  24000000  /* XCLK 频率 24MHz */
#define CAMERA_SCCB_I2C_PORT 0         /* SCCB 复用的 I2C 外设：I2C0 */

/* 摄像头电源控制引脚（PCA9557 扩展 IO 位号） */
#define CAMERA_PWDN_PIN    2           /* PCA9557 第 2 位，低电平上电 */

/* ====================== 函数声明（extern "C" 防止 C++ 名字修饰） ============================== */
#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief 初始化 DVP 摄像头（GC0308）
 *
 * 功能：拉低 DVP_PWDN 打开摄像头电源，配置 DVP 引脚 / SCCB / 时钟 / 图像格式
 *       （RGB565、QVGA 320x240、双帧缓冲、PSRAM），调用 esp_camera_init()
 *       完成传感器检测与初始化，并对 GC0308 开启水平镜像。
 * @return esp_err_t 返回 ESP_OK 表示初始化成功，否则返回对应错误码
 */
esp_err_t camera_init(void);

/**
 * @brief 启动摄像头采集并显示到 LCD
 *
 * 功能：创建帧队列，并创建两个任务——采集任务（Core1）通过 esp_camera_fb_get()
 *       获取图像帧，显示任务（Core0）裁剪画面中央 LCD_WIDTH x LCD_HEIGHT 区域，
 *       通过 esp_lcd_panel_draw_bitmap() 满屏绘制到 LCD，形成实时预览。
 */
void camera_lcd_start(void);

#ifdef __cplusplus
}
#endif

#endif
