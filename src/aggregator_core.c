/* src/aggregator_core.c */
#include "common.h"
#include "gui.h"
#include "utils_net.h"
#include "utils_base64.h"
#include "cJSON.h"
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- 外部函数声明 ---
extern void SearchGitHubKeywords(const char* token, const char* query, int pages, HWND hNotify);
extern void ParseNodeBasic(const char* link, ProxyNode* node);
extern double SpeedTest_TcpPing(const char* addr, int port, int timeout);
extern double SpeedTest_Singbox(const char* node_link, int port_index, int timeout, const char* test_url);

// --- 全局变量引用 ---
extern HWND g_hMainWnd; 
static HANDLE hWorkerThread = NULL;

// --- 线程参数结构体 ---
typedef void (*TaskCallback)(void* data, int thread_idx);

typedef struct {
    void* item_ptr;
    int thread_idx;
    TaskCallback cb;
    HANDLE hSem;
    volatile long* pActive;
} ThreadArg;

// --- 辅助：检查是否已中止 ---
static BOOL IsTaskStopped() {
    return WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0;
}

// --- 辅助：重置全局节点列表 ---
static void ClearGlobalNodes() {
    EnterCriticalSection(&g_dataLock);
    g_node_count = 0;
    LeaveCriticalSection(&g_dataLock);
}

// --- 辅助：添加节点并去重 ---
static int AddNodeSafe(const char* link) {
    if (!link || strlen(link) < 10) return 0;
    
    int ret = 0;
    EnterCriticalSection(&g_dataLock);
    
    if (g_node_count >= MAX_NODES) {
        LeaveCriticalSection(&g_dataLock);
        return 0;
    }

    int exists = 0;
    for (int i = 0; i < g_node_count; i++) {
        if (strcmp(g_nodes[i].original_link, link) == 0) {
            exists = 1;
            break;
        }
    }

    if (!exists) {
        ProxyNode* node = &g_nodes[g_node_count];
        memset(node, 0, sizeof(ProxyNode));
        node->id = g_node_count;
        node->latency = -1.0; 
        strncpy(node->original_link, link, MAX_URL_LEN - 1);
        
        ParseNodeBasic(link, node);
        
        g_node_count++;
        ret = 1;
    }
    
    LeaveCriticalSection(&g_dataLock);
    return ret;
}

// --- 任务 1: 搜索线程 ---
unsigned int __stdcall SearchThreadProc(void* arg) {
    ClearGlobalNodes();
    SearchGitHubKeywords(g_config.github_token, g_config.search_keywords, g_config.search_pages, g_hMainWnd);
    PostMessage(g_hMainWnd, WM_APP_TASK_DONE, 0, 0);
    return 0;
}

// --- 任务 2: 聚合处理线程 ---

// 工作子线程函数
unsigned int __stdcall WorkerProc(void* p) {
    ThreadArg* arg = (ThreadArg*)p;
    
    // 执行前检查停止信号
    if (WaitForSingleObject(g_hStopEvent, 0) != WAIT_OBJECT_0) {
        arg->cb(arg->item_ptr, arg->thread_idx);
    }
    
    InterlockedDecrement(arg->pActive);
    ReleaseSemaphore(arg->hSem, 1, NULL);
    free(arg);
    return 0;
}

// 并发任务执行器
static void RunConcurrentTasks(void* items, int count, int item_size, TaskCallback callback, int concurrency) {
    if (count <= 0) return;
    
    if (concurrency < 1) concurrency = 1;
    if (concurrency > 64) concurrency = 64; 
    
    HANDLE hSemaphore = CreateSemaphore(NULL, concurrency, concurrency, NULL);
    volatile long active_count = 0;
    
    HANDLE waitHandles[2] = { g_hStopEvent, hSemaphore };

    for (int i = 0; i < count; i++) {
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        
        if (waitResult == WAIT_OBJECT_0) {
            PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(">>> 中止信号收到，停止分发..."));
            break; 
        }
        else if (waitResult == WAIT_OBJECT_0 + 1) {
            InterlockedIncrement(&active_count);
            
            ThreadArg* arg = (ThreadArg*)malloc(sizeof(ThreadArg));
            if (arg) {
                arg->item_ptr = (char*)items + (i * item_size);
                arg->thread_idx = i % concurrency;
                arg->cb = callback;
                arg->hSem = hSemaphore;
                arg->pActive = &active_count;
                
                HANDLE hT = (HANDLE)_beginthreadex(NULL, 0, WorkerProc, arg, 0, NULL);
                if (hT) CloseHandle(hT);
                else {
                    InterlockedDecrement(&active_count);
                    ReleaseSemaphore(hSemaphore, 1, NULL);
                    free(arg);
                }
            } else {
                InterlockedDecrement(&active_count);
                ReleaseSemaphore(hSemaphore, 1, NULL);
            }
            
            if (i % 5 == 0) PostMessage(g_hMainWnd, WM_APP_PROGRESS, i, count);
        }
        else {
            break; 
        }
    }
    
    // [关键修复] 防止卡死：增加超时等待
    // 如果子线程卡在 DNS 解析(getaddrinfo)，active_count 不会归零
    // 我们最多等 3 秒，等不到就强制退出，避免界面无响应
    DWORD start_tick = GetTickCount();
    while (active_count > 0) {
        if (GetTickCount() - start_tick > 3000) { 
            PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(">>> 等待子线程超时，强制结束当前流程..."));
            break; 
        }
        Sleep(50);
    }
    
    CloseHandle(hSemaphore);
}

// --- 子任务 A: 下载并解析订阅 ---
typedef struct {
    char url[MAX_URL_LEN];
} SubLink;

static char* CleanBase64(const char* input) {
    if (!input) return NULL;
    size_t len = strlen(input);
    char* clean = (char*)malloc(len + 1);
    if (!clean) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if ((input[i] >= 'A' && input[i] <= 'Z') ||
            (input[i] >= 'a' && input[i] <= 'z') ||
            (input[i] >= '0' && input[i] <= '9') ||
            input[i] == '+' || input[i] == '/' || input[i] == '=' || input[i] == '-' || input[i] == '_') {
            clean[j++] = input[i];
        }
    }
    clean[j] = 0;
    return clean;
}

void DownloadSubWorker(void* data, int idx) {
    SubLink* link = (SubLink*)data;
    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), "[T-%d] 下载: %s", idx, link->url);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    
    const char* proxy = g_config.enable_proxy ? g_config.proxy_url : NULL;
    char* content = HttpGet(link->url, proxy, g_config.timeout);
    
    if (content) {
        int added = 0;
        int original_len = (int)strlen(content);
        BOOL is_plaintext = FALSE;
        if (strstr(content, "vmess://") || strstr(content, "ss://") || 
            strstr(content, "trojan://") || strstr(content, "vless://")) {
            is_plaintext = TRUE;
        }

        char* decoded = NULL;
        char* target = content;

        if (!is_plaintext) {
            char* clean_content = CleanBase64(content);
            if (clean_content && strlen(clean_content) > 0) {
                decoded = Base64Decode(clean_content);
            }
            if (clean_content) free(clean_content);
            if (decoded) target = decoded;
        }

        char* ctx = NULL;
        char* line = strtok_s(target, "\n\r", &ctx);
        while (line) {
            while (*line && (*line == ' ' || *line == '\t')) line++;
            if (*line) {
                if (strncmp(line, "vmess://", 8) == 0 ||
                    strncmp(line, "vless://", 8) == 0 ||
                    strncmp(line, "ss://", 5) == 0 ||
                    strncmp(line, "trojan://", 9) == 0 ||
                    strncmp(line, "hysteria2://", 12) == 0 ||
                    strncmp(line, "hy2://", 6) == 0) {
                    if (AddNodeSafe(line)) added++;
                }
            }
            line = strtok_s(NULL, "\n\r", &ctx);
        }
        
        if (decoded) free(decoded);
        free(content);
        snprintf(logBuf, sizeof(logBuf), "  -> [T-%d] 解析: %d 个", idx, added);
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    } else {
        snprintf(logBuf, sizeof(logBuf), "  -> [T-%d] 下载失败", idx);
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    }
}

// --- 子任务 B: 测速 ---
void SpeedTestWorker(void* data, int idx) {
    ProxyNode* node = (ProxyNode*)data;
    if (node->type == NODE_UNKNOWN && g_config.test_mode == TEST_MODE_TCP) {
        node->latency = -1.0;
        return;
    }
    double lat = -1.0;
    if (g_config.test_mode == TEST_MODE_TCP) {
        lat = SpeedTest_TcpPing(node->address, node->port, g_config.timeout);
    } else {
        lat = SpeedTest_Singbox(node->original_link, idx, g_config.timeout, g_config.test_url);
    }
    node->latency = lat;
    node->is_alive = (lat >= 0);
}

// --- 主聚合流程 ---
unsigned int __stdcall ProcessThreadProc(void* arg) {
    ClearGlobalNodes();
    char* input_text = (char*)arg; 
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("=== 开始聚合处理 ==="));

    SubLink* sub_links = (SubLink*)malloc(100 * sizeof(SubLink));
    int sub_count = 0;
    
    char* ctx = NULL;
    char* line = strtok_s(input_text, "\n\r", &ctx);
    while (line) {
        while (*line && isspace((unsigned char)*line)) line++;
        if (strlen(line) > 0) {
            if (strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0) {
                if (sub_count < 100) {
                    strncpy(sub_links[sub_count].url, line, MAX_URL_LEN-1);
                    sub_count++;
                }
            } else {
                AddNodeSafe(line);
            }
        }
        line = strtok_s(NULL, "\n\r", &ctx);
    }
    
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "直接节点: %d, 订阅链接: %d", g_node_count, sub_count);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    
    if (sub_count > 0 && !IsTaskStopped()) {
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("--- 开始下载订阅 ---"));
        RunConcurrentTasks(sub_links, sub_count, sizeof(SubLink), DownloadSubWorker, g_config.concurrency);
    }
    free(sub_links);

    snprintf(logBuf, sizeof(logBuf), "总节点数: %d", g_node_count);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    
    if (g_node_count == 0) {
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("错误: 未找到任何节点。"));
        goto cleanup;
    }

    if (g_config.enable_speedtest && !IsTaskStopped()) {
        char modeStr[64];
        snprintf(modeStr, sizeof(modeStr), "--- 开始测速 (%s, 并发 %d) ---", 
            g_config.test_mode == TEST_MODE_TCP ? "TCP Ping" : "Sing-box", 
            g_config.concurrency);
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(modeStr));
        
        RunConcurrentTasks(g_nodes, g_node_count, sizeof(ProxyNode), SpeedTestWorker, g_config.concurrency);
        
        if (!IsTaskStopped()) {
            int NodeCompare(const void* a, const void* b) {
                ProxyNode* na = (ProxyNode*)a;
                ProxyNode* nb = (ProxyNode*)b;
                if (na->latency < 0 && nb->latency < 0) return 0;
                if (na->latency < 0) return 1;
                if (nb->latency < 0) return -1;
                return (na->latency > nb->latency) ? 1 : -1;
            }
            qsort(g_nodes, g_node_count, sizeof(ProxyNode), NodeCompare);
            
            int alive = 0;
            for (int i=0; i<g_node_count; i++) if(g_nodes[i].latency >= 0) alive++;
            snprintf(logBuf, sizeof(logBuf), "测速完成。存活节点: %d", alive);
            PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
        } else {
             PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("测速已中止，保留现有结果..."));
        }
    }

    size_t bufSize = g_node_count * (MAX_URL_LEN + 2) + 1024;
    char* fullText = (char*)malloc(bufSize);
    if (fullText) {
        char* ptr = fullText;
        *ptr = 0;

        for (int i = 0; i < g_node_count; i++) {
            if (g_config.enable_speedtest && !IsTaskStopped()) {
                if (g_nodes[i].latency < 0) continue; 
            }
            size_t len = strlen(g_nodes[i].original_link);
            if (len > 0) {
                memcpy(ptr, g_nodes[i].original_link, len);
                ptr += len;
                *ptr++ = '\n'; 
            }
        }
        *ptr = 0;
        
        if ((ptr == fullText) && g_node_count > 0) {
             ptr = fullText;
             for (int i = 0; i < g_node_count; i++) {
                size_t len = strlen(g_nodes[i].original_link);
                memcpy(ptr, g_nodes[i].original_link, len);
                ptr += len;
                *ptr++ = '\n';
            }
            *ptr = 0;
        }
        
        char* b64Result = Base64Encode((unsigned char*)fullText, strlen(fullText));
        if (b64Result) {
            PostMessage(g_hMainWnd, WM_APP_PREVIEW, 0, (LPARAM)b64Result);
        }
        free(fullText);
    }

cleanup:
    free(input_text);
    PostMessage(g_hMainWnd, WM_APP_TASK_DONE, 1, 0);
    return 0;
}

void StartSearchTask() {
    if (hWorkerThread) CloseHandle(hWorkerThread);
    ResetEvent(g_hStopEvent);
    hWorkerThread = (HANDLE)_beginthreadex(NULL, 0, SearchThreadProc, NULL, 0, NULL);
}

void StartProcessTask() {
    if (hWorkerThread) CloseHandle(hWorkerThread);
    ResetEvent(g_hStopEvent);
    
    int maxLen = 1024 * 1024;
    char* buf = (char*)malloc(maxLen);
    extern int GetSubsInputText(char* buffer, int maxLen);
    if (GetSubsInputText(buf, maxLen) <= 0) buf[0] = 0;
    
    hWorkerThread = (HANDLE)_beginthreadex(NULL, 0, ProcessThreadProc, buf, 0, NULL);
}

void StopCurrentTask() {
    SetEvent(g_hStopEvent);
}

void SaveResultToFile(HWND hOwner, const char* content) {
    OPENFILENAMEA ofn;
    char szFile[260] = "subscriptions.txt";

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hOwner;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Text Files\0*.txt\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    if (GetSaveFileNameA(&ofn) == TRUE) {
        FILE* fp = fopen(ofn.lpstrFile, "w");
        if (fp) {
            fputs(content, fp);
            fclose(fp);
            MessageBoxA(hOwner, "保存成功！", "提示", MB_OK);
        } else {
            MessageBoxA(hOwner, "保存失败，无法写入文件。", "错误", MB_ICONERROR);
        }
    }
}
