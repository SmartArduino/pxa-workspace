#include "rpc_music_now.h"
#include "rpc_wrap.h"
#include "rpc_slave_if.h"

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

#define TAG "rpc_music_now"

static rpc_music_now_t   s_now;
static SemaphoreHandle_t s_mtx;
static bool              s_inited;

/* 事件负载里的字符串**不保证有结尾 0**（rpc_wrap.c 是按 len 拷进去的），
 * 所以一律定长拷 + 自己补 0，不要用 strncpy/%s。 */
static void copy_str(char *dst, size_t cap, const char *src, size_t len)
{
    if (src == NULL || len == 0) {
        dst[0] = '\0';
        return;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void on_title(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const rpc_vb_music_title_event_t *e = (const rpc_vb_music_title_event_t *)data;
    if (e == NULL) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    char tmp[RPC_MUSIC_NOW_TITLE_CAP];
    copy_str(tmp, sizeof(tmp), e->title, e->title_len);
    if (strcmp(s_now.title, tmp) != 0) {
        memcpy(s_now.title, tmp, sizeof(tmp));
        /* 换曲了：进度和时长立刻作废。
         * 宁可让调用方拿到 0（页面显示 0:00，随后第一个 TIME 事件就修正），
         * 也不要给上一首的秒数 —— 那会让进度条先跳到一个错的位置再弹回来。
         * 歌词不清：TITLE 和 LYRC 是两条独立事件、没有先后保证，清了会误杀
         * 刚到的新歌第一句。间奏和换曲之间本来就分不出来，留着代价更小。 */
        s_now.pos_sec = 0;
        s_now.dur_sec = 0;
    }
    xSemaphoreGive(s_mtx);
}

static void on_lyric(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const rpc_vb_music_lyrc_event_t *e = (const rpc_vb_music_lyrc_event_t *)data;
    if (e == NULL) {
        return;
    }
    char tmp[RPC_MUSIC_NOW_LYRIC_CAP];
    copy_str(tmp, sizeof(tmp), e->lyrc, e->lyrc_len);
    /* 间奏期间 701 会推空串。存进去的话，进歌词页正好赶上间奏就是一片空白，
     * 而"上一句还挂着"才是用户预期的。所以空串不覆盖。 */
    if (tmp[0] == '\0') {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(s_now.lyric, tmp, sizeof(tmp));
    xSemaphoreGive(s_mtx);
}

static void on_time(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const event_music_time_t *e = (const event_music_time_t *)data;
    if (e == NULL) {
        return;
    }
    int32_t cur = e->current_time_sec < 0 ? 0 : e->current_time_sec;
    int32_t tot = e->total_time_sec   < 0 ? 0 : e->total_time_sec;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_now.pos_sec = (uint16_t)cur;
    /* 总时长只在报了有效值时更新：有些播放器间歇性报 0，覆盖掉会让进度条
     * 一闪一闪地在"有总长"和"未知"之间跳。 */
    if (tot > 0) {
        s_now.dur_sec = (uint16_t)tot;
    }
    xSemaphoreGive(s_mtx);
}

/* 这里**不订阅** BT_MUSIC_PLAY / PAUSE / MODE_CHANGED —— 那几个 701 有查询
 * 接口（rpc_music_is_playing / rpc_music_mode_get），应用层直接查就行，
 * 没必要在这儿存一份可能过期的影子。本模块只管"查不到的那几个"。 */
static const struct {
    int32_t id;
    esp_event_handler_t cb;
} k_subs[] = {
    { VB_EVT_MUSIC_INFO_TITLE, on_title },
    { VB_EVT_MUSIC_INFO_LYRC,  on_lyric },
    { VB_EVT_MUSIC_INFO_TIME,  on_time  },
};
#define SUB_CNT ((int)(sizeof(k_subs) / sizeof(k_subs[0])))

int rpc_music_now_init(void)
{
    if (s_inited) {
        return RPC_ERR_SUCCESS;
    }
    s_mtx = xSemaphoreCreateMutex();
    if (s_mtx == NULL) {
        ESP_LOGE(TAG, "failed to create mutex");
        return RPC_ERR_FAILURE;
    }
    memset(&s_now, 0, sizeof(s_now));

    for (int i = 0; i < SUB_CNT; i++) {
        if (esp_event_handler_register(RPC_VB_EVENT, k_subs[i].id,
                                       k_subs[i].cb, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "failed to subscribe event %d", (int)k_subs[i].id);
            for (int j = 0; j < i; j++) {
                esp_event_handler_unregister(RPC_VB_EVENT, k_subs[j].id, k_subs[j].cb);
            }
            vSemaphoreDelete(s_mtx);
            s_mtx = NULL;
            return RPC_ERR_FAILURE;
        }
    }
    s_inited = true;
    return RPC_ERR_SUCCESS;
}

int rpc_music_now_get(rpc_music_now_t *out)
{
    if (out == NULL) {
        return RPC_ERR_INVALID_PARAM;
    }
    if (!s_inited) {
        memset(out, 0, sizeof(*out));
        return RPC_ERR_FAILURE;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_now;
    xSemaphoreGive(s_mtx);
    return RPC_ERR_SUCCESS;
}
