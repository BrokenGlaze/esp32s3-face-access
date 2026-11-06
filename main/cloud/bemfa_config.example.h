/**
 * @file    bemfa_config.example.h
 * @brief   巴法云账号与主题配置模板
 *
 * @note    使用方法：
 *          1. 复制本文件为 bemfa_config.h：cp bemfa_config.example.h bemfa_config.h
 *          2. 把 YOUR_BEMFA_UID 替换为巴法云控制台的私钥（bemfa.com 注册后可见）
 *          3. MQTT 主题需在控制台创建（设备类型码 004 = 传感器），
 *             图片主题需在控制台"图片主题"中创建
 *          4. 微信通知需在巴法云控制台绑定微信（关注公众号"巴法云"）
 *
 *          bemfa_config.h 含私钥，已加入 .gitignore，不会被提交。
 */

#ifndef __BEMFA_CONFIG_H__
#define __BEMFA_CONFIG_H__

#define BEMFA_UID          "YOUR_BEMFA_UID"
#define BEMFA_MQTT_TOPIC   "door004"       /* MQTT 设备主题（在线/通知） */
#define BEMFA_IMAGE_TOPIC  "facehistory"   /* 图片主题（识别照片上传） */
#define BEMFA_MQTT_URI     "mqtt://bemfa.com:9501"

/* 微信通知：控制台需先绑定微信（关注公众号"巴法云"） */
#define BEMFA_WECHAT_URL    "http://apis.bemfa.com/vb/wechat/v1/wechatAlertJson"
#define BEMFA_WECHAT_DEVICE "门禁"    /* 通知中显示的设备名 */
#define BEMFA_WECHAT_GROUP  "access"  /* 分组标识，仅限字母数字 */

#endif
