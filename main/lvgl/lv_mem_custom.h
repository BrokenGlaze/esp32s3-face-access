/**
 * @file    lv_mem_custom.h
 * @brief   LVGL 内存池改用 PSRAM（由 lv_conf 的 LV_MEM_CUSTOM_INCLUDE 引入）
 *
 * @note    32KB 内部 RAM 堆在多页面 UI 下会被圆角遮罩等临时缓冲耗尽，
 *          绘制时分配失败会触发 LVGL 断言并卡死 LVGL 任务（看门狗）。
 *          这里把 lv_mem 的分配全部落到 PSRAM，内部 RAM 只留 DMA/协议栈。
 *          路径为相对 lvgl 组件 lv_conf_internal.h 的引用，改动请保持层级。
 */

#ifndef LV_MEM_CUSTOM_PSRAM_H
#define LV_MEM_CUSTOM_PSRAM_H

#include "esp_heap_caps.h"

/* lv_conf_internal.h 已给出默认 malloc/free/realloc，这里覆盖为 PSRAM 版本 */
#undef LV_MEM_CUSTOM_ALLOC
#undef LV_MEM_CUSTOM_FREE
#undef LV_MEM_CUSTOM_REALLOC

#define LV_MEM_CUSTOM_ALLOC(size)     heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE(p)         heap_caps_free(p)
#define LV_MEM_CUSTOM_REALLOC(p, s)   heap_caps_realloc((p), (s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#endif
