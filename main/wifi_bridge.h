#ifndef WIFI_BRIDGE_H
#define WIFI_BRIDGE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Bitle <-> wt channel bridge (方案 C, 轻量私有协议).
 *
 * 节点通过 WiFi STA 常驻连接 VPS 上的同步中转服务器(wt-bridge)，实现本地
 * BLE mesh 与 wt 聊天频道的双向同步：
 *   - 上行: mesh 内 peer 发给本节点的私信 -> 上报到 wt 频道
 *   - 下行: wt 频道其他成员的消息 -> 由 noise 层广播成私信注入本地 mesh
 *
 * 协议帧见 wt-bridge/src/main.rs：明文 TCP，magic 0x42 0x54。
 * ------------------------------------------------------------------------ */

/* 连接参数（可改为 NVS / 私信配置）。 */
#define WIFI_BRIDGE_SSID        "B408-1000"
#define WIFI_BRIDGE_PASS        "17533305771"
#define WIFI_BRIDGE_HOST        "3.17.63.33"
#define WIFI_BRIDGE_PORT        8099
#define WIFI_BRIDGE_WT          "#wt"        /* 订阅的 wt 频道（BitChat Nostr #<geohash> 频道编码） */

/* 下行消息注入 mesh 时，私信正文每行可携带的最长本地缓存。 */
#define WIFI_BRIDGE_CONTENT_MAX 128

/* 初始化 WiFi + 连接中转服务器（常驻）。 */
esp_err_t wifi_bridge_init(void);

/* 周期驱动：维护 WiFi/TCP 连接、发送上行队列、接收下行帧。 */
void wifi_bridge_poll(void);

/* 当前是否已连上中转服务器（TCP）。 */
bool wifi_bridge_connected(void);

/* 上行：mesh -> wt。sender_id 为本地 peer_id，sender_name 为发送者昵称。
 * 若未连接，会进入待发队列等待。 */
void wifi_bridge_send_up(const uint8_t *sender_id, const char *sender_name,
                         const char *wt, const char *content);

/* 注册下行回调：收到 wt 消息时调用（由 noise 层实现 mesh 广播注入）。 */
void wifi_bridge_set_down_cb(int (*cb)(const char *sender_name, const char *wt,
                                        const char *content));

#ifdef __cplusplus
}
#endif

#endif // WIFI_BRIDGE_H