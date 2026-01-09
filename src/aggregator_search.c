#include "common.h"
#include "utils_net.h"
#include "cJSON.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// --- 补充函数：strnstr 实现 (Windows/MinGW 缺省不支持) ---
static char *strnstr(const char *haystack, const char *needle, size_t len) {
    size_t i;
    size_t needle_len;

    if (0 == (needle_len = strlen(needle)))
        return (char *)haystack;

    for (i = 0; i < len; i++) {
        if (i + needle_len > len) {
            return NULL;
        }
        if ((haystack[0] == needle[0]) &&
            (strncmp(haystack, needle, needle_len) == 0)) {
            return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

// --- 辅助函数：判断字符是否为 URL 有效字符 ---
static int IsUrlChar(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || 
           c == ':' || c == '/' || c == '?' || c == '=' || c == '&' || 
           c == '%' || c == '#' || c == '@' || c == '+';
}

// --- 辅助函数：从文本中提取链接并去重 ---
// 模拟 Python 的正则提取逻辑
static void ExtractLinksFromContent(const char* content, HWND hNotifyWnd) {
    if (!content) return;

    const char* p = content;
    const char* protocols[] = {
        "http://", "https://", 
        "vmess://", "vless://", "trojan://", "ss://", "ssr://", "hysteria://", "hysteria2://", "hy2://"
    };
    int proto_count = sizeof(protocols) / sizeof(protocols[0]);

    while (*p) {
        // 1. 快速扫描协议头
        int match_idx = -1;
        for (int i = 0; i < proto_count; i++) {
            if (strncasecmp(p, protocols[i], strlen(protocols[i])) == 0) {
                match_idx = i;
                break;
            }
        }

        if (match_idx >= 0) {
            // 2. 找到协议头，提取完整链接
            const char* start = p;
            while (*p && IsUrlChar(*p)) {
                p++;
            }
            int len = (int)(p - start);

            // 3. 验证链接有效性 (简单的启发式过滤)
            if (len > 10) { // 至少要有一定长度
                // 对于 http/https，需要进一步检查是否像订阅链接
                int is_sub_link = 0;
                if (match_idx <= 1) { // http or https
                    // 检查关键字 (参考 Python 正则)
                    if (strnstr(start, "/api/v1/client/subscribe", len) ||
                        strnstr(start, "/link/", len) ||
                        strnstr(start, "/sub", len) ||
                        strnstr(start, "token=", len) ||
                        strnstr(start, "clash=", len)) {
                        is_sub_link = 1;
                    }
                } else {
                    // 节点协议直接通过
                    is_sub_link = 1; 
                }

                if (is_sub_link) {
                    char* link = (char*)malloc(len + 1);
                    if (link) {
                        strncpy(link, start, len);
                        link[len] = '\0';
                        
                        EnterCriticalSection(&g_dataLock);
                        // 简单的线性查重 (性能一般，但数量级不大时可接受)
                        int exists = 0;
                        for (int i = 0; i < g_node_count; i++) {
                            if (strcmp(g_nodes[i].original_link, link) == 0) {
                                exists = 1;
                                break;
                            }
                        }
                        
                        if (!exists && g_node_count < MAX_NODES) {
                            // 存入全局列表 (暂作为原始链接存储)
                            ProxyNode* node = &g_nodes[g_node_count++];
                            memset(node, 0, sizeof(ProxyNode));
                            strncpy(node->original_link, link, MAX_URL_LEN - 1);
                            
                            // 通知 UI 更新计数
                            char msg[256];
                            snprintf(msg, sizeof(msg), "发现: %s...", link);
                            // Log 会导致刷屏，改为仅通知进度或每N个通知一次
                            // GuiAppendLog(msg); 
                        } else {
                            // 已存在或满
                            free(link);
                            link = NULL;
                        }
                        LeaveCriticalSection(&g_dataLock);
                        
                        // 如果是新链接，才 free (因为 copy 到了全局数组)
                        if (link) free(link);
                    }
                }
            }
        } else {
            p++;
        }
    }
}

// --- 核心搜索任务 ---
void SearchGitHubKeywords(const char* token, const char* query_str, int pages, HWND hNotifyWnd) {
    char* queries[10]; // 最多支持 10 个关键词
    int query_count = 0;

    // 1. 分割关键词
    char q_buf[512];
    strncpy(q_buf, query_str, sizeof(q_buf));
    char* ctx = NULL;
    char* t = strtok_s(q_buf, ",", &ctx);
    while (t && query_count < 10) {
        queries[query_count++] = t;
        t = strtok_s(NULL, ",", &ctx);
    }

    if (query_count == 0) return;

    // 2. 遍历关键词
    for (int q = 0; q < query_count; q++) {
        // 检查停止标志
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;

        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg), ">>> 正在搜索关键字: %s (共 %d 页)", queries[q], pages);
        PostMessage(hNotifyWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logMsg));

        // 3. 遍历页码
        for (int page = 1; page <= pages; page++) {
            if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;

            // 构造 API URL
            char api_url[1024];
            snprintf(api_url, sizeof(api_url), 
                "https://api.github.com/search/code?q=\"%s\"+in:file&sort=indexed&order=desc&per_page=30&page=%d", // 减小每页数量以防超时
                queries[q], page);

            // Log
            snprintf(logMsg, sizeof(logMsg), "  请求第 %d 页...", page);
            PostMessage(hNotifyWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logMsg));

            char* json_resp = HttpGet(api_url, g_config.enable_proxy ? g_config.proxy_url : NULL, 10);
            
            if (!json_resp) {
                PostMessage(hNotifyWnd, WM_APP_LOG, 0, (LPARAM)_strdup("  请求失败或超时。"));
                continue;
            }

            // 4. 解析 JSON
            cJSON* root = cJSON_Parse(json_resp);
            if (!root) {
                free(json_resp);
                PostMessage(hNotifyWnd, WM_APP_LOG, 0, (LPARAM)_strdup("  JSON 解析失败。"));
                continue;
            }

            cJSON* items = cJSON_GetObjectItem(root, "items");
            if (cJSON_IsArray(items)) {
                int count = cJSON_GetArraySize(items);
                snprintf(logMsg, sizeof(logMsg), "  第 %d 页找到 %d 个文件，开始提取...", page, count);
                PostMessage(hNotifyWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logMsg));

                for (int i = 0; i < count; i++) {
                    if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;

                    cJSON* item = cJSON_GetArrayItem(items, i);
                    cJSON* html_url = cJSON_GetObjectItem(item, "html_url");
                    
                    if (html_url && html_url->valuestring) {
                        char raw_url[2048];
                        char* src = html_url->valuestring;
                        char* p_blob = strstr(src, "/blob/");
                        
                        if (p_blob) {
                            int prefix_len = (int)(p_blob - src);
                            // replace host
                            char* p_host = strstr(src, "github.com");
                            if (p_host && p_host < p_blob) {
                                int host_offset = (int)(p_host - src);
                                snprintf(raw_url, sizeof(raw_url), "%.*sraw.githubusercontent.com%s", 
                                    host_offset, src, // https://
                                    p_host + 10); // /user/repo/blob/...
                                
                                // 现在 raw_url 里还有 /blob/，需要去掉
                                char* p_blob_in_raw = strstr(raw_url, "/blob/");
                                if (p_blob_in_raw) {
                                    memmove(p_blob_in_raw + 1, p_blob_in_raw + 6, strlen(p_blob_in_raw + 6) + 1);
                                }

                                // 下载文件内容
                                char* file_content = HttpGet(raw_url, g_config.enable_proxy ? g_config.proxy_url : NULL, 10);
                                if (file_content) {
                                    ExtractLinksFromContent(file_content, hNotifyWnd);
                                    free(file_content);
                                }
                                
                                // 速率限制保护
                                Sleep(500); 
                            }
                        }
                    }
                }
            }
            
            cJSON_Delete(root);
            free(json_resp);
            
            // 翻页间隔保护
            Sleep(2000);
        }
    }
}
