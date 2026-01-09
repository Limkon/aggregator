#ifndef COMMON_H
#define COMMON_H

#include <windows.h>
#include <stdbool.h>

// --- 1. 常量定义 ---
#define APP_NAME            L"ProxyAggregator"
#define APP_VERSION         L"v1.0.0"

#define MAX_URL_LEN         2048
#define MAX_REMARK_LEN      256
#define MAX_HOST_LEN        256
#define MAX_NODES           10000  // 最大支持节点数

// Sing-box 默认配置
#define SINGBOX_BASE_PORT   10900
#define SINGBOX_EXE_NAME    "sing-box.exe"

// --- 2. 自定义 Windows 消息 ---
// 用于工作线程向主 UI 线程发送通知
#define WM_APP_LOG          (WM_USER + 100)  // 参数: lParam = (char*) message
#define WM_APP_PROGRESS     (WM_USER + 101)  // 参数: wParam = current, lParam = total
#define WM_APP_TASK_DONE    (WM_USER + 102)  // 参数: wParam = task_type (0=Search, 1=Process)
#define WM_APP_PREVIEW      (WM_USER + 103)  // 参数: 无 (通知 UI 刷新预览文本)

// --- 3. 数据结构 ---

// 节点协议类型
typedef enum {
    NODE_UNKNOWN = 0,
    NODE_VMESS,
    NODE_VLESS,
    NODE_TROJAN,
    NODE_SS,
    NODE_SSR,
    NODE_HYSTERIA2
} NodeType;

// 节点信息结构体
typedef struct {
    int id;                     // 唯一 ID
    NodeType type;              // 协议类型
    char original_link[MAX_URL_LEN]; // 原始订阅链接
    
    // 解析后的核心信息 (用于测速)
    char address[MAX_HOST_LEN];
    int port;
    char remark[MAX_REMARK_LEN];
    
    // 测速结果
    double latency;             // 延迟 (毫秒)，-1 表示超时/失败
    bool is_alive;              // 是否存活
} ProxyNode;

// 测速模式
typedef enum {
    TEST_MODE_TCP = 0,
    TEST_MODE_SINGBOX
} TestMode;

// 全局配置结构体 (对应 UI 上的输入项)
typedef struct {
    // 搜索配置
    char github_token[128];
    char search_keywords[512]; // 逗号分隔
    int search_pages;
    
    // 网络代理配置 (用于搜索和下载)
    bool enable_proxy;
    char proxy_url[128];
    
    // 测速配置
    bool enable_speedtest;
    TestMode test_mode;
    int timeout;          // 秒
    int concurrency;      // 并发数
    char test_url[256];   // Sing-box 测速目标 URL
} AppConfig;

// --- 4. 全局变量声明 ---
// 定义在 main.c 中，此处 extern 引用

extern AppConfig g_config;            // 全局配置
extern ProxyNode g_nodes[MAX_NODES];  // 节点数组
extern int g_node_count;              // 当前节点数量
extern CRITICAL_SECTION g_dataLock;   // 数据读写锁 (保护 g_nodes)
extern HWND g_hMainWnd;               // 主窗口句柄

// 任务控制
extern HANDLE g_hStopEvent;           // 用于通知工作线程停止的事件

#endif // COMMON_H
