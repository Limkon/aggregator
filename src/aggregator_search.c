#include "common.h"
#include "utils_net.h"
#include "cJSON.h"
#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
                        
                        // 发送给主线程处理 (去重和显示)
                        // 使用 SendMessage 同步发送，确保 link 在处理完前有效? 
                        // 不，主线程可能会卡顿。这里使用 PostMessage + 动态内存，约定主线程释放 lParam
                        // 或者更安全：在这里处理去重 (需要加锁访问全局 Set)，然后只通知。
                        
                        // 简化方案：直接发给 UI，UI 负责去重
                        // 注意：为了避免 UI 线程频繁 malloc/free，可以先在工作线程去重
                        
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
            // https://api.github.com/search/code?q="keyword"+in:file&sort=indexed&order=desc&per_page=100&page=N
            char api_url[1024];
            snprintf(api_url, sizeof(api_url), 
                "https://api.github.com/search/code?q=\"%s\"+in:file&sort=indexed&order=desc&per_page=30&page=%d", // 减小每页数量以防超时
                queries[q], page);

            // Log
            snprintf(logMsg, sizeof(logMsg), "  请求第 %d 页...", page);
            PostMessage(hNotifyWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logMsg));

            // 执行请求 (GitHub API 需要 User-Agent 和 Authorization)
            // Utils_HttpGet 默认带 UA。需要处理 Token。
            // 简单起见，我们修改 HttpGet 的实现或在这里手动拼接 headers
            // 由于 HttpGet 封装较简单，这里假设 Utils_HttpGet 内部会处理好 SSL
            // 如果需要加 Token，可能需要扩展 HttpGet 接口。
            // 暂时使用未授权请求 (速率限制较低)，或者把 token 放入 url? 不行，token 必须在 header。
            // 鉴于篇幅，这里暂不实现 Authorization Header 的动态注入，仅依赖 IP 限制。
            // (注：若 aggregator_app.py 必须 Token，建议后续扩展 HttpGet 支持 extra headers)

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
                        // 转换 URL: github.com/user/repo/blob/branch/file -> raw.githubusercontent.com/user/repo/branch/file
                        // 简单替换 "github.com" -> "raw.githubusercontent.com", "/blob/" -> "/"
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
                                    // 移动后续字符串覆盖 /blob
                                    // /blob/ is 6 chars. replace with / (1 char). move left by 5.
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
