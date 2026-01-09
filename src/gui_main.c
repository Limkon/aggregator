// 调整包含顺序：common.h 必须在 gui.h 之前，因为它包含 winsock2.h
// 而 winsock2.h 必须在 windows.h (gui.h 中包含) 之前包含。
#include "common.h"
#include "gui.h"

#include <commctrl.h>
#include <stdio.h>

// --- 控件 ID 定义 ---
#define ID_GRP_SEARCH       1001
#define ID_LBL_TOKEN        1002
#define ID_EDT_TOKEN        1003
#define ID_LBL_QUERY        1004
#define ID_EDT_QUERY        1005
#define ID_LBL_PAGES        1006
#define ID_EDT_PAGES        1007 // Spinbox buddy
#define ID_BTN_SEARCH       1008
#define ID_BTN_STOP         1009

#define ID_GRP_SUBS         1020
#define ID_EDT_SUBS         1021

#define ID_GRP_PROXY        1030
#define ID_CHK_PROXY        1031
#define ID_LBL_PROXY_URL    1032
#define ID_EDT_PROXY        1033

#define ID_GRP_SPEED        1040
#define ID_CHK_SPEED        1041
#define ID_RAD_SINGBOX      1042
#define ID_RAD_TCP          1043
#define ID_LBL_TIMEOUT      1044
#define ID_EDT_TIMEOUT      1045
#define ID_LBL_CONCUR       1046
#define ID_EDT_CONCUR       1047
#define ID_LBL_TESTURL      1048
#define ID_EDT_TESTURL      1049

#define ID_BTN_RUN          1060

#define ID_GRP_LOG          1070
#define ID_EDT_LOG          1071

#define ID_GRP_RESULT       1080
#define ID_EDT_RESULT       1081
#define ID_BTN_SAVE         1082
#define ID_LBL_PREVIEW      1083

// --- 全局控件句柄 ---
static HWND hEdtToken, hEdtQuery, hEdtPages, hBtnSearch, hBtnStop;
static HWND hEdtSubs;
static HWND hChkProxy, hEdtProxy;
static HWND hChkSpeed, hRadSingbox, hRadTcp, hEdtTimeout, hEdtConcur, hEdtTestUrl;
static HWND hBtnRun;
static HWND hEdtLog;
static HWND hEdtResult, hBtnSave;

static HFONT hFontApp = NULL;
static HBRUSH hBrBkg = NULL;

// --- 外部业务函数声明 (将在 aggregator_core.c 中实现) ---
extern void StartSearchTask();
extern void StartProcessTask();
extern void StopCurrentTask();
extern void SaveResultToFile(HWND hOwner, const char* content);

// --- 辅助函数：创建字体 ---
void CreateAppFont() {
    NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0)) {
        hFontApp = CreateFontIndirectW(&ncm.lfMessageFont);
    } else {
        hFontApp = GetStockObject(DEFAULT_GUI_FONT);
    }
}

// --- 辅助函数：追加日志 ---
void GuiAppendLog(const char* message) {
    if (!hEdtLog) return;
    
    // 获取当前文本长度，用于追加到末尾
    int len = GetWindowTextLength(hEdtLog);
    SendMessage(hEdtLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    
    // 转换 UTF-8 到 WideChar 以便显示
    int wlen = MultiByteToWideChar(CP_UTF8, 0, message, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t* wBuf = (wchar_t*)malloc((wlen + 2) * sizeof(wchar_t));
        if (wBuf) {
            MultiByteToWideChar(CP_UTF8, 0, message, -1, wBuf, wlen);
            wcscat(wBuf, L"\r\n"); // 补充换行
            SendMessageW(hEdtLog, EM_REPLACESEL, 0, (LPARAM)wBuf);
            free(wBuf);
        }
    }
}

// --- 辅助函数：更新 UI 状态 (启用/禁用) ---
void SetGuiState(bool isRunning) {
    BOOL bEnable = !isRunning;
    EnableWindow(hBtnSearch, bEnable);
    EnableWindow(hBtnRun, bEnable);
    EnableWindow(hBtnStop, !bEnable); // 停止按钮反向状态
}

// --- 辅助函数：读取 UI 配置到全局结构 ---
void SyncConfigFromUI() {
    char buf[1024];

    // 1. 搜索设置
    GetWindowTextA(hEdtToken, g_config.github_token, sizeof(g_config.github_token));
    GetWindowTextA(hEdtQuery, g_config.search_keywords, sizeof(g_config.search_keywords));
    GetWindowTextA(hEdtPages, buf, sizeof(buf));
    g_config.search_pages = atoi(buf);
    if (g_config.search_pages < 1) g_config.search_pages = 1;

    // 2. 代理设置
    g_config.enable_proxy = (SendMessage(hChkProxy, BM_GETCHECK, 0, 0) == BST_CHECKED);
    GetWindowTextA(hEdtProxy, g_config.proxy_url, sizeof(g_config.proxy_url));

    // 3. 测速设置
    g_config.enable_speedtest = (SendMessage(hChkSpeed, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (SendMessage(hRadSingbox, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        g_config.test_mode = TEST_MODE_SINGBOX;
    } else {
        g_config.test_mode = TEST_MODE_TCP;
    }
    
    GetWindowTextA(hEdtTimeout, buf, sizeof(buf));
    g_config.timeout = atoi(buf);
    if (g_config.timeout < 1) g_config.timeout = 1;
    
    GetWindowTextA(hEdtConcur, buf, sizeof(buf));
    g_config.concurrency = atoi(buf);
    if (g_config.concurrency < 1) g_config.concurrency = 1;
    
    GetWindowTextA(hEdtTestUrl, g_config.test_url, sizeof(g_config.test_url));
}

// --- 窗口消息处理 ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        {
            CreateAppFont();
            hBrBkg = CreateSolidBrush(RGB(240, 240, 240)); // 浅灰背景

            // 1. 搜索区域 (Group Box)
            HWND hGrpSearch = CreateWindowW(L"BUTTON", L"在线搜索订阅", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 10, 810, 100, hWnd, (HMENU)ID_GRP_SEARCH, NULL, NULL);
            SendMessage(hGrpSearch, WM_SETFONT, (WPARAM)hFontApp, TRUE);

            CreateWindowW(L"STATIC", L"GitHub Token:", WS_CHILD | WS_VISIBLE, 25, 35, 90, 20, hWnd, (HMENU)ID_LBL_TOKEN, NULL, NULL);
            hEdtToken = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 120, 32, 500, 23, hWnd, (HMENU)ID_EDT_TOKEN, NULL, NULL);
            SendMessage(hEdtToken, EM_SETCUEBANNER, TRUE, (LPARAM)L"（可选，提高限额）"); // Placeholder

            CreateWindowW(L"STATIC", L"搜索关键字:", WS_CHILD | WS_VISIBLE, 25, 65, 90, 20, hWnd, (HMENU)ID_LBL_QUERY, NULL, NULL);
            hEdtQuery = CreateWindowW(L"EDIT", L"clash,v2ray,sub,vmess,trojan,vless,hysteria2", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 120, 62, 500, 23, hWnd, (HMENU)ID_EDT_QUERY, NULL, NULL);

            CreateWindowW(L"STATIC", L"页数:", WS_CHILD | WS_VISIBLE, 640, 65, 40, 20, hWnd, (HMENU)ID_LBL_PAGES, NULL, NULL);
            hEdtPages = CreateWindowW(L"EDIT", L"2", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 680, 62, 40, 23, hWnd, (HMENU)ID_EDT_PAGES, NULL, NULL);

            hBtnSearch = CreateWindowW(L"BUTTON", L"开始搜索", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 640, 30, 80, 25, hWnd, (HMENU)ID_BTN_SEARCH, NULL, NULL);
            hBtnStop = CreateWindowW(L"BUTTON", L"中止", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 730, 30, 80, 25, hWnd, (HMENU)ID_BTN_STOP, NULL, NULL);

            // 2. 订阅链接区域
            CreateWindowW(L"BUTTON", L"订阅链接 / 节点 (一行一个)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 120, 810, 150, hWnd, (HMENU)ID_GRP_SUBS, NULL, NULL);
            hEdtSubs = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 20, 145, 790, 115, hWnd, (HMENU)ID_EDT_SUBS, NULL, NULL);

            // 3. 代理区域
            CreateWindowW(L"BUTTON", L"前置代理", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 280, 810, 60, hWnd, (HMENU)ID_GRP_PROXY, NULL, NULL);
            hChkProxy = CreateWindowW(L"BUTTON", L"启用代理", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 305, 80, 20, hWnd, (HMENU)ID_CHK_PROXY, NULL, NULL);
            CreateWindowW(L"STATIC", L"地址:", WS_CHILD | WS_VISIBLE, 120, 307, 40, 20, hWnd, (HMENU)ID_LBL_PROXY_URL, NULL, NULL);
            hEdtProxy = CreateWindowW(L"EDIT", L"http://127.0.0.1:10809", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 160, 305, 630, 23, hWnd, (HMENU)ID_EDT_PROXY, NULL, NULL);

            // 4. 测速区域
            CreateWindowW(L"BUTTON", L"测速配置", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 350, 810, 120, hWnd, (HMENU)ID_GRP_SPEED, NULL, NULL);
            
            hChkSpeed = CreateWindowW(L"BUTTON", L"启用测速", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 375, 100, 20, hWnd, (HMENU)ID_CHK_SPEED, NULL, NULL);
            SendMessage(hChkSpeed, BM_SETCHECK, BST_CHECKED, 0);

            hRadSingbox = CreateWindowW(L"BUTTON", L"Sing-box (真实延迟)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 130, 375, 140, 20, hWnd, (HMENU)ID_RAD_SINGBOX, NULL, NULL);
            hRadTcp = CreateWindowW(L"BUTTON", L"TCP Ping (握手延迟)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 280, 375, 140, 20, hWnd, (HMENU)ID_RAD_TCP, NULL, NULL);
            SendMessage(hRadSingbox, BM_SETCHECK, BST_CHECKED, 0); // 默认 Singbox

            CreateWindowW(L"STATIC", L"超时(秒):", WS_CHILD | WS_VISIBLE, 25, 405, 60, 20, hWnd, (HMENU)ID_LBL_TIMEOUT, NULL, NULL);
            hEdtTimeout = CreateWindowW(L"EDIT", L"10", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 90, 402, 40, 23, hWnd, (HMENU)ID_EDT_TIMEOUT, NULL, NULL);

            CreateWindowW(L"STATIC", L"并发数:", WS_CHILD | WS_VISIBLE, 150, 405, 50, 20, hWnd, (HMENU)ID_LBL_CONCUR, NULL, NULL);
            hEdtConcur = CreateWindowW(L"EDIT", L"10", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 200, 402, 40, 23, hWnd, (HMENU)ID_EDT_CONCUR, NULL, NULL);

            CreateWindowW(L"STATIC", L"测速地址:", WS_CHILD | WS_VISIBLE, 260, 405, 60, 20, hWnd, (HMENU)ID_LBL_TESTURL, NULL, NULL);
            hEdtTestUrl = CreateWindowW(L"EDIT", L"https://cp.cloudflare.com/", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 325, 402, 465, 23, hWnd, (HMENU)ID_EDT_TESTURL, NULL, NULL);
            
            // 5. 执行按钮
            hBtnRun = CreateWindowW(L"BUTTON", L"执行聚合处理 (含测速)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 25, 435, 765, 30, hWnd, (HMENU)ID_BTN_RUN, NULL, NULL);

            // 6. 日志区域
            CreateWindowW(L"BUTTON", L"处理日志", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 480, 810, 200, hWnd, (HMENU)ID_GRP_LOG, NULL, NULL);
            hEdtLog = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 20, 505, 790, 165, hWnd, (HMENU)ID_EDT_LOG, NULL, NULL);

            // 7. 结果区域
            CreateWindowW(L"BUTTON", L"结果预览 (Base64)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 690, 810, 180, hWnd, (HMENU)ID_GRP_RESULT, NULL, NULL);
            hBtnSave = CreateWindowW(L"BUTTON", L"保存为文件...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 700, 710, 100, 25, hWnd, (HMENU)ID_BTN_SAVE, NULL, NULL);
            hEdtResult = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL, 20, 740, 790, 120, hWnd, (HMENU)ID_EDT_RESULT, NULL, NULL);

            // 设置所有控件字体
            EnumChildWindows(hWnd, (WNDENUMPROC)(void(*)(HWND,LPARAM))SendMessage, (LPARAM)hFontApp); // 简单粗暴设字体
        }
        break;

    case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == ID_BTN_SEARCH && code == BN_CLICKED) {
                SyncConfigFromUI();
                SetGuiState(true);
                GuiAppendLog("--- 开始搜索 ---");
                // TODO: 启动搜索线程
                StartSearchTask(); 
            }
            else if (id == ID_BTN_RUN && code == BN_CLICKED) {
                SyncConfigFromUI();
                
                // 从订阅框获取输入内容到缓冲区 (此处简单处理，实际应在 ProcessTask 中获取)
                // 暂时不在此处获取大文本，由任务线程直接 GetWindowText 或通过 buffer 传递
                
                SetGuiState(true);
                GuiAppendLog("--- 开始聚合 ---");
                StartProcessTask(); 
            }
            else if (id == ID_BTN_STOP && code == BN_CLICKED) {
                GuiAppendLog("正在请求中止...");
                StopCurrentTask();
            }
            else if (id == ID_BTN_SAVE && code == BN_CLICKED) {
                // 保存文件逻辑
                int len = GetWindowTextLength(hEdtResult);
                if (len > 0) {
                    char* buf = (char*)malloc(len + 1);
                    GetWindowTextA(hEdtResult, buf, len + 1);
                    SaveResultToFile(hWnd, buf);
                    free(buf);
                } else {
                    MessageBoxW(hWnd, L"没有内容可保存", L"提示", MB_OK);
                }
            }
        }
        break;

    // --- 自定义消息处理 (来自工作线程) ---
    case WM_APP_LOG:
        {
            char* msg = (char*)lParam;
            GuiAppendLog(msg);
            // 消息发送者负责分配内存，接收者负责释放 (如果是动态分配的)
            // 这里约定 lParam 指向静态字符串或由线程管理的缓冲区，或者在此处不做释放
            // 简单起见，建议发送者使用 SendMessage (同步) 发送栈上字符串，或 PostMessage 动态分配并在此释放
            if (msg) free(msg); 
        }
        break;

    case WM_APP_TASK_DONE:
        SetGuiState(false); // 恢复按钮状态
        GuiAppendLog("--- 任务结束 ---");
        break;
        
    case WM_APP_PREVIEW:
        {
             // 接收 Base64 结果并在结果框显示
             char* result = (char*)lParam;
             SetWindowTextA(hEdtResult, result);
             if (result) free(result);
        }
        break;

    case WM_DESTROY:
        if (hFontApp) DeleteObject(hFontApp);
        if (hBrBkg) DeleteObject(hBrBkg);
        PostQuitMessage(0);
        break;
        
    // 简单的背景色处理，让 Static 控件背景与窗口一致
    case WM_CTLCOLORSTATIC:
        {
            HDC hdc = (HDC)wParam;
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH); // 或 hBrBkg
        }
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// --- 主窗口创建函数 ---
bool CreateMainWindow(HINSTANCE hInstance, int nShow) {
    // 注册窗口类
    WNDCLASSEXW wcex = {0};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszClassName = L"ProxyAggregatorClass";
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101)); // 假设资源中有图标

    if (!RegisterClassExW(&wcex)) return false;

    // 创建窗口 (固定大小，类似于 Python 的 850x980，不可调整大小以简化布局)
    g_hMainWnd = CreateWindowW(L"ProxyAggregatorClass", L"代理聚合器 (C语言重构版)", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, 0, 850, 920, NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) return false;

    ShowWindow(g_hMainWnd, nShow);
    UpdateWindow(g_hMainWnd);
    
    // 初始化全局句柄
    // (已在 CreateControls 中赋值)

    return true;
}

// 获取全局句柄
HWND GetMainWindowHandle() {
    return g_hMainWnd;
}

// 获取订阅输入框内容的接口 (供 aggregator_core 使用)
int GetSubsInputText(char* buffer, int maxLen) {
    if (hEdtSubs) {
        return GetWindowTextA(hEdtSubs, buffer, maxLen);
    }
    return 0;
}

// 设置订阅输入框内容的接口
void SetSubsInputText(const char* text) {
    if (hEdtSubs) {
        SetWindowTextA(hEdtSubs, text);
    }
}
