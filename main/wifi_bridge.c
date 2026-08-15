#include "wifi_bridge.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>

#define TAG "wifi_bridge"

/* 轻量私有协议帧：magic(2) + len(2 BE) + type(1) + payload */
#define MAGIC0 0x42
#define MAGIC1 0x54
#define FRAME_HEADER 5u

#define T_HELLO   0x01
#define T_MSG_UP  0x02
#define T_MSG_DOWN 0x03
#define T_PING    0x04
#define T_PONG    0x05

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define UP_QUEUE_MAX 8

typedef struct {
    bool used;
    uint8_t sender_id[8];
    char name[16];
    char wt[16];
    char content[WIFI_BRIDGE_CONTENT_MAX];
    size_t content_len;
} up_entry_t;

static EventGroupHandle_t s_wifi_events;
static int s_sock = -1;
static bool s_down_cb_set;
static void (*s_down_cb)(const char *sender_name, const char *wt, const char *content);
static up_entry_t s_up_queue[UP_QUEUE_MAX];

static void send_hello(void);
static void pump_up(void);
static void try_recv(void);
static void connect_tcp(void);

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "STA disconnected");
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            if (s_sock >= 0) {
                close(s_sock);
                s_sock = -1;
            }
            /* 稍后自动重连 */
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_bridge_init(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_up_queue, 0, sizeof(s_up_queue));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wc = {
        .sta = {
            .ssid = WIFI_BRIDGE_SSID,
            .password = WIFI_BRIDGE_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA starting (SSID=%s, wt=%s)", WIFI_BRIDGE_SSID, WIFI_BRIDGE_WT);
    return ESP_OK;
}

void wifi_bridge_set_down_cb(void (*cb)(const char *, const char *, const char *))
{
    s_down_cb = cb;
    s_down_cb_set = (cb != NULL);
}

static void send_hello(void)
{
    /* peer_id[8] + nl + name + wtl + wt */
    uint8_t peer_id[8];
    extern const uint8_t *noise_get_local_peer_id(void);
    extern const char *noise_get_local_nickname(void);
    memcpy(peer_id, noise_get_local_peer_id(), 8);

    const char *name = noise_get_local_nickname();
    size_t name_len = strnlen(name, 15);
    size_t wt_len = strlen(WIFI_BRIDGE_WT);

    uint8_t frame[8 + 1 + 15 + 1 + 15 + FRAME_HEADER];
    size_t o = 0;
    frame[o++] = MAGIC0;
    frame[o++] = MAGIC1;
    size_t payload_len = 8 + 1 + name_len + 1 + wt_len;
    frame[o++] = (uint8_t)(payload_len >> 8);
    frame[o++] = (uint8_t)(payload_len & 0xFF);
    frame[o++] = T_HELLO;
    memcpy(frame + o, peer_id, 8);
    o += 8;
    frame[o++] = (uint8_t)name_len;
    memcpy(frame + o, name, name_len);
    o += name_len;
    frame[o++] = (uint8_t)wt_len;
    memcpy(frame + o, WIFI_BRIDGE_WT, wt_len);
    o += wt_len;

    if (send(s_sock, frame, o, 0) < 0) {
        ESP_LOGW(TAG, "HELLO send failed");
    }
}

static void connect_tcp(void)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WIFI_BRIDGE_PORT);
    if (!inet_aton(WIFI_BRIDGE_HOST, &addr.sin_addr)) {
        ESP_LOGE(TAG, "bad host %s", WIFI_BRIDGE_HOST);
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "TCP connect to %s:%d failed", WIFI_BRIDGE_HOST, WIFI_BRIDGE_PORT);
        close(fd);
        return;
    }
    s_sock = fd;
    ESP_LOGI(TAG, "TCP connected to %s:%d", WIFI_BRIDGE_HOST, WIFI_BRIDGE_PORT);
    send_hello();
}

static void pump_up(void)
{
    if (s_sock < 0) {
        return;
    }
    for (int i = 0; i < UP_QUEUE_MAX; i++) {
        up_entry_t *e = &s_up_queue[i];
        if (!e->used) {
            continue;
        }
        /* sender_id[8] + nl + name + wtl + wt + clen(2) + content */
        size_t plen = 8 + 1 + strlen(e->name) + 1 + strlen(e->wt) + 2 + e->content_len;
        uint8_t frame[FRAME_HEADER + 8 + 1 + 15 + 1 + 15 + 2 + WIFI_BRIDGE_CONTENT_MAX];
        size_t o = 0;
        frame[o++] = MAGIC0;
        frame[o++] = MAGIC1;
        frame[o++] = (uint8_t)(plen >> 8);
        frame[o++] = (uint8_t)(plen & 0xFF);
        frame[o++] = T_MSG_UP;
        memcpy(frame + o, e->sender_id, 8);
        o += 8;
        size_t nl = strlen(e->name);
        frame[o++] = (uint8_t)nl;
        memcpy(frame + o, e->name, nl);
        o += nl;
        size_t wl = strlen(e->wt);
        frame[o++] = (uint8_t)wl;
        memcpy(frame + o, e->wt, wl);
        o += wl;
        frame[o++] = (uint8_t)(e->content_len >> 8);
        frame[o++] = (uint8_t)(e->content_len & 0xFF);
        memcpy(frame + o, e->content, e->content_len);
        o += e->content_len;

        if (send(s_sock, frame, o, 0) < 0) {
            ESP_LOGW(TAG, "MSG_UP send failed");
            return;
        }
        ESP_LOGI(TAG, "up: %s wt=%s", e->name, e->wt);
        e->used = false;
    }
}

static void try_recv(void)
{
    if (s_sock < 0) {
        return;
    }
    uint8_t header[FRAME_HEADER];
    int n = recv(s_sock, header, FRAME_HEADER, MSG_PEEK);
    if (n <= 0) {
        if (n < 0 && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recv error, closing");
            close(s_sock);
            s_sock = -1;
        }
        return;
    }
    if (n < FRAME_HEADER) {
        return;
    }
    if (header[0] != MAGIC0 || header[1] != MAGIC1) {
        close(s_sock);
        s_sock = -1;
        return;
    }
    size_t plen = ((size_t)header[2] << 8) | header[3];
    uint8_t ty = header[4];
    if (plen > 512) {
        close(s_sock);
        s_sock = -1;
        return;
    }
    uint8_t payload[512];
    /* 消费并丢弃 header，再读 payload */
    n = recv(s_sock, header, FRAME_HEADER, 0);
    if (n != FRAME_HEADER) {
        return;
    }
    size_t got = 0;
    while (got < plen) {
        int r = recv(s_sock, payload + got, plen - got, 0);
        if (r <= 0) {
            close(s_sock);
            s_sock = -1;
            return;
        }
        got += (size_t)r;
    }

    if (ty == T_MSG_DOWN) {
        /* sender_id[8] + nl+name + wtl+wt + clen(2)+content */
        size_t pos = 0;
        if (got < 8) {
            return;
        }
        pos = 8;
        if (pos + 1 > got) {
            return;
        }
        size_t nl = payload[pos++];
        char name[16];
        if (nl >= sizeof(name) || pos + nl > got) {
            return;
        }
        memcpy(name, payload + pos, nl);
        name[nl] = '\0';
        pos += nl;
        if (pos + 1 > got) {
            return;
        }
        size_t wl = payload[pos++];
        char wt[16];
        if (wl >= sizeof(wt) || pos + wl > got) {
            return;
        }
        memcpy(wt, payload + pos, wl);
        wt[wl] = '\0';
        pos += wl;
        if (pos + 2 > got) {
            return;
        }
        size_t clen = ((size_t)payload[pos] << 8) | payload[pos + 1];
        pos += 2;
        char content[WIFI_BRIDGE_CONTENT_MAX];
        if (clen >= sizeof(content) || pos + clen > got) {
            return;
        }
        memcpy(content, payload + pos, clen);
        content[clen] = '\0';

        ESP_LOGI(TAG, "down: %s wt=%s", name, wt);
        if (s_down_cb_set && s_down_cb) {
            s_down_cb(name, wt, content);
        }
    } else if (ty == T_PONG) {
        ESP_LOGD(TAG, "pong");
    }
}

void wifi_bridge_poll(void)
{
    /* 1) 等 WiFi 起来 */
    EventBits_t bits = xEventGroupGetBits(s_wifi_events);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        return;
    }
    /* 2) 维护 TCP 连接 */
    if (s_sock < 0) {
        connect_tcp();
        return;
    }
    /* 3) 上行队列 + 下行接收 */
    pump_up();
    try_recv();
}

bool wifi_bridge_connected(void)
{
    return s_sock >= 0;
}

void wifi_bridge_send_up(const uint8_t *sender_id, const char *sender_name,
                         const char *wt, const char *content)
{
    up_entry_t *slot = NULL;
    for (int i = 0; i < UP_QUEUE_MAX; i++) {
        if (!s_up_queue[i].used) {
            slot = &s_up_queue[i];
            break;
        }
    }
    if (!slot) {
        ESP_LOGW(TAG, "up queue full, drop");
        return;
    }
    slot->used = true;
    memcpy(slot->sender_id, sender_id, 8);
    snprintf(slot->name, sizeof(slot->name), "%s", sender_name ? sender_name : "?");
    snprintf(slot->wt, sizeof(slot->wt), "%s", wt ? wt : WIFI_BRIDGE_WT);
    slot->content_len = content ? strnlen(content, sizeof(slot->content) - 1) : 0;
    memcpy(slot->content, content, slot->content_len);
    slot->content[slot->content_len] = '\0';
}