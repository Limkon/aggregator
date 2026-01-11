// src/gui_main.c
// 调整包含顺序：common.h 必须在 gui.h 之前
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
#define ID_EDT_PAGES        1007 
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
static HBRUSH hBrBkg = NULL; // 全局背景画刷

// --- 外部业务函数声明 ---
extern void StartSearchTask();
extern void StartProcessTask();
extern void StopCurrentTask();
extern void SaveResultToFile(HWND hOwner, const char* content);

// --- 辅助函数：创建字体 (强制与系统一致) ---
void CreateAppFont() {
    NONCLIENTMETRICSW ncm = { sizeof(NONCLIENTMETRICSW) };
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0)) {
        hFontApp = CreateFontIndirectW(&ncm.lfMessageFont);
    } 
    if (!hFontApp) {
        hFontApp = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    }
    if (!hFontApp) hFontApp = GetStockObject(DEFAULT_GUI_FONT);
}

// --- 辅助函数：回调设置子控件字体 ---
BOOL CALLBACK EnumChildProcSetFont(HWND hwnd, LPARAM lParam) {
    SendMessage(hwnd, WM_SETFONT, (WPARAM)lParam, TRUE);
    return TRUE;
}

// --- 辅助函数：追加日志 (优化版) ---
void GuiAppendLog(const char* message) {
    if (!hEdtLog) return;
    
    // [优化] 防止日志堆积导致卡顿
    // 如果日志超过 30KB，清空一半或全部，保持 Edit 控件轻量
    int len = GetWindowTextLength(hEdtLog);
    if (len > 30000) {
        SetWindowTextA(hEdtLog, ""); // 简单粗暴：清空，防止越来越卡
        len = 0;
    }

    SendMessage(hEdtLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    
    // 转换编码
    int wlen = MultiByteToWideChar(CP_UTF8, 0, message, -1, NULL, 0);
    if (wlen > 0) {
        wchar_t* wBuf = (wchar_t*)malloc((wlen + 2) * sizeof(wchar_t));
        if (wBuf) {
            MultiByteToWideChar(CP_UTF8, 0, message, -1, wBuf, wlen);
            wcscat(wBuf, L"\r\n");
            SendMessageW(hEdtLog, EM_REPLACESEL, 0, (LPARAM)wBuf);
            free(wBuf);
        }
    }
}

// --- 辅助函数：更新 UI 状态 ---
void SetGuiState(bool isRunning) {
    BOOL bEnable = !isRunning;
    EnableWindow(hBtnSearch, bEnable);
    EnableWindow(hBtnRun, bEnable);
    EnableWindow(hBtnStop, !bEnable);
}

// --- 辅助函数：读取 UI 配置 ---
void SyncConfigFromUI() {
    char buf[1024];
    GetWindowTextA(hEdtToken, g_config.github_token, sizeof(g_config.github_token));
    GetWindowTextA(hEdtQuery, g_config.search_keywords, sizeof(g_config.search_keywords));
    GetWindowTextA(hEdtPages, buf, sizeof(buf));
    g_config.search_pages = atoi(buf);
    if (g_config.search_pages < 1) g_config.search_pages = 1;

    g_config.enable_proxy = (SendMessage(hChkProxy, BM_GETCHECK, 0, 0) == BST_CHECKED);
    GetWindowTextA(hEdtProxy, g_config.proxy_url, sizeof(g_config.proxy_url));

    g_config.enable_speedtest = (SendMessage(hChkSpeed, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_config.test_mode = (SendMessage(hRadSingbox, BM_GETCHECK, 0, 0) == BST_CHECKED) ? TEST_MODE_SINGBOX : TEST_MODE_TCP;
    
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
            hBrBkg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE)); 

            // 1. 搜索
            HWND hGrpSearch = CreateWindowW(L"BUTTON", L"在线搜索订阅", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 10, 810, 100, hWnd, (HMENU)ID_GRP_SEARCH, NULL, NULL);
            SendMessage(hGrpSearch, WM_SETFONT, (WPARAM)hFontApp, TRUE);

            CreateWindowW(L"STATIC", L"GitHub Token:", WS_CHILD | WS_VISIBLE, 25, 35, 90, 20, hWnd, (HMENU)ID_LBL_TOKEN, NULL, NULL);
            hEdtToken = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 120, 32, 500, 23, hWnd, (HMENU)ID_EDT_TOKEN, NULL, NULL);
            SendMessage(hEdtToken, EM_SETCUEBANNER, TRUE, (LPARAM)L"（可选，提高限额）");

            CreateWindowW(L"STATIC", L"搜索关键字:", WS_CHILD | WS_VISIBLE, 25, 65, 90, 20, hWnd, (HMENU)ID_LBL_QUERY, NULL, NULL);
            hEdtQuery = CreateWindowW(L"EDIT", L"clash,v2ray,sub,vmess,trojan,vless,hysteria2", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 120, 62, 500, 23, hWnd, (HMENU)ID_EDT_QUERY, NULL, NULL);

            CreateWindowW(L"STATIC", L"页数:", WS_CHILD | WS_VISIBLE, 640, 65, 40, 20, hWnd, (HMENU)ID_LBL_PAGES, NULL, NULL);
            hEdtPages = CreateWindowW(L"EDIT", L"2", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 680, 62, 40, 23, hWnd, (HMENU)ID_EDT_PAGES, NULL, NULL);

            hBtnSearch = CreateWindowW(L"BUTTON", L"开始搜索", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 640, 30, 80, 25, hWnd, (HMENU)ID_BTN_SEARCH, NULL, NULL);
            hBtnStop = CreateWindowW(L"BUTTON", L"中止", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 730, 30, 80, 25, hWnd, (HMENU)ID_BTN_STOP, NULL, NULL);

            // 2. 订阅
            CreateWindowW(L"BUTTON", L"订阅链接 / 节点 (一行一个)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 120, 810, 150, hWnd, (HMENU)ID_GRP_SUBS, NULL, NULL);
            hEdtSubs = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 20, 145, 790, 115, hWnd, (HMENU)ID_EDT_SUBS, NULL, NULL);

            // 3. 代理
            CreateWindowW(L"BUTTON", L"前置代理", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 280, 810, 60, hWnd, (HMENU)ID_GRP_PROXY, NULL, NULL);
            hChkProxy = CreateWindowW(L"BUTTON", L"启用代理", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 305, 80, 20, hWnd, (HMENU)ID_CHK_PROXY, NULL, NULL);
            CreateWindowW(L"STATIC", L"地址:", WS_CHILD | WS_VISIBLE, 120, 307, 40, 20, hWnd, (HMENU)ID_LBL_PROXY_URL, NULL, NULL);
            hEdtProxy = CreateWindowW(L"EDIT", L"http://127.0.0.1:10809", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 160, 305, 630, 23, hWnd, (HMENU)ID_EDT_PROXY, NULL, NULL);

            // 4. 测速
            CreateWindowW(L"BUTTON", L"测速配置", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 350, 810, 120, hWnd, (HMENU)ID_GRP_SPEED, NULL, NULL);
            hChkSpeed = CreateWindowW(L"BUTTON", L"启用测速", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 25, 375, 100, 20, hWnd, (HMENU)ID_CHK_SPEED, NULL, NULL);
            SendMessage(hChkSpeed, BM_SETCHECK, BST_UNCHECKED, 0);

            hRadSingbox = CreateWindowW(L"BUTTON", L"Sing-box (真实延迟)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP, 130, 375, 140, 20, hWnd, (HMENU)ID_RAD_SINGBOX, NULL, NULL);
            hRadTcp = CreateWindowW(L"BUTTON", L"TCP Ping (握手延迟)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 280, 375, 140, 20, hWnd, (HMENU)ID_RAD_TCP, NULL, NULL);
            SendMessage(hRadSingbox, BM_SETCHECK, BST_CHECKED, 0);

            CreateWindowW(L"STATIC", L"超时(ms):", WS_CHILD | WS_VISIBLE, 25, 405, 60, 20, hWnd, (HMENU)ID_LBL_TIMEOUT, NULL, NULL);
            hEdtTimeout = CreateWindowW(L"EDIT", L"3000", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 90, 402, 50, 23, hWnd, (HMENU)ID_EDT_TIMEOUT, NULL, NULL);
            
            CreateWindowW(L"STATIC", L"并发数:", WS_CHILD | WS_VISIBLE, 160, 405, 50, 20, hWnd, (HMENU)ID_LBL_CONCUR, NULL, NULL);
            hEdtConcur = CreateWindowW(L"EDIT", L"10", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 210, 402, 40, 23, hWnd, (HMENU)ID_EDT_CONCUR, NULL, NULL);
            
            CreateWindowW(L"STATIC", L"测速地址:", WS_CHILD | WS_VISIBLE, 270, 405, 60, 20, hWnd, (HMENU)ID_LBL_TESTURL, NULL, NULL);
            hEdtTestUrl = CreateWindowW(L"EDIT", L"https://www.google.com", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 335, 402, 455, 23, hWnd, (HMENU)ID_EDT_TESTURL, NULL, NULL);
            
            // 5. 执行
            hBtnRun = CreateWindowW(L"BUTTON", L"执行聚合处理 (含测速)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 25, 435, 765, 30, hWnd, (HMENU)ID_BTN_RUN, NULL, NULL);

            // 6. 日志
            CreateWindowW(L"BUTTON", L"处理日志", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 480, 810, 200, hWnd, (HMENU)ID_GRP_LOG, NULL, NULL);
            hEdtLog = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 20, 505, 790, 165, hWnd, (HMENU)ID_EDT_LOG, NULL, NULL);

            // 7. 结果
            CreateWindowW(L"BUTTON", L"结果预览 (Base64)", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 10, 690, 810, 180, hWnd, (HMENU)ID_GRP_RESULT, NULL, NULL);
            hBtnSave = CreateWindowW(L"BUTTON", L"保存为文件...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 700, 710, 100, 25, hWnd, (HMENU)ID_BTN_SAVE, NULL, NULL);
            hEdtResult = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL, 20, 740, 790, 120, hWnd, (HMENU)ID_EDT_RESULT, NULL, NULL);

            // 设置字体
            if (hFontApp) EnumChildWindows(hWnd, EnumChildProcSetFont, (LPARAM)hFontApp);
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
                StartSearchTask(); 
            }
            else if (id == ID_BTN_RUN && code == BN_CLICKED) {
                SyncConfigFromUI();
                SetGuiState(true);
                GuiAppendLog("--- 开始聚合 ---");
                StartProcessTask(); 
            }
            else if (id == ID_BTN_STOP && code == BN_CLICKED) {
                GuiAppendLog("正在请求中止...");
                StopCurrentTask();
            }
            else if (id == ID_BTN_SAVE && code == BN_CLICKED) {
                // [修改] 优先使用全局缓存保存文件
                if (g_full_result_content && strlen(g_full_result_content) > 0) {
                     SaveResultToFile(hWnd, g_full_result_content);
                } else {
                    // 如果没有全局缓存，说明可能没运行或已清理，尝试从 UI 获取（兼容旧逻辑）
                    int len = GetWindowTextLength(hEdtResult);
                    if (len > 0) {
                        char* buf = (char*)malloc(len + 1);
                        if (buf) {
                            GetWindowTextA(hEdtResult, buf, len + 1);
                            SaveResultToFile(hWnd, buf);
                            free(buf);
                        }
                    } else {
                        MessageBoxW(hWnd, L"没有内容可保存", L"提示", MB_OK);
                    }
                }
            }
        }
        break;

    case WM_APP_LOG:
        {
            char* msg = (char*)lParam;
            GuiAppendLog(msg);
            if (msg) free(msg); 
        }
        break;

    case WM_APP_TASK_DONE:
        SetGuiState(false);
        GuiAppendLog("--- 任务结束 ---");
        break;
        
    case WM_APP_PREVIEW:
        {
             // [修改] 读取全局缓存进行预览
             char* result = g_full_result_content;
             if (result) {
                 // [优化] 预览界面截断显示，防止 Edit 控件卡死，但保存时会使用完整数据
                 if (strlen(result) > 65535) {
                     char temp[66000];
                     memcpy(temp, result, 65000);
                     strcpy(temp + 65000, "\r\n... (内容过长，请使用保存文件功能查看完整结果) ...");
                     SetWindowTextA(hEdtResult, temp);
                 } else {
                     SetWindowTextA(hEdtResult, result);
                 }
                 // 注意：此处不再释放 result，因为它是全局缓存 g_full_result_content 的指针
             }
        }
        break;

    case WM_DESTROY:
        // [新增] 退出前清理全局结果缓存
        if (g_full_result_content) {
            free(g_full_result_content);
            g_full_result_content = NULL;
        }
        if (hFontApp) DeleteObject(hFontApp);
        if (hBrBkg) DeleteObject(hBrBkg);
        PostQuitMessage(0);
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: 
        {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
            SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)hBrBkg;
        }
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// --- 主窗口创建函数 ---
bool CreateMainWindow(HINSTANCE hInstance, int nShow) {
    WNDCLASSEXW wcex = {0};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wcex.lpszClassName = L"ProxyAggregatorClass";
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(101)); 

    if (!RegisterClassExW(&wcex)) return false;

    g_hMainWnd = CreateWindowW(L"ProxyAggregatorClass", L"代理聚合器", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, 0, 850, 920, NULL, NULL, hInstance, NULL);

    if (!g_hMainWnd) return false;

    ShowWindow(g_hMainWnd, nShow);
    UpdateWindow(g_hMainWnd);
    return true;
}

HWND GetMainWindowHandle() { return g_hMainWnd; }

int GetSubsInputText(char* buffer, int maxLen) {
    if (hEdtSubs) return GetWindowTextA(hEdtSubs, buffer, maxLen);
    return 0;
}

void SetSubsInputText(const char* text) {
    if (hEdtSubs) SetWindowTextA(hEdtSubs, text);
}
