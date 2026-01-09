#include "common.h"
#include "gui.h"
#include <commctrl.h>
#include <stdio.h>

// --- 全局变量定义 ---
// 在 common.h 中声明为 extern，在此处进行实体定义

AppConfig g_config;               // 全局配置实例
ProxyNode g_nodes[MAX_NODES];     // 节点存储数组
int g_node_count = 0;             // 当前节点计数
CRITICAL_SECTION g_dataLock;      // 数据线程锁
HWND g_hMainWnd = NULL;           // 主窗口句柄
HANDLE g_hStopEvent = NULL;       // 停止信号事件

// --- 辅助函数：初始化默认配置 ---
void InitDefaultConfig() {
    memset(&g_config, 0, sizeof(g_config));
    
    // 设置默认搜索参数 (参考 Python 版默认值)
    // 聚合器 v6.1.16 默认值调整
    strcpy(g_config.search_keywords, "clash,v2ray,sub,SSR,vmess,trojan,vless,SS,hysteria,hy2,节点");
    g_config.search_pages = 2;
    
    // 设置默认测速参数
    g_config.enable_speedtest = true;
    g_config.test_mode = TEST_MODE_SINGBOX; // 默认使用 Sing-box
    g_config.timeout = 10;                  // 默认超时 10秒
    g_config.concurrency = 10;              // 默认并发 10
    strcpy(g_config.test_url, "https://cp.cloudflare.com/");
    
    // 代理默认关闭
    g_config.enable_proxy = false;
    strcpy(g_config.proxy_url, "http://127.0.0.1:10809");
    g_config.github_token[0] = '\0'; // 默认为空
}

// --- 程序主入口 ---
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow) {
    // 1. 初始化全局临界区 (用于多线程保护 g_nodes)
    InitializeCriticalSection(&g_dataLock);

    // 2. 初始化 Winsock (网络库)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(NULL, L"WSAStartup failed! Network functions will not work.", L"Critical Error", MB_ICONERROR);
        return 1;
    }

    // 3. 初始化 Common Controls (启用现代 UI 风格)
    INITCOMMONCONTROLSEX ic = {sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&ic);
    
    // 4. 加载配置
    InitDefaultConfig();
    // TODO: 这里可以添加 LoadSettings() 从 ini 文件读取配置的逻辑

    // 5. 创建全局停止事件 (手动重置，初始无信号)
    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_hStopEvent) {
        MessageBoxW(NULL, L"Failed to create event object.", L"Error", MB_ICONERROR);
        return 1;
    }

    // 6. 创建并显示主窗口
    // CreateMainWindow 实现在 src/gui_main.c 中
    if (!CreateMainWindow(hInst, nShow)) {
        MessageBoxW(NULL, L"Failed to create main window application.", L"Error", MB_ICONERROR);
        return 1;
    }

    // 7. 标准消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 8. 清理资源 (退出前)
    if (g_hStopEvent) CloseHandle(g_hStopEvent);
    DeleteCriticalSection(&g_dataLock);
    WSACleanup();

    return (int)msg.wParam;
}
