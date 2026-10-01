#pragma once

/* 曲目列表缓存 —— 框架级能力，应用不必各自实现一套。
 *
 * 解决的问题：701 读 FAT 目录约 12ms/条，拉一页 16 条要 190ms。UI 每翻一页
 * 都真去 UART 上跑一趟，翻页就是等待；而每个应用各写一套「分页 + 缓存 +
 * 插拔失效」的代码，既重复又容易漏掉失效场景。
 *
 * 用法（最简）：启动时 rpc_music_cache_init() 一次，之后只调
 * rpc_music_cache_get()。首次访问会自动在后台把整份列表预取进来，
 * 期间 get() 返回已到的部分，不阻塞。数据到齐/有新进展时会发
 * VB_EVT_MUSIC_LIST_UPDATED 事件，UI 收到重画即可。
 *
 * 失效：内部订阅了 VB_EVT_STORAGE_DEV_CHANGED，插拔自动清空重取；
 * 701 重连等场景由调用方显式调 rpc_music_cache_invalidate_all()。
 *
 * 线程模型：
 *   读侧 rpc_music_cache_get()/status()  任意任务，内部持锁拷贝，不阻塞
 *                                        （只做内存拷贝，不发 RPC）。
 *   写侧 组件内部的预取任务，跑同步 RPC，与调用方任务无关。
 * 因此 get() 可以安全地在 LVGL 线程里调。
 */

#include <stdbool.h>
#include <stdint.h>

#include "rpc_slave_if.h"   /* storage_dev_type_t, sd_music_file_info_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 单次向 701 拉取的条数。16 条≈190ms；再大单次响应会撑爆 rpc_adapter
 * 的 UART 接收缓冲（实测并发大响应会 "UART buffer full" 丢包）。 */
#define RPC_MUSIC_CACHE_BATCH       16

/* 缓存内存上限。每条 sizeof(sd_music_file_info_t) = 68B，
 * 500KB 约合 7500 首；超出部分不缓存（get 到那里会返回 0 条）。 */
#define RPC_MUSIC_CACHE_MAX_BYTES   (500 * 1024)

typedef enum {
    RPC_MUSIC_CACHE_EMPTY = 0,   /* 无数据，也未在预取 */
    RPC_MUSIC_CACHE_LOADING,     /* 后台预取中，已到的部分可读 */
    RPC_MUSIC_CACHE_READY,       /* 全部就位 */
    RPC_MUSIC_CACHE_FAILED,      /* 取总数失败：设备不在位或 701 无响应 */
} rpc_music_cache_state_t;

/* VB_EVT_MUSIC_LIST_UPDATED 的事件负载，也是 status() 的输出。 */
typedef struct {
    storage_dev_type_t      dev;
    rpc_music_cache_state_t state;
    uint32_t total;      /* 设备上的曲目总数，0 = 未知 */
    uint32_t cached;     /* 已缓存条数 */
} rpc_music_cache_status_t;

/**
 * @brief 初始化缓存并启动后台预取任务，调一次。重复调用无副作用。
 *
 * 需要 esp_event 默认事件循环已创建（内部要订阅存储插拔事件）。
 * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
 */
int rpc_music_cache_init(void);

/**
 * @brief 读取 [offset, offset+count) 区间的曲目，立即返回。
 *
 * 只返回从 offset 起**连续已缓存**的部分：中间遇到还没取回来的批次就截断，
 * *n_out 即实际填充条数，可能为 0。调用方按 n_out 渲染，剩下的等
 * VB_EVT_MUSIC_LIST_UPDATED 再来一次即可。
 *
 * 缓存为空时会自动触发一次后台全量预取，无需调用方操心；未命中的区间会被
 * 优先安排到预取队首，所以直接跳到列表末尾也不用等前面全部取完。
 *
 * @param dev    存储设备类型。
 * @param offset 起始索引 (0-based)。
 * @param count  期望条数。
 * @param out    输出缓冲，容量至少 count 条，由调用方提供。
 * @param n_out  输出实际填充条数，需非空。
 * @return 成功返回 RPC_ERR_SUCCESS（*n_out < count 也算成功），
 *         参数非法或未初始化返回负数错误码。
 */
int rpc_music_cache_get(storage_dev_type_t dev, uint32_t offset, uint32_t count,
                        sd_music_file_info_t *out, uint32_t *n_out);

/**
 * @brief 查询某设备的缓存进度。
 * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
 */
int rpc_music_cache_status(storage_dev_type_t dev, rpc_music_cache_status_t *out);

/**
 * @brief 主动触发一次后台预取。get() 已经会自动触发，一般无需显式调用；
 *        想在进页面前把数据先暖起来时可以用。已在预取或已就绪则是空操作。
 * @return 成功返回 RPC_ERR_SUCCESS，失败返回负数错误码。
 */
int rpc_music_cache_prime(storage_dev_type_t dev);

/**
 * @brief 丢弃某设备的缓存。下次 get() 会重新预取。
 *
 * 插拔已由内部订阅事件自动处理，这里留给「切音源后想强制刷新」等场景。
 */
void rpc_music_cache_invalidate(storage_dev_type_t dev);

/**
 * @brief 丢弃所有设备的缓存。701 重连后调。
 */
void rpc_music_cache_invalidate_all(void);

#ifdef __cplusplus
}
#endif
