#ifndef COMMON_H
#define COMMON_H

#include <windows.h>
#include <stdbool.h>

// 对应 Python 的 GUI Queue 消息类型
#define WM_APP_LOG          (WM_USER + 100)
#define WM_APP_FOUND_LINKS  (WM_USER + 101)
#define WM_APP_PREVIEW      (WM_USER + 102)
#define WM_APP_TASK_DONE    (WM_USER + 103)

// 节点类型枚举
typedef enum {
    NODE_UNKNOWN,
    NODE_VMESS,
    NODE_VLESS,
    NODE_TROJAN,
    NODE_SS,
    NODE_HYSTERIA2
} NodeType;

// 节点结构体 (替代 Python 字典)
typedef struct {
    NodeType type;
    char* original_link; // 原始链接
    char* server;
    int port;
    char* remark;        // 备注/Tag
    double latency;      // 延迟 (ms)
    // ... 其他协议特定字段 (uuid, password 等) 可用 void* 扩展或大结构体
} ProxyNode;

// 聚合配置结构体
typedef struct {
    char github_token[128];
    char search_keywords[512];
    int search_pages;
    bool enable_proxy;
    char proxy_url[128];
    bool enable_speedtest;
    char speedtest_mode[16]; // "tcp" or "singbox"
    int timeout;
    int concurrency;
} AppConfig;

// 全局变量声明 (类似 MandalaECH 的 globals.c)
extern AppConfig g_config;

#endif
