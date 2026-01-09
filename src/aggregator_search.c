#include "common.h"
#include "cJSON.h"
#include "utils_net.h"
#include "utils_base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- 外部引用 ---
extern void GuiAppendLog(const char* message);
extern void SetSubsInputText(const char* text);
extern int GetSubsInputText(char* buffer, int maxLen);

// --- 辅助：从文本提取节点 ---
// 返回提取到的数量
static int ExtractNodesFromText(char* text, char* out_buf, size_t buf_max, size_t* current_len) {
    if (!text || !out_buf) return 0;
    
    int count = 0;
    char* ctx = NULL;
    char* line = strtok_s(text, "\n", &ctx);
    
    while (line) {
        // Trim whitespace
        while(*line && (*line==' ' || *line=='\r' || *line=='\t')) line++;
        
        // Check protocols
        if (strncmp(line, "vmess://", 8) == 0 ||
            strncmp(line, "vless://", 8) == 0 ||
            strncmp(line, "ss://", 5) == 0 ||
            strncmp(line, "trojan://", 9) == 0 ||
            strncmp(line, "hysteria2://", 12) == 0 ||
            strncmp(line, "hy2://", 6) == 0) {
            
            size_t line_len = strlen(line);
            if (*current_len + line_len + 2 < buf_max) {
                // 简单的防重复检查可以放在这里 (此处略过以保性能)
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

// --- 搜索任务线程入口 ---
DWORD WINAPI SearchThreadProc(LPVOID lpParam) {
    // 1. 初始化
    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), "搜索线程启动，关键字: %s, 页数: %d", 
             g_config.search_keywords, g_config.search_pages);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup(logBuf));

    // [修复] 不要直接修改 g_config.search_keywords，使用副本
    char* kw_copy = strdup(g_config.search_keywords);
    char* keywords[10];
    int kw_count = 0;
    char* ctx_token = NULL;
    char* token = strtok_s(kw_copy, ",", &ctx_token);
    while (token && kw_count < 10) {
        keywords[kw_count++] = token;
        token = strtok_s(NULL, ",", &ctx_token);
    }

    if (kw_count == 0) {
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup("错误: 未指定搜索关键字"));
        free(kw_copy);
        PostMessage(g_hMainWnd, WM_APP_TASK_DONE, 0, 0);
        return 0;
    }

    // 结果缓冲区 (1MB)
    size_t total_buf_size = 1024 * 1024; 
    char* result_buffer = (char*)malloc(total_buf_size);
    if (!result_buffer) { free(kw_copy); return 0; }
    result_buffer[0] = 0;
    size_t current_len = 0;
    
    // 读取现有输入框内容
    GetSubsInputText(result_buffer, total_buf_size);
    current_len = strlen(result_buffer);
    if (current_len > 0 && result_buffer[current_len-1] != '\n') {
        strcat(result_buffer, "\r\n");
        current_len += 2;
    }

    // [修复] 扩展名列表：分别搜索，避免 "AND" 逻辑导致无结果
    // GitHub 代码搜索必须指定 user, repo 或 extension 之一
    const char* extensions[] = {"yaml", "txt", "conf"}; 
    int ext_count = 3;

    // 3. 开始循环搜索
    for (int k = 0; k < kw_count; k++) {
        for (int e = 0; e < ext_count; e++) { // 遍历扩展名
            for (int page = 1; page <= g_config.search_pages; page++) {
                
                // 检查停止信号
                if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) {
                    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup("搜索已中止"));
                    goto cleanup;
                }

                char url[MAX_URL_LEN];
                // [修复] 查询语法: keyword + extension:ext
                snprintf(url, sizeof(url), 
                    "https://api.github.com/search/code?q=%s+extension:%s&per_page=10&page=%d&sort=indexed&order=desc", 
                    keywords[k], extensions[e], page);
                
                snprintf(logBuf, sizeof(logBuf), "搜索: %s (Ext: %s, 页 %d)", keywords[k], extensions[e], page);
                PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup(logBuf));

                // 发起 HTTP 请求
                char* response = NetRequest(url, g_config.github_token[0] ? g_config.github_token : NULL);
                
                if (!response) {
                    // 如果没有 Token，GitHub 很容易 403，不要立即放弃，可能只是限流
                    snprintf(logBuf, sizeof(logBuf), "请求失败 (可能触发限流，等待中...)");
                    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup(logBuf));
                    Sleep(2000); 
                    continue;
                }

                // 解析 JSON
                cJSON* root = cJSON_Parse(response);
                if (!root) { free(response); continue; }

                cJSON* items = cJSON_GetObjectItem(root, "items");
                if (cJSON_IsArray(items)) {
                    int count = cJSON_GetArraySize(items);
                    if (count == 0) {
                         // 该组合无结果，跳过后续页码
                         cJSON_Delete(root);
                         free(response);
                         break; 
                    }

                    snprintf(logBuf, sizeof(logBuf), "  > 发现 %d 个文件", count);
                    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup(logBuf));

                    for (int i = 0; i < count; i++) {
                        // 再次检查停止
                        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) goto cleanup;

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

                                Sleep(300); // 避免请求 raw 内容过快
                                char* file_content = NetRequest(raw_url, NULL);
                                
                                if (file_content) {
                                    // 1. 尝试直接提取
                                    char* content_copy = strdup(file_content); // ExtractNodes 会破坏字符串
                                    int found = ExtractNodesFromText(content_copy, result_buffer, total_buf_size, &current_len);
                                    free(content_copy);

                                    // 2. [关键修复] 如果直接提取失败，尝试 Base64 解码后再提取
                                    if (found == 0) {
                                        char* decoded = Base64Decode(file_content);
                                        if (decoded) {
                                            found = ExtractNodesFromText(decoded, result_buffer, total_buf_size, &current_len);
                                            free(decoded);
                                            if (found > 0) {
                                                 snprintf(logBuf, sizeof(logBuf), "    + 经Base64解码提取到 %d 个节点", found);
                                                 PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup(logBuf));
                                            }
                                        }
                                    } else {
                                        snprintf(logBuf, sizeof(logBuf), "    + 直接提取到 %d 个节点", found);
                                        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup(logBuf));
                                    }

                                    free(file_content);
                                }
                            }
                        }
                    }
                }
                
                cJSON_Delete(root);
                free(response);
                Sleep(1500); // 每一页搜索后休息一下
            }
        }
    }

    // 4. 更新 UI
    SetSubsInputText(result_buffer);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)strdup("搜索任务完成，结果已填入订阅框"));

cleanup:
    if (kw_copy) free(kw_copy);
    if (result_buffer) free(result_buffer);
    PostMessage(g_hMainWnd, WM_APP_TASK_DONE, 0, 0);
    return 0;
}

void StartSearchTask() {
    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, SearchThreadProc, NULL, 0, NULL);
}
