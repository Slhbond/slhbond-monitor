/*
 * mirror.h - 更新加速节点（第三方 GitHub 代理）
 *
 * 为什么需要：国内网络直连 github.com 时，TCP 连接可能要 19～36 秒，
 * 甚至根本连不上（实测本机就是这种情况）。发布包有 2.4 MB，直连下载
 * 经常超时；而走代理只要 1 秒左右。
 *
 * 信任边界没有变化，这一点很重要：
 *   - 加速节点只是「搬运工」，它拿到的字节必须与官方一致，
 *     否则下面 updater 的 SHA-256 校验会失败并拒绝安装。
 *   - 但清单本身没有摘要可校验，所以清单优先直连；只有直连失败才退到代理，
 *     而且退到代理时会额外校验清单里的下载地址必须指向**同一个仓库**，
 *     这样代理最多只能让我们「不更新」，没法把包换成自己的。
 *
 * 实测（RK3568，本机网络）：
 *   gh-proxy.com      0.79 s  ✓
 *   ghfast.top        1.33 s  ✓
 *   ghproxy.net       1.53 s  ✓
 *   hk.gh-proxy.com   4.21 s  ✓
 *   直连 github.com   19～36 s（多数情况下超时失败）
 */
#ifndef SLH_MIRROR_H
#define SLH_MIRROR_H

#include <stddef.h>

#define SLH_MAX_MIRRORS   8
#define SLH_MIRROR_PREFIX 96

/** 一个候选节点：前缀（空表示直连）+ 探测到的延迟（毫秒，-1 未知） */
typedef struct {
    char prefix[SLH_MIRROR_PREFIX];
    int  latency_ms;
} mirror_t;

/**
 * 解析配置里的节点列表（逗号或空格分隔的前缀）。
 * 会在最前面插入一个「直连」候选项。
 * 返回候选总数（至少 1，即只有直连）。
 */
int mirror_parse(const char *spec, mirror_t *out, int max);

/** 探测单个地址的延迟（毫秒）。不可用返回 -1。 */
int mirror_probe(const char *url, int timeout_ms);

/**
 * 在候选里挑一个最快的可用地址。
 *
 * 逐个测延迟，一旦发现比 `good_enough_ms` 更快的就停止探测（避免手点检查时等太久）。
 * 探测到的延迟会回写到 cands[i].latency_ms 里，方便接口展示。
 * 返回 0 并写出选中的完整地址；全部不可用返回 -1。
 */
int mirror_pick(const char *url, mirror_t *cands, int count,
                int probe_ms, int good_enough_ms,
                char *out_url, size_t out_size,
                int *out_latency_ms, int *out_index);

/** 节点前缀为空表示直连。 */
int mirror_is_direct(const mirror_t *m);

/**
 * 从清单地址推出「同一个仓库的发布地址前缀」，
 * 例如 https://github.com/A/B/releases/ 。推不出来返回 0。
 *
 * 用于：清单如果来自加速节点，就要求它给的下载地址必须落在这个前缀下。
 */
int mirror_repo_prefix(const char *manifest_url, char *out, size_t out_size);

#endif /* SLH_MIRROR_H */
