/**
 * @file who_ai_utils.cpp
 * @brief AI工具函数库 — 人脸检测结果绘制与打印
 *
 * ============================================================
 * 使用说明：
 * ============================================================
 * 1. 功能概述：
 *    - 提供人脸检测结果的可视化绘制函数
 *    - 支持RGB565和RGB888两种像素格式
 *    - 支持检测结果的串口打印输出
 *    - 提供摄像头帧格式解码函数
 *
 * 2. 绘制函数说明：
 *    - draw_detection_result():
 *      在图像上绘制人脸边界框（矩形框）和五个面部关键点
 *      关键点包括：左眼、右眼、鼻子、左嘴角、右嘴角
 *
 * 3. 颜色编码（RGB565格式）：
 *    - 左眼：红色 (0b0000000011111000)
 *    - 右眼：蓝色 (0b0001111100000000)
 *    - 鼻子：绿色 (0b1110000000000111)
 *    - 左嘴角：红色 (0b0000000011111000)
 *    - 右嘴角：蓝色 (0b0001111100000000)
 *    - 边界框：绿色 (0b1110000000000111)
 *
 * 4. 颜色编码（RGB888格式）：
 *    - 左眼：蓝色 (0x0000FF)
 *    - 右眼：红色 (0xFF0000)
 *    - 鼻子：绿色 (0x00FF00)
 *    - 左嘴角：蓝色 (0x0000FF)
 *    - 右嘴角：红色 (0xFF0000)
 *    - 边界框：绿色 (0x00FF00)
 *
 * 5. 注意事项：
 *    - 关键点判定：当keypoint.size() == 10时绘制
 *    - RGB565格式用于LCD直接显示
 *    - RGB888格式用于MJPG编码或其他处理
 *    - app_camera_decode()需要手动free返回的内存
 *
 * ============================================================
 */

#include "who_ai_utils.hpp"            // AI工具函数头文件

#include "esp_log.h"                   // ESP-IDF日志系统
#include "esp_camera.h"                // ESP32摄像头驱动

#include "dl_image.hpp"                // 深度学习图像处理库

// 日志标签，用于标识本模块的日志输出
static const char *TAG = "ai_utils";

/**
 * ┌───────┬────────────────────┬──────────┐
 * │       │       RGB565       │  RGB888  │
 * ├───────┼────────────────────┼──────────┤
 * │  红   │ 0b0000000011111000 │ 0x0000FF │
 * │  绿   │ 0b1110000000000111 │ 0x00FF00 │
 * │  蓝   │ 0b0001111100000000 │ 0xFF0000 │
 * └───────┴────────────────────┴──────────┘
 * RGB565格式中，红色占高5位，绿色占中间6位，蓝色占低5位
 * RGB888格式中，按字节顺序B/G/R排列
 */

/**
 * @brief 在RGB565格式图像上绘制人脸检测结果
 *
 * 在图像缓冲区中直接绘制人脸边界框和五个面部关键点（左眼、右眼、
 * 鼻子、左嘴角、右嘴角）。所有绘制操作直接修改图像缓冲区内容，
 * 不会创建新的图像副本。
 *
 * @param image_ptr   图像缓冲区指针（RGB565格式，16位像素）
 * @param image_height 图像高度（像素）
 * @param image_width  图像宽度（像素）
 * @param results     人脸检测结果列表，包含边界框和关键点坐标
 * @return 无返回值
 *
 * @note 边界框颜色为绿色 (RGB565: 0b1110000000000111)
 * @note 左眼和左嘴角使用红色 (RGB565: 0b0000000011111000)
 * @note 右眼和右嘴角使用蓝色 (RGB565: 0b0001111100000000)
 * @note 鼻子使用绿色 (RGB565: 0b1110000000000111)
 * @note 关键点坐标为原始图像坐标系，可能为负值，使用DL_MAX限制边界
 */
void draw_detection_result(uint16_t *image_ptr, int image_height, int image_width, std::list<dl::detect::result_t> &results)
{
    // 循环索引，用于标识第几个人脸
    int i = 0;

    // 遍历所有检测到的人脸结果
    for (std::list<dl::detect::result_t>::iterator prediction = results.begin();
         prediction != results.end();          // 直到列表末尾
         prediction++, i++)                     // 递增迭代器和计数器
    {
        /**
         * 在图像上绘制空心矩形作为人脸边界框
         * 使用DL_MAX将负坐标限制为0，防止越界访问
         * 颜色：绿色 (0b1110000000000111)
         */
        dl::image::draw_hollow_rectangle(
            image_ptr,                    // 图像缓冲区指针
            image_height,                 // 图像高度
            image_width,                  // 图像宽度
            DL_MAX(prediction->box[0], 0),// 边界框左上角X坐标（限制≥0）
            DL_MAX(prediction->box[1], 0),// 边界框左上角Y坐标（限制≥0）
            DL_MAX(prediction->box[2], 0),// 边界框右下角X坐标（限制≥0）
            DL_MAX(prediction->box[3], 0),// 边界框右下角Y坐标（限制≥0）
            0b1110000000000111            // 绿色RGB565
        );

        /**
         * 检查关键点数量是否为10个
         * 标准人脸关键点排列顺序：
         *   [0,1] = 左眼    [2,3] = 左嘴角
         *   [4,5] = 鼻子    [6,7] = 右眼
         *   [8,9] = 右嘴角
         */
        if (prediction->keypoint.size() == 10)
        {
            /**
             * 绘制左眼关键点（红色）
             * 点半径：4像素
             * 颜色：0b0000000011111000（红色RGB565）
             * 坐标索引：[0] = X, [1] = Y
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,             // 图像数据
                DL_MAX(prediction->keypoint[0], 0),                // 左眼X坐标
                DL_MAX(prediction->keypoint[1], 0),                // 左眼Y坐标
                4,                                                // 点半径（像素）
                0b0000000011111000                                // 红色
            );

            /**
             * 绘制左嘴角关键点（红色）
             * 点半径：4像素
             * 颜色：0b0000000011111000（红色RGB565）
             * 坐标索引：[2] = X, [3] = Y
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,             // 图像数据
                DL_MAX(prediction->keypoint[2], 0),                // 左嘴角X坐标
                DL_MAX(prediction->keypoint[3], 0),                // 左嘴角Y坐标
                4,                                                // 点半径（像素）
                0b0000000011111000                                // 红色
            );

            /**
             * 绘制鼻子关键点（绿色）
             * 点半径：4像素
             * 颜色：0b1110000000000111（绿色RGB565）
             * 坐标索引：[4] = X, [5] = Y
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,             // 图像数据
                DL_MAX(prediction->keypoint[4], 0),                // 鼻子X坐标
                DL_MAX(prediction->keypoint[5], 0),                // 鼻子Y坐标
                4,                                                // 点半径（像素）
                0b1110000000000111                                // 绿色
            );

            /**
             * 绘制右眼关键点（蓝色）
             * 点半径：4像素
             * 颜色：0b0001111100000000（蓝色RGB565）
             * 坐标索引：[6] = X, [7] = Y
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,             // 图像数据
                DL_MAX(prediction->keypoint[6], 0),                // 右眼X坐标
                DL_MAX(prediction->keypoint[7], 0),                // 右眼Y坐标
                4,                                                // 点半径（像素）
                0b0001111100000000                                // 蓝色
            );

            /**
             * 绘制右嘴角关键点（蓝色）
             * 点半径：4像素
             * 颜色：0b0001111100000000（蓝色RGB565）
             * 坐标索引：[8] = X, [9] = Y
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,             // 图像数据
                DL_MAX(prediction->keypoint[8], 0),                // 右嘴角X坐标
                DL_MAX(prediction->keypoint[9], 0),                // 右嘴角Y坐标
                4,                                                // 点半径（像素）
                0b0001111100000000                                // 蓝色
            );
        }
    }
}

/**
 * @brief 在RGB888格式图像上绘制人脸检测结果
 *
 * 在图像缓冲区中直接绘制人脸边界框和五个面部关键点。
 * RGB888格式使用24位表示一个像素（B/G/R各8位）。
 * 此重载版本适用于MJGP编码或其他需要RGB888格式的场景。
 *
 * @param image_ptr   图像缓冲区指针（RGB888格式，24位像素）
 * @param image_height 图像高度（像素）
 * @param image_width  图像宽度（像素）
 * @param results     人脸检测结果列表，包含边界框和关键点坐标
 * @return 无返回值
 *
 * @note RGB888颜色编码：B（位0-7）、G（位8-15）、R（位16-23）
 * @note 边界框颜色为绿色 (0x00FF00)
 * @note 左眼和左嘴角使用蓝色 (0x0000FF)
 * @note 右眼和右嘴角使用红色 (0xFF0000)
 * @note 鼻子使用绿色 (0x00FF00)
 */
void draw_detection_result(uint8_t *image_ptr, int image_height, int image_width, std::list<dl::detect::result_t> &results)
{
    // 循环索引，用于标识第几个人脸
    int i = 0;

    // 遍历所有检测到的人脸结果
    for (std::list<dl::detect::result_t>::iterator prediction = results.begin();
         prediction != results.end();          // 直到列表末尾
         prediction++, i++)                     // 递增迭代器和计数器
    {
        /**
         * 在图像上绘制空心矩形作为人脸边界框（绿色）
         * RGB888颜色：0x00FF00（绿色）
         */
        dl::image::draw_hollow_rectangle(
            image_ptr,                    // 图像缓冲区指针
            image_height,                 // 图像高度
            image_width,                  // 图像宽度
            DL_MAX(prediction->box[0], 0),// 边界框左上角X坐标
            DL_MAX(prediction->box[1], 0),// 边界框左上角Y坐标
            DL_MAX(prediction->box[2], 0),// 边界框右下角X坐标
            DL_MAX(prediction->box[3], 0),// 边界框右下角Y坐标
            0x00FF00                      // 绿色RGB888
        );

        /**
         * 检查关键点数量是否为10个（5个关键点×2坐标）
         */
        if (prediction->keypoint.size() == 10)
        {
            /**
             * 绘制左眼关键点（蓝色）
             * RGB888颜色：0x0000FF（蓝色）
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,
                DL_MAX(prediction->keypoint[0], 0), DL_MAX(prediction->keypoint[1], 0),
                4,              // 点半径（像素）
                0x0000FF        // 蓝色
            );

            /**
             * 绘制左嘴角关键点（蓝色）
             * RGB888颜色：0x0000FF（蓝色）
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,
                DL_MAX(prediction->keypoint[2], 0), DL_MAX(prediction->keypoint[3], 0),
                4,              // 点半径（像素）
                0x0000FF        // 蓝色
            );

            /**
             * 绘制鼻子关键点（绿色）
             * RGB888颜色：0x00FF00（绿色）
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,
                DL_MAX(prediction->keypoint[4], 0), DL_MAX(prediction->keypoint[5], 0),
                4,              // 点半径（像素）
                0x00FF00        // 绿色
            );

            /**
             * 绘制右眼关键点（红色）
             * RGB888颜色：0xFF0000（红色）
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,
                DL_MAX(prediction->keypoint[6], 0), DL_MAX(prediction->keypoint[7], 0),
                4,              // 点半径（像素）
                0xFF0000        // 红色
            );

            /**
             * 绘制右嘴角关键点（红色）
             * RGB888颜色：0xFF0000（红色）
             */
            dl::image::draw_point(
                image_ptr, image_height, image_width,
                DL_MAX(prediction->keypoint[8], 0), DL_MAX(prediction->keypoint[9], 0),
                4,              // 点半径（像素）
                0xFF0000        // 红色
            );
        }
    }
}

/**
 * @brief 在控制台打印人脸检测结果
 *
 * 将检测到的人脸边界框和五个关键点坐标输出到串口控制台。
 * 使用ESP_LOGI日志系统，标签为"detection_result"。
 *
 * @param results 人脸检测结果列表，包含边界框和关键点坐标
 * @return 无返回值
 *
 * @note 输出格式：
 *   [序号]: (左上X, 左上Y, 右下X, 右下Y)
 *   left eye: (X, Y), right eye: (X, Y), nose: (X, Y),
 *   mouth left: (X, Y), mouth right: (X, Y)
 */
void print_detection_result(std::list<dl::detect::result_t> &results)
{
    // 循环索引，用于标识第几个人脸
    int i = 0;

    // 遍历所有检测到的人脸结果
    for (std::list<dl::detect::result_t>::iterator prediction = results.begin();
         prediction != results.end();          // 直到列表末尾
         prediction++, i++)                     // 递增迭代器和计数器
    {
        /**
         * 打印人脸边界框坐标
         * 格式：[序号]: (左上X, 左上Y, 右下X, 右下Y)
         * 例如：[ 0]: (120, 80, 200, 180)
         */
        ESP_LOGI("detection_result",
                 "[%2d]: (%3d, %3d, %3d, %3d)",
                 i,                                                // 人脸序号
                 prediction->box[0], prediction->box[1],           // 左上角坐标
                 prediction->box[2], prediction->box[3]);          // 右下角坐标

        /**
         * 检查关键点数量是否为10个（5个关键点×2坐标）
         * 只有10个关键点时才打印详细信息
         */
        if (prediction->keypoint.size() == 10)
        {
            /**
             * 打印五个面部关键点坐标
             * 关键点类型：左眼、右眼、鼻子、左嘴角、右嘴角
             * 坐标单位：像素
             */
            ESP_LOGI("detection_result",
                     "      left eye: (%3d, %3d), right eye: (%3d, %3d), "
                     "nose: (%3d, %3d), mouth left: (%3d, %3d), "
                     "mouth right: (%3d, %3d)",
                     prediction->keypoint[0], prediction->keypoint[1],  // 左眼坐标
                     prediction->keypoint[6], prediction->keypoint[7],  // 右眼坐标
                     prediction->keypoint[4], prediction->keypoint[5],  // 鼻子坐标
                     prediction->keypoint[2], prediction->keypoint[3],  // 左嘴角坐标
                     prediction->keypoint[8], prediction->keypoint[9]); // 右嘴角坐标
        }
    }
}

/**
 * @brief 解码摄像头帧数据为RGB888格式
 *
 * 将摄像头采集的帧数据统一转换为RGB888格式返回。
 * 如果帧已经是RGB565格式，直接返回原始数据指针。
 * 否则通过fmt2rgb888函数进行格式转换，转换后的内存
 * 需要调用者手动释放。
 *
 * @param fb 摄像头帧缓冲区指针，包含图像数据和格式信息
 * @return void* 成功返回图像数据指针（RGB565或RGB888），
 *               失败返回NULL
 *
 * @note 返回值如果是RGB888格式（非RGB565），调用者必须
 *       使用free()释放返回的内存，否则会造成内存泄漏
 * @note 支持的输入格式：RGB565、JPEG、GRAYSCALE等
 * @note 转换失败时会打印错误日志并释放已分配的内存
 *
 * @warning 返回的RGB888内存由fmt2rgb888内部使用dl::tool::malloc_aligned
 *          分配，不能直接用free()释放。应该用dl::tool::free_aligned()
 */
void *app_camera_decode(camera_fb_t *fb)
{
    // 检查帧格式是否为RGB565
    if (fb->format == PIXFORMAT_RGB565)
    {
        /**
         * RGB565格式直接返回原始缓冲区指针
         * 不需要格式转换，节省内存和CPU开销
         */
        return (void *)fb->buf;
    }
    else
    {
        /**
         * 非RGB565格式需要转换为RGB888
         * 分配目标缓冲区：height × width × 3字节（RGB888）
         */
        uint8_t *image_ptr = (uint8_t *)malloc(
            fb->height * fb->width * 3 * sizeof(uint8_t)  // RGB888：每像素3字节
        );

        // 检查内存分配是否成功
        if (image_ptr)
        {
            /**
             * 调用格式转换函数将帧数据转换为RGB888
             * fb->buf：原始帧数据
             * fb->len：原始帧数据长度
             * fb->format：原始帧格式（如JPEG、GRAYSCALE等）
             * image_ptr：目标RGB888缓冲区
             */
            if (fmt2rgb888(fb->buf, fb->len, fb->format, image_ptr))
            {
                // 转换成功，返回RGB888数据指针
                return (void *)image_ptr;
            }
            else
            {
                // 转换失败，打印错误日志
                ESP_LOGE(TAG, "fmt2rgb888 failed");
                // 释放已分配的缓冲区，防止内存泄漏
                dl::tool::free_aligned(image_ptr);
            }
        }
        else
        {
            // 内存分配失败，打印错误日志
            ESP_LOGE(TAG, "malloc memory for image rgb888 failed");
        }
    }

    // 所有分支都失败时返回NULL
    return NULL;
}
