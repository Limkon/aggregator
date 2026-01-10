#include "common.h"
#include "cJSON.h"
#include "utils_net.h"
#include "utils_base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h> // for _beginthreadex

// --- 外部引用 ---
extern void GuiAppendLog(const char* message);
extern void SetSubsInputText(const char* text);
extern int GetSubsInputText(char* buffer, int maxLen);
extern char* NetRequestWithProxy(const char* url, const char* token, const char* proxy);

// --- 全局变量与锁 (用于搜索任务) ---
static char* g_searchResultBuf = NULL;
static size_t g_searchResultMax = 0;
static size_t g_searchResultLen = 0;
static CRITICAL_SECTION g_searchLock;

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
            // 简单的缓冲区检查
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

// --- 线程任务结构 ---
typedef struct {
    char url[MAX_URL_LEN];
    char proxy[MAX_URL_LEN];
    HWND hNotify;
    int index;
} SearchDlTask;

// --- 搜索下载工作线程 ---
unsigned int __stdcall SearchDownloadWorker(void* p) {
    SearchDlTask* task = (SearchDlTask*)p;
    char logBuf[256];
    
    // 下载 (使用代理)
    char* file_content = NetRequestWithProxy(task->url, NULL, task->proxy[0] ? task->proxy : NULL);
    
    if (file_content) {
        // 提取逻辑需要独占缓冲区，或者我们先提取到临时 buffer 再加锁合并
        // 为了简单高效，先提取到本地 buffer
        size_t local_buf_size = 64 * 1024; // 64KB enough for one file usually
        char* local_buf = (char*)malloc(local_buf_size);
        size_t local_len = 0;
        int found = 0;

        if (local_buf) {
            local_buf[0] = 0;
            
            // 1. 直接提取
            char* content_copy = _strdup(file_content);
            found = ExtractNodesFromText(content_copy, local_buf, local_buf_size, &local_len);
            free(content_copy);

            // 2. Base64 尝试
            if (found == 0) {
                char* decoded = Base64Decode(file_content);
                if (decoded) {
                    found = ExtractNodesFromText(decoded, local_buf, local_buf_size, &local_len);
                    free(decoded);
                }
            }
            
            // 3. 合并到全局结果 (加锁)
            if (found > 0) {
                EnterCriticalSection(&g_searchLock);
                if (g_searchResultLen + local_len < g_searchResultMax) {
                    strcat(g_searchResultBuf, local_buf);
                    g_searchResultLen += local_len;
                }
                LeaveCriticalSection(&g_searchLock);
                
                snprintf(logBuf, sizeof(logBuf), "    [T-%d] 提取到 %d 个节点", task->index, found);
                PostMessage(task->hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
            }

            free(local_buf);
        }
        free(file_content);
    }
    
    free(task);
    return 0;
}

// --- 批量执行下载 ---
static void ProcessBatchDownloads(cJSON* items, const char* proxy, HWND hNotify) {
    int count = cJSON_GetArraySize(items);
    if (count <= 0) return;

    // 收集任务
    HANDLE* threads = (HANDLE*)malloc(count * sizeof(HANDLE));
    int thread_count = 0;

    for (int i = 0; i < count; i++) {
        // 检查停止
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;

        cJSON* item = cJSON_GetArrayItem(items, i);
        cJSON* html_url = cJSON_GetObjectItem(item, "html_url");
        
        if (html_url && html_url->valuestring) {
            char raw_url[MAX_URL_LEN];
            char* p = strstr(html_url->valuestring, "github.com");
            if (p) {
                // 构造 raw url
                snprintf(raw_url, sizeof(raw_url), "https://raw.githubusercontent.com%s", p + 10);
                char* blob = strstr(raw_url, "/blob/");
                if (blob) {
                    char temp[MAX_URL_LEN];
                    *blob = 0; 
                    snprintf(temp, sizeof(temp), "%s%s", raw_url, blob + 5); 
                    strcpy(raw_url, temp);
                }

                // 创建任务
                SearchDlTask* task = (SearchDlTask*)malloc(sizeof(SearchDlTask));
                strncpy(task->url, raw_url, MAX_URL_LEN - 1);
                if (proxy) strncpy(task->proxy, proxy, MAX_URL_LEN - 1);
                else task->proxy[0] = 0;
                task->hNotify = hNotify;
                task->index = i;

                // 启动线程
                threads[thread_count] = (HANDLE)_beginthreadex(NULL, 0, SearchDownloadWorker, task, 0, NULL);
                if (threads[thread_count]) {
                    thread_count++;
                } else {
                    free(task);
                }
            }
        }
    }

    // 等待本页所有下载完成 (最长等待 15秒，避免单个卡死拖累整体)
    if (thread_count > 0) {
        WaitForMultipleObjects(thread_count, threads, TRUE, 15000); 
        for (int i = 0; i < thread_count; i++) CloseHandle(threads[i]);
    }
    free(threads);
}

// --- 搜索主入口 ---
void SearchGitHubKeywords(const char* token, const char* query, int pages, HWND hNotify) {
    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), "搜索任务启动，关键字: %s, 页数: %d (并发下载)", 
             query ? query : "", pages);
    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));

    if (!query || strlen(query) == 0) return;

    // 1. 初始化全局缓冲区与锁
    InitializeCriticalSection(&g_searchLock);
    
    g_searchResultMax = 2 * 1024 * 1024; // 2MB 缓冲区
    g_searchResultBuf = (char*)malloc(g_searchResultMax);
    if (!g_searchResultBuf) {
        DeleteCriticalSection(&g_searchLock);
        return;
    }
    g_searchResultBuf[0] = 0;
    
    // 读取现有内容
    GetSubsInputText(g_searchResultBuf, (int)g_searchResultMax);
    g_searchResultLen = strlen(g_searchResultBuf);
    if (g_searchResultLen > 0 && g_searchResultBuf[g_searchResultLen-1] != '\n') {
        strcat(g_searchResultBuf, "\r\n");
        g_searchResultLen += 2;
    }

    // 2. 准备关键字
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

    // 3. 循环搜索
    for (int k = 0; k < kw_count; k++) {
        for (int e = 0; e < ext_count; e++) {
            for (int page = 1; page <= pages; page++) {
                
                if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) {
                    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup("搜索已中止"));
                    goto cleanup;
                }

                char url[MAX_URL_LEN];
                // 构造搜索 API URL
                snprintf(url, sizeof(url), 
                    "https://api.github.com/search/code?q=%s+extension:%s&per_page=10&page=%d&sort=indexed&order=desc", 
                    keywords[k], extensions[e], page);
                
                snprintf(logBuf, sizeof(logBuf), "搜索 API: %s (Ext: %s, 页 %d)", keywords[k], extensions[e], page);
                PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));

                // API 请求 (单线程执行，避免触发 GitHub 403)
                const char* api_token = (token && token[0]) ? token : NULL;
                char* response = NetRequestWithProxy(url, api_token, proxy_url);
                
                if (!response) {
                    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup("请求失败 (可能触发限流，等待 2s)"));
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
                         break; // 本组合无更多结果
                    }

                    snprintf(logBuf, sizeof(logBuf), "  > 发现 %d 个文件，正在并发下载...", count);
                    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));

                    // [核心] 批量并发下载本页所有文件
                    ProcessBatchDownloads(items, proxy_url, hNotify);
                }
                
                cJSON_Delete(root);
                free(response);
                
                // 搜索完一页后，稍作休眠保护 API 额度
                Sleep(1000); 
            }
        }
    }

    // 4. 完成
    SetSubsInputText(g_searchResultBuf);
    PostMessage(hNotify, WM_APP_LOG, 0, (LPARAM)_strdup("搜索完成，结果已更新"));

cleanup:
    // 即使中止，也保存已获取的内容
    if (g_searchResultBuf) {
        SetSubsInputText(g_searchResultBuf);
        free(g_searchResultBuf);
        g_searchResultBuf = NULL;
    }
    DeleteCriticalSection(&g_searchLock);
    if (kw_copy) free(kw_copy);
}
