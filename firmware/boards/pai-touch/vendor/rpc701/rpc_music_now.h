#pragma once

/* 「当前这首」缓存 —— 701 只推不给查的那几个字段。
 *
 * 解决的问题：曲名、歌词、播放进度在协议里**没有查询接口**，只能等事件。
 * 于是页面每次退出再进来，屏幕上是占位值，要等下一个事件（进度约 1s、
 * 曲名要等换曲）才填上，肉眼可见地空一两秒。
 *
 * 这一层把最后一次事件存下来，只为一件事：**进页面时拿一个初值先显示上**。
 *
 * ⚠ 拿到的值可能是过期的（换曲那一瞬间、701 重连之后），所以：
 *   - 只在初始化显示时调一次，别拿它当权威；
 *   - 正确性由随后的事件保证，收到事件就以事件为准，不要再回来查。
 *
 * 线程模型：
 *   写侧  内部的事件回调，跑在 esp_event 任务上，只加互斥改结构体。
 *   读侧  rpc_music_now_get() 任意任务，持锁整份拷贝，纯内存操作不发 RPC。
 *
 * 事件顺序：本模块在 rpc_resister_event_callbacks() 里注册，早于任何应用层
 * 订阅者。esp_event 对同一事件按注册顺序同步派发，所以应用层收到事件时，
 * 这里的缓存**必然已经是新值**。这个保证由驱动层给出，不要求调用方排顺序。
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 和 701 的事件负载对齐：曲名来自文件名（最长 64），歌词一次一行。 */
#define RPC_MUSIC_NOW_TITLE_CAP 64
#define RPC_MUSIC_NOW_LYRIC_CAP 64

typedef struct {
    char     title[RPC_MUSIC_NOW_TITLE_CAP];  /* 空串 = 没收到过 */
    char     lyric[RPC_MUSIC_NOW_LYRIC_CAP];  /* 最后一句**有内容的**歌词 */
    uint16_t pos_sec;                         /* 最后一次上报的播放位置 */
    uint16_t dur_sec;                         /* 总时长，0 = 未知 */
} rpc_music_now_t;

/* 由 rpc_resister_event_callbacks() 调用，重复调用幂等。
 * 应用层不需要自己调。 */
int rpc_music_now_init(void);

/* 整份拷贝。out 非空。未初始化时填全 0 并返回失败。 */
int rpc_music_now_get(rpc_music_now_t *out);

#ifdef __cplusplus
}
#endif
