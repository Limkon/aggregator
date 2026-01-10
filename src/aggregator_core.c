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

typedef void (*TaskCallback)(void* data, int thread_idx);

static void RunConcurrentTasks(void* items, int count, int item_size, TaskCallback callback, int concurrency) {
    if (count <= 0) return;
    
    HANDLE* threads = (HANDLE*)malloc(concurrency * sizeof(HANDLE));
    HANDLE hSemaphore = CreateSemaphore(NULL, concurrency, concurrency, NULL);
    volatile long active_count = 0;
    
    typedef struct {
        void* item_ptr;
        int thread_idx;
        TaskCallback cb;
        HANDLE hSem;
        volatile long* pActive;
    } ThreadArg;

    unsigned int __stdcall Worker(void* p) {
        ThreadArg* arg = (ThreadArg*)p;
        if (WaitForSingleObject(g_hStopEvent, 0) != WAIT_OBJECT_0) {
            arg->cb(arg->item_ptr, arg->thread_idx);
        }
        InterlockedDecrement(arg->pActive);
        ReleaseSemaphore(arg->hSem, 1, NULL);
        free(arg);
        return 0;
    }

    for (int i = 0; i < count; i++) {
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;
        
        WaitForSingleObject(hSemaphore, INFINITE);
        InterlockedIncrement(&active_count);
        
        ThreadArg* arg = (ThreadArg*)malloc(sizeof(ThreadArg));
        arg->item_ptr = (char*)items + (i * item_size);
        arg->thread_idx = i % concurrency;
        arg->cb = callback;
        arg->hSem = hSemaphore;
        arg->pActive = &active_count;
        
        HANDLE hT = (HANDLE)_beginthreadex(NULL, 0, Worker, arg, 0, NULL);
        if (hT) CloseHandle(hT); 
        else {
             InterlockedDecrement(&active_count);
             ReleaseSemaphore(hSemaphore, 1, NULL);
             free(arg);
        }
        
        if (i % 5 == 0) PostMessage(g_hMainWnd, WM_APP_PROGRESS, i, count);
    }
    
    while (active_count > 0) Sleep(50);
    CloseHandle(hSemaphore);
    free(threads);
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
    char* content = HttpGet(link->url, proxy, 20);
    
    if (content) {
        int added = 0;
        int original_len = (int)strlen(content);

        // [修复核心] 智能探测：如果已经是明文，千万不要解码！
        BOOL is_plaintext = FALSE;
        if (strstr(content, "vmess://") || strstr(content, "ss://") || 
            strstr(content, "trojan://") || strstr(content, "vless://")) {
            is_plaintext = TRUE;
        }

        char* decoded = NULL;
        char* target = content;

        if (!is_plaintext) {
            // 只有不包含协议头时，才尝试 Base64 解码
            char* clean_content = CleanBase64(content);
            if (clean_content && strlen(clean_content) > 0) {
                decoded = Base64Decode(clean_content);
            }
            if (clean_content) free(clean_content);
            
            if (decoded) target = decoded;
        }

        // 2. 按行扫描
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
        
        snprintf(logBuf, sizeof(logBuf), "  -> [T-%d] 完成，大小: %d B, 解析: %d 个", idx, original_len, added);
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    } else {
        snprintf(logBuf, sizeof(logBuf), "  -> [T-%d] 下载失败 (超时或网络错误)", idx);
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    }
}

// --- 子任务 B: 测速 ---
void SpeedTestWorker(void* data, int idx) {
    ProxyNode* node = (ProxyNode*)data;
    
    if (node->type == NODE_UNKNOWN && g_config.test_mode == TEST_MODE_TCP) {
        node->latency = -1;
        return;
    }
    
    double lat = -1;
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
    
    if (sub_count > 0) {
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("--- 开始下载订阅 ---"));
        RunConcurrentTasks(sub_links, sub_count, sizeof(SubLink), DownloadSubWorker, g_config.concurrency);
    }
    free(sub_links);
    
    if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) goto cleanup;

    snprintf(logBuf, sizeof(logBuf), "总节点数: %d", g_node_count);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    
    if (g_node_count == 0) {
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("错误: 未找到任何节点。"));
        goto cleanup;
    }

    if (g_config.enable_speedtest) {
        char modeStr[64];
        snprintf(modeStr, sizeof(modeStr), "--- 开始测速 (%s, 并发 %d) ---", 
            g_config.test_mode == TEST_MODE_TCP ? "TCP Ping" : "Sing-box", 
            g_config.concurrency);
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(modeStr));
        
        RunConcurrentTasks(g_nodes, g_node_count, sizeof(ProxyNode), SpeedTestWorker, g_config.concurrency);
        
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
    }

    size_t bufSize = g_node_count * (MAX_URL_LEN + 2) + 1024;
    char* fullText = (char*)malloc(bufSize);
    if (fullText) {
        fullText[0] = 0;
        for (int i = 0; i < g_node_count; i++) {
            if (g_config.enable_speedtest) {
                if (g_nodes[i].latency < 0) continue; 
            }
            strcat(fullText, g_nodes[i].original_link);
            strcat(fullText, "\n");
        }
        
        if (strlen(fullText) == 0 && g_node_count > 0) {
             for (int i = 0; i < g_node_count; i++) {
                strcat(fullText, g_nodes[i].original_link);
                strcat(fullText, "\n");
            }
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
    GetSubsInputText(buf, maxLen);
    
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
