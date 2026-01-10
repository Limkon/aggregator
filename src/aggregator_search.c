#include "common.h"
#include "cJSON.h"
#include "utils_net.h"
#include "utils_base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

// --- 外部引用 ---
extern void GuiAppendLog(const char* message);
extern void SetSubsInputText(const char* text);
extern int GetSubsInputText(char* buffer, int maxLen);
extern char* NetRequestWithProxy(const char* url, const char* token, const char* proxy, int timeout_sec);

// --- 任务控制块 (用于搜索) ---
typedef struct {
    HANDLE hSem;
    volatile long active_count;
    volatile long ref_count;
    // [修复] 将缓冲区由 TCB 管理，防止主线程退出后被释放
    char* result_buf; 
    size_t result_max;
    size_t result_len;
    CRITICAL_SECTION lock;
} SearchTCB;

// --- 线程任务结构 ---
typedef struct {
    char url[MAX_URL_LEN];
    char proxy[MAX_URL_LEN];
    HWND hNotify;
    int index;
    int timeout;
    SearchTCB* tcb; // 引用 TCB
} SearchDlTask;

// --- 辅助：从文本提取节点 ---
static int ExtractNodesFromText(char* text, char* out_buf, size_t buf_max, size_t* current_len) {
    if (!text || !out_buf) return 0;
    
    int count = 0;
    char* ctx = NULL;
    char* line = strtok_s(text, "\n", &ctx);
    
    while (line) {
        while(*line && (*line==' ' || *line=='\r' || *line=='\t')) line++;
        if (strncmp(line, "vmess://", 8) == 0 ||
            strncmp(line, "vless://", 8) == 0 ||
            strncmp(line, "ss://", 5) == 0 ||
            strncmp(line, "trojan://", 9) == 0 ||
            strncmp(line, "hysteria2://", 12) == 0 ||
            strncmp(line, "hy2://", 6) == 0) {
            
            size_t line_len = strlen(line);
            if (*current_len + line_len + 2 < buf_max) {
                strcat(out_buf, line);
                strcat(out_buf, "\r\n");
                *current_len += (line_len + 2);
                count++;
            }
        }
        line = strtok_s(NULL, "\n", &ctx);
    }
    return count;
}

// --- 搜索下载工作线程 ---
unsigned int __stdcall SearchDownloadWorker(void* p) {
    SearchDlTask* task = (SearchDlTask*)p;
    SearchTCB* tcb = task->tcb;
    char logBuf[256];
    
    // 如果任务未中止，则执行网络请求
    if (WaitForSingleObject(g_hStopEvent, 0) != WAIT_OBJECT_0) {
        char* file_content = NetRequestWithProxy(task->url, NULL, task->proxy[0] ? task->proxy : NULL, task->timeout);
        
        if (file_content) {
            size_t local_buf_size = 64 * 1024; 
            char* local_buf = (char*)malloc(local_buf_size);
            size_t local_len = 0;
            int found = 0;

            if (local_buf) {
                local_buf[0] = 0;
                char* content_copy = _strdup(file_content);
                found = ExtractNodesFromText(content_copy, local_buf, local_buf_size, &local_len);
                free(content_copy);

                if (found == 0) {
                    char* decoded = Base64Decode(file_content);
                    if (decoded) {
                        found = ExtractNodesFromText(decoded, local_buf, local_buf_size, &local_len);
                        free(decoded);
                    }
                }
                
                if (found > 0) {
                    // 安全写入 TCB 缓冲区
                    EnterCriticalSection(&tcb->lock);
                    if (tcb->result_len + local_len < tcb->result_max) {
                        strcat(tcb->result_buf, local_buf);
                        tcb->result_len += local_len;
                    }
                    LeaveCriticalSection(&tcb->lock);
                    
                    snprintf(logBuf, sizeof(logBuf), "    [T-%d] 提取到 %d 个节点", task->index, found);
                    PostMessage(task->hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
                }
                free(local_buf);
            }
            free(file_content);
        } else {
             snprintf(logBuf, sizeof(logBuf), "    [T-%d] 下载失败或超时", task->index);
             PostMessage(task->hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
        }
    }
    
    // 释放资源
    ReleaseSemaphore(tcb->hSem, 1, NULL);
    
    // 引用计数递减与自清理
    if (InterlockedDecrement(&tcb->ref_count) == 0) {
        DeleteCriticalSection(&tcb->lock);
        CloseHandle(tcb->hSem);
        free(tcb->result_buf);
        free(tcb);
    }
    
    free(task);
    return 0;
}

// --- 批量执行下载 (安全版) ---
static void ProcessBatchDownloads(cJSON* items, const char* proxy, HWND hNotify, SearchTCB* tcb) {
    int count = cJSON_GetArraySize(items);
    if (count <= 0) return;

    for (int i = 0; i < count; i++) {
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;

        // 等待并发名额
        DWORD waitResult = WaitForSingleObject(tcb->hSem, 5000); 
        if (waitResult != WAIT_OBJECT_0) continue; 

        cJSON* item = cJSON_GetArrayItem(items, i);
        cJSON* html_url = cJSON_GetObjectItem(item, "html_url");
        
        if (html_url && html_url->valuestring) {
            char raw_url[MAX_URL_LEN];
            char* p = strstr(html_url->valuestring, "github.com");
            if (p) {
                snprintf(raw_url, sizeof(raw_url), "https://raw.githubusercontent.com%s", p + 10);
                char* blob = strstr(raw_url, "/blob/");
                if (blob) {
                    char temp[MAX_URL_LEN];
                    *blob = 0; 
                    snprintf(temp, sizeof(temp), "%s%s", raw_url, blob + 5); 
                    strcpy(raw_url, temp);
                }

                SearchDlTask* task = (SearchDlTask*)malloc(sizeof(SearchDlTask));
                strncpy(task->url, raw_url, MAX_URL_LEN - 1);
                if (proxy) strncpy(task->proxy, proxy, MAX_URL_LEN - 1);
                else task->proxy[0] = 0;
                task->hNotify = hNotify;
                task->index = i;
                task->tcb = tcb;
                task->timeout = g_config.timeout;

                InterlockedIncrement(&tcb->ref_count); // 增加引用
                
                HANDLE hT = (HANDLE)_beginthreadex(NULL, 0, SearchDownloadWorker, task, 0, NULL);
                if (hT) {
                    CloseHandle(hT);
                } else {
                    free(task);
                    ReleaseSemaphore(tcb->hSem, 1, NULL);
                    InterlockedDecrement(&tcb->ref_count); // 回滚引用
                }
            } else {
                ReleaseSemaphore(tcb->hSem, 1, NULL);
            }
        } else {
             ReleaseSemaphore(tcb->hSem, 1, NULL);
        }
    }
}

// --- 搜索主入口 ---
void SearchGitHubKeywords(const char* token, const char* query, int pages, HWND hNotify) {
    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), "搜索任务启动，并发: %d, 超时: %dms", 
             g_config.concurrency, g_config.timeout);
    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));

    if (!query || strlen(query) == 0) return;

    // 1. 初始化 TCB
    SearchTCB* tcb = (SearchTCB*)malloc(sizeof(SearchTCB));
    if (!tcb) return;
    
    tcb->result_max = 2 * 1024 * 1024;
    tcb->result_buf = (char*)malloc(tcb->result_max);
    if (!tcb->result_buf) { free(tcb); return; }
    tcb->result_buf[0] = 0;
    
    int concurrency = g_config.concurrency;
    if (concurrency < 1) concurrency = 1;
    tcb->hSem = CreateSemaphore(NULL, concurrency, concurrency, NULL);
    InitializeCriticalSection(&tcb->lock);
    tcb->ref_count = 1; // 主线程持有

    // 读取现有内容
    GetSubsInputText(tcb->result_buf, (int)tcb->result_max);
    tcb->result_len = strlen(tcb->result_buf);
    if (tcb->result_len > 0 && tcb->result_buf[tcb->result_len-1] != '\n') {
        strcat(tcb->result_buf, "\r\n");
        tcb->result_len += 2;
    }

    char* kw_copy = _strdup(query);
    char* keywords[10];
    int kw_count = 0;
    char* ctx_token = NULL;
    char* k_token = strtok_s(kw_copy, ",", &ctx_token);
    while (k_token && kw_count < 10) {
        keywords[kw_count++] = k_token;
        k_token = strtok_s(NULL, ",", &ctx_token);
    }

    const char* proxy_url = (g_config.enable_proxy && g_config.proxy_url[0]) ? g_config.proxy_url : NULL;
    const char* extensions[] = {"yaml", "txt", "conf"}; 
    int ext_count = 3;

    for (int k = 0; k < kw_count; k++) {
        for (int e = 0; e < ext_count; e++) {
            for (int page = 1; page <= pages; page++) {
                
                if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) {
                    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup("搜索已中止"));
                    goto cleanup;
                }

                char url[MAX_URL_LEN];
                snprintf(url, sizeof(url), 
                    "https://api.github.com/search/code?q=%s+extension:%s&per_page=10&page=%d&sort=indexed&order=desc", 
                    keywords[k], extensions[e], page);
                
                snprintf(logBuf, sizeof(logBuf), "搜索 API: %s (Ext: %s, 页 %d)", keywords[k], extensions[e], page);
                PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));

                // 主线程请求 API，如果超时会卡住，但可以接受 (不是死锁)
                const char* api_token = (token && token[0]) ? token : NULL;
                char* response = NetRequestWithProxy(url, api_token, proxy_url, g_config.timeout);
                
                if (!response) {
                    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup("请求失败 (可能超时或被限流)"));
                    Sleep(2000); 
                    continue;
                }

                cJSON* root = cJSON_Parse(response);
                if (!root) { free(response); continue; }

                cJSON* items = cJSON_GetObjectItem(root, "items");
                if (cJSON_IsArray(items)) {
                    int count = cJSON_GetArraySize(items);
                    if (count == 0) {
                         cJSON_Delete(root);
                         free(response);
                         break; 
                    }
                    snprintf(logBuf, sizeof(logBuf), "  > 发现 %d 个文件，加入下载队列...", count);
                    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));

                    ProcessBatchDownloads(items, proxy_url, hNotify, tcb);
                }
                
                cJSON_Delete(root);
                free(response);
                Sleep(1000); 
            }
        }
    }

    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup("搜索完成，等待剩余任务..."));

cleanup:
    // 等待所有后台线程结束，或者超时强制退出 (防止卡 UI)
    // 因为用了 TCB 引用计数，这里可以安全地“放弃”等待
    DWORD start_wait = GetTickCount();
    while (tcb->ref_count > 1) { // >1 说明还有子线程
        if (GetTickCount() - start_wait > 5000) { // 等待 5 秒
             PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(">>> 等待子线程超时，后台处理中..."));
             break;
        }
        Sleep(100);
    }
    
    // 只要主线程还没放弃 TCB，result_buf 就是有效的，可以更新 UI
    SetSubsInputText(tcb->result_buf);
    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup("结果已更新"));

    // 释放主线程引用
    if (InterlockedDecrement(&tcb->ref_count) == 0) {
        DeleteCriticalSection(&tcb->lock);
        CloseHandle(tcb->hSem);
        free(tcb->result_buf);
        free(tcb);
    }
    
    if (kw_copy) free(kw_copy);
}
