/**
 * @file who_human_face_recognition.cpp
 * @brief 人脸识别应用主程序 — 支持人脸注册、识别和清空
 *
 * ============================================================
 * 使用说明：
 * ============================================================
 * 1. 功能概述：
 *    - 本程序实现基于ESP32-S3的实时人脸检测与识别系统
 *    - 通过摄像头采集视频帧，使用AI模型进行人脸检测和识别
 *    - 支持三种按键操作：长按注册、单击识别、双击清空
 *
 * 2. 硬件要求：
 *    - ESP32-S3开发板（需要PSRAM）
 *    - 摄像头模块（OV2640/OV3660等，支持RGB565输出）
 *    - SPI接口LCD显示屏
 *    - 按键（GPIO11）
 *
 * 3. 操作说明（通过按键操作）：
 *    - 长按(>1秒)：保存当前检测到的人脸数据到Flash分区表
 *    - 单击：识别当前检测到的人脸并在控制台显示结果
 *    - 双击：清空Flash中存储的所有人脸数据（无需检测到人脸）
 *
 * 4. 软件架构：
 *    4个FreeRTOS任务流水线工作：
 *    a) task_process_camera：摄像头帧采集（核心1，优先级5）
 *    b) task_process_ai：AI人脸检测与识别（核心0，优先级5）
 *    c) task_process_lcd：LCD屏幕显示（核心0，优先级5）
 *    d) button_task：按键检测（核心0，优先级3）
 *
 * 5. 数据流：
 *    摄像头 → [xQueueAIFrame] → AI检测 → [xQueueLCDFrame] → LCD显示
 *                                     ↕
 *                              按键事件触发人脸注册/识别/清空
 *
 * 6. 存储说明：
 *    - 人脸数据存储在Flash的DATA分区，子类型ANY，标签"storage"
 *    - 支持写入Flash持久化和从Flash加载已注册的人脸数据
 *    - 分区表需要在menuconfig或partitions.csv中预留足够空间
 *
 * 7. 编译配置：
 *    - ESP-IDF 5.5.0
 *    - 需要在menuconfig中使能PSRAM
 *    - 需要配置摄像头和LCD的引脚
 *    - 需要配置Flash分区表（建议使用factory_8MB或自定义）
 *
 * 8. 注意事项：
 *    - AI推理需要较大栈空间，task_process_ai 栈大小设为8KB
 *    - LCD显示与Flash写入共用SPI总线，需避免同时操作
 *    - 按键事件使用互斥锁保护，线程安全
 *    - 双击清空操作不依赖于摄像头帧数据
 *
 * ============================================================
 */

// ======================== 头文件包含 ========================
#include "who_human_face_recognition.hpp"  // 人脸识别主头文件
#include "dl_image.hpp"                    // 深度学习图像处理库
#include "driver/gpio.h"                   // GPIO驱动
#include "audio_prompt.h"                  // 固定语音片段播报
#include "esp_camera.h"                    // ESP32摄像头驱动
#include "esp_heap_caps.h"                 // PSRAM 分配（头像裁剪缓冲）
#include "esp_log.h"                       // ESP-IDF日志系统
#include "esp_timer.h"                     // 高精度计时，用于统计 AI/显示耗时
#include "esp_vfs_fat.h"                   // FATFS 挂载（vfs 分区，存人脸头像）
#include "wear_levelling.h"                // 磨损均衡句柄（FATFS + WL 搭配使用）
#include "face_recognition_112_v1_s16.hpp" // 人脸识别器（S16量化）
#include "face_recognition_tool.hpp"       // 人脸识别工具函数
#include "human_face_detect_mnp01.hpp"     // MNP01人脸精检测器
#include "human_face_detect_msr01.hpp"     // MSR01人脸粗检测器
#include "led.h"                           // LED（IO10）用作开门/锁门状态指示
#include "lvgl_ui.h"                       // LVGL 界面模块：预览/登入按钮/信息文字
#include "nvs.h"                           // NVS：保存管理员界面调节的相似度阈值
#include "tp.h"                            // 触摸快照：按下期间暂停预览，避免抢占 LVGL 输入轮询
#include "who_ai_utils.hpp"                // AI通用工具函数
#include "time_sync.h"                     // 识别时间戳（历史记录 + 云端上传）
#include "bemfa_uploader.h"                // 巴法云照片上传队列
#include <stdarg.h>                        // 可变参数接口：va_list/vsnprintf
#include <stdio.h>                         // 文件接口：fopen/fread/fwrite（头像文件）
#include <string.h>                        // C字符串接口：strlen/strncpy/strcmp
#include <sys/stat.h>                      // 目录接口：mkdir（创建头像目录）
#include <math.h>                          // 数学接口：sqrtf（模板自学习归一化）
#include <map>                             // 模板自学习次数：姓名 -> 已学习次数
#include <string>                          // C++标准字符串

// ======================== 日志与全局变量 ========================

/**
 * @var TAG
 * @brief 日志标签，用于ESP-IDF日志系统输出识别信息
 *
 * 所有日志输出使用此标签，便于在串口监视器中过滤
 */
static const char* TAG = "human_face_recognition";

/**
 * @var xQueueLCDFrame
 * @brief LCD显示帧队列句柄
 *
 * 用于在AI处理任务和LCD显示任务之间传递已处理的帧数据
 * 队列元素类型：camera_fb_t *（帧缓冲区指针）
 * 队列长度：2
 */
static QueueHandle_t xQueueLCDFrame = NULL;

/**
 * @var xQueueAIFrame
 * @brief AI处理帧队列句柄
 *
 * 用于在摄像头采集任务和AI处理任务之间传递原始帧数据
 * 队列元素类型：camera_fb_t *（帧缓冲区指针）
 * 队列长度：2
 */
static QueueHandle_t xQueueAIFrame = NULL;

/**
 * @var s_login_mode
 * @brief 是否处于登入界面
 *
 * true 时：摄像头采集、按键检测、人脸识别、预览刷新全部暂停，
 * 界面由 lvgl_ui_show_login() 切换到整屏登入页。
 * 例外：管理员配置界面（管理员页显示中）和录入模式（s_enroll_mode）
 * 仍需采集与推理，因此这两处判断都放行。
 */
static volatile bool s_login_mode = false;

/**
 * @var s_enroll_mode
 * @brief 是否处于人脸录入模式
 *
 * 由管理员界面“录入”按钮进入：界面切换为纯摄像头画面（隐藏所有按钮），
 * 主循环按一次长按事件执行注册，注册结束后自动退回管理员界面。
 * 处于该模式时摄像头与 AI 各任务保持运行，不受 s_login_mode 暂停影响。
 */
static volatile bool s_enroll_mode = false;

/**
 * @var s_enroll_countdown
 * @brief 录入倒计时是否进行中
 *
 * 由“录入”按钮置位；AI 任务在倒计时期间只把帧透传给 LCD 做实时预览、
 * 不跑检测与识别，3 秒后才投递长按事件执行注册，给用户时间对准摄像头。
 */
static volatile bool s_enroll_countdown = false;

/**
 * @var s_enroll_start_tick
 * @brief 录入倒计时起点（Tick），用于按时间而非帧数计算剩余秒数
 */
static TickType_t s_enroll_start_tick = 0;

/**
 * @var s_capture_paused
 * @brief 是否临时暂停摄像头采集
 *
 * 写 Flash（注册/清空人脸）期间必须置 true：
 * 摄像头 DMA 缓冲位于 PSRAM，PSRAM 与 SPI Flash 共用 MSPI，写 Flash 时
 * 缓存关闭会让 DMA 取数超时，出现 cam_hal: EV-EOF-OVF / FB-SIZE 不匹配。
 */
static volatile bool s_capture_paused = false;

/**
 * @var s_camera_stream_enabled
 * @brief 摄像头是否允许开始采集（开机默认关闭，Wi-Fi 初始化完成后再打开）
 */
static volatile bool s_camera_stream_enabled = false;

/**
 * @var s_ai_boot_sem
 * @brief AI 任务启动闸门
 *
 * main_task 创建完所有任务后给出，AI 任务据此才开始分配模型内存，
 * 避免与任务栈分配争抢内部 RAM（详见 task_process_ai 开头说明）
 */
static SemaphoreHandle_t s_ai_boot_sem = NULL;

/**
 * @var s_ai_ready_sem
 * @brief AI 模型创建完成信号
 *
 * AI 任务创建完检测器/识别器并恢复人脸库后给出该信号，
 * 用于释放“模型下沉 PSRAM”时临时占用的内部 RAM。
 */
static SemaphoreHandle_t s_ai_ready_sem = NULL;

/**
 * @def MODEL_PSRAM_BLOCK_CHUNK
 * @brief 模型下沉占位内存的单块大小（分块申请可避开内部 RAM 碎片）
 */
#define MODEL_PSRAM_BLOCK_CHUNK (8 * 1024)

/**
 * @def MODEL_PSRAM_BLOCK_MAX_CHUNKS
 * @brief 最多占用的块数（4 块 = 32KB）
 */
#define MODEL_PSRAM_BLOCK_MAX_CHUNKS 4

/**
 * @var s_model_psram_blocks
 * @brief 模型下沉的占位内存块（创建模型时占用，模型就绪后释放）
 */
static void *s_model_psram_blocks[MODEL_PSRAM_BLOCK_MAX_CHUNKS] = {};

/**
 * @var s_model_psram_block_count
 * @brief 实际成功占用的块数
 */
static int s_model_psram_block_count = 0;

/**
 * @var recognizer
 * @brief 人脸识别器实例指针
 *
 * 使用FaceRecognition112V1S16类，基于S16量化模型
 * 负责人脸特征提取、注册、识别、删除等操作
 */
static FaceRecognition112V1S16* recognizer = NULL;    // 人脸识别器实例指针（静态全局，仅本文件内部访问）

/* 人脸识别相似度阈值（百分比，50~95）：由管理员界面调节，写入 NVS 掉电保存。
 * 识别器由 AI 任务创建，UI 任务只读写这个标量 + 调用 set_thresh */
static volatile int s_face_thresh_percent = 80;

/**
 * @var s_self_learn_enabled
 * @brief “模板自学习”开关：1 开启（识别成功后拟合特征到模板），0 关闭
 *
 * 由管理员界面“系统设置”调节，写入 NVS 掉电保存；读写都是单个 int，可跨任务访问
 */
static volatile int s_self_learn_enabled = 1;

/**
 * @def FACE_FUSION_MAX
 * @brief 每个模板最多学习的次数（达到后仍按固定小权重更新）
 */
#define FACE_FUSION_MAX 10

/**
 * @def FACE_LEARN_MIN_SIM
 * @brief 参与模板自学习的最低相似度：擦边通过的样本不学习，避免模板漂移
 */
#define FACE_LEARN_MIN_SIM 0.90f

/**
 * @def FACE_ENROLL_MAX
 * @brief 允许录入的人脸上限；达到后“录入人脸”会给出提示并拒绝继续录入
 */
#define FACE_ENROLL_MAX 10

#define FACE_CFG_NVS_NS "face_cfg"     // NVS 命名空间
#define FACE_CFG_NVS_THRESH "thresh"   // NVS 键：相似度百分比
#define FACE_CFG_NVS_LEARN "learn"     // NVS 键：模板自学习开关

/* 前向声明：实现在文件末尾的持久化区，供启动初始化与注册/删除流程调用 */
static bool face_db_save(void);
static int face_db_load(void);

/**
 * @var s_face_db_mutex
 * @brief 人脸特征库互斥锁
 *
 * 识别器的人脸列表由 AI 任务修改（注册/清空），人脸信息库界面会从 LVGL
 * 任务中读取同一份列表，用该锁避免并发读写导致崩溃
 */
static SemaphoreHandle_t s_face_db_mutex = NULL;

/**
 * @def FACE_AVATAR_DIR
 * @brief 人脸头像文件目录（挂载在 vfs(FAT) 分区上）
 */
#define FACE_AVATAR_DIR "/vfs/avatars"

/**
 * @def FACE_DB_FILE
 * @brief 人脸特征库文件（与头像同一文件系统）
 *
 * esp-dl 自带的 Flash 读写依赖非扇区对齐的擦除，IDF 5.x 会直接拒绝，
 * 因此人脸特征改由本模块自己序列化到该文件。
 */
#define FACE_DB_FILE "/vfs/face_db.bin"

/**
 * @def FACE_DB_MAGIC
 * @brief 文件魔数 'FDB1'，用于识别有效的人脸库文件
 */
#define FACE_DB_MAGIC 0x46444231u

/**
 * @def FACE_DB_VERSION
 * @brief 文件格式版本，格式变更时递增
 *
 * v1：int32 id + char name[32] + float emb[dim]
 * v2：在 name 之后增加 1 字节“模板自学习次数”，其余不变（v1 文件仍可读取）
 */
#define FACE_DB_VERSION 2u
#define FACE_DB_VERSION_MIN 1u

/**
 * @def FACE_DB_NAME_LEN
 * @brief 姓名占用的字节数（含结束符）
 */
#define FACE_DB_NAME_LEN 32

/**
 * @def FACE_DB_MAX_DIM
 * @brief 允许的最大特征维度，防止损坏文件申请过大缓冲
 */
#define FACE_DB_MAX_DIM 1024

/**
 * @def FACE_DB_MAX_COUNT
 * @brief 允许的最大记录条数，防止损坏文件申请过大缓冲
 */
#define FACE_DB_MAX_COUNT 2000

/**
 * @var s_avatar_fs_ready
 * @brief 头像文件系统是否挂载成功
 */
static bool s_avatar_fs_ready = false;

/**
 * @var s_avatar_save_buf
 * @brief 录入时裁剪缩放的临时缓冲（PSRAM，FACE_AVATAR_PIXELS 个 RGB565）
 */
static uint16_t* s_avatar_save_buf = NULL;

/* 前向声明：实现在文件后部的“人脸头像读写”小节 */
static void avatar_fs_mount(void);
static bool avatar_save(int id, const uint16_t* src, int src_w, int src_h,
                        const std::vector<int>& box);
static void avatar_delete(int id);

/* 前向声明：实现在文件后部的“识别历史记录”小节 */
static void history_init(void);
static void history_add(const char* name, float similarity, const uint16_t* src,
                        int src_w, int src_h, const std::vector<int>& box);

/* 前向声明：实现在文件后部的“人脸模板自学习”小节 */
static bool face_template_learn(int id, const char* name, float similarity);

/**
 * @var s_fusion_counts
 * @brief 每个已录入人脸（按姓名）已完成的模板自学习次数，0~FACE_FUSION_MAX
 *
 * 次数随 face_db.bin 一起持久化（v2 格式）；姓名重复属于异常情况，
 * 新注册同名时会重新从 0 开始计数
 */
static std::map<std::string, uint8_t> s_fusion_counts;

/**
 * @var s_learn_buf
 * @brief 模板拟合临时缓冲（PSRAM，首次自学习时按 FACE_DB_MAX_DIM 分配）
 */
static float* s_learn_buf = NULL;

/**
 * @var s_db_op_done
 * @brief 人脸库变更完成信号（UI 线程投递请求后在此等待）
 */
static SemaphoreHandle_t s_db_op_done = NULL;

/**
 * @var s_db_op_kind
 * @brief 待处理的人脸库变更类型：0=无，1=删除单人，2=清空全部
 */
static volatile int s_db_op_kind = 0;

/**
 * @var s_db_op_id
 * @brief 待删除的人脸 ID（kind==1 时有效）
 */
static volatile int s_db_op_id = -1;

/**
 * @var s_db_op_result
 * @brief 最近一次人脸库变更的结果：1=成功，0=失败
 */
static volatile int s_db_op_result = 0;

/* 前向声明：在 AI 任务中执行的人脸库变更 */
static void process_db_request(void);

/**
 * @def INFO_CLEAR_MS
 * @brief 状态横幅自动清除时间（毫秒）
 *
 * 横幅显示后，超过此时间自动清除
 */
#define INFO_CLEAR_MS (3000) // 自动清除时间(3秒)

/**
 * @struct lcd_info_msg_t
 * @brief 识别状态横幅内容
 *
 * 用于在task_process_ai和task_process_lcd之间传递需要显示的状态，
 * 由 LCD 任务经 lvgl_ui_set_status() 渲染成预览画面下方的彩色横幅：
 * 第一行是大号标题，第二行是细节文字。
 */
typedef struct
{
    char title[40];        /**< 横幅标题（如“识别成功”） */
    char detail[80];       /**< 细节文字（如“Person_0 92%”），可为空 */
    uint8_t kind;          /**< 状态类别（ui_status_t，决定横幅配色） */
    TickType_t timestamp;  /**< 设置时的时间戳（用于超时清除） */
    bool active;           /**< 是否有效显示标志 */
} lcd_info_msg_t;

/**
 * @var g_lcd_info
 * @brief 全局状态横幅内容
 *
 * 在AI任务中设置，在LCD显示任务中读取显示
 * 通过互斥锁保护并发访问
 */
static lcd_info_msg_t g_lcd_info = {{0}, {0}, UI_STATUS_INFO, 0, false};

/**
 * @var lcd_info_mutex
 * @brief 状态横幅互斥锁
 *
 * 保护g_lcd_info的并发访问
 * 在task_process_ai（写）和task_process_lcd（读/写）之间提供同步
 */
static SemaphoreHandle_t lcd_info_mutex = NULL;

/**
 * @brief 设置识别状态横幅（线程安全）
 *
 * 将状态类别、标题和格式化后的细节文字写入全局结构体并激活显示。
 * 由task_process_ai在按键事件或识别结果处理时调用，
 * LCD显示任务会读取并渲染到预览画面下方的彩色横幅上。
 *
 * @param kind       状态类别（绿=成功、红=失败、橙黄=提示、深灰蓝=一般信息）
 * @param title      横幅标题（UTF-8，不以 NULL 结尾时自动截断）
 * @param detail_fmt 细节文字的格式化字符串，可为 NULL（无细节文字）
 * @param ...        可变参数（与 detail_fmt 对应）
 *
 * @note 标题最大 39 字节、细节最大 79 字节，超出部分将被截断
 * @note 此函数会更新时间戳，重置 3 秒清除计时
 */
static void set_lcd_status(int kind, const char* title, const char* detail_fmt, ...)
{
    if (!lcd_info_mutex)
    {
        return;
    }

    va_list args;
    char detail[80];

    detail[0] = '\0';
    if (detail_fmt != NULL)
    {
        va_start(args, detail_fmt);
        vsnprintf(detail, sizeof(detail), detail_fmt, args);
        va_end(args);
    }

    if (xSemaphoreTake(lcd_info_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (title != NULL)
        {
            strncpy(g_lcd_info.title, title, sizeof(g_lcd_info.title) - 1);
        }
        else
        {
            g_lcd_info.title[0] = '\0';
        }
        g_lcd_info.title[sizeof(g_lcd_info.title) - 1] = '\0';

        strncpy(g_lcd_info.detail, detail, sizeof(g_lcd_info.detail) - 1);
        g_lcd_info.detail[sizeof(g_lcd_info.detail) - 1] = '\0';

        g_lcd_info.kind = (uint8_t)kind;
        g_lcd_info.timestamp = xTaskGetTickCount();
        g_lcd_info.active = true;

        xSemaphoreGive(lcd_info_mutex);
    }
}

// ======================== “登入”按钮回调 ========================

/**
 * @brief “登入”按钮按下回调（由 LVGL 触摸输入触发）
 *
 * 按钮的创建、触摸判定与按下态描边全部由 lvgl_ui 模块负责，
 * 本模块只处理业务状态切换：进入登入界面后暂停摄像头采集、
 * 按键控制与预览刷新。
 */
static void on_login_button_pressed(void)
{
    s_login_mode = true;
    ESP_LOGI(TAG, "进入登入界面：摄像头与按键控制已暂停");
}

/**
 * @brief 登入页“退出”按钮回调
 *
 * 退出后恢复摄像头采集、按键控制与预览刷新；界面由 lvgl_ui 自动切回预览页。
 */
static void on_login_exit(void)
{
    s_login_mode = false;
    ESP_LOGI(TAG, "退出登入界面：恢复摄像头与按键控制");
}

// ======================== 按键相关定义 ========================

/**
 * @def BUTTON_GPIO
 * @brief 按键GPIO引脚定义
 *
 * 主按键连接到 GPIO11。
 * 按下时电平为低（0），释放时电平为高（1）
 */
#define BUTTON_GPIO GPIO_NUM_11

/**
 * @def BUTTON_GPIO_ALT
 * @brief 备用按键GPIO引脚定义
 *
 * 部分板子的 SW3 实际接在 GPIO0。
 * 这里同时监听两个引脚，任一为低即视为按下，避免因板子接线不同而按键失效。
 */
#define BUTTON_GPIO_ALT GPIO_NUM_0

/**
 * @brief 读取按键原始状态（未消抖）
 * @return true 表示任一按键引脚被按下（电平为低）
 */
static bool button_raw_pressed(void)
{
    return (gpio_get_level(BUTTON_GPIO) == 0) || (gpio_get_level(BUTTON_GPIO_ALT) == 0);
}

/**
 * @enum button_event_t
 * @brief 按键事件类型枚举
 *
 * 定义支持的按键操作事件类型
 * 由button_task检测后设置，由task_process_ai消费处理
 */
typedef enum
{
    BUTTON_EVENT_NONE = 0,     /**< 无按键事件，默认状态 */
    BUTTON_EVENT_SINGLE_CLICK, /**< 单击事件：识别人脸 */
    BUTTON_EVENT_DOUBLE_CLICK, /**< 双击事件：清空所有人脸数据 */
    BUTTON_EVENT_LONG_PRESS,   /**< 长按事件：注册人脸 */
} button_event_t;

/**
 * @enum button_state_t
 * @brief 按键状态机状态定义
 *
 * 按键检测使用有限状态机实现，包含4个状态
 * 确保准确的单击、双击和长按识别
 */
typedef enum
{
    BUTTON_STATE_IDLE,         /**< 空闲状态：等待按键按下 */
    BUTTON_STATE_PRESSED,      /**< 按下状态：检测释放或长按 */
    BUTTON_STATE_WAIT_RELEASE, /**< 等待释放状态：长按后等待松开 */
    BUTTON_STATE_WAIT_DOUBLE,  /**< 等待双击状态：检测第二次点击 */
} button_state_t;

/**
 * @var g_button_event
 * @brief 全局按键事件标志
 *
 * 使用volatile修饰，确保在中断上下文和任务间访问的一致性
 * 通过互斥锁保护，实现线程安全的读写操作
 */
static volatile button_event_t g_button_event = BUTTON_EVENT_NONE;

/**
 * @var button_mutex
 * @brief 按键事件互斥锁
 *
 * 保护g_button_event的并发访问
 * 在button_task（写）和task_process_ai（读/写）之间提供同步
 */
static SemaphoreHandle_t button_mutex = NULL;

// ======================== 函数前向声明 ========================
static button_event_t get_button_event(void);
static button_event_t peek_button_event(void);

// =================================================================
// 按键事件管理函数
// =================================================================

/**
 * @brief 获取当前按键事件（消费模式）
 *
 * 线程安全地获取当前检测到的按键事件，并立即清除事件标志。
 * 调用此函数后，按键事件标志被重置为BUTTON_EVENT_NONE，
 * 防止同一事件被重复处理。
 *
 * @return button_event_t 返回检测到的按键事件类型
 *         - BUTTON_EVENT_NONE: 无按键事件
 *         - BUTTON_EVENT_SINGLE_CLICK: 单击事件
 *         - BUTTON_EVENT_DOUBLE_CLICK: 双击事件
 *         - BUTTON_EVENT_LONG_PRESS: 长按事件
 *
 * @note 如果互斥锁获取失败（极端情况），返回BUTTON_EVENT_NONE
 * @warning 此函数会清除事件标志，每次事件只会被消费一次
 */
static button_event_t get_button_event(void)
{
    // 初始化返回值为无事件
    button_event_t event = BUTTON_EVENT_NONE;

    /**
     * 获取互斥锁，确保对共享变量g_button_event的原子访问
     * portMAX_DELAY表示无限等待，直到获取到互斥锁
     */
    if (xSemaphoreTake(button_mutex, portMAX_DELAY) == pdTRUE)
    {
        // 读取当前按键事件值
        event = g_button_event;
        // 清除事件标志，防止重复处理
        g_button_event = BUTTON_EVENT_NONE;
        // 释放互斥锁
        xSemaphoreGive(button_mutex);
    }

    // 返回获取到的按键事件类型
    return event;
}

/**
 * @brief 查看当前按键事件（非消费模式）
 *
 * 线程安全地查看当前检测到的按键事件，但不清除事件标志。
 * 用于在不消费事件的情况下检查是否有按键事件发生，
 * 常用于非阻塞的事件轮询场景。
 *
 * @return button_event_t 返回当前检测到的按键事件类型
 *         - BUTTON_EVENT_NONE: 无按键事件
 *         - BUTTON_EVENT_SINGLE_CLICK: 单击事件
 *         - BUTTON_EVENT_DOUBLE_CLICK: 双击事件
 *         - BUTTON_EVENT_LONG_PRESS: 长按事件
 *
 * @note 此函数不会清除事件标志，事件仍然可以被后续的get_button_event()消费
 * @note 如果互斥锁获取失败，返回BUTTON_EVENT_NONE
 */
static button_event_t peek_button_event(void)
{
    // 初始化返回值为无事件
    button_event_t event = BUTTON_EVENT_NONE;

    // 获取互斥锁，读取当前按键事件值但不清除
    if (xSemaphoreTake(button_mutex, portMAX_DELAY) == pdTRUE)
    {
        // 仅读取事件值，不清除标志
        event = g_button_event;
        // 释放互斥锁
        xSemaphoreGive(button_mutex);
    }

    // 返回当前按键事件类型
    return event;
}

/**
 * @brief 设置按键事件
 *
 * 线程安全地设置按键事件标志。
 * 由button_task按键检测任务调用，将检测到的事件类型
 * 写入全局变量，供task_process_ai处理。
 *
 * @param event 要设置的按键事件类型
 *         - BUTTON_EVENT_SINGLE_CLICK: 单击事件
 *         - BUTTON_EVENT_DOUBLE_CLICK: 双击事件
 *         - BUTTON_EVENT_LONG_PRESS: 长按事件
 * @return 无返回值
 *
 * @note 此函数会覆盖之前未处理的事件
 * @note 如果互斥锁获取失败，事件设置失败（不会阻塞）
 */
static void set_button_event(button_event_t event)
{
    // 获取互斥锁，保护对g_button_event的写入操作
    if (xSemaphoreTake(button_mutex, portMAX_DELAY) == pdTRUE)
    {
        // 设置按键事件值
        g_button_event = event;
        // 释放互斥锁
        xSemaphoreGive(button_mutex);
    }
    /**
     * 注意：如果互斥锁获取失败，事件将不会被设置。
     * 在正常系统负载下，portMAX_DELAY等待不应超时。
     */
}

// =================================================================
// 屏幕按钮回调（由 LVGL 触摸输入触发）
// =================================================================

/**
 * @brief “识别”按钮按下回调：等价于 KEY2 单击
 *
 * 只往按键事件队列里塞一个单击事件，识别逻辑仍在 AI 任务里执行，
 * 与物理按键走完全相同的一条路径（含人脸未检测到的提示）。
 */
static void on_recognize_button_pressed(void)
{
    set_button_event(BUTTON_EVENT_SINGLE_CLICK);
}

/**
 * @brief 退出录入模式：收起纯摄像头画面，回到管理员配置界面
 */
static void exit_enroll_mode(void)
{
    if (!s_enroll_mode && !s_enroll_countdown)
    {
        return;
    }

    s_enroll_mode = false;
    s_enroll_countdown = false;
    lvgl_ui_show_admin();
    ESP_LOGI(TAG, "录入结束，返回管理员界面");
}

/**
 * @brief “录入”按钮按下回调
 *
 * 流程：切到纯摄像头画面 -> 启动 3 秒倒计时（期间实时预览、不跑识别），
 * 倒计时结束后由 AI 任务用实时帧执行注册，注册结束自动回到管理员界面。
 */
static void on_enroll_button_pressed(void)
{
    int total = 0;
    int ids[1];
    char names[1][32];

    /* 录入人数上限：达到上限时不再进入录入流程，直接在管理员页提示 */
    human_face_list_enrolled(ids, names, 1, &total);
    if (total >= FACE_ENROLL_MAX)
    {
        ESP_LOGW(TAG, "录入被拒绝：已录入 %d 人，达到上限 %d", total, FACE_ENROLL_MAX);
        lvgl_ui_admin_toast("已达10人上限，请先删除");
        audio_prompt_play(AUDIO_PROMPT_INPUT_ERROR);
        return;
    }

    s_enroll_mode = true;
    s_enroll_countdown = true;
    s_enroll_start_tick = xTaskGetTickCount();
    lvgl_ui_show_camera_only();
    /* 倒计时提示由 AI 任务在下一帧自动显示（此处运行在 LVGL 任务中，
       不直接操作 lcd_info，避免与 LCD 任务的锁顺序冲突） */
    ESP_LOGI(TAG, "进入录入模式，3 秒后开始录入");
}

// =================================================================
// 按键检测任务（button_task）
// =================================================================

/**
 * @brief 按键检测任务
 *
 * 该任务负责检测按键的状态，通过四状态有限状态机
 * 实现单击、双击和长按的准确识别。
 * 使用软件消抖机制（20ms延时）确保按键检测的可靠性。
 *
 * 状态机说明：
 *   BUTTON_STATE_IDLE         → 按下 → BUTTON_STATE_PRESSED
 *   BUTTON_STATE_PRESSED      → 释放 → BUTTON_STATE_WAIT_DOUBLE
 *   BUTTON_STATE_PRESSED      → 长按超时 → 触发长按 → WAIT_RELEASE
 *   BUTTON_STATE_WAIT_DOUBLE  → 再次按下 → BUTTON_STATE_PRESSED
 *   BUTTON_STATE_WAIT_DOUBLE  → 超时 → 判定单击/双击 → IDLE
 *   BUTTON_STATE_WAIT_RELEASE → 释放 → BUTTON_STATE_IDLE
 *
 * @param arg 任务参数指针（本实现中未使用，传入NULL）
 * @return 无返回值（任务循环运行，永不返回）
 *
 * @note 任务栈大小：3KB（2KB 时在 ESP_LOGI 调用 vsnprintf 处栈溢出）
 * @note 优先级：6（高于 AI 任务，保证采样周期稳定）
 * @note 运行核心：0（应用核心）
 * @note 轮询周期：10ms（vTaskDelay）
 * @note 消抖方式：计时消抖，电平需持续 40ms 不变才生效
 * @note 长按阈值：1000ms
 * @note 双击间隔：300ms
 */
static void button_task(void* arg)
{
    // ---- 初始化按键GPIO配置 ----

    /**
     * 创建GPIO配置结构体并初始化为零
     * gpio_config_t包含中断类型、模式、引脚掩码、上拉/下拉配置
     */
    gpio_config_t io_conf = {};

    // 禁用中断，采用轮询方式读取按键状态
    io_conf.intr_type = GPIO_INTR_DISABLE;

    // 将GPIO设置为输入模式
    io_conf.mode = GPIO_MODE_INPUT;

    /**
     * 设置引脚位掩码
     * 同时选中主按键引脚与备用按键引脚，两者都是输入
     */
    io_conf.pin_bit_mask = (1ULL << BUTTON_GPIO) | (1ULL << BUTTON_GPIO_ALT);

    // 使能内部上拉电阻，确保按键未按下时引脚为高电平
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;

    // 禁用下拉电阻
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;

    // 将配置应用到GPIO控制器
    gpio_config(&io_conf);

    // ---- 状态机变量初始化 ----

    // 按键状态机初始状态：空闲等待
    button_state_t state = BUTTON_STATE_IDLE;

    // 记录按键按下的系统Tick计数
    TickType_t press_time = 0;

    // 记录按键释放的系统Tick计数
    TickType_t release_time = 0;

    // 点击次数计数器（用于区分单击和双击）
    int click_count = 0;

    // ---- 按键检测参数定义 ----

    /**
     * 采样周期：10ms
     * 每 10ms 读取一次按键电平，用于连续采样消抖
     */
    const TickType_t SAMPLE_PERIOD = pdMS_TO_TICKS(10);

    /**
     * 消抖确认时间：40ms
     * 电平与当前稳定状态不一致后，必须持续 40ms 仍未回弹才被认可。
     * 采用计时而非“连续 N 次采样”计数：任务被高优先级任务抢占时采样间隔
     * 会变长（见过 60~100ms 一次），按次数计算会把实际消抖时间放大数倍，
     * 导致长按阈值被推迟，1 秒的长按被误判成单击
     */
    const TickType_t DEBOUNCE_TIME = pdMS_TO_TICKS(40);

    /**
     * 长按判定阈值：1000ms（1秒）
     * 按键持续按下超过此时间则触发长按事件
     */
    const TickType_t LONG_PRESS_TIME = pdMS_TO_TICKS(1000);

    /**
     * 双击判定间隔：300ms
     * 两次按键释放之间的间隔小于此值判定为双击
     * 超过此值则判定为单击
     */
    const TickType_t DOUBLE_CLICK_TIME = pdMS_TO_TICKS(300);

    // 消抖状态：stable_level 为已确认的稳定电平；pending 表示正在确认一次电平变化
    int stable_level = button_raw_pressed() ? 0 : 1;
    bool level_change_pending = false;
    TickType_t level_change_tick = 0;

    // 打印按键检测任务启动信息和操作说明
    ESP_LOGI(TAG, "按键检测任务已启动，等待按键操作...");
    ESP_LOGI(TAG, "[DBG] 按键引脚 GPIO%d 电平=%d, GPIO%d 电平=%d（未按下应为1）",
             (int)BUTTON_GPIO, gpio_get_level(BUTTON_GPIO),
             (int)BUTTON_GPIO_ALT, gpio_get_level(BUTTON_GPIO_ALT));
    ESP_LOGI(TAG, "操作说明：");
    ESP_LOGI(TAG, "  - 长按(>1秒)：保存人脸数据到分区表");
    ESP_LOGI(TAG, "  - 单击：识别人脸并显示结果");
    ESP_LOGI(TAG, "  - 双击：清空所有人脸数据");

    // ---- 任务主循环 ----
    while (true)
    {
        /**
         * 登入界面期间暂停按键控制（管理员界面/录入模式需要按键，因此放行）：
         * 状态机复位到空闲，且不再消费任何按键事件
         */
        if ((s_login_mode && !lvgl_ui_admin_active() && !s_enroll_mode) ||
            lvgl_ui_standby_active())
        {
            state = BUTTON_STATE_IDLE;
            click_count = 0;
            level_change_pending = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /**
         * 计时消抖
         * 原始电平与稳定电平不一致后开始计时，只要 40ms 内回弹就作废；
         * 持续 40ms 仍一致才认可这次电平变化
         */
        int raw_level = button_raw_pressed() ? 0 : 1;
        if (raw_level == stable_level)
        {
            level_change_pending = false;
        }
        else if (!level_change_pending)
        {
            level_change_pending = true;
            level_change_tick = xTaskGetTickCount();
        }
        else if ((xTaskGetTickCount() - level_change_tick) >= DEBOUNCE_TIME)
        {
            stable_level = raw_level;
            level_change_pending = false;
        }

        int level = stable_level;

        // 按键状态机处理（level 已经是消抖后的稳定电平）
        switch (state)
        {

            // ==================== 状态1：空闲状态 ====================
            case BUTTON_STATE_IDLE:
                // 检测到按键按下（电平为低）
                if (level == 0)
                {
                    // 切换到按下状态，记录按下时间
                    state = BUTTON_STATE_PRESSED;
                    press_time = xTaskGetTickCount();
                    ESP_LOGI(TAG, "检测到按键按下（GPIO%d=%d, GPIO%d=%d）",
                             (int)BUTTON_GPIO, gpio_get_level(BUTTON_GPIO),
                             (int)BUTTON_GPIO_ALT, gpio_get_level(BUTTON_GPIO_ALT));
                }
                break;

            // ==================== 状态2：按下状态 ====================
            case BUTTON_STATE_PRESSED:
                // 检测到按键释放（电平为高）
                if (level == 1)
                {
                    // 记录释放时间，用于后续双击间隔计算
                    release_time = xTaskGetTickCount();
                    // 点击次数加1
                    click_count++;
                    // 进入等待双击状态，检测是否有第二次点击
                    state = BUTTON_STATE_WAIT_DOUBLE;
                }
                else if ((xTaskGetTickCount() - press_time) > LONG_PRESS_TIME)
                {
                    /**
                     * 按键持续按下超过长按阈值（1秒）
                     * 判定为长按事件，立即触发
                     */
                    set_button_event(BUTTON_EVENT_LONG_PRESS);
                    ESP_LOGI(TAG, "检测到长按事件");
                    // 进入等待释放状态，等待按键松开
                    state = BUTTON_STATE_WAIT_RELEASE;
                }
                break;

            // ==================== 状态3：等待释放状态 ====================
            case BUTTON_STATE_WAIT_RELEASE:
                // 检测到按键释放（电平为高）
                if (level == 1)
                {
                    // 回到空闲状态，等待下一次按键
                    state = BUTTON_STATE_IDLE;
                }
                break;

            // ==================== 状态4：等待双击状态 ====================
            case BUTTON_STATE_WAIT_DOUBLE:
                // 检测到第二次按键按下（即将产生双击）
                if (level == 0)
                {
                    // 点击次数加1（此时click_count为2）
                    click_count++;
                    // 回到按下状态，等待第二次释放
                    state = BUTTON_STATE_PRESSED;
                    // 更新按下时间
                    press_time = xTaskGetTickCount();
                }
                else if ((xTaskGetTickCount() - release_time) > DOUBLE_CLICK_TIME)
                {
                    /**
                     * 超过双击判定间隔（300ms），未检测到第二次点击
                     * 根据click_count确定事件类型：
                     *   - click_count == 1：单击事件
                     *   - click_count >= 2：双击事件
                     */
                    if (click_count == 1)
                    {
                        // 触发单击事件
                        set_button_event(BUTTON_EVENT_SINGLE_CLICK);
                        ESP_LOGI(TAG, "检测到单击事件");
                    }
                    else if (click_count >= 2)
                    {
                        // 触发双击事件
                        set_button_event(BUTTON_EVENT_DOUBLE_CLICK);
                        ESP_LOGI(TAG, "检测到双击事件");
                    }

                    // 重置点击计数器和状态
                    click_count = 0;
                    state = BUTTON_STATE_IDLE;
                }
                break;

            // 默认情况（不应发生）
            default:
                break;
        }

        /**
         * 按采样周期延时，控制轮询频率
         * 同时为其他任务提供调度机会
         */
        vTaskDelay(SAMPLE_PERIOD);
    }
}

// =================================================================
// 结果显示函数
// =================================================================

/**
 * @brief 在LCD上显示人脸识别结果（通过串口日志）
 *
 * 根据识别结果（姓名和相似度）在控制台输出相应的日志信息。
 * 如果识别成功，显示姓名和相似度分数；
 * 如果识别失败，提示未匹配到已知人脸。
 * 该版本的实现通过ESP_LOG输出到串口，未使用LCD图形绘制。
 *
 * @param name       识别到的人脸姓名（C字符串）
 * @param similarity 相似度分数（范围0.0~1.0，越接近1.0越匹配）
 * @param x          LCD显示位置的X坐标（当前版本未使用）
 * @param y          LCD显示位置的Y坐标（当前版本未使用）
 * @return 无返回值
 *
 * @note 当前版本仅通过串口输出结果，LCD显示仅用于摄像头预览
 * @note name参数可以为NULL或空字符串，此时显示"识别失败"
 */
static void display_recognition_result(const char* name, float similarity, int x, int y)
{
    // 检查name是否有效且非空
    if (name && strlen(name) > 0)
    {
        // 识别成功：打印姓名和相似度
        ESP_LOGI(TAG, "识别成功！姓名: %s, 相似度: %.2f", name, similarity);
    }
    else
    {
        // 识别失败：未匹配到任何人脸
        ESP_LOGI(TAG, "识别失败！未匹配到已知人脸");
    }
}

// =================================================================
// 开门/锁门控制（LED：IO10 低电平点亮，亮 = 开门）
// =================================================================

/** 开门后 3 秒自动锁门的单次定时器（在 app_human_face_recognition 中创建） */
static esp_timer_handle_t s_door_lock_timer = NULL;

/**
 * @brief 自动锁门定时器回调：熄灭 LED，表示门已重新锁上
 */
static void door_lock_cb(void* arg)
{
    (void)arg;
    led_off();
    ESP_LOGI(TAG, "开门超时，门已重新锁上");
}

/**
 * @brief 识别成功后开门：点亮 LED 并启动 3 秒后自动锁门
 * @note 若门已处于打开状态，则重新计时，保证每次成功识别都能保持 3 秒
 */
static void door_unlock_3s(void)
{
    led_on();
    if (s_door_lock_timer != NULL)
    {
        esp_timer_stop(s_door_lock_timer); /* 已在计时则重新计时，返回错误可忽略 */
        esp_timer_start_once(s_door_lock_timer, 3 * 1000 * 1000);
    }
    ESP_LOGI(TAG, "识别成功，开门 3 秒");
}

// =================================================================
// AI人脸检测与识别任务（task_process_ai）
// =================================================================

/**
 * @brief AI人脸检测与识别主任务
 *
 * 该任务是系统的核心处理单元，负责：
 * 1. 从AI帧队列接收摄像头采集的帧数据
 * 2. 使用两级检测器（MSR01 + MNP01）进行人脸检测
 * 3. 将检测结果绘制到帧数据上
 * 4. 将处理后的帧发送到LCD显示队列
 * 5. 根据按键事件执行人脸注册、识别、清空操作
 *
 * 执行流程：
 *   peek按键事件 → 接收帧 → 人脸检测 → LCD显示 → 处理按键事件
 *                                                     ↓
 *                                           长按: 注册人脸到Flash
 *                                           单击: 识别当前人脸
 *                                           双击: 清空所有人脸
 *
 * @param arg 任务参数指针（本实现中未使用，传入NULL）
 * @return 无返回值（任务循环运行，永不返回）
 *
 * @note 任务栈大小：8KB（需要较大栈空间用于神经网络推理）
 * @note 优先级：5
 * @note 运行核心：0（应用核心）
 * @note 帧队列接收超时：100ms，确保按键事件能被及时响应
 * @note LCD显示优先于Flash写入，避免SPI总线冲突导致黑屏
 */

/**
 * @brief 暂停摄像头采集并清空流水线中的残留帧
 *
 * 用于写 Flash（注册/清空人脸）之前调用：
 *   1. 置 s_capture_paused，采集任务不再调用 esp_camera_fb_get()；
 *   2. 把 AI / LCD 队列里尚未处理的帧全部归还给驱动；
 *   3. 等一个调度周期，让摄像头 DMA 的当前传输结束。
 * 这样后续擦写 Flash 时就不会因为 PSRAM 与 MSPI 冲突而丢帧。
 *
 * @note 调用者自己持有的帧不受影响，仍可继续用于注册/识别推理。
 */
static void capture_pause_and_drain(void)
{
    s_capture_paused = true;

    // 给采集任务一个调度周期，让它从 esp_camera_fb_get() 返回并进入暂停分支
    vTaskDelay(pdMS_TO_TICKS(50));

    camera_fb_t* f = NULL;
    if (xQueueAIFrame)
    {
        while (xQueueReceive(xQueueAIFrame, &f, 0) == pdTRUE)
        {
            esp_camera_fb_return(f);
        }
    }
    if (xQueueLCDFrame)
    {
        while (xQueueReceive(xQueueLCDFrame, &f, 0) == pdTRUE)
        {
            esp_camera_fb_return(f);
        }
    }

    // 再等一会儿，确保 DMA 与 LCD 刷屏都把总线释放干净
    vTaskDelay(pdMS_TO_TICKS(100));
}

/**
 * @brief 恢复摄像头采集
 */
static void capture_resume(void)
{
    s_capture_paused = false;
}

/**
 * @brief 暂停摄像头采集（供 Wi-Fi 初始化等会读写 Flash/NVS 的模块调用）
 *
 * 与内部写 Flash 前的处理一致：停止采集、清空流水线中的残留帧，
 * 避免 PSRAM 上的摄像头 DMA 在 MSPI 被占时溢出（cam_hal: EV-EOF-OVF）。
 */
extern "C" void human_face_capture_pause(void)
{
    capture_pause_and_drain();
}

/**
 * @brief 恢复摄像头采集（与 human_face_capture_pause 成对使用）
 */
extern "C" void human_face_capture_resume(void)
{
    capture_resume();
}

/**
 * @brief 允许摄像头开始采集（开机时默认关闭，Wi-Fi 初始化完成后再调用）
 */
extern "C" void human_face_camera_start(void)
{
    s_camera_stream_enabled = true;
    ESP_LOGI(TAG, "摄像头采集已开启");
}

// =================================================================
// AI人脸检测与识别任务（task_process_ai）
// =================================================================

static void task_process_ai(void* arg)
{
    /**
     * 等待启动闸门
     * AI 任务优先级高于 main_task，创建后会立刻抢占 CPU 并开始分配模型内存
     * （检测器/识别器共约 28KB 内部 RAM）。如果同时 main_task 还在创建
     * 摄像头/LCD/按键任务，就会互相抢内存，导致后面的任务栈申请不到连续块
     * 而创建失败。这里先等 main_task 把所有任务建完再开始分配。
     */
    if (s_ai_boot_sem != NULL)
    {
        xSemaphoreTake(s_ai_boot_sem, portMAX_DELAY);
    }

    // ---- 局部变量声明 ----

    /**
     * 帧缓冲区指针
     * 从xQueueAIFrame接收摄像头帧数据
     * 初始为NULL，接收到有效帧后指向帧缓冲区
     */
    camera_fb_t* frame = NULL;

    // ---- AI模型初始化 ----

    /**
     * 创建MSR01人脸检测器实例（第一级：粗检测）
     * 功能：快速扫描全图，生成人脸候选区域
     * 参数说明：
     *   - 0.3F：检测分数阈值（低于此分数的候选被丢弃）
     *   - 0.3F：NMS的IOU阈值（重叠超过此值的候选被合并）
     *   - 10：最大候选框数量
     *   - 0.3F：分数阈值（最终过滤用）
     */
    ESP_LOGI(TAG, "[BOOT] AI task running, creating detectors...");
    HumanFaceDetectMSR01 detector(0.3F, 0.3F, 10, 0.3F);
    ESP_LOGI(TAG, "[BOOT] MSR01 ready");

    /**
     * 创建MNP01人脸检测器实例（第二级：精检测）
     * 功能：在MSR01的候选区域上进行精确检测，输出精确边界框和关键点
     * 参数说明：
     *   - 0.4F：检测分数阈值（比MSR01更严格）
     *   - 0.3F：NMS的IOU阈值
     *   - 10：最大检测结果数量
     */
    HumanFaceDetectMNP01 detector2(0.4F, 0.3F, 10);
    ESP_LOGI(TAG, "[BOOT] MNP01 ready");

    /**
     * 创建人脸识别器实例
     * FaceRecognition112V1S16：基于S16量化的轻量级人脸识别模型
     * 支持人脸特征提取、注册、识别和删除功能
     */
    recognizer = new FaceRecognition112V1S16();
    ESP_LOGI(TAG, "[BOOT] recognizer created");

    /**
     * 设置人脸识别相似度阈值
     * 默认 0.80（即相似度需 ≥80% 才算同一人），可在管理员界面“系统设置”中调节，
     * 调节结果写入 NVS，重启后保持
     */
    {
        nvs_handle_t handle;
        uint8_t saved = 0;

        if (nvs_open(FACE_CFG_NVS_NS, NVS_READONLY, &handle) == ESP_OK)
        {
            if (nvs_get_u8(handle, FACE_CFG_NVS_THRESH, &saved) == ESP_OK)
            {
                s_face_thresh_percent = (saved < 50) ? 50 : (saved > 95 ? 95 : (int)saved);
            }
            if (nvs_get_u8(handle, FACE_CFG_NVS_LEARN, &saved) == ESP_OK)
            {
                s_self_learn_enabled = (saved != 0) ? 1 : 0;
            }
            nvs_close(handle);
        }
    }
    recognizer->set_thresh((float)s_face_thresh_percent / 100.0f);
    ESP_LOGI(TAG, "人脸相似度阈值：%.2f", recognizer->get_thresh());

    /**
     * 从 vfs(FAT) 分区的人脸库文件恢复特征库
     *
     * 不再调用 recognizer->set_partition()/set_ids_from_flash()：
     * esp-dl v2.0 自带的 Flash 读写依赖“非扇区对齐的擦除”
     * （erase(0, 12+36*N) / erase(emb_base, 4096)），而 IDF 5.x 的
     * esp_partition_erase_range 对非对齐参数直接返回 ESP_ERR_INVALID_SIZE，
     * 库又没有检查返回值 → 头部永远擦不掉，一旦 id_num 被写成 0 就再也改不回来，
     * 重启后 set_ids_from_flash() 恒为 0。因此这里改用自建持久化：
     * 开机从 /vfs/face_db.bin 读回特征，注册/删除后整库重写该文件。
     */
    int enrolled_num = face_db_load();
    if (enrolled_num < 0)
    {
        ESP_LOGI(TAG, "人脸库文件尚无可恢复数据（返回 %d）", enrolled_num);
    }
    else
    {
        ESP_LOGI(TAG, "从文件恢复 %d 个人脸ID", enrolled_num);
    }

    /**
     * 模型创建完成：通知 app_human_face_recognition 释放“模型下沉”占位内存。
     * 此时检测器、识别器及人脸库特征都已分配完毕，模型张量已尽量落到 PSRAM。
     */
    if (s_ai_ready_sem != NULL)
    {
        ESP_LOGI(TAG, "[MEM] 模型创建完成，internal free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        xSemaphoreGive(s_ai_ready_sem);
    }

    /**
     * 用于存储当前帧检测到的人脸关键点坐标
     * 关键点数组包含10个整数（5个关键点 × 2个坐标）
     * 在注册和识别操作时传递给识别器
     */
    std::vector<int> current_landmarks;

    /**
     * 当前帧第一个人脸的外接框 [x0, y0, x1, y1]（帧坐标系），
     * 注册成功后用它从原始帧裁剪出 48×48 头像
     */
    std::vector<int> current_box;

    /**
     * 人脸检测状态标志
     * true：当前帧中检测到人脸
     * false：当前帧中未检测到人脸
     */
    bool face_detected = false;

    // ---- 任务主循环 ----
    while (true)
    {
        /**
         * 人脸信息库的删除/清空请求由 UI 线程投递、在此执行：
         * 写 Flash 必须在 AI 任务里成对做“暂停采集 → 改库 → 写 Flash → 恢复采集”，
         * 避免 UI 线程直接写 Flash 与摄像头 DMA 冲突
         */
        if (s_db_op_kind != 0)
        {
            process_db_request();
            continue;
        }

        /**
         * 登入界面期间暂停人脸识别（管理员界面与录入模式需要取帧，因此继续）：
         * 不消费 AI 队列、不跑模型推理，队列里残留的帧留待返回预览后处理
         */
        if (s_login_mode && !lvgl_ui_admin_active() && !s_enroll_mode)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /**
         * 待机期间停止 AI 推理（10 秒无触摸自动熄屏）：
         * 排空队列中的残留帧并归还驱动，触摸唤醒后自动恢复检测/识别
         */
        if (lvgl_ui_standby_active())
        {
            if (xQueueReceive(xQueueAIFrame, &frame, 0))
            {
                esp_camera_fb_return(frame);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /**
         * 第一步：检查是否有按键事件等待处理（非阻塞查看）
         * 使用peek模式，只查看不清除事件标志
         * 后续在帧到达且处理后，再实际消费事件
         * 这样即使没有帧到达，事件也不会丢失
         */
        button_event_t pending_event = peek_button_event();

        /**
         * 本帧是否用于“录入”：录入帧不绘制检测框与关键点，
         * 保证裁剪出的头像来自干净的原始像素（否则会带上绿框和彩色圆点）
         */
        bool enroll_capture = (pending_event == BUTTON_EVENT_LONG_PRESS) &&
                              (s_enroll_mode || lvgl_ui_admin_active());

        /**
         * 识别帧同样不绘制检测框：识别成功后会从这一帧裁剪历史记录照片，
         * 留档照片应当是干净的人脸画面，不应带绿框与关键点
         */
        bool clean_capture = enroll_capture || (pending_event == BUTTON_EVENT_SINGLE_CLICK);

        // ---- 第二步：处理AI帧数据 ----

        /**
         * 从AI帧队列接收摄像头采集的帧数据
         * 设置100ms超时，避免无限阻塞导致按键响应延迟
         * 接收成功时返回pdTRUE，frame指向接收到的帧缓冲区
         * 超时返回pdFALSE，继续执行后续按键检查
         */
        if (xQueueReceive(xQueueAIFrame, &frame, pdMS_TO_TICKS(100)))
        {

            /**
             * 录入倒计时期间只做“预览透传”：
             * 跳过检测与识别，把帧直接交给 LCD，让用户看着实时画面调整姿势。
             * 按时间计算剩余秒数（不按帧数），倒计时归零后投递一次长按事件，
             * 下一轮循环就会用倒计时结束后的实时帧执行注册。
             */
            if (s_enroll_countdown)
            {
                if (xQueueLCDFrame && xQueueSend(xQueueLCDFrame, &frame, 0) == pdTRUE)
                {
                    frame = NULL; // 所有权交给 LCD 任务
                }
                else
                {
                    esp_camera_fb_return(frame);
                    frame = NULL;
                }

                int remain = 3 - (int)((xTaskGetTickCount() - s_enroll_start_tick) /
                                       pdMS_TO_TICKS(1000));
                if (remain > 0)
                {
                    set_lcd_status(UI_STATUS_INFO, "准备录入", "对准摄像头 %d秒", remain);
                }
                else
                {
                    s_enroll_countdown = false;
                    set_lcd_status(UI_STATUS_INFO, "正在录入", NULL);
                    ESP_LOGI(TAG, "倒计时结束，开始录入");
                    set_button_event(BUTTON_EVENT_LONG_PRESS);
                }

                continue;
            }

            // ----- 第三阶段：人脸检测（两级级联）-----

            /**
             * 第一阶段：使用MSR01检测器进行粗检测
             * 输入参数说明：
             *   - (uint16_t *)frame->buf：RGB565格式的帧数据缓冲区
             *   - {(int)frame->height, (int)frame->width, 3}：图像形状
             *     包含高度、宽度、通道数（RGB为3通道）
             * 输出：人脸候选区域列表，每个结果包含边界框坐标和关键点
             */
            std::list<dl::detect::result_t>& detect_candidates = detector.infer(
                (uint16_t*)frame->buf, {(int)frame->height, (int)frame->width, 3});

            /**
             * 第二阶段：使用MNP01检测器进行精确检测
             * 在MSR01生成的候选区域基础上进一步确认和精确定位
             * 输出更精确的边界框和面部关键点（眼睛、鼻子、嘴角）
             */
            std::list<dl::detect::result_t>& detect_results =
                detector2.infer((uint16_t*)frame->buf,
                                {(int)frame->height, (int)frame->width, 3},
                                detect_candidates);

            // ----- 更新人脸检测状态 -----

            // 检查是否检测到至少一个人脸
            if (detect_results.size() > 0)
            {
                // 设置人脸检测标志为true
                face_detected = true;

                /**
                 * 保存第一个人脸的关键点数据
                 * 用于后续的人脸注册和识别操作
                 * detect_results.front()返回第一个检测结果
                 * keypoint包含10个int：5个关键点的x/y坐标
                 */
                current_landmarks = detect_results.front().keypoint;

                // 缓存人脸框，注册成功后用它裁剪头像
                current_box = detect_results.front().box;

                /**
                 * 在图像上绘制人脸检测结果
                 * 包括：绿色边界框和五个面部关键点
                 * 绘制操作直接在帧缓冲区进行，不影响后续显示
                 * 录入帧跳过绘制，保证头像裁自原始像素
                 */
                if (!clean_capture)
                {
                    draw_detection_result(
                        (uint16_t*)frame->buf, frame->height, frame->width, detect_results);
                }
            }
            else
            {
                // 未检测到人脸，更新标志并清空关键点数据
                face_detected = false;
                current_landmarks.clear();
                current_box.clear();
            }

            // ----- 第四步：帧保留到按键处理结束后再送 LCD -----
            /**
             * 注意：帧不能在按键处理之前交给 LCD 任务。
             * LCD 任务（核心1，优先级5）收到帧后立刻调用 esp_camera_fb_return()
             * 把缓冲还给驱动，而本任务随后还要用 frame->buf 做注册/识别推理，
             * 会读到已被复用的数据（误识别、推理出错甚至崩溃）。
             * 因此这里只保留帧，等下面按键处理完成后再交给 LCD。
             */
        }

        // ---- 第五步：处理按键事件 ----

        /**
         * 独立处理按键事件（无论是否有帧到达）
         * 使用peek模式查看的事件，在此处统一消费处理
         * 确保所有按键事件都被及时响应
         */
        if (pending_event != BUTTON_EVENT_NONE)
        {
            /**
             * 消费按键事件（清除标志，防止重复处理）
             * 注意：必须调用get_button_event()清除事件标志
             * 否则事件会一直挂起，导致下次循环重复触发
             */
            get_button_event();

            // ==================== 双击事件处理 ====================
            /**
             * 双击事件：清空所有人脸数据
             * 此操作不需要摄像头帧数据，任何状态下双击都立即执行
             * 流程：
             *   1. 获取所有已注册的人脸ID列表
             *   2. 逐个删除每个人脸ID（不更新Flash）
             *   3. 最后统一写入Flash更新
             */
            if (pending_event == BUTTON_EVENT_DOUBLE_CLICK)
            {
                ESP_LOGI(TAG, "执行清空所有人脸数据操作...");
                set_lcd_status(UI_STATUS_INFO, "清空人脸库", NULL);

                // 先归还当前帧，否则这一帧会随 continue 一起泄漏
                if (frame != NULL)
                {
                    esp_camera_fb_return(frame);
                    frame = NULL;
                }

                // 写 Flash 前暂停采集并清空流水线，避免 PSRAM DMA 与 MSPI 冲突
                capture_pause_and_drain();

                // 获取所有已注册的人脸ID信息列表
                if (xSemaphoreTake(s_face_db_mutex, portMAX_DELAY) != pdTRUE)
                {
                    ESP_LOGE(TAG, "清空数据：获取人脸特征库锁失败");
                    capture_resume();
                    continue;
                }
                std::vector<face_info_t> ids = recognizer->get_enrolled_ids();

                // 遍历所有人脸ID，逐个删除（不立即写入Flash）
                for (auto& info : ids)
                {
                    recognizer->delete_id(info.id, false);
                }

                // 统一把删除结果写回人脸库文件
                bool saved = face_db_save();
                xSemaphoreGive(s_face_db_mutex);
                ESP_LOGI(TAG, "已清空 %d 个人脸数据并写入 %s(%s)", (int)ids.size(),
                         FACE_DB_FILE, saved ? "成功" : "失败");

                capture_resume();

                // 使用continue跳过后续的帧数据检查，直接进入下一轮循环
                continue;
            }

            // ==================== 单击/长按事件检查 ====================
            /**
             * 单击和长按事件需要有效的摄像头帧数据才能执行
             * 检查条件：
             *   1. face_detected == true：当前帧检测到人脸
             *   2. frame != NULL：有有效的帧缓冲区（未释放）
             * 如果条件不满足，打印警告并跳过事件处理
             */

            // 检查是否检测到人脸
            if (!face_detected)
            {
                ESP_LOGW(TAG, "检测到按键事件(%d)，但未检测到人脸，请对准摄像头后重试",
                         pending_event);
                if (pending_event == BUTTON_EVENT_LONG_PRESS &&
                    (s_enroll_mode || lvgl_ui_admin_active()))
                {
                    set_lcd_status(UI_STATUS_FAIL, "录入失败", "未检测到人脸");
                    audio_prompt_play(AUDIO_PROMPT_INPUT_ERROR);
                }
                else
                {
                    set_lcd_status(UI_STATUS_WARN, "未检测到人脸", "请对准摄像头");
                    audio_prompt_play(AUDIO_PROMPT_NO_FACE); // 播报未检测到人脸
                }
            }
            // 检查帧缓冲区是否有效
            else if (frame == NULL)
            {
                ESP_LOGW(TAG, "检测到按键事件(%d)，但无有效的帧数据",
                         pending_event);
                set_lcd_status(UI_STATUS_WARN, "无帧数据", NULL);
            }
            // 条件满足，执行具体的按键事件处理
            else
            {
                // 根据事件类型执行不同的操作
                switch (pending_event)
                {

                    // ========== 长按事件：注册人脸 ==========
                    case BUTTON_EVENT_LONG_PRESS:
                    {
                        /**
                         * 录入权限检查：只有停留在管理员配置界面（或管理员页
                         * 按下的“录入”按钮转入的录入模式）时才允许录入
                         * 防止在预览页误长按就把新人脸写入 Flash
                         */
                        if (!lvgl_ui_admin_active() && !s_enroll_mode)
                        {
                            ESP_LOGW(TAG, "长按录入仅在管理员界面可用，请先通过登入页进入管理员界面");
                            set_lcd_status(UI_STATUS_WARN, "无法录入", "请先进入管理员界面");
                            audio_prompt_play(AUDIO_PROMPT_NO_FACE);
                            break;
                        }

                        ESP_LOGI(TAG, "执行人脸注册操作...");

                        /**
                         * 生成默认人脸名称
                         * 格式：Person_n（n为当前已注册的人脸数量）
                         * 例如：Person_0, Person_1, ...
                         * 使用snprintf确保缓冲区不溢出
                         */
                        char name[32];

                        /* 注册会修改人脸特征库，全程持有特征库锁，避免与信息库界面并发 */
                        xSemaphoreTake(s_face_db_mutex, portMAX_DELAY);

                        /* 录入人数上限：达到上限后拒绝继续录入，并提示先删除 */
                        {
                            int enrolled = recognizer->get_enrolled_id_num();

                            if (enrolled >= FACE_ENROLL_MAX)
                            {
                                xSemaphoreGive(s_face_db_mutex);
                                ESP_LOGW(TAG, "录入被拒绝：已录入 %d 人，达到上限 %d",
                                         enrolled, FACE_ENROLL_MAX);
                                set_lcd_status(UI_STATUS_WARN, "无法录入", "已达10人上限");
                                lvgl_ui_admin_toast("已达10人上限，请先删除");
                                audio_prompt_play(AUDIO_PROMPT_INPUT_ERROR);
                                break;
                            }
                        }

                        snprintf(name, sizeof(name), "Person_%d",
                                 recognizer->get_enrolled_id_num());

                        // 注册会写 Flash：先暂停采集并清空流水线，
                        // 否则 PSRAM 上的摄像头 DMA 会在写 Flash 期间溢出
                        capture_pause_and_drain();

                        /**
                         * 执行人脸注册（录入）
                         * 参数说明：
                         *   - frame->buf：摄像头帧数据（RGB565格式）
                         *   - 图像形状：{高, 宽, 3}（3通道RGB）
                         *   - current_landmarks：人脸关键点坐标
                         *   - name：人脸名称
                         *   - false：只写内存。esp-dl 自带的 Flash 写入在 IDF 5.x 上
                         *            无法擦除头部（见开机加载处注释），持久化由
                         *            face_db_save() 写 /vfs/face_db.bin 完成
                         * 返回值：>=0 注册成功（返回人脸ID索引），<0 注册失败
                         */
                        int id = recognizer->enroll_id(
                            (uint16_t*)frame->buf,
                            {(int)frame->height, (int)frame->width, 3},
                            current_landmarks, name, false);

                        // 检查注册结果
                        if (id >= 0)
                        {
                            // 注册成功，打印ID和名称
                            ESP_LOGI(TAG, "人脸注册成功！ID: %d, 名称: %s", id, name);

                            /**
                             * 用这一帧的原始像素裁剪出 32×32 头像并写入 vfs 分区：
                             * 此刻采集已暂停（capture_pause_and_drain），写 Flash 安全
                             */
                            avatar_save(id, (const uint16_t*)frame->buf,
                                        (int)frame->width, (int)frame->height, current_box);

                            /* 新注册的人脸从 0 次自学习开始（不沿用同名旧计数） */
                            s_fusion_counts[std::string(name)] = 0;

                            /**
                             * 把整库特征写入 /vfs/face_db.bin：注册成功后立刻落盘，
                             * 重启由 face_db_load() 恢复（不再依赖 esp-dl 的 Flash 实现）
                             */
                            if (face_db_save())
                            {
                                ESP_LOGI(TAG, "[DB] 人脸库已保存，共 %d 人",
                                         recognizer->get_enrolled_id_num());
                            }
                            else
                            {
                                ESP_LOGE(TAG, "[DB] 人脸库保存失败");
                            }
                            set_lcd_status(UI_STATUS_OK, "录入成功", "%s", name);

                            audio_prompt_play(AUDIO_PROMPT_INPUT_TRUE); // 播报录入成功
                        }
                        else
                        {
                            // 注册失败，常见原因：人脸未对齐、特征提取失败
                            ESP_LOGE(TAG, "人脸注册失败！");
                            set_lcd_status(UI_STATUS_FAIL, "录入失败", "请重试");
                            audio_prompt_play(AUDIO_PROMPT_INPUT_ERROR); // 播报录入失败
                        }

                        xSemaphoreGive(s_face_db_mutex);
                        capture_resume();
                        break;
                    }

                    // ========== 单击事件：识别人脸 ==========
                    case BUTTON_EVENT_SINGLE_CLICK:
                    {
                        ESP_LOGI(TAG, "执行人脸识别操作...");

                        /**
                         * 执行人脸识别
                         * 使用当前帧和检测到的人脸关键点进行特征比对
                         * 识别器会与已注册的所有人脸特征进行相似度计算
                         * 返回匹配结果（包含ID、姓名、相似度）
                         */
                        face_info_t result = recognizer->recognize(
                            (uint16_t*)frame->buf,
                            {(int)frame->height, (int)frame->width, 3},
                            current_landmarks);

                        // 检查识别结果
                        if (result.id >= 0)
                        {
                            /**
                             * 识别成功：在LCD和串口显示结果
                             * result.name：匹配到的人脸姓名
                             * result.similarity：相似度分数（0~1）
                             * result.id：匹配到的人脸ID索引
                             */
                            display_recognition_result(
                                result.name.c_str(),
                                result.similarity,
                                10, 10);

                            // 状态横幅显示识别结果（大号“识别成功” + 姓名与相似度）
                            set_lcd_status(UI_STATUS_OK, "识别成功", "%s %.0f%%",
                                           result.name.c_str(),
                                           result.similarity * 100.0f);

                            // 在控制台输出详细识别结果
                            ESP_LOGI(TAG, "识别结果 - ID: %d, 姓名: %s, 相似度: %.2f",
                                     result.id,
                                     result.name.c_str(),
                                     result.similarity);

                            /* 记录本次识别成功的人脸照片（管理员界面“历史记录”可查看）。
                               照片要写入 vfs 文件，先暂停采集并清空流水线，
                               避免 PSRAM 上的摄像头 DMA 与 MSPI 写 Flash 冲突 */
                            ESP_LOGI(TAG, "[HIST] 识别成功，暂停采集并写入历史照片");
                            capture_pause_and_drain();
                            history_add(result.name.c_str(), result.similarity,
                                        (const uint16_t*)frame->buf,
                                        (int)frame->width, (int)frame->height, current_box);

                            /* 模板自学习：高置信样本拟合进模板，并写回人脸库文件。
                               与历史记录共用同一次“暂停采集”窗口，避免二次停采。
                               加超时而不是无限等待，避免界面占用特征库锁时把 AI 卡死 */
                            if (xSemaphoreTake(s_face_db_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
                            {
                                if (face_template_learn(result.id,
                                                        result.name.c_str(),
                                                        result.similarity) &&
                                    !face_db_save())
                                {
                                    ESP_LOGW(TAG, "[LEARN] 模板已更新但写入 %s 失败",
                                             FACE_DB_FILE);
                                }
                                xSemaphoreGive(s_face_db_mutex);
                            }
                            else
                            {
                                ESP_LOGW(TAG, "[LEARN] 特征库锁超时，跳过本次自学习");
                            }
                            capture_resume();
                            ESP_LOGI(TAG, "[HIST] 恢复采集");

                            audio_prompt_play(AUDIO_PROMPT_FACE_TRUE); // 播报识别成功

                            // 开门：LED 点亮表示门开，3 秒后自动熄灭（锁门）
                            door_unlock_3s();

                        }
                        else
                        {
                            // 未匹配到已知人脸
                            ESP_LOGW(TAG, "未识别到匹配的人脸");
                            set_lcd_status(UI_STATUS_FAIL, "识别失败", "请重试");
                            audio_prompt_play(AUDIO_PROMPT_FACE_ERROR); // 播报识别失败提示音
                        }
                        break;
                    }

                    // 默认情况（不应发生）
                    default:
                        break;
                }
            }

            /**
             * 录入模式收尾：无论注册成功、失败，还是本次没检测到人脸，
             * 处理完这次长按事件都要退出录入模式并回到管理员界面
             */
            if (s_enroll_mode && pending_event == BUTTON_EVENT_LONG_PRESS)
            {
                exit_enroll_mode();
            }
        }

        // ---- 第六步：按键处理完成后再把帧交给 LCD 预览 ----

        if (frame != NULL)
        {
            // 诊断日志：前 3 帧打印 AI 推理结果，用于判断推理是否完成
            static int s_ai_dbg_count = 0;
            if (s_ai_dbg_count < 3)
            {
                s_ai_dbg_count++;
                ESP_LOGI(TAG, "[DBG] AI done frame %d, faces=%d, sent to LCD queue",
                         s_ai_dbg_count, face_detected ? 1 : 0);
            }

            /**
             * 把帧发送到 LCD 显示队列
             * 队列满时直接丢弃本帧并归还驱动，避免阻塞导致摄像头 DMA 停采
             */
            if (xQueueLCDFrame && xQueueSend(xQueueLCDFrame, &frame, 0) == pdTRUE)
            {
                frame = NULL; // 所有权已交给 LCD 任务，由它归还驱动
            }
            else
            {
                esp_camera_fb_return(frame);
                frame = NULL;
            }
        }

        /**
         * 每轮让出 1 个 tick：
         * AI 任务优先级 5 且帧队列常满时会连续推理不阻塞，IDLE0 永远拿不到
         * CPU，task_wdt 就会报“IDLE0 未喂狗”。这里主动让出，保证看门狗正常。
         */
        vTaskDelay(1);
    }
}

// =================================================================
// LCD图像显示任务（task_process_lcd）
// =================================================================

/**
 * @brief LCD图像显示任务
 *
 * 该任务从LCD帧队列接收已处理的帧数据，通过SPI接口
 * 将图像绘制到LCD屏幕上。显示完成后将帧缓冲区返回
 * 给摄像头驱动以供复用。
 *
 * @param arg 任务参数指针（本实现中未使用，传入NULL）
 * @return 无返回值（任务循环运行，永不返回）
 *
 * @note 任务栈大小：4KB
 * @note 优先级：5
 * @note 运行核心：0（应用核心）
 * @note 使用portMAX_DELAY无限等待队列数据
 * @note 显示完成后必须调用esp_camera_fb_return()释放帧缓冲区
 */
static void task_process_lcd(void* arg)
{
    // 声明帧缓冲区指针，用于接收AI处理后的帧数据
    camera_fb_t* frame = NULL;

    // 登入页是否已切换（进入时只切换一次）
    static bool s_login_page_shown = false;

    // 任务主循环，持续处理LCD显示请求
    while (true)
    {
        // ---- 0. 待机 / 登入界面：暂停预览 ----

        /**
         * 待机期间停止预览刷新（屏幕已熄屏）：
         * 排空队列中的残留帧并归还驱动，避免帧缓冲泄漏
         */
        if (lvgl_ui_standby_active())
        {
            if (xQueueReceive(xQueueLCDFrame, &frame, pdMS_TO_TICKS(50)))
            {
                esp_camera_fb_return(frame);
            }
            continue;
        }

        /**
         * 录入模式下仍要显示摄像头画面（管理员页已隐藏，界面为纯预览），
         * 管理员界面也不能被登入页覆盖，因此只有“登入页 + 非管理员 + 非录入”
         * 才走下面的暂停分支
         */
        if (s_login_mode && !lvgl_ui_admin_active() && !s_enroll_mode)
        {
            if (!s_login_page_shown)
            {
                lvgl_ui_show_login();
                s_login_page_shown = true;
            }

            // 排空队列中残留的帧并归还给摄像头驱动，避免帧缓冲泄漏
            if (xQueueReceive(xQueueLCDFrame, &frame, pdMS_TO_TICKS(50)))
            {
                esp_camera_fb_return(frame);
            }
            continue;
        }
        s_login_page_shown = false;

        /**
         * 从LCD帧队列接收已处理的帧数据
         * 50ms 超时：无新帧时也能及时检测到登入界面切换
         * 队列为空时任务阻塞，不占用CPU资源
         */
        if (xQueueReceive(xQueueLCDFrame, &frame, pdMS_TO_TICKS(50)))
        {
            // 诊断日志：前 3 帧打印“收到帧 / 绘制返回”，用于定位卡在预览绘制还是队列
            static int s_lcd_dbg_count = 0;
            bool dbg_log = (s_lcd_dbg_count < 3);
            if (dbg_log)
            {
                s_lcd_dbg_count++;
                ESP_LOGI(TAG, "[DBG] LCD got frame %d (%ux%u), drawing...", s_lcd_dbg_count,
                         frame->width, frame->height);
            }

            // ---- 1. 缩放并刷新摄像头预览（LVGL 图像对象，内部持锁避免画面撕裂） ----
            // 手指按住期间跳过预览重绘：一帧预览要往 SPI 刷 217KB（约 22ms），
            // 会把 LVGL 的输入轮询周期从 30ms 拖到 60-85ms，导致按压/抬起晚一拍。
            // 跳过之后这一轮 LVGL 只剩“处理输入 + 重绘按钮”，点击立刻生效；
            // 手指松开后自动恢复预览（预览只定格几百毫秒，观感无异常）。
            if (!tp_snapshot_pressed(NULL, NULL))
            {
                lvgl_ui_preview_frame((const uint16_t*)frame->buf, frame->width, frame->height);

                if (dbg_log)
                {
                    ESP_LOGI(TAG, "[DBG] LCD draw done frame %d", s_lcd_dbg_count);
                }
            }
            else
            {
                s_lcd_dbg_count = 0; // 触摸后重新统计调试帧，便于下次进入时打印
            }

            /**
             * 将帧缓冲区返回给摄像头驱动
             * 驱动会将该缓冲区加入空闲队列等待复用
             * 注意：调用此函数后frame指针变为无效
             */
            esp_camera_fb_return(frame);

            // ---- 2. 在预览画面下方的状态横幅显示识别结果 ----

            if (lcd_info_mutex)
            {
                // 记录上次已显示的内容，只有变化时才重绘，避免闪烁
                static char prev_title[40] = "";
                static char prev_detail[80] = "";
                static uint8_t prev_kind = UI_STATUS_INFO;

                char title[40];
                char detail[80];
                uint8_t kind = UI_STATUS_INFO;
                bool need_draw = false;
                bool need_clear = false;

                /**
                 * 关键：只在锁内复制状态数据，然后在锁外调用 LVGL。
                 * 若持有 lcd_info_mutex 时再去等 LVGL 锁，会和“LVGL 任务持有
                 * LVGL 锁后调用 set_lcd_status（等 lcd_info_mutex）”形成 ABBA 死锁，
                 * 表现为点某个按钮后整机卡死。
                 */
                if (xSemaphoreTake(lcd_info_mutex, 0) == pdTRUE)
                {
                    if (g_lcd_info.active)
                    {
                        TickType_t now = xTaskGetTickCount();

                        if ((now - g_lcd_info.timestamp) >= pdMS_TO_TICKS(INFO_CLEAR_MS))
                        {
                            // 超过3秒，清除横幅
                            g_lcd_info.active = false;
                            prev_title[0] = '\0';
                            prev_detail[0] = '\0';
                            prev_kind = UI_STATUS_INFO;
                            need_clear = true;
                        }
                        else if (strcmp(prev_title, g_lcd_info.title) != 0 ||
                                 strcmp(prev_detail, g_lcd_info.detail) != 0 ||
                                 prev_kind != g_lcd_info.kind)
                        {
                            // 内容或类别变化：记录新内容，稍后在锁外重绘
                            strncpy(prev_title, g_lcd_info.title, sizeof(prev_title) - 1);
                            prev_title[sizeof(prev_title) - 1] = '\0';
                            strncpy(prev_detail, g_lcd_info.detail, sizeof(prev_detail) - 1);
                            prev_detail[sizeof(prev_detail) - 1] = '\0';
                            prev_kind = g_lcd_info.kind;

                            strncpy(title, g_lcd_info.title, sizeof(title) - 1);
                            title[sizeof(title) - 1] = '\0';
                            strncpy(detail, g_lcd_info.detail, sizeof(detail) - 1);
                            detail[sizeof(detail) - 1] = '\0';
                            kind = g_lcd_info.kind;
                            need_draw = true;
                        }
                    }
                    xSemaphoreGive(lcd_info_mutex);
                }

                if (need_clear)
                {
                    lvgl_ui_clear_status();
                }
                else if (need_draw)
                {
                    lvgl_ui_set_status(kind, title, detail);
                }
            }
        }
    }
}

// =================================================================
// 摄像头图像采集任务（task_process_camera）
// =================================================================

/**
 * @brief 摄像头图像采集任务
 *
 * 该任务循环从摄像头驱动获取一帧图像数据，然后将帧指针
 * 发送到AI处理队列供检测任务处理。
 * 摄像头的帧率由硬件自动控制，本任务仅负责搬运数据。
 *
 * @param arg 任务参数指针（本实现中未使用，传入NULL）
 * @return 无返回值（任务循环运行，永不返回）
 *
 * @note 任务栈大小：3KB
 * @note 优先级：5
 * @note 运行核心：1（协议核心，与CPU密集型AI任务分离）
 * @note 如果AI队列满，xQueueSend会阻塞等待
 * @note 帧缓冲区由摄像头驱动管理，任务不需要关心分配和释放
 */
static void task_process_camera(void* arg)
{
    // 任务主循环，持续采集摄像头帧数据
    while (true)
    {
        /**
         * 登入界面期间停止摄像头采集（管理员界面与录入模式例外：录入需要实时画面），
         * 不再调用 esp_camera_fb_get()，摄像头停止出帧，
         * 下游 AI 与 LCD 任务因收不到新帧而自然暂停
         *
         * 写 Flash 期间（s_capture_paused）同样停止采集，
         * 避免 DMA 在 MSPI 被 Flash 占用时溢出丢帧
         */
        if (!s_camera_stream_enabled ||
            ((s_login_mode && !lvgl_ui_admin_active() && !s_enroll_mode) || s_capture_paused ||
             lvgl_ui_standby_active()))
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /**
         * 从摄像头驱动获取一帧图像数据
         * 返回指向帧缓冲区结构体的指针，包含：
         *   - buf：图像数据缓冲区
         *   - len：数据长度
         *   - width：图像宽度（像素）
         *   - height：图像高度（像素）
         *   - format：像素格式（RGB565/JPEG等）
         * 如果获取失败（摄像头未初始化或硬件错误）返回NULL
         */
        camera_fb_t* frame = esp_camera_fb_get();

        // 检查帧数据是否获取成功
        if (frame)
        {
            // 前几帧到达时打印，用于确认摄像头 DMA 是否持续出帧（>5 帧说明链路正常）
            static int s_frame_cnt = 0;
            if (s_frame_cnt < 5)
            {
                s_frame_cnt++;
                ESP_LOGI(TAG, "[DBG] cam frame %d: %ux%u len=%u fmt=%d", s_frame_cnt,
                         frame->width, frame->height, frame->len, (int)frame->format);
            }

            /**
             * 将帧指针发送到AI处理队列
             * 队列满时不阻塞，直接丢弃当前帧（0 超时）：
             * 若阻塞等待，帧缓冲会积压在队列里回不到 DMA，摄像机会停止采集
             */
            if (xQueueSend(xQueueAIFrame, &frame, 0) != pdTRUE)
            {
                esp_camera_fb_return(frame); // 丢弃本帧并立即还给驱动，保持 DMA 持续工作
            }
        }
    }
}

// =================================================================
// 人脸特征库持久化（/vfs/face_db.bin）
// =================================================================

/**
 * @brief 人脸库文件头
 */
typedef struct
{
    uint32_t magic;   /**< FACE_DB_MAGIC */
    uint32_t version; /**< FACE_DB_VERSION */
    uint32_t count;   /**< 记录条数 */
    uint32_t dim;     /**< 每条记录的特征维度（float 个数） */
} face_db_header_t;

/**
 * @brief 把识别器内存中的人脸特征库整库写入 /vfs/face_db.bin
 *
 * 记录格式（v2）：int32 id + char name[32] + uint8 自学习次数 + float emb[dim]，
 * 按识别器当前顺序排列。空库也会写一个 count=0 的文件，语义为“明确没有已注册人脸”。
 *
 * @note 必须在摄像头采集暂停时调用（写 FAT 同样会占用 Flash）
 * @return true 保存成功
 */
static bool face_db_save(void)
{
    int64_t t_start;

    if (!s_avatar_fs_ready)
    {
        ESP_LOGW(TAG, "[DB] 文件系统未就绪，无法保存人脸库");
        return false;
    }

    t_start = esp_timer_get_time();
    std::vector<face_info_t> infos = recognizer->get_enrolled_ids();

    uint32_t dim = 0;
    if (!infos.empty())
    {
        dl::Tensor<float>& emb = recognizer->get_face_emb(infos.front().id);
        if (emb.shape.size() > 0 && emb.element != NULL)
        {
            dim = (uint32_t)emb.shape[0];
        }
    }
    if (dim == 0)
    {
        dim = 512; /* 空库时写模型默认维度，仅作占位 */
    }

    FILE* f = fopen(FACE_DB_FILE, "wb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "[DB] 打开 %s 失败", FACE_DB_FILE);
        return false;
    }

    face_db_header_t hdr;
    hdr.magic = FACE_DB_MAGIC;
    hdr.version = FACE_DB_VERSION;
    hdr.count = (uint32_t)infos.size();
    hdr.dim = dim;

    bool ok = (fwrite(&hdr, sizeof(hdr), 1, f) == 1);

    for (size_t i = 0; ok && i < infos.size(); i++)
    {
        int32_t id = infos[i].id;
        char name[FACE_DB_NAME_LEN];
        memset(name, 0, sizeof(name));
        strncpy(name, infos[i].name.c_str(), sizeof(name) - 1);

        dl::Tensor<float>& emb = recognizer->get_face_emb(id);
        if (emb.element == NULL || emb.shape.size() == 0 ||
            (uint32_t)emb.shape[0] != dim)
        {
            ESP_LOGE(TAG, "[DB] ID %d 的特征维度与文件头不一致，保存中止", id);
            ok = false;
            break;
        }

        uint8_t fusion_count = 0;
        auto it = s_fusion_counts.find(infos[i].name);
        if (it != s_fusion_counts.end())
        {
            fusion_count = it->second;
        }

        ok = (fwrite(&id, sizeof(id), 1, f) == 1) &&
             (fwrite(name, sizeof(name), 1, f) == 1) &&
             (fwrite(&fusion_count, sizeof(fusion_count), 1, f) == 1) &&
             (fwrite(emb.element, sizeof(float), dim, f) == dim);
    }

    fclose(f);

    if (!ok)
    {
        ESP_LOGE(TAG, "[DB] 写 %s 失败（耗时 %lld ms）", FACE_DB_FILE,
                 (long long)((esp_timer_get_time() - t_start) / 1000));
    }
    else
    {
        ESP_LOGI(TAG, "[DB] 人脸库写入完成（%d 人，耗时 %lld ms）", (int)infos.size(),
                 (long long)((esp_timer_get_time() - t_start) / 1000));
    }
    return ok;
}

/**
 * @brief 从 /vfs/face_db.bin 恢复人脸特征库到识别器内存
 *
 * 用 set_ids() 一次性重建（该接口会先清空内存中的旧库，再深拷贝传入的特征），
 * 因此不会重复注册，也不需要识别器再跑一遍模型。
 *
 * @return >=0 成功恢复的个数；-1 文件不存在；-2 文件系统未就绪；
 *         -3 文件头非法；-4 记录损坏
 */
static int face_db_load(void)
{
    if (!s_avatar_fs_ready)
    {
        ESP_LOGW(TAG, "[DB] 文件系统未就绪，跳过人脸库恢复");
        return -2;
    }

    FILE* f = fopen(FACE_DB_FILE, "rb");
    if (f == NULL)
    {
        /* 首次开机/从未注册过：没有文件不是错误 */
        return -1;
    }

    face_db_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != FACE_DB_MAGIC ||
        hdr.version < FACE_DB_VERSION_MIN || hdr.version > FACE_DB_VERSION ||
        hdr.dim == 0 || hdr.dim > FACE_DB_MAX_DIM ||
        hdr.count > FACE_DB_MAX_COUNT)
    {
        fclose(f);
        return -3;
    }

    s_fusion_counts.clear();

    std::vector<FaceID<float>*> ids;
    ids.reserve(hdr.count);

    bool ok = true;
    uint32_t read_count = 0;

    for (uint32_t i = 0; i < hdr.count && ok; i++)
    {
        int32_t id = -1;
        char name[FACE_DB_NAME_LEN];
        uint8_t fusion_count = 0;

        if (fread(&id, sizeof(id), 1, f) != 1 ||
            fread(name, sizeof(name), 1, f) != 1)
        {
            ok = false;
            break;
        }
        name[FACE_DB_NAME_LEN - 1] = '\0';

        /* v2 起在姓名后多存 1 字节自学习次数；v1 文件按 0 处理 */
        if (hdr.version >= 2 &&
            fread(&fusion_count, sizeof(fusion_count), 1, f) != 1)
        {
            ok = false;
            break;
        }

        /* Tensor 自己管理特征缓冲；FaceID 构造时会再深拷贝一份 */
        dl::Tensor<float> emb;
        emb.set_shape({(int)hdr.dim}).malloc_element();
        if (emb.element == NULL ||
            fread(emb.element, sizeof(float), hdr.dim, f) != hdr.dim)
        {
            ok = false;
            break;
        }

        ids.push_back(new FaceID<float>(id, emb, std::string(name)));
        s_fusion_counts[std::string(name)] = fusion_count;
        read_count++;
    }

    fclose(f);

    if (!ok)
    {
        ESP_LOGW(TAG, "[DB] 人脸库文件损坏（读到 %u/%u 条），放弃加载",
                 read_count, hdr.count);
        for (FaceID<float>* p : ids)
        {
            delete p;
        }
        return -4;
    }

    int loaded = recognizer->set_ids(ids, false);

    for (FaceID<float>* p : ids)
    {
        delete p;
    }

    return loaded;
}

// =================================================================
// 人脸头像（vfs FAT 分区）读写
// =================================================================

/**
 * @brief 挂载 vfs(FAT) 分区并创建头像目录
 *
 * 头像文件很小（48×48 RGB565 约 4.6KB），放在独立的 vfs 分区里，
 * 不占用识别器使用的 storage 分区。挂载失败时仅关闭头像功能，不影响识别。
 *
 * @note 必须在摄像头采集任务启动前调用（挂载会读写 Flash）
 */
static void avatar_fs_mount(void)
{
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true; /* 首次使用自动格式化 */
    cfg.max_files = 4;
    cfg.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;
    cfg.disk_status_check_enable = false;
    cfg.use_one_fat = false;

    static wl_handle_t wl = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/vfs", "vfs", &cfg, &wl);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "头像分区挂载失败(0x%x)，头像功能关闭", err);
        return;
    }

    s_avatar_save_buf = (uint16_t*)heap_caps_malloc(FACE_AVATAR_PIXELS * sizeof(uint16_t),
                                                    MALLOC_CAP_SPIRAM);
    if (s_avatar_save_buf == NULL)
    {
        ESP_LOGW(TAG, "头像缓冲分配失败，头像功能关闭");
        return;
    }

    /* 目录已存在时返回错误，忽略即可 */
    mkdir(FACE_AVATAR_DIR, 0777);
    s_avatar_fs_ready = true;
    ESP_LOGI(TAG, "头像存储已就绪：%s", FACE_AVATAR_DIR);
}

/**
 * @brief 从当前帧裁剪人脸并缩放到 32×32，写入 /vfs/avatars/face_<id>.bin
 *
 * @param id    注册返回的人脸 ID
 * @param src   摄像头 RGB565 帧（未绘制检测框的原始数据）
 * @param src_w 帧宽
 * @param src_h 帧高
 * @param box   人脸框 [x0, y0, x1, y1]（帧坐标系）
 * @return true 保存成功
 */
static bool avatar_save(int id, const uint16_t* src, int src_w, int src_h,
                        const std::vector<int>& box)
{
    if (!s_avatar_fs_ready || s_avatar_save_buf == NULL || src == NULL || box.size() < 4)
    {
        return false;
    }

    int x0 = box[0];
    int y0 = box[1];
    int x1 = box[2];
    int y1 = box[3];

    /* 四周外扩 15%，避免额头/下巴被裁掉 */
    int mw = (x1 - x0) * 15 / 100;
    int mh = (y1 - y0) * 15 / 100;
    x0 -= mw;
    y0 -= mh;
    x1 += mw;
    y1 += mh;

    if (x0 < 0)
    {
        x0 = 0;
    }
    if (y0 < 0)
    {
        y0 = 0;
    }
    if (x1 > src_w)
    {
        x1 = src_w;
    }
    if (y1 > src_h)
    {
        y1 = src_h;
    }

    int bw = x1 - x0;
    int bh = y1 - y0;
    if (bw <= 0 || bh <= 0)
    {
        return false;
    }

    /* 最近邻缩放；像素保持摄像头原始字节序，LVGL(16_SWAP) 可直接显示 */
    for (int dy = 0; dy < FACE_AVATAR_H; dy++)
    {
        const uint16_t* src_row = src + (y0 + dy * bh / FACE_AVATAR_H) * src_w;
        uint16_t* dst_row = s_avatar_save_buf + dy * FACE_AVATAR_W;
        for (int dx = 0; dx < FACE_AVATAR_W; dx++)
        {
            dst_row[dx] = src_row[x0 + dx * bw / FACE_AVATAR_W];
        }
    }

    /* FATFS 未开启长文件名（8.3 限制），故用 "<id>.bin" 而非 "face_<id>.bin" */
    char path[64];
    snprintf(path, sizeof(path), FACE_AVATAR_DIR "/%d.bin", id);

    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        ESP_LOGW(TAG, "头像文件创建失败: %s", path);
        return false;
    }
    size_t n = fwrite(s_avatar_save_buf, sizeof(uint16_t), FACE_AVATAR_PIXELS, f);
    fclose(f);

    if (n != FACE_AVATAR_PIXELS)
    {
        ESP_LOGW(TAG, "头像写入不完整(%u/%u): %s", (unsigned)n,
                 (unsigned)FACE_AVATAR_PIXELS, path);
        return false;
    }

    ESP_LOGI(TAG, "头像已保存: %s", path);
    return true;
}

extern "C" int human_face_avatar_load(int id, uint16_t* buf)
{
    if (id < 0 || buf == NULL || !s_avatar_fs_ready)
    {
        return 0;
    }

    char path[64];
    snprintf(path, sizeof(path), FACE_AVATAR_DIR "/%d.bin", id);

    FILE* f = fopen(path, "rb");
    if (f == NULL)
    {
        return 0;
    }
    size_t n = fread(buf, sizeof(uint16_t), FACE_AVATAR_PIXELS, f);
    fclose(f);

    return (n == FACE_AVATAR_PIXELS) ? 1 : 0;
}

/**
 * @brief 删除指定人脸的头像文件
 */
static void avatar_delete(int id)
{
    if (!s_avatar_fs_ready || id < 0)
    {
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), FACE_AVATAR_DIR "/%d.bin", id);
    remove(path);
}

// =================================================================
// 识别历史记录（PSRAM 缓存 + vfs 文件，最多最近 FACE_HISTORY_MAX 条）
// =================================================================

/**
 * @def FACE_HISTORY_DIR
 * @brief 历史照片目录（挂载在 vfs(FAT) 分区上，掉电保存）
 */
#define FACE_HISTORY_DIR "/vfs/history"

/**
 * @def FACE_HISTORY_MAGIC
 * @brief 历史照片文件魔数（v2：带识别时间与“已上传”标记）
 */
#define FACE_HISTORY_MAGIC 0x48495332u

/**
 * @def FACE_HISTORY_MAGIC_V1
 * @brief 旧版历史照片文件魔数（无时间/上传标记，开机读取时自动迁移）
 */
#define FACE_HISTORY_MAGIC_V1 0x48495354u

/**
 * @def FACE_HISTORY_FLAG_UPLOADED
 * @brief 文件头 flags 的 bit0：照片已上传到巴法云
 */
#define FACE_HISTORY_FLAG_UPLOADED 0x01u

/**
 * @def FACE_HISTORY_SEQ_INVALID
 * @brief 槽位无效标记（对应文件不存在或文件损坏）
 */
#define FACE_HISTORY_SEQ_INVALID 0xFFFFFFFFu

/**
 * @struct history_header_t
 * @brief 历史照片文件头 v2（后接 FACE_HISTORY_PIXELS 个 RGB565 像素）
 */
typedef struct
{
    uint32_t magic;                         /**< FACE_HISTORY_MAGIC */
    uint32_t seq;                           /**< 写入序号（越大越新） */
    char name[FACE_DB_NAME_LEN];            /**< 识别到的姓名 */
    uint8_t sim;                            /**< 相似度百分比 */
    uint8_t flags;                          /**< FACE_HISTORY_FLAG_UPLOADED 等标记 */
    uint16_t reserved;                      /**< 对齐填充 */
    uint32_t epoch;                         /**< 识别时间（UNIX 秒，0=未校时） */
} history_header_t;

/**
 * @struct history_header_v1_t
 * @brief 旧版历史照片文件头（仅用于开机时读取并迁移旧文件）
 */
typedef struct
{
    uint32_t magic;                         /**< FACE_HISTORY_MAGIC_V1 */
    uint32_t seq;                           /**< 写入序号 */
    char name[FACE_DB_NAME_LEN];            /**< 识别到的姓名 */
    uint8_t sim;                            /**< 相似度百分比 */
    uint8_t reserved[3];                    /**< 对齐填充 */
} history_header_v1_t;

/**
 * @var s_history_img
 * @brief 历史记录照片缓冲（PSRAM，每条 FACE_HISTORY_W × FACE_HISTORY_H 个 RGB565）
 */
static uint16_t* s_history_img[FACE_HISTORY_MAX] = {};

/**
 * @var s_history_names
 * @brief 每条历史记录对应的人脸姓名
 */
static char s_history_names[FACE_HISTORY_MAX][FACE_DB_NAME_LEN] = {};

/**
 * @var s_history_sim
 * @brief 每条历史记录的相似度百分比
 */
static uint8_t s_history_sim[FACE_HISTORY_MAX] = {};

/**
 * @var s_history_epoch
 * @brief 每条记录的识别时间（UNIX 秒，0 表示当时尚未校时）
 */
static uint32_t s_history_epoch[FACE_HISTORY_MAX] = {};

/**
 * @var s_history_flags
 * @brief 每条记录的标记位（FACE_HISTORY_FLAG_UPLOADED）
 */
static uint8_t s_history_flags[FACE_HISTORY_MAX] = {};

/**
 * @var s_history_entry_seq
 * @brief 每个槽位当前记录的写入序号；FACE_HISTORY_SEQ_INVALID 表示空槽
 * @note  序号越大越新，界面按序号从大到小显示
 */
static uint32_t s_history_entry_seq[FACE_HISTORY_MAX] = {};

/**
 * @var s_history_seq
 * @brief 下一条记录使用的写入序号；槽位 = seq % FACE_HISTORY_MAX
 * @note  读写都在 s_history_mutex 保护下进行
 */
static uint32_t s_history_seq = 0;

/**
 * @var s_history_count
 * @brief 当前有效条数（不超过 FACE_HISTORY_MAX）
 * @note  读写都在 s_history_mutex 保护下进行
 */
static int s_history_count = 0;

/**
 * @var s_history_mutex
 * @brief 历史记录互斥锁：AI 任务写入，LVGL 任务读取
 */
static SemaphoreHandle_t s_history_mutex = NULL;

/**
 * @var s_history_ready
 * @brief 历史记录功能是否就绪（缓冲与互斥锁创建成功）
 */
static bool s_history_ready = false;

/**
 * @brief 把某个槽位的照片与头部写入 vfs 文件（掉电保存）
 *
 * @param slot 槽位号
 * @return true 写入成功
 * @note 必须在摄像头采集暂停时调用（写 Flash 与 PSRAM 上的 DMA 会冲突）
 */
static bool history_save_file(int slot)
{
    char path[64];
    history_header_t hdr;
    bool ok;
    int64_t t0;

    if (!s_avatar_fs_ready)
    {
        return false;
    }

    snprintf(path, sizeof(path), FACE_HISTORY_DIR "/%d.bin", slot);
    ESP_LOGI(TAG, "[HIST] 开始写入 %s（%d 字节）", path,
             (int)(sizeof(hdr) + FACE_HISTORY_PIXELS * sizeof(uint16_t)));
    t0 = esp_timer_get_time();

    FILE* f = fopen(path, "wb");
    if (f == NULL)
    {
        ESP_LOGW(TAG, "历史照片文件创建失败: %s", path);
        return false;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FACE_HISTORY_MAGIC;
    hdr.seq = s_history_entry_seq[slot];
    snprintf(hdr.name, sizeof(hdr.name), "%s", s_history_names[slot]);
    hdr.sim = s_history_sim[slot];
    hdr.flags = s_history_flags[slot];
    hdr.epoch = s_history_epoch[slot];

    ok = (fwrite(&hdr, sizeof(hdr), 1, f) == 1) &&
         (fwrite(s_history_img[slot], sizeof(uint16_t), FACE_HISTORY_PIXELS, f) ==
          FACE_HISTORY_PIXELS);
    fclose(f);

    if (!ok)
    {
        ESP_LOGW(TAG, "历史照片写入不完整: %s（耗时 %lld ms）", path,
                 (long long)((esp_timer_get_time() - t0) / 1000));
        remove(path); /* 写失败的文件不留在分区里，下次同槽位重新写 */
        return false;
    }

    ESP_LOGI(TAG, "[HIST] 写入完成 %s（耗时 %lld ms）", path,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return true;
}

/**
 * @brief 读取一个槽位的照片文件（自动识别 v1/v2 文件头）
 *
 * @param slot    槽位号
 * @param hdr     输出 v2 文件头（v1 文件会被转换为 v2 结构，时间/标记为 0）
 * @param migrated 输出是否来自旧版文件（需要回写为新格式）
 * @return true 读取成功
 * @note 调用者必须保证摄像头采集已暂停（写 Flash 前）
 */
static bool history_read_file(int slot, history_header_t* hdr, bool* migrated)
{
    char path[64];
    history_header_v1_t hdr_v1;
    history_header_t hdr_v2;
    bool ok = false;

    *migrated = false;

    snprintf(path, sizeof(path), FACE_HISTORY_DIR "/%d.bin", slot);
    FILE* f = fopen(path, "rb");
    if (f == NULL)
    {
        return false;
    }

    memset(&hdr_v2, 0, sizeof(hdr_v2));
    if ((fread(&hdr_v2, sizeof(hdr_v2), 1, f) == 1) &&
        (hdr_v2.magic == FACE_HISTORY_MAGIC))
    {
        ok = (fread(s_history_img[slot], sizeof(uint16_t), FACE_HISTORY_PIXELS, f) ==
              FACE_HISTORY_PIXELS);
    }
    else if ((fseek(f, 0, SEEK_SET) == 0) &&
             (fread(&hdr_v1, sizeof(hdr_v1), 1, f) == 1) &&
             (hdr_v1.magic == FACE_HISTORY_MAGIC_V1))
    {
        /* 旧版文件：像素位置不同，读完后按 v2 结构返回并回写迁移 */
        ok = (fread(s_history_img[slot], sizeof(uint16_t), FACE_HISTORY_PIXELS, f) ==
              FACE_HISTORY_PIXELS);
        if (ok)
        {
            memset(&hdr_v2, 0, sizeof(hdr_v2));
            hdr_v2.magic = FACE_HISTORY_MAGIC;
            hdr_v2.seq = hdr_v1.seq;
            memcpy(hdr_v2.name, hdr_v1.name, sizeof(hdr_v2.name));
            hdr_v2.sim = hdr_v1.sim;
            hdr_v2.flags = 0;
            hdr_v2.epoch = 0;
            *migrated = true;
        }
    }
    fclose(f);

    if (ok)
    {
        *hdr = hdr_v2;
    }
    return ok;
}

/**
 * @brief 开机从 vfs 目录恢复历史照片（文件损坏的条目直接忽略）
 *
 * 旧版（v1）文件会在此时迁移为新格式；迁移写入发生在摄像头采集启动前，安全。
 */
static void history_load(void)
{
    int loaded = 0;
    int migrated = 0;
    uint32_t max_seq = 0;
    bool have_seq = false;

    for (int slot = 0; slot < FACE_HISTORY_MAX; slot++)
    {
        history_header_t hdr;
        bool was_v1 = false;

        s_history_entry_seq[slot] = FACE_HISTORY_SEQ_INVALID;
        s_history_epoch[slot] = 0;
        s_history_flags[slot] = 0;

        bool ok = history_read_file(slot, &hdr, &was_v1);
        if (!ok)
        {
            continue;
        }

        snprintf(s_history_names[slot], sizeof(s_history_names[slot]), "%s", hdr.name);
        s_history_sim[slot] = hdr.sim;
        s_history_epoch[slot] = hdr.epoch;
        s_history_flags[slot] = hdr.flags;
        s_history_entry_seq[slot] = hdr.seq;

        loaded++;
        if (!have_seq || hdr.seq > max_seq)
        {
            max_seq = hdr.seq;
            have_seq = true;
        }

        if (was_v1)
        {
            if (history_save_file(slot))
            {
                migrated++;
            }
        }
    }

    s_history_seq = have_seq ? (max_seq + 1) : 0;
    s_history_count = loaded;

    if (loaded > 0)
    {
        ESP_LOGI(TAG, "从文件恢复 %d 条历史记录（下一条序号 %u，迁移旧格式 %d 条）",
                 loaded, (unsigned)s_history_seq, migrated);
    }
}

/**
 * @brief 按“从新到旧”的顺序找出第 index 条记录所在槽位
 * @param index 0 表示最新一条
 * @return 槽位号；-1 表示没有该条记录
 * @note 调用者必须已持有 s_history_mutex
 */
static int history_slot_for_index(int index)
{
    uint32_t limit = FACE_HISTORY_SEQ_INVALID;
    bool have_limit = false;
    int chosen = -1;

    if (index < 0 || index >= s_history_count)
    {
        return -1;
    }

    /* 每轮找出“小于上一轮序号”的最大序号，即排在上一轮之后的一条 */
    for (int k = 0; k <= index; k++)
    {
        uint32_t best_seq = 0;
        int best_slot = -1;

        for (int slot = 0; slot < FACE_HISTORY_MAX; slot++)
        {
            uint32_t sq = s_history_entry_seq[slot];

            if (sq == FACE_HISTORY_SEQ_INVALID)
            {
                continue;
            }
            if (have_limit && sq >= limit)
            {
                continue;
            }
            if (best_slot < 0 || sq > best_seq)
            {
                best_seq = sq;
                best_slot = slot;
            }
        }

        if (best_slot < 0)
        {
            return -1;
        }

        chosen = best_slot;
        limit = best_seq;
        have_limit = true;
    }

    return chosen;
}

/**
 * @brief 分配历史记录缓冲、创建互斥锁并从 vfs 恢复已有照片（在任务启动前调用）
 */
static void history_init(void)
{
    bool ok = true;

    for (int i = 0; i < FACE_HISTORY_MAX; i++)
    {
        s_history_img[i] = (uint16_t*)heap_caps_malloc(
            FACE_HISTORY_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (s_history_img[i] == NULL)
        {
            ok = false;
            break;
        }
    }

    if (!ok)
    {
        for (int i = 0; i < FACE_HISTORY_MAX; i++)
        {
            if (s_history_img[i] != NULL)
            {
                heap_caps_free(s_history_img[i]);
                s_history_img[i] = NULL;
            }
        }
        ESP_LOGW(TAG, "历史记录缓冲分配失败，历史功能关闭");
        return;
    }

    s_history_mutex = xSemaphoreCreateMutex();
    if (s_history_mutex == NULL)
    {
        ESP_LOGW(TAG, "历史记录互斥锁创建失败，历史功能关闭");
        return;
    }

    s_history_ready = true;

    if (!s_avatar_fs_ready)
    {
        ESP_LOGW(TAG, "文件系统未就绪，历史记录只在内存中保留，重启后清空");
        return;
    }

    /* 目录已存在时返回错误，忽略即可；随后恢复掉电前保存的照片 */
    mkdir(FACE_HISTORY_DIR, 0777);
    history_load();

    ESP_LOGI(TAG, "识别历史记录已就绪（最多 %d 条，掉电保存）", FACE_HISTORY_MAX);
}


/**
 * @brief 保存一条识别成功的历史记录（从当前帧裁剪人脸区域）
 *
 * 以人脸框为中心外扩 15%，并扩展成 3:2（与界面显示比例一致），
 * 最近邻缩放到 FACE_HISTORY_W × FACE_HISTORY_H 后写入环形槽位。
 *
 * @param name       识别到的姓名
 * @param similarity 相似度（0~1）
 * @param src        摄像头 RGB565 帧（未绘制检测框的原始数据）
 * @param src_w      帧宽
 * @param src_h      帧高
 * @param box        人脸框 [x0, y0, x1, y1]（帧坐标系）
 */
static void history_add(const char* name, float similarity, const uint16_t* src,
                        int src_w, int src_h, const std::vector<int>& box)
{
    int x0 = 0;
    int y0 = 0;
    int w = 0;
    int h = 0;

    if (!s_history_ready || src == NULL || src_w <= 0 || src_h <= 0)
    {
        return;
    }

    if (box.size() >= 4)
    {
        int bw = box[2] - box[0];
        int bh = box[3] - box[1];
        int cx = (box[0] + box[2]) / 2;
        int cy = (box[1] + box[3]) / 2;

        w = bw * 130 / 100;
        h = bh * 130 / 100;

        /* 扩展成 3:2，避免放大显示时人脸变形 */
        if (w * 2 < h * 3)
        {
            w = h * 3 / 2;
        }
        else
        {
            h = w * 2 / 3;
        }

        x0 = cx - w / 2;
        y0 = cy - h / 2;
    }
    else
    {
        /* 没有有效人脸框：退化为画面中央 3:2 区域 */
        h = src_h;
        w = h * 3 / 2;
        if (w > src_w)
        {
            w = src_w;
            h = w * 2 / 3;
        }
        x0 = (src_w - w) / 2;
        y0 = (src_h - h) / 2;
    }

    /* 保证裁剪区域落在帧内（保持 3:2 比例） */
    if (w > src_w)
    {
        w = src_w;
        h = w * 2 / 3;
    }
    if (h > src_h)
    {
        h = src_h;
        w = h * 3 / 2;
    }
    if (w <= 0 || h <= 0)
    {
        return;
    }

    if (x0 < 0)
    {
        x0 = 0;
    }
    if (y0 < 0)
    {
        y0 = 0;
    }
    if (x0 + w > src_w)
    {
        x0 = src_w - w;
    }
    if (y0 + h > src_h)
    {
        y0 = src_h - h;
    }

    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGW(TAG, "历史记录：获取互斥锁超时，丢弃本条");
        return;
    }

    uint32_t slot = s_history_seq % FACE_HISTORY_MAX;
    uint16_t* dst = s_history_img[slot];

    /* 最近邻缩放；像素保持摄像头原始字节序，LVGL(16_SWAP) 可直接显示 */
    for (int dy = 0; dy < FACE_HISTORY_H; dy++)
    {
        const uint16_t* src_row = src + (y0 + dy * h / FACE_HISTORY_H) * src_w;
        uint16_t* dst_row = dst + dy * FACE_HISTORY_W;
        for (int dx = 0; dx < FACE_HISTORY_W; dx++)
        {
            dst_row[dx] = src_row[x0 + dx * w / FACE_HISTORY_W];
        }
    }

    snprintf(s_history_names[slot], sizeof(s_history_names[slot]), "%s",
             (name != NULL && name[0] != '\0') ? name : "未知");
    float sim = similarity * 100.0f;
    s_history_sim[slot] = (uint8_t)((sim < 0.0f) ? 0.0f : (sim > 255.0f ? 255.0f : sim));
    s_history_epoch[slot] = (uint32_t)time_sync_epoch(); /* 未校时写入 0，上传前补当前时间 */
    s_history_flags[slot] = 0;                            /* 新记录尚未上传 */

    s_history_entry_seq[slot] = s_history_seq; /* 该槽位当前记录的写入序号 */
    s_history_seq++;
    if (s_history_count < FACE_HISTORY_MAX)
    {
        s_history_count++;
    }

    uint32_t record_seq = s_history_entry_seq[slot];
    uint32_t record_epoch = s_history_epoch[slot];
    xSemaphoreGive(s_history_mutex);

    /* 写文件放在互斥锁之外：写 Flash 的时间较长，不应阻塞界面读取 */
    bool saved = history_save_file(slot);

    ESP_LOGI(TAG, "历史记录已保存：%s（%d%%，序号 %u，槽位 %u，写文件%s，时间 %s）",
             s_history_names[slot], (int)s_history_sim[slot], (unsigned)record_seq,
             (unsigned)slot, saved ? "成功" : "失败",
             record_epoch ? "已记录" : "未校时");

    /* 交给巴法云上传任务（后台逐张处理，失败自动重试；未联网时排队等待） */
    bemfa_uploader_enqueue(record_seq, record_epoch, s_history_names[slot],
                           (int)s_history_sim[slot]);
}

extern "C" int human_face_history_count(void)
{
    int n = 0;

    if (!s_history_ready)
    {
        return 0;
    }
    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return 0;
    }

    n = s_history_count;
    xSemaphoreGive(s_history_mutex);
    return n;
}

/**
 * @brief 把槽位照片最近邻缩放到调用者缓冲
 * @note 调用者必须已持有 s_history_mutex
 */
static void history_scale_slot(int slot, uint16_t* dst, int dst_w, int dst_h)
{
    const uint16_t* src = s_history_img[slot];

    for (int dy = 0; dy < dst_h; dy++)
    {
        const uint16_t* src_row = src + (dy * FACE_HISTORY_H / dst_h) * FACE_HISTORY_W;
        uint16_t* dst_row = dst + dy * dst_w;
        for (int dx = 0; dx < dst_w; dx++)
        {
            dst_row[dx] = src_row[dx * FACE_HISTORY_W / dst_w];
        }
    }
}

extern "C" int human_face_history_image(int index, uint16_t* dst, int dst_w, int dst_h)
{
    int ok = 0;

    if (!s_history_ready || dst == NULL || dst_w <= 0 || dst_h <= 0)
    {
        return 0;
    }
    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return 0;
    }

    /* 索引 0 为最新一条：按写入序号从大到小定位槽位 */
    int slot = history_slot_for_index(index);
    if (slot >= 0)
    {
        history_scale_slot(slot, dst, dst_w, dst_h);
        ok = 1;
    }

    xSemaphoreGive(s_history_mutex);
    return ok;
}

extern "C" int human_face_history_get_meta(int index, human_face_history_meta_t* meta)
{
    int ok = 0;

    if (!s_history_ready || meta == NULL)
    {
        return 0;
    }
    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return 0;
    }

    int slot = history_slot_for_index(index);
    if (slot >= 0)
    {
        memset(meta, 0, sizeof(*meta));
        meta->seq = s_history_entry_seq[slot];
        meta->epoch = s_history_epoch[slot];
        meta->sim = s_history_sim[slot];
        meta->uploaded = (s_history_flags[slot] & FACE_HISTORY_FLAG_UPLOADED) ? 1 : 0;
        snprintf(meta->name, sizeof(meta->name), "%s", s_history_names[slot]);
        ok = 1;
    }

    xSemaphoreGive(s_history_mutex);
    return ok;
}

extern "C" int human_face_history_read_seq(uint32_t seq, uint16_t* dst, int dst_w, int dst_h)
{
    int ok = 0;

    if (!s_history_ready || dst == NULL || dst_w <= 0 || dst_h <= 0)
    {
        return 0;
    }
    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return 0;
    }

    for (int slot = 0; slot < FACE_HISTORY_MAX; slot++)
    {
        if (s_history_entry_seq[slot] == seq)
        {
            history_scale_slot(slot, dst, dst_w, dst_h);
            ok = 1;
            break;
        }
    }

    xSemaphoreGive(s_history_mutex);
    return ok;
}

extern "C" int human_face_history_mark_uploaded(uint32_t seq)
{
    char path[64];
    history_header_t hdr;
    int slot = -1;
    int ok = 1;

    if (!s_history_ready || !s_avatar_fs_ready)
    {
        return 0;
    }
    if (xSemaphoreTake(s_history_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return 0;
    }

    for (int i = 0; i < FACE_HISTORY_MAX; i++)
    {
        if (s_history_entry_seq[i] == seq)
        {
            slot = i;
            break;
        }
    }

    /* 记录已被新识别覆盖：无需再标记（视作完成） */
    if (slot < 0 || (s_history_flags[slot] & FACE_HISTORY_FLAG_UPLOADED))
    {
        xSemaphoreGive(s_history_mutex);
        return 1;
    }

    s_history_flags[slot] |= FACE_HISTORY_FLAG_UPLOADED;
    xSemaphoreGive(s_history_mutex);

    /* 只改写 48 字节文件头，不暂停采集：上传是低频事件，最多丢失一帧预览 */
    snprintf(path, sizeof(path), FACE_HISTORY_DIR "/%d.bin", slot);
    FILE* f = fopen(path, "r+b");
    if (f == NULL)
    {
        ESP_LOGW(TAG, "[HIST] 无法打开文件回写上传标记: %s", path);
        return 0;
    }

    if ((fread(&hdr, sizeof(hdr), 1, f) == 1) &&
        (hdr.magic == FACE_HISTORY_MAGIC) && (hdr.seq == seq))
    {
        hdr.flags |= FACE_HISTORY_FLAG_UPLOADED;
        ok = (fseek(f, 0, SEEK_SET) == 0) && (fwrite(&hdr, sizeof(hdr), 1, f) == 1);
    }
    fclose(f);

    if (!ok)
    {
        ESP_LOGW(TAG, "[HIST] 上传标记写入失败: %s", path);
    }
    return ok;
}

// =================================================================
// 人脸模板自学习（识别成功的高置信样本拟合进模板）
// =================================================================

/**
 * @brief 把本次识别成功的特征拟合进该人脸模板
 *
 * 仅当相似度 >= max(用户阈值, 90%) 时才学习，避免擦边样本把模板带偏；
 * 新模板 = normalize(旧模板 × n + 本次特征)，其中旧模板按 n 个样本计权：
 *   已完成 f 次学习时，n = f + 1（首次学习时新旧各占一半）。
 * f 最多累加到 FACE_FUSION_MAX 次，达到上限后仍按固定小权重更新，
 * 新数据会逐渐替代旧数据，模板不会无限“记忆”早期样本。
 *
 * @param id         识别结果的人脸 ID
 * @param name       识别结果的人脸姓名（作为自学习次数的索引）
 * @param similarity 本次识别的相似度（0~1）
 * @return true 表示模板已被更新（调用者需随后调用 face_db_save 落盘）
 * @note 调用者必须已持有 s_face_db_mutex，并且已暂停摄像头采集
 */
static bool face_template_learn(int id, const char* name, float similarity)
{
    float gate;
    int dim;
    int fused;
    float w_new;
    float w_old;
    float norm = 0.0f;

    if (!s_self_learn_enabled || recognizer == NULL || name == NULL)
    {
        return false;
    }

    gate = (float)s_face_thresh_percent / 100.0f;
    if (gate < FACE_LEARN_MIN_SIM)
    {
        gate = FACE_LEARN_MIN_SIM;
    }
    if (similarity < gate)
    {
        return false; /* 擦边样本不学习 */
    }

    /* get_face_emb(-1) 为本次输入特征，get_face_emb(id) 为该ID的模板 */
    dl::Tensor<float>& in_emb = recognizer->get_face_emb(-1);
    dl::Tensor<float>& id_emb = recognizer->get_face_emb(id);

    if (in_emb.element == NULL || id_emb.element == NULL ||
        in_emb.shape.size() == 0 || id_emb.shape.size() == 0 ||
        in_emb.shape[0] != id_emb.shape[0])
    {
        return false;
    }

    dim = id_emb.shape[0];
    if (dim <= 0 || dim > FACE_DB_MAX_DIM)
    {
        return false;
    }

    if (s_learn_buf == NULL)
    {
        s_learn_buf = (float*)heap_caps_malloc(FACE_DB_MAX_DIM * sizeof(float),
                                               MALLOC_CAP_SPIRAM);
        if (s_learn_buf == NULL)
        {
            ESP_LOGW(TAG, "[LEARN] 自学习缓冲分配失败，跳过本次学习");
            return false;
        }
    }

    /* 诊断 + 安全校验：输入特征与模板的余弦相似度应当与 recognize() 返回的
       相似度一致；若不一致说明取到的“输入特征”并不是本次识别用的特征，
       此时宁可不学习，也不能把错误特征拟合进模板 */
    {
        float cos = face_recognition_tool::cos_distance(in_emb, id_emb, true, 1);

        ESP_LOGI(TAG, "[LEARN] 输入/模板余弦相似度 %.3f（识别结果 %.3f）",
                 cos, similarity);

        if (fabsf(cos - similarity) > 0.05f)
        {
            ESP_LOGW(TAG, "[LEARN] 特征来源校验不一致，跳过本次学习");
            return false;
        }
    }

    fused = 0;
    auto it = s_fusion_counts.find(std::string(name));
    if (it != s_fusion_counts.end())
    {
        fused = it->second;
    }
    if (fused > FACE_FUSION_MAX)
    {
        fused = FACE_FUSION_MAX;
    }

    /* 已学习 fused 次时旧模板相当于 fused+1 个样本，本次样本权重 1/(fused+2) */
    w_new = 1.0f / (float)(fused + 2);
    w_old = 1.0f - w_new;

    for (int i = 0; i < dim; i++)
    {
        s_learn_buf[i] = id_emb.element[i] * w_old + in_emb.element[i] * w_new;
        norm += s_learn_buf[i] * s_learn_buf[i];
    }

    norm = sqrtf(norm);
    if (norm < 1e-6f)
    {
        return false;
    }

    for (int i = 0; i < dim; i++)
    {
        id_emb.element[i] = s_learn_buf[i] / norm;
    }

    fused = (fused < FACE_FUSION_MAX) ? (fused + 1) : FACE_FUSION_MAX;
    s_fusion_counts[std::string(name)] = (uint8_t)fused;

    ESP_LOGI(TAG, "[LEARN] 模板已拟合：%s（累计 %d 次，本次权重 %.3f）",
             name, fused, w_new);
    return true;
}

/**
 * @brief 执行 UI 投递的人脸库变更请求（在 AI 任务中调用）
 *
 * 流程：暂停采集并清空流水线 → 加特征库锁 → 改库 / 写人脸库文件 → 删头像 → 恢复采集。
 * 完成后置结果并给出 s_db_op_done，唤醒等待中的 UI 线程。
 */
static void process_db_request(void)
{
    int kind = s_db_op_kind;
    int id = s_db_op_id;
    int ok = 0;

    s_db_op_kind = 0; /* 先清标志，允许 UI 在完成后立即投递下一次请求 */

    capture_pause_and_drain();

    if (xSemaphoreTake(s_face_db_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        if (kind == 2)
        {
            std::vector<face_info_t> infos = recognizer->get_enrolled_ids();
            for (auto& info : infos)
            {
                recognizer->delete_id(info.id, false);
                avatar_delete(info.id);
            }
            ok = face_db_save() ? 1 : 0;
            ESP_LOGI(TAG, "信息库清空完成：移除 %d 个人脸（写文件%s）",
                     (int)infos.size(), ok ? "成功" : "失败");
        }
        else if (kind == 1 && id >= 0)
        {
            int remained = recognizer->delete_id(id, false);
            if (remained >= 0)
            {
                avatar_delete(id);
                ok = face_db_save() ? 1 : 0;
                ESP_LOGI(TAG, "信息库删除完成：ID %d，剩余 %d 人（写文件%s）",
                         id, remained, ok ? "成功" : "失败");
            }
            else
            {
                ESP_LOGW(TAG, "信息库删除失败：未找到 ID %d", id);
            }
        }
        xSemaphoreGive(s_face_db_mutex);
    }
    else
    {
        ESP_LOGW(TAG, "信息库操作：获取特征库锁超时");
    }

    capture_resume();
    s_db_op_result = ok;
    if (s_db_op_done != NULL)
    {
        xSemaphoreGive(s_db_op_done);
    }
}

/**
 * @brief 投递人脸库变更请求并等待完成
 * @param kind 1=删除单人，2=清空全部
 * @param id   kind==1 时要删除的人脸 ID
 * @return 1 成功；0 失败（未就绪、忙或超时）
 */
static int db_op_request(int kind, int id)
{
    if (s_db_op_done == NULL || s_db_op_kind != 0)
    {
        return 0;
    }

    s_db_op_id = id;
    s_db_op_result = 0;
    s_db_op_kind = kind;

    if (xSemaphoreTake(s_db_op_done, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        s_db_op_kind = 0;
        ESP_LOGW(TAG, "信息库操作超时（kind=%d）", kind);
        return 0;
    }

    return s_db_op_result;
}

extern "C" int human_face_db_delete(int id)
{
    return db_op_request(1, id);
}

extern "C" int human_face_db_clear(void)
{
    return db_op_request(2, -1);
}

// =================================================================
// 应用初始化函数（app_human_face_recognition）
// =================================================================

/**
 * @brief 初始化并启动人脸识别应用
 *
 * 该函数创建所有人脸识别系统所需的同步原语和FreeRTOS任务。
 * 调用该函数前必须先完成摄像头和LCD的硬件初始化。
 *
 * 初始化顺序：
 * 1. 创建按键事件互斥锁 → 失败则退出
 * 2. 创建LCD帧队列（长度2）→ 用于AI→LCD的数据传递
 * 3. 创建AI帧队列（长度2）→ 用于摄像头→AI的数据传递
 * 4. 创建摄像头采集任务 → 绑定到核心1（协议核心）
 * 5. 创建LCD显示任务 → 绑定到核心1（与AI推理并行）
 * 6. 创建AI处理任务 → 绑定到核心0，栈8KB
 * 7. 创建按键检测任务 → 绑定到核心0，栈4KB
 *
 * @return 无返回值
 *
 * @note 互斥锁创建失败时直接返回，不启动任何任务
 * @note 摄像头初始化应在调用此函数之前完成
 * @note LCD初始化应在调用此函数之前完成
 * @note 队列深度为2，适合3级流水线（采集→处理→显示）工作模式
 * @note 核心分配策略：
 *   - 核心1：摄像头采集（I/O密集型）、LCD显示（SPI + 内存拷贝）
 *   - 核心0：AI计算、按键检测（CPU密集型）
 */
void app_human_face_recognition(void)
{
    // ---- 0. 挂载头像分区（在采集任务启动前完成，避免与摄像头 DMA 冲突）----
    avatar_fs_mount();

    // ---- 0b. 分配识别历史记录缓冲（PSRAM，最多保留最近 10 条）----
    history_init();

    /* 诊断：打印 vfs 分区容量与剩余空间，便于判断“写满/空间不足” */
    if (s_avatar_fs_ready)
    {
        uint64_t total = 0;
        uint64_t free_bytes = 0;

        if (esp_vfs_fat_info("/vfs", &total, &free_bytes) == ESP_OK)
        {
            ESP_LOGI(TAG, "[FS] vfs 容量 %llu KB，剩余 %llu KB",
                     (unsigned long long)(total / 1024),
                     (unsigned long long)(free_bytes / 1024));
        }
    }

    // ---- 1. 创建按键互斥锁 ----

    /**
     * 创建互斥锁，用于保护按键事件全局变量的并发访问
     * 互斥锁相比二值信号量具有优先级继承特性，
     * 可防止优先级反转问题
     */
    button_mutex = xSemaphoreCreateMutex();

    // 检查互斥锁是否创建成功
    if (button_mutex == NULL)
    {
        /**
         * 互斥锁创建失败，系统资源不足
         * 打印错误日志并退出初始化
         * 此时系统无法保证按键事件的线程安全
         */
        ESP_LOGE(TAG, "创建按键互斥锁失败！");
        return;
    }

    // ---- 1b. 创建LCD信息互斥锁 ----

    /**
     * 创建互斥锁，用于保护LCD关键信息显示变量的并发访问
     * 在task_process_ai（写）和task_process_lcd（读/写）之间提供同步
     */
    lcd_info_mutex = xSemaphoreCreateMutex();

    // 检查互斥锁是否创建成功
    if (lcd_info_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建LCD信息互斥锁失败！");
        return;
    }

    // ---- 1c. 创建人脸特征库互斥锁 ----

    /**
     * 创建互斥锁，用于保护识别器人脸列表的并发访问
     * 在task_process_ai（注册/清空）与人脸信息库界面（读取）之间提供同步
     */
    s_face_db_mutex = xSemaphoreCreateMutex();

    if (s_face_db_mutex == NULL)
    {
        ESP_LOGE(TAG, "创建人脸特征库互斥锁失败！");
        return;
    }

    /**
     * 人脸库变更完成信号：UI 线程投递删除/清空请求后在此等待 AI 任务完成。
     * 创建失败只影响信息库界面的删除功能，仍继续启动其余任务。
     */
    s_db_op_done = xSemaphoreCreateBinary();
    if (s_db_op_done == NULL)
    {
        ESP_LOGE(TAG, "创建人脸库变更信号失败，信息库删除功能不可用");
    }

    // ---- 2. 创建消息队列 ----

    /**
     * 创建LCD帧队列
     * 队列长度：2个元素
     * 元素大小：sizeof(camera_fb_t *) = 4字节（指针）
     * 作用：AI处理任务 → LCD显示任务 的数据传递通道
     * 深度2可以缓冲1帧，应对短暂的显示延迟
     */
    xQueueLCDFrame = xQueueCreate(2, sizeof(camera_fb_t*));

    /**
     * 创建AI帧队列
     * 队列长度：2个元素
     * 元素大小：sizeof(camera_fb_t *) = 4字节（指针）
     * 作用：摄像头采集任务 → AI处理任务 的数据传递通道
     * 深度2可以缓冲1帧，平滑处理帧率波动
     */
    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t*));

    // ---- 3. 创建FreeRTOS任务 ----

    /**
     * 创建摄像头采集任务
     * 任务名：     "task_process_camera"
     * 函数：      task_process_camera
     * 栈大小：    3KB（足够用于帧获取和队列发送）
     * 优先级：    5（较高优先级，确保帧数据及时采集）
     * 绑定核心：  1（协议核心，与CPU密集型任务分离）
     * 任务参数：  NULL
     * 任务句柄：  NULL（无需挂起任务）
     */
    // 摄像头采集任务与 LCD 显示任务在 AI 任务之后创建，保证 AI 任务能拿到连续的内部 RAM

    /**
     * 创建LCD显示任务
     * 任务名：     "task_process_lcd"
     * 函数：      task_process_lcd
     * 栈大小：    4KB（足够用于SPI数据传输）
     * 优先级：    5（与摄像采集任务同级）
     * 绑定核心：  1（与核心0上的AI推理并行，避免AI耗时与LCD耗时串行累加）
     * 任务句柄：  NULL（无需挂起任务）
     */
    // 摄像头采集任务与 LCD 显示任务在 AI 任务之后创建，保证 AI 任务能拿到连续的内部 RAM

    /**
     * 创建AI人脸检测与识别任务
     * 任务名：     "task_process_ai"
     * 函数：      task_process_ai
     * 栈大小：    8KB（满足神经网络推理与图像处理需求）
     * 优先级：    5（核心任务，最高优先级组）
     * 绑定核心：  0（应用核心，与摄像头采集任务分离）
     * 任务句柄：  NULL
     */
    TaskHandle_t ai_task_handle = NULL;

    // 先打印创建任务前的内部 RAM 余量，便于判断任务创建失败是否由内存不足引起
    ESP_LOGI(TAG, "[DBG] internal heap free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /**
     * 创建 AI 任务启动闸门
     * AI 任务会先阻塞在这把锁上，等下面所有任务都创建完成再放行，
     * 避免模型内存分配与任务栈分配同时抢内部 RAM
     */
    s_ai_boot_sem = xSemaphoreCreateBinary();
    if (s_ai_boot_sem == NULL)
    {
        ESP_LOGE(TAG, "[DBG] create AI boot semaphore FAILED");
    }

    /* 模型就绪信号：用于释放模型下沉时占用的内部 RAM */
    s_ai_ready_sem = xSemaphoreCreateBinary();
    if (s_ai_ready_sem == NULL)
    {
        ESP_LOGW(TAG, "[MEM] 模型就绪信号创建失败，模型下沉功能不可用");
    }

    /**
     * 注意：AI 任务必须最先创建。
     * 它需要 8KB 连续内部 RAM，若放在摄像头/LCD/按键任务之后创建，
     * 内部 RAM 已被前面的任务栈和 DMA 缓冲切碎，容易申请失败（任务不运行，
     * 帧会全部堆在队列里，表现为摄像头 4 秒超时、画面不显示）。
     */
    xTaskCreatePinnedToCore(task_process_ai, "task_process_ai", 8 * 1024, NULL, 5,
                            &ai_task_handle, 0);
    if (ai_task_handle == NULL)
    {
        ESP_LOGE(TAG, "[DBG] create AI task FAILED, no internal RAM");
    }
    else
    {
        ESP_LOGI(TAG, "[DBG] AI task created");
    }

    // ---- 摄像头采集任务与 LCD 显示任务（在 AI 任务之后创建） ----

    /**
     * 创建按键检测任务
     * 任务名：     "button_task"
     * 函数：      button_task
     * 栈大小：    3KB（从原2KB增加，解决栈溢出；再降到4KB以下以适配剩余内部RAM）
     * 优先级：    6（高于 AI 任务 5，保证 10ms 采样不被打断；单次只跑几十微秒）
     * 绑定核心：  0（应用核心）
     * 说明：      必须紧跟 AI 任务创建。内部 RAM 碎片化严重时，排在摄像头/LCD
     *             任务之后创建会因找不到连续块而失败（任务不运行，按键完全无反应）
     */
    TaskHandle_t button_task_handle = NULL;
    xTaskCreatePinnedToCore(button_task, "button_task", 3 * 1024, NULL, 6, &button_task_handle, 0);
    if (button_task_handle == NULL)
    {
        ESP_LOGE(TAG, "[DBG] create button task FAILED, free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
    else
    {
        ESP_LOGI(TAG, "[DBG] button task created");
    }

    TaskHandle_t cam_task_handle = NULL;
    xTaskCreatePinnedToCore(task_process_camera, "task_process_camera", 3 * 1024, NULL, 5, &cam_task_handle, 1);
    if (cam_task_handle == NULL)
    {
        ESP_LOGE(TAG, "[DBG] create camera task FAILED, free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    TaskHandle_t lcd_task_handle = NULL;
    xTaskCreatePinnedToCore(task_process_lcd, "task_process_lcd", 4 * 1024, NULL, 5, &lcd_task_handle, 1);
    if (lcd_task_handle == NULL)
    {
        ESP_LOGE(TAG, "[DBG] create LCD task FAILED, free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    /**
     * 创建“开门 3 秒后自动锁门”的单次定时器
     * 放在所有任务创建之后，避免小内存块加剧内部 RAM 碎片，
     * 影响 AI 任务 8KB 连续栈的申请
     */
    const esp_timer_create_args_t door_timer_args = {
        .callback = door_lock_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "door_lock",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&door_timer_args, &s_door_lock_timer) != ESP_OK)
    {
        s_door_lock_timer = NULL;
        ESP_LOGE(TAG, "创建自动锁门定时器失败，开门后将不会自动锁门");
    }

    /**
     * 注册“登入”按钮按下回调：按钮本身由 LVGL 创建并处理触摸，
     * 这里只把“按下”事件接到业务状态切换上
     */
    lvgl_ui_set_login_cb(on_login_button_pressed);

    /* “识别”按钮：与 KEY2 单击完全等价（投递一个单击事件给 AI 任务） */
    lvgl_ui_set_recognize_cb(on_recognize_button_pressed);

    /* 管理员界面“录入”按钮：切纯摄像头画面并投递一次长按事件开始录入 */
    lvgl_ui_set_enroll_cb(on_enroll_button_pressed);

    /* 注册登入页“退出”按钮回调：退出后恢复摄像头与按键控制 */
    lvgl_ui_set_exit_cb(on_login_exit);

    /**
     * 模型下沉 PSRAM：
     * esp-dl 申请模型张量时“优先内部 RAM，失败后才回退 PSRAM”。这里在放行
     * AI 任务前临时占住一块内部 RAM（分 8KB 申请以避开碎片），迫使模型张量
     * 落到 PSRAM；模型创建完成后立即释放，把内部 RAM 留给常驻 Wi-Fi。
     */
    if (s_ai_ready_sem != NULL)
    {
        for (int i = 0; i < MODEL_PSRAM_BLOCK_MAX_CHUNKS; i++)
        {
            void *p = heap_caps_malloc(MODEL_PSRAM_BLOCK_CHUNK,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

            if (p == NULL)
            {
                break;
            }
            s_model_psram_blocks[s_model_psram_block_count++] = p;
        }

        if (s_model_psram_block_count > 0)
        {
            ESP_LOGI(TAG, "[MEM] 模型下沉：临时占用内部 RAM %d KB",
                     s_model_psram_block_count * (int)(MODEL_PSRAM_BLOCK_CHUNK / 1024));
        }
        else
        {
            ESP_LOGW(TAG, "[MEM] 模型下沉占位申请失败，模型仍优先使用内部 RAM");
        }
    }

    // 所有任务已创建完毕，放行 AI 任务开始分配模型内存
    if (s_ai_boot_sem != NULL)
    {
        xSemaphoreGive(s_ai_boot_sem);
    }

    /* 等模型创建完成后释放占位内存（超时也会释放，避免永久占用） */
    if (s_ai_ready_sem != NULL && s_model_psram_block_count > 0)
    {
        if (xSemaphoreTake(s_ai_ready_sem, pdMS_TO_TICKS(15000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "[MEM] 等待模型创建超时，仍释放占位内存");
        }

        for (int i = 0; i < s_model_psram_block_count; i++)
        {
            heap_caps_free(s_model_psram_blocks[i]);
            s_model_psram_blocks[i] = NULL;
        }
        s_model_psram_block_count = 0;

        ESP_LOGI(TAG, "[MEM] 释放占位后 internal free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    // 打印应用启动完成日志
    ESP_LOGI(TAG, "人脸识别应用已启动");
}

extern "C" int human_face_get_threshold(void)
{
    return s_face_thresh_percent;
}

extern "C" void human_face_set_threshold(int percent)
{
    nvs_handle_t handle;
    esp_err_t ret;

    if (percent < 50)
    {
        percent = 50;
    }
    else if (percent > 95)
    {
        percent = 95;
    }

    if (s_face_thresh_percent == percent)
    {
        return; /* 值未变化就不写 Flash */
    }

    s_face_thresh_percent = percent;

    /* 识别器由 AI 任务创建，界面可能在它创建之前就调用这里 */
    if (recognizer != NULL)
    {
        recognizer->set_thresh((float)percent / 100.0f);
    }

    ret = nvs_open(FACE_CFG_NVS_NS, NVS_READWRITE, &handle);
    if (ret == ESP_OK)
    {
        ret = nvs_set_u8(handle, FACE_CFG_NVS_THRESH, (uint8_t)percent);
        if (ret == ESP_OK)
        {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "保存相似度阈值失败，ret=%d", (int)ret);
    }
}

extern "C" int human_face_get_self_learn(void)
{
    return s_self_learn_enabled;
}

extern "C" void human_face_set_self_learn(int enable)
{
    nvs_handle_t handle;
    esp_err_t ret;
    int value = enable ? 1 : 0;

    if (s_self_learn_enabled == value)
    {
        return; /* 值未变化就不写 Flash */
    }

    s_self_learn_enabled = value;

    ret = nvs_open(FACE_CFG_NVS_NS, NVS_READWRITE, &handle);
    if (ret == ESP_OK)
    {
        ret = nvs_set_u8(handle, FACE_CFG_NVS_LEARN, (uint8_t)value);
        if (ret == ESP_OK)
        {
            ret = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "保存模板自学习开关失败，ret=%d", (int)ret);
    }
}

extern "C" int human_face_list_enrolled(int *ids, char names[][32], int max_count, int *total)
{
    int n = 0;
    std::vector<face_info_t> infos;

    if (total != NULL)
    {
        *total = 0;
    }

    if (recognizer == NULL || s_face_db_mutex == NULL || max_count <= 0)
    {
        return 0;
    }

    /* 可能正在写 Flash（注册/清空），拿锁超时后返回空列表，界面显示“暂无” */
    if (xSemaphoreTake(s_face_db_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return 0;
    }

    infos = recognizer->get_enrolled_ids();
    if (total != NULL)
    {
        *total = (int)infos.size();
    }

    for (size_t i = 0; i < infos.size() && n < max_count; i++)
    {
        if (ids != NULL)
        {
            ids[n] = infos[i].id;
        }
        snprintf(names[n], 32, "%s", infos[i].name.c_str());
        n++;
    }

    xSemaphoreGive(s_face_db_mutex);
    return n;
}
