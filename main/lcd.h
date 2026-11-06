#ifndef __LCD_H__
#define __LCD_H__

#include "driver/gpio.h"
#include "esp_lcd_types.h"

/* ==================== 屏幕方向配置 ====================
 * 通过修改 LCD_ORIENTATION 宏即可在竖屏/横屏之间切换，本工程默认横屏。
 *   - 竖屏：LCD_WIDTH=320、LCD_HEIGHT=480（MADCTL: 仅水平镜像）
 *   - 横屏：LCD_WIDTH=480、LCD_HEIGHT=320（MADCTL: 交换坐标轴 + 水平/垂直镜像）
 * 横屏由 ST7796 控制器的 MADCTL 寄存器（MV/MX/MY 位）实现，无需改动上层绘制逻辑。
 */
#define LCD_ORIENTATION_PORTRAIT   0
#define LCD_ORIENTATION_LANDSCAPE  1
#define LCD_ORIENTATION            LCD_ORIENTATION_LANDSCAPE   // 当前选择：横屏

#if (LCD_ORIENTATION == LCD_ORIENTATION_LANDSCAPE)
#define LCD_WIDTH    480   /* 横屏有效显示宽度（像素） */
#define LCD_HEIGHT   320   /* 横屏有效显示高度（像素） */
#define LCD_SWAP_XY  1     /* 交换坐标轴（ST7796 MADCTL MV 位） */
#define LCD_MIRROR_X 1     /* 水平镜像（ST7796 MADCTL MX 位） */
#define LCD_MIRROR_Y 1     /* 垂直镜像（ST7796 MADCTL MY 位） */
#else
#define LCD_WIDTH    320   /* 竖屏有效显示宽度（像素） */
#define LCD_HEIGHT   480   /* 竖屏有效显示高度（像素） */
#define LCD_SWAP_XY  0     /* 不交换坐标轴 */
#define LCD_MIRROR_X 1     /* 水平镜像（与当前板卡实际接法匹配） */
#define LCD_MIRROR_Y 0     /* 不垂直镜像 */
#endif

    // LCD 背光引脚配置
#define LCD_BACKLIGHT_GPIO_PIN GPIO_NUM_42 



/* ==================== 函数声明（extern "C" 防止 C++ 名字修饰） ==================== */
#ifdef __cplusplus
extern "C" {
#endif

void lcd_backlight_init(void);
void lcd_backlight_set(uint32_t state);
esp_err_t lcd_init(void);
void lcd_cs(uint32_t state);
void lcd_set_color(uint16_t color);
void lcd_draw_picture(uint32_t x_s,uint32_t y_s,uint32_t width,uint32_t height,uint16_t* pic);
esp_lcd_panel_handle_t lcd_get_panel_handle(void);
esp_lcd_panel_io_handle_t lcd_get_io_handle(void);

/* ==================== 文本/图形绘制接口（人脸识别等模块使用） ==================== */
/**
 * @brief 在屏幕指定区域绘制图像数据（内部按 LCD_CHUNK_ROWS 行分块，经内部 RAM 中转后交给 SPI）
 * @param x_start    起始 X 坐标（左上角）
 * @param y_start    起始 Y 坐标（左上角）
 * @param x_end      结束 X 坐标（右下角，开区间）
 * @param y_end      结束 Y 坐标（右下角，开区间）
 * @param color_data RGB565 像素数据指针
 */
void lcd_draw_bitmap(uint32_t x_start, uint32_t y_start, uint32_t x_end, uint32_t y_end, const void *color_data);

/**
 * @brief 在 LCD 指定区域填充纯色（用于清除信息显示区域）
 * @param x     起始 X 坐标
 * @param y     起始 Y 坐标
 * @param w     区域宽度（像素）
 * @param h     区域高度（像素）
 * @param color 填充颜色（RGB565 格式）
 */
void lcd_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color);

/**
 * @brief 在 LCD 上显示字符串（支持中文，使用 font 目录点阵字库，font_size 仅支持 16/32）
 * @param x         起始 X 坐标
 * @param y         起始 Y 坐标
 * @param fc        前景色（RGB565）
 * @param bc        背景色（RGB565）
 * @param font_size 字号（16 或 32）
 * @param str       要显示的字符串（UTF-8 编码）
 */
void lcd_display_string(uint32_t x, uint32_t y, uint32_t fc, uint32_t bc, uint32_t font_size, char *str);

/**
 * @brief 格式化显示字符串（printf 风格格式化后调用 lcd_display_string）
 */
void lcd_display_string_fmt(uint32_t x, uint32_t y, uint32_t fc, uint32_t bc, uint32_t font_size, char *str, ...);

/**
 * @brief 把 UTF-8 字符串渲染到 RGB565 缓冲区（点阵字库，供 LVGL 图像控件使用）
 *
 * 写入的像素为“高位字节在前”的 RGB565 字节序（匹配 CONFIG_LV_COLOR_16_SWAP=y），
 * 字库中不存在的字符会被跳过，超出缓冲宽度自动换行。
 *
 * @param dst       目标缓冲（每行 dst_w 个像素）
 * @param dst_w     缓冲宽度（像素）
 * @param dst_h     缓冲高度（像素）
 * @param x         文字起始 X
 * @param y         文字起始 Y
 * @param fc        前景色（标准 RGB565）
 * @param bc        背景色（标准 RGB565）
 * @param font_size 字号（16 或 32）
 * @param str       字符串（UTF-8）
 */
void lcd_render_string_to_buf(uint16_t *dst, uint32_t dst_w, uint32_t dst_h,
                              uint32_t x, uint32_t y, uint16_t fc, uint16_t bc,
                              uint32_t font_size, const char *str);

#ifdef __cplusplus
}
#endif

#endif
