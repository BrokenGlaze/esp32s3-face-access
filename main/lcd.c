/**
 * @file    lcd.c
 * @brief   LCD 驱动封装：初始化 ST7796 显示屏（SPI 接口 + PCA9557 扩展 IO 控制片选），
 *          提供整屏填充颜色、绘制图片等功能。
 *
 * @note    使用说明：
 *          - 硬件接口：
 *            * SPI3_HOST 总线：SCLK=GPIO41、MOSI=GPIO40（MISO 未使用）
 *            * DC 引脚：GPIO39（ESP32-S3 直接控制）
 *            * CS 引脚：由 PCA9557 扩展 IO 第 0 位控制（ESP32-S3 的 IO 不直连 CS）
 *            * RST 引脚：连接复位按键，非 ESP32-S3 IO，初始化时置 GPIO_NUM_NC
 *          - 前置条件：必须先调用 i2c_init() 和 pca9557_init()（CS 引脚依赖 PCA9557）
 *          - 屏幕参数：分辨率与方向由 lcd.h 的 LCD_ORIENTATION 宏决定
 *            （横屏 480x320 / 竖屏 320x480），RGB565（16 位色），RGB 字节序为 BGR
 *          - SPI 总线：SPI3_HOST，时钟 80MHz，8bit 命令/参数，DMA 自动分配
 *          - 初始化后默认：开启显示、颜色取反（invert true）、按宏配置坐标交换与镜像
 *          - 调用流程：lcd_init() -> lcd_set_color() / lcd_draw_picture()
 *            （摄像头显示任务通过 lcd_get_panel_handle() 直接调用 draw_bitmap）
 *          - 构建：idf.py build flash monitor
 */

#include "lcd.h"               // 自定义 LCD 头文件，提供 lcd_init、lcd_cs、lcd_set_color 等函数声明
#include "driver/gpio.h"       // GPIO 驱动头文件，提供 gpio 的函数接口
#include "freertos/FreeRTOS.h" // FreeRTOS 内核头文件，提供任务、延时等接口
#include "freertos/task.h"     // FreeRTOS 任务头文件，提供 vTaskDelay 等任务函数
#include "pca9557.h"           // 自定义 PCA9557 头文件，提供 pca9557_set_output_state 函数声明（用于控制 LCD 片选）
#include <stdbool.h>           // 标准布尔类型头文件，提供 true/false 定义
#include <stdio.h>             // 标准输入输出头文件，提供 printf 等函数
#include <stdarg.h>            // 可变参数头文件，提供 va_list / vsprintf 等接口
#include <string.h>            // 字符串处理头文件，提供 strncmp / strncpy 等接口
#include "esp_heap_caps.h"     // PSRAM 内存分配接口，提供 heap_caps_malloc / heap_caps_free
#include "FontDotMatrix16.h"   // 16x16 点阵字库（lcd_display_string 使用）
#include "FontDotMatrix32.h"   // 32x32 点阵字库（lcd_display_string 使用）

// 需包含的头文件
#include "driver/spi_master.h" // SPI 主机驱动头文件，提供 spi_bus_initialize 等函数
#include "esp_lcd_panel_io.h"  // ESP LCD 面板 IO 头文件，提供 esp_lcd_new_panel_io_spi 等函数
#include "esp_lcd_panel_ops.h" // ESP LCD 面板操作头文件，提供 esp_lcd_panel_draw_bitmap 等操作接口
#include "esp_lcd_panel_vendor.h" // ESP LCD 面板厂商头文件，提供通用面板创建接口
#include "esp_lcd_st7796.h"    // ST7796 面板驱动头文件，提供 esp_lcd_new_panel_st7796 函数
#include "esp_lcd_types.h"     // ESP LCD 类型定义头文件，提供 esp_lcd_panel_handle_t 等类型
#include "pca9557.h"           // 自定义 PCA9557 头文件（重复包含，用于确保接口可用）

// 创建 ST7796 Panel 句柄示例
static esp_lcd_panel_handle_t g_panel_handle = NULL; // ST7796 面板句柄（静态全局变量），后续所有面板操作都基于该句柄
static esp_lcd_panel_io_handle_t g_io_handle = NULL;  // 面板 IO 句柄（LVGL 端口需要 io_handle 注册 flush 回调）

/* ===================== 内部 RAM 绘图缓冲 =====================
 * 摄像头帧缓冲位于 PSRAM，而 SPI DMA 不能直接访问 PSRAM。若把 PSRAM 指针
 * 交给 esp_lcd_panel_draw_bitmap()，SPI 驱动会为每次传输动态申请一块
 * MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL 的中转缓冲；本工程内部 RAM 紧张时
 * 会报 "Failed to allocate priv TX buffer"，表现为画面只刷出一部分。
 *
 * 这里改用静态的内部 RAM 缓冲：先把像素拷贝到缓冲，再交给 SPI。
 * 双缓冲配合下面的 trans_queue_depth = 1 使用：
 * 发送第 N+1 行前，第 N 行必定已经传输完成，因此第 N-1 行的缓冲可以安全复用。
 */
static uint16_t s_line_buf[2][LCD_WIDTH] __attribute__((aligned(4))); // 两行 RGB565 像素（2 × 960 字节，4 字节对齐以支持 DMA）
static uint8_t s_line_buf_idx = 0;          // 当前使用的行缓冲序号
static uint16_t s_font_buf[2][32 * 32] __attribute__((aligned(4)));   // 两个字模缓冲（最大 32x32，2 × 2KB）
static uint8_t s_font_buf_idx = 0;          // 当前使用的字模缓冲序号

/* ===================== 整帧绘制的分块缓冲 =====================
 * 逐行调用 draw_bitmap 时，每行都要重发一次窗口命令并等待 SPI 事务完成，
 * 240 行累计开销可达 50ms 以上（实测 54.5ms/帧），而 153.6KB 像素本身
 * 只需约 2ms。这里按 8 行一块批量传输，把 240 次事务降到 30 次。
 * 两个缓冲交替使用，配合 trans_queue_depth = 1 保证上一块已发完。
 */
#define LCD_CHUNK_ROWS 8
static uint16_t s_chunk_buf[2][LCD_CHUNK_ROWS * LCD_WIDTH] __attribute__((aligned(4))); // 2 × 8 × 480 × 2B = 15KB 内部 RAM
static uint8_t s_chunk_idx = 0;             // 当前使用的分块缓冲序号

/**
 * @brief 获取 LCD 面板句柄，供上层模块（如摄像头显示任务）直接绘制整帧图像
 * @return esp_lcd_panel_handle_t 返回 ST7796 面板句柄，需在 lcd_init() 成功之后调用
 */
esp_lcd_panel_handle_t lcd_get_panel_handle(void)
{
    return g_panel_handle;
}

/**
 * @brief 获取 LCD 面板 IO 句柄，供 LVGL 端口（lvgl_port_add_disp）注册刷新回调
 * @return esp_lcd_panel_io_handle_t 面板 IO 句柄，需在 lcd_init() 成功之后调用
 */
esp_lcd_panel_io_handle_t lcd_get_io_handle(void)
{
    return g_io_handle;
}

void lcd_backlight_init(void)
{
    // 定义 GPIO 配置结构体，设置各配置项
    gpio_config_t gpio = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT_GPIO_PIN, // 位掩码：选中 GPIO42 引脚（bit42 置 1）
        .mode = GPIO_MODE_OUTPUT,                 // 模式：推挽输出模式
        .pull_up_en = GPIO_PULLUP_DISABLE,        // 上拉：禁用内部上拉电阻
        .pull_down_en = GPIO_PULLDOWN_DISABLE,    // 下拉：禁用内部下拉电阻
        .intr_type = GPIO_INTR_DISABLE,           // 中断：不使用中断（禁用 GPIO 中断）
    };
    // gpio的配置：将上述配置写入硬件寄存器，完成 GPIO 初始化
    gpio_config(&gpio);

    // 背光灯亮
    gpio_set_level(LCD_BACKLIGHT_GPIO_PIN, 1);
}

void lcd_backlight_set(uint32_t state)
{
   if(state)
   {
        // 背光灯亮
        gpio_set_level(LCD_BACKLIGHT_GPIO_PIN, 1);

   }
   else
   {
        // 背光灯灭
        gpio_set_level(LCD_BACKLIGHT_GPIO_PIN, 0);
   }
}

/**
 * @brief 控制 LCD 片选（CS）引脚电平
 * @param state 片选电平：0 表示选中 LCD（低电平有效），1 表示取消选中
 * @note  CS 引脚由 PCA9557 扩展 IO 的第 0 位控制，不占用 ESP32-S3 的 GPIO
 */
void lcd_cs(uint32_t state)                 // 片选控制函数：通过 PCA9557 扩展 IO 控制 LCD 的 CS 引脚
{
    pca9557_set_output_state(0, state);     // 调用 PCA9557 驱动，设置第 0 路输出电平为 state（0 选中 / 1 取消）
}

/**
 * @brief 通过内部 RAM 行缓冲发送一行像素（调用者需保证 width <= LCD_WIDTH）
 * @param x_start 行起始 X 坐标
 * @param y       行 Y 坐标
 * @param width   本行像素数
 * @param src     源像素（可位于 PSRAM）
 */
static void lcd_draw_row(uint32_t x_start, uint32_t y, uint32_t width, const uint16_t *src)
{
    uint16_t *dst = s_line_buf[s_line_buf_idx]; // 取一行内部 RAM 缓冲
    s_line_buf_idx ^= 1;                        // 切换到另一行缓冲，供下一行使用
    memcpy(dst, src, width * sizeof(uint16_t)); // 从 PSRAM/其他区域拷贝到内部 RAM
    esp_lcd_panel_draw_bitmap(g_panel_handle, x_start, y, x_start + width, y + 1, dst);
}

/**
 * @brief 初始化 LCD：配置 SPI 总线、创建面板 IO 与 ST7796 面板，完成显示参数配置
 * @return esp_err_t 返回 ESP_OK 表示初始化成功
 */
esp_err_t lcd_init(void)                    // LCD 初始化函数：返回 ESP_OK 表示成功
{
    // 初始化背光灯
    lcd_backlight_init();

    // SPI 总线初始化示例
    spi_bus_config_t buscfg = {             // 定义 SPI 总线配置结构体
        .mosi_io_num = GPIO_NUM_40,         // MOSI 数据输出引脚为 GPIO40（主机写数据给 LCD）
        .miso_io_num = GPIO_NUM_NC,         // 不使用 MISO 引脚，NC：No Connection，该值-1
        .sclk_io_num = GPIO_NUM_41,         // SPI 时钟引脚为 GPIO41
        .quadwp_io_num = GPIO_NUM_NC,       // 该值-1，不使用 quadwp 引脚（未用 QSPI 模式）
        .quadhd_io_num = GPIO_NUM_NC,       // 该值-1，不使用 quadhd 引脚（未用 QSPI 模式）
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2, // 最大单次传输字节数：宽*高*2（一屏 RGB565 数据）
    };
    // 初始化 SPI 总线，使用 DMA 通道自动分配
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO)); // 初始化 SPI3 主机并自动分配 DMA 通道，失败则断言重启

    // 创建 SPI Panel IO 句柄示例
    esp_lcd_panel_io_handle_t io_handle = NULL; // 定义 SPI 面板 IO 句柄并初始化为空
    esp_lcd_panel_io_spi_config_t io_config = { // 定义 SPI 面板 IO 配置结构体
        .cs_gpio_num =
            GPIO_NUM_NC, // 该CS引脚是使用pca9557的IO拓展芯片控制的，不是来源于ESP32-S3的IO
        .dc_gpio_num = GPIO_NUM_39,  // 该DC引脚是使用的ESP32-S3的IO39（命令/数据选择线）
        .spi_mode = 0,               // SPI模式0（CPOL=0, CPHA=0）
        .pclk_hz = 80 * 1000 * 1000, // 时钟线的速度80MHz
        .trans_queue_depth = 1,      // 队列深度 1：保证下一行发送前上一行已完成，静态行缓冲可安全复用
        .lcd_cmd_bits = 8,           // 命令字节宽度8位
        .lcd_param_bits = 8,         // 参数字节宽度8位
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle)); // 在 SPI3 上创建面板 IO，失败则断言重启
    g_io_handle = io_handle;                                                     // 保存到静态变量，供 lcd_get_io_handle() 使用

    esp_lcd_panel_dev_config_t panel_config = {    // 定义 ST7796 面板设备配置结构体
        .reset_gpio_num = GPIO_NUM_NC, // 该RST引脚是连接到复位按键，不是来源于ESP32-S3的IO
        .rgb_endian = LCD_RGB_ENDIAN_BGR, // 该RGB字节序是BGR（颜色分量在存储中的排列为蓝-绿-红）
        .bits_per_pixel = 16,             // 每个像素16位（RGB565 格式）
    };
    // 初始化 ST7796 Panel
    // g_panel_handle 是 ST7796 Panel 句柄，用于后续操作 ST7796
    // Panel(复位、设置显示模式、绘制图像等)
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(io_handle, &panel_config, &g_panel_handle)); // 基于面板 IO 创建 ST7796 面板句柄，失败则断言重启

    // 选中LCD，依赖于LCD_CS引脚输出低电平,才能配置LCD的参数
    lcd_cs(0);                                 // 拉低 CS 片选（低电平有效），选中 LCD，后续才能配置显示参数

    // 打开显示
    esp_lcd_panel_disp_on_off(g_panel_handle, true); // 通过面板句柄打开显示（发送开显示命令）

    // 开启颜色倒置（ST7796 发送 0x21 命令），使 RGB565 数值与屏幕显示颜色一致。
    // 注意：ST7796 的倒置方向与常见 ILI9341 相反，若实测画面颜色反相，
    // 请改为 false（发送 0x20 关闭倒置）
    esp_lcd_panel_invert_color(g_panel_handle, true);    // 开启颜色倒置，使显示颜色与 RGB 数值对应正确

    // 按 lcd.h 的 LCD_ORIENTATION 宏配置屏幕方向：
    // 横屏时交换坐标轴（MADCTL MV 位），并按宏设置水平/垂直镜像
    esp_lcd_panel_swap_xy(g_panel_handle, LCD_SWAP_XY); // 交换坐标轴：横屏开启，竖屏关闭
    esp_lcd_panel_mirror(g_panel_handle, LCD_MIRROR_X, LCD_MIRROR_Y); // 设置面板镜像（适配屏幕实际接法）

    // 初始化液晶屏，配置LCD的内部寄存器
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel_handle)); // 初始化面板内部寄存器，失败则断言重启

    return ESP_OK;                                      // 全部初始化完成，返回成功
}

/**
 * @brief 设置液晶屏颜色函数，将整个屏幕填充为指定的颜色
 * @param color 颜色值（RGB565 格式，16 位）
 */
void lcd_set_color(uint16_t color)          // 整屏填充颜色函数：将整个屏幕绘制为指定颜色
{
    // 直接使用内部 RAM 的静态行缓冲，避免向 SPI DMA 提交 PSRAM 指针
    for (int y = 0; y < LCD_HEIGHT; y++)           // 遍历屏幕的全部 LCD_HEIGHT 行
    {
        uint16_t *dst = s_line_buf[s_line_buf_idx]; // 取当前行缓冲
        s_line_buf_idx ^= 1;                        // 切换到另一行缓冲
        for (size_t i = 0; i < LCD_WIDTH; i++)      // 填满整行颜色
        {
            dst[i] = color;
        }
        esp_lcd_panel_draw_bitmap(g_panel_handle, 0, y, LCD_WIDTH, y + 1, dst); // 将一行颜色数据绘制到屏幕第 y 行
    }
}

/**
 * @brief 在屏幕指定区域绘制一张图片
 * @param x_s    图片起始 X 坐标（左上角）
 * @param y_s    图片起始 Y 坐标（左上角）
 * @param width  图片宽度（像素）
 * @param height 图片高度（像素）
 * @param pic    图片数据指针（RGB565 格式，大小需为 width*height 个 16 位数据）
 */
void lcd_draw_picture(uint32_t x_s,uint32_t y_s,uint32_t width,uint32_t height,uint16_t* pic) // 绘制图片函数：将指定大小的 RGB565 图片绘制到屏幕指定区域
{
    uint32_t x_e = x_s + width;             // 计算图片终止 X 坐标（右边界，开区间）
    uint32_t y_e = y_s + height;            // 计算图片终止 Y 坐标（下边界，开区间）

    // 循环遍历图片的每一行，经内部 RAM 行缓冲绘制到屏幕
    for(uint32_t y = y_s; y < y_e; y++)     // 遍历图片的每一行（从起始行到终止行）
    {
        lcd_draw_row(x_s, y, width, pic + (y - y_s) * width); // 拷贝图片相对行（而不是屏幕绝对行）的数据
    }
}

/**
 * @brief 在屏幕指定区域绘制图像数据（逐行经内部 RAM 行缓冲发送）
 * @param x_start    起始 X 坐标（左上角）
 * @param y_start    起始 Y 坐标（左上角）
 * @param x_end      结束 X 坐标（右下角，开区间）
 * @param y_end      结束 Y 坐标（右下角，开区间）
 * @param color_data RGB565 像素数据指针（可位于 PSRAM）
 * @note 人脸识别等 AI 模块通过此接口将摄像头整帧 RGB565 数据绘制到屏幕。
 *       逐行拷贝到内部 RAM 再交给 SPI，避免 SPI 驱动为 PSRAM 指针动态申请
 *       大块 DMA 中转缓冲导致分配失败。
 */
void lcd_draw_bitmap(uint32_t x_start, uint32_t y_start, uint32_t x_end, uint32_t y_end, const void *color_data)
{
    uint32_t width = x_end - x_start;               // 本区域每行像素数
    const uint16_t *src = (const uint16_t *)color_data; // 源像素按 RGB565 访问
    uint32_t y = y_start;
    uint32_t rows = y_end - y_start;                // 待绘制总行数

    if (width > LCD_WIDTH)                          // 分块缓冲每行只有 LCD_WIDTH 个像素
    {
        width = LCD_WIDTH;
    }

    /* 按 LCD_CHUNK_ROWS 行一块批量传输：一次 draw_bitmap 只发一组窗口命令，
     * 把每帧 240 次 SPI 事务降到 30 次，是帧率提升的主要来源 */
    while (rows > 0)
    {
        uint32_t chunk = (rows > LCD_CHUNK_ROWS) ? LCD_CHUNK_ROWS : rows;
        uint16_t *dst = s_chunk_buf[s_chunk_idx];
        s_chunk_idx ^= 1;                           // 切换到另一块缓冲，供下一块使用

        for (uint32_t r = 0; r < chunk; r++)        // 逐行拷贝到内部 RAM（源可能位于 PSRAM）
        {
            memcpy(dst + r * width, src + r * width, width * sizeof(uint16_t));
        }

        esp_lcd_panel_draw_bitmap(g_panel_handle, x_start, y, x_start + width, y + chunk, dst);

        src += chunk * width;                       // 指向下一块的源数据
        y += chunk;
        rows -= chunk;
    }
}

/**
 * @brief 获取 UTF-8 字符的字节长度
 * @param target 指向字符首字节的指针
 * @return int 字符占用字节数（1~4），非法序列返回 1
 */
static int GetUtf8CharLength(char *target)
{
    if (target[0] <= 0x7F) {
        return 1;
    }
    else if ((target[0] >= 0xC2 && target[0] <= 0xDF) && (target[1] & 0xC0) == 0x80) {
        return 2;
    }
    else if ((target[0] >= 0xE0 && target[0] <= 0xEF) && (target[1] & 0xC0) == 0x80
             && (target[2] & 0xC0) == 0x80) {
        return 3;
    }
    else if ((target[0] >= 0xF0 && target[0] <= 0xF7) && (target[1] & 0xC0) == 0x80
             && (target[2] & 0xC0) == 0x80 && (target[3] & 0xC0) == 0x80) {
        return 4;
    }
    return 1;
}

/**
 * @brief 在字库中查找字符对应的索引
 * @param target    指向字符首字节的指针（UTF-8）
 * @param font_size 字号（16 或 32），决定使用哪个字库
 * @return int 命中返回字库索引，未命中返回 -1
 */
static int FindFontIndex(char *target, uint32_t font_size)
{
    int i;
    int char_len = GetUtf8CharLength(target);

    if (font_size == 16) {
        for (i = 0;
             i < (sizeof(g_font_dot_matrix_16_index) / sizeof(g_font_dot_matrix_16_index[0]));
             i++) {
            if (strncmp(g_font_dot_matrix_16_index[i], (char *)target, char_len) == 0) {
                return i;
            }
        }
    }

    if (font_size == 32) {
        for (i = 0;
             i < (sizeof(g_font_dot_matrix_32_index) / sizeof(g_font_dot_matrix_32_index[0]));
             i++) {
            if (strncmp(g_font_dot_matrix_32_index[i], (char *)target, char_len) == 0) {
                return i;
            }
        }
    }

    return -1;
}

/**
 * @brief 在 LCD 上绘制单个点阵字符
 * @param x         起始 X 坐标
 * @param y         起始 Y 坐标
 * @param fc        前景色（RGB565）
 * @param bc        背景色（RGB565）
 * @param font_size 字号（16 或 32）
 * @param index     字库索引
 */
static void lcd_display_char(uint32_t x, uint32_t y, uint32_t fc, uint32_t bc,
                             uint32_t font_size, uint32_t index)
{
    uint32_t i = 0, j = 0;
    uint8_t tmp = 0;
    /* 使用内部 RAM 的静态字模缓冲，避免 PSRAM 指针触发 SPI DMA 中转分配 */
    uint16_t *pixels = s_font_buf[s_font_buf_idx];
    s_font_buf_idx ^= 1;                        // 切换到另一个字模缓冲
    uint32_t pixel_idx = 0;

    for (i = 0; i < font_size * font_size / 8; i++) {
        if (font_size == 16)
            tmp = g_font_dot_matrix_16[index][i];
        else if (font_size == 32)
            tmp = g_font_dot_matrix_32[index][i];

        for (j = 0; j < 8; j++) {
            if (tmp & (1 << j)) {
                pixels[pixel_idx] = (uint16_t)fc;
            } else {
                pixels[pixel_idx] = (uint16_t)bc;
            }
            pixel_idx++;
        }
    }

    esp_lcd_panel_draw_bitmap(g_panel_handle, x, y, x + font_size, y + font_size, pixels);
}

/**
 * @brief 在 LCD 上显示字符串（支持中文，使用 font 目录点阵字库）
 * @param x         起始 X 坐标
 * @param y         起始 Y 坐标
 * @param fc        前景色（RGB565）
 * @param bc        背景色（RGB565）
 * @param font_size 字号（16 或 32）
 * @param str       要显示的字符串（UTF-8 编码）
 * @note 中文字符占 font_size 像素宽，ASCII 字符占 font_size/2 像素宽
 * @note 超出屏幕宽度自动换行
 */
void lcd_display_string(uint32_t x, uint32_t y, uint32_t fc, uint32_t bc,
                        uint32_t font_size, char *str)
{
    uint32_t x_s = x;
    uint32_t i = 0;
    int32_t font_index = 0;

    while (str[i] != '\0') {
        font_index = FindFontIndex(&str[i], font_size);

        if (font_index < 0) {
            i++;
            continue;
        }

        lcd_display_char(x, y, fc, bc, font_size, font_index);

        if (str[i] > 0x7F) {
            x += font_size;
            i += 3;
        } else {
            x += font_size / 2;
            i += 1;
        }

        if ((x + font_size) >= LCD_WIDTH) {
            x = x_s;
            y += font_size;
        }
    }
}

/**
 * @brief 格式化显示字符串（printf 风格格式化后调用 lcd_display_string）
 */
void lcd_display_string_fmt(uint32_t x, uint32_t y, uint32_t fc, uint32_t bc,
                            uint32_t font_size, char *str, ...)
{
    va_list args;
    char temp_buf[256];
    va_start(args, str);
    vsprintf(temp_buf, str, args);
    va_end(args);
    lcd_display_string(x, y, fc, bc, font_size, temp_buf);
}

/**
 * @brief 把 UTF-8 字符串渲染到 RGB565 缓冲区（点阵字库，供 LVGL 图像控件使用）
 *
 * 与 lcd_display_string() 使用同一套 16/32 点阵字库，但结果写入内存缓冲而不是
 * 直接刷屏。写入的像素按“高位字节在前”的 RGB565 字节序存放，与
 * CONFIG_LV_COLOR_16_SWAP=y 时 LVGL 期望的图像数据一致。
 *
 * @param dst       目标缓冲（按 dst_w 像素一行存放）
 * @param dst_w     缓冲宽度（像素）
 * @param dst_h     缓冲高度（像素）
 * @param x         文字起始 X（相对缓冲左上角）
 * @param y         文字起始 Y
 * @param fc        前景色（标准 RGB565）
 * @param bc        背景色（标准 RGB565）
 * @param font_size 字号（16 或 32）
 * @param str       要渲染的字符串（UTF-8），字库中不存在的字符会被跳过
 */
void lcd_render_string_to_buf(uint16_t *dst, uint32_t dst_w, uint32_t dst_h,
                              uint32_t x, uint32_t y, uint16_t fc, uint16_t bc,
                              uint32_t font_size, const char *str)
{
    /* 面板按小端接收 RGB565，而 LVGL（16 位色交换）按大端解释图像数据，
     * 因此这里把颜色高低字节对调后再写入缓冲 */
    const uint16_t fc_swap = (uint16_t)((fc >> 8) | (fc << 8));
    const uint16_t bc_swap = (uint16_t)((bc >> 8) | (bc << 8));
    const char *p = str;
    uint32_t i = 0;
    uint32_t x_s = x;
    uint32_t cur_x = x;
    uint32_t cur_y = y;
    int32_t font_index = 0;

    while (p[i] != '\0') {
        font_index = FindFontIndex((char *)&p[i], font_size);

        if (font_index < 0) {
            i++;
            continue;
        }

        /* 逐像素解码点阵并写入缓冲（像素序号按行优先排列） */
        for (uint32_t n = 0; n < font_size * font_size; n++) {
            uint8_t byte = (font_size == 16) ? g_font_dot_matrix_16[font_index][n / 8]
                                             : g_font_dot_matrix_32[font_index][n / 8];
            uint32_t px = cur_x + (n % font_size);
            uint32_t py = cur_y + (n / font_size);

            if (px < dst_w && py < dst_h) {
                dst[py * dst_w + px] = (byte & (1 << (n % 8))) ? fc_swap : bc_swap;
            }
        }

        if (p[i] > 0x7F) {
            cur_x += font_size;
            i += 3;
        } else {
            cur_x += font_size / 2;
            i += 1;
        }

        /* 超出缓冲宽度自动换行 */
        if ((cur_x + font_size) > dst_w) {
            cur_x = x_s;
            cur_y += font_size;
        }
    }
}

/**
 * @brief 在 LCD 指定区域填充纯色（用于清除信息显示区域）
 * @param x     起始 X 坐标
 * @param y     起始 Y 坐标
 * @param w     区域宽度（像素）
 * @param h     区域高度（像素）
 * @param color 填充颜色（RGB565 格式）
 * @note 逐行绘制，使用内部 RAM 的静态行缓冲，不动态分配内存
 */
void lcd_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color)
{
    if (w > LCD_WIDTH) {
        w = LCD_WIDTH;
    }

    // 逐行填充并通过内部 RAM 行缓冲发送
    for (uint32_t row = 0; row < h; row++) {
        uint16_t *dst = s_line_buf[s_line_buf_idx];
        s_line_buf_idx ^= 1;
        for (uint32_t i = 0; i < w; i++) {
            dst[i] = color;
        }
        esp_lcd_panel_draw_bitmap(g_panel_handle, x, y + row, x + w, y + row + 1, dst);
    }
}
