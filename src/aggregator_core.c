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

// --- 外部函数声明 (来自其他模块) ---
extern void SearchGitHubKeywords(const char* token, const char* query, int pages, HWND hNotify);
extern void ParseNodeBasic(const char* link, ProxyNode* node);
// 测速函数 (在 speedtest_*.c 中实现)
extern double SpeedTest_TcpPing(const char* addr, int port, int timeout);
extern double SpeedTest_Singbox(const char* node_link, int port_index, int timeout, const char* test_url);

// --- 全局变量引用 ---
extern HWND g_hMainWnd; // from main.c
static HANDLE hWorkerThread = NULL;

// --- 辅助：重置全局节点列表 ---
static void ClearGlobalNodes() {
    EnterCriticalSection(&g_dataLock);
    g_node_count = 0;
    // 不释放 g_nodes 数组本身的内存，只是重置计数
    LeaveCriticalSection(&g_dataLock);
}

// --- 辅助：添加节点并去重 ---
// 返回 1 表示添加成功，0 表示重复或满
static int AddNodeSafe(const char* link) {
    if (!link || strlen(link) < 10) return 0;
    
    int ret = 0;
    EnterCriticalSection(&g_dataLock);
    
    if (g_node_count >= MAX_NODES) {
        LeaveCriticalSection(&g_dataLock);
        return 0;
    }

    // 简单线性去重 (性能瓶颈点，但对于 <10000 节点尚可)
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
        
        // 预解析基础信息 (用于 TCP Ping 和显示)
        ParseNodeBasic(link, node);
        
        g_node_count++;
        ret = 1;
    }
    
    LeaveCriticalSection(&g_dataLock);
    return ret;
}

// --- 任务 1: 搜索线程 ---
unsigned int __stdcall SearchThreadProc(void* arg) {
    // 搜索结果直接由 aggregator_search.c 写入 g_nodes
    // 这里只需调用并处理完成信号
    ClearGlobalNodes();
    
    SearchGitHubKeywords(g_config.github_token, g_config.search_keywords, g_config.search_pages, g_hMainWnd);
    
    PostMessage(g_hMainWnd, WM_APP_TASK_DONE, 0, 0);
    return 0;
}

// --- 任务 2: 聚合处理线程 ---

// 定义并发任务的回调函数类型
typedef void (*TaskCallback)(void* data, int thread_idx);

// 简易并发控制器 (替代 ThreadPoolExecutor)
// items: 数据数组指针
// count: 数据数量
// item_size: 每个数据的大小
// callback: 处理函数
// concurrency: 并发数
static void RunConcurrentTasks(void* items, int count, int item_size, TaskCallback callback, int concurrency) {
    if (count <= 0) return;
    
    HANDLE* threads = (HANDLE*)malloc(concurrency * sizeof(HANDLE));
    HANDLE hSemaphore = CreateSemaphore(NULL, concurrency, concurrency, NULL);
    volatile long active_count = 0;
    
    // 简单的线程参数包装
    typedef struct {
        void* item_ptr;
        int thread_idx; // 用于 Sing-box 端口分配
        TaskCallback cb;
        HANDLE hSem;
        volatile long* pActive;
    } ThreadArg;

    // 内部线程函数
    unsigned int __stdcall Worker(void* p) {
        ThreadArg* arg = (ThreadArg*)p;
        
        // 执行任务
        if (WaitForSingleObject(g_hStopEvent, 0) != WAIT_OBJECT_0) {
            arg->cb(arg->item_ptr, arg->thread_idx);
        }
        
        // 清理
        InterlockedDecrement(arg->pActive);
        ReleaseSemaphore(arg->hSem, 1, NULL);
        free(arg);
        return 0;
    }

    // 调度循环
    for (int i = 0; i < count; i++) {
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;
        
        // 等待信号量 (控制并发)
        WaitForSingleObject(hSemaphore, INFINITE);
        
        InterlockedIncrement(&active_count);
        
        ThreadArg* arg = (ThreadArg*)malloc(sizeof(ThreadArg));
        arg->item_ptr = (char*)items + (i * item_size);
        arg->thread_idx = i % concurrency; // 简单的索引分配
        arg->cb = callback;
        arg->hSem = hSemaphore;
        arg->pActive = &active_count;
        
        // 启动线程 (不保存句柄，detach运行，通过 active_count 等待)
        HANDLE hT = (HANDLE)_beginthreadex(NULL, 0, Worker, arg, 0, NULL);
        if (hT) CloseHandle(hT); 
        else {
             // 失败回退
             InterlockedDecrement(&active_count);
             ReleaseSemaphore(hSemaphore, 1, NULL);
             free(arg);
        }
        
        // 简易进度通知
        if (i % 5 == 0) {
             PostMessage(g_hMainWnd, WM_APP_PROGRESS, i, count);
        }
    }
    
    // 等待所有活跃线程结束
    while (active_count > 0) {
        Sleep(50);
    }
    
    CloseHandle(hSemaphore);
    free(threads);
}

// ----------------------------------------------------------------
// 子任务 A: 下载并解析订阅 [Refactored]
// ----------------------------------------------------------------

typedef struct {
    char url[MAX_URL_LEN];
} SubLink;

void DownloadSubWorker(void* data, int idx) {
    SubLink* link = (SubLink*)data;
    char logBuf[512];
    
    // [Log] 增加线程索引标识
    snprintf(logBuf, sizeof(logBuf), "[T-%d] 下载: %s", idx, link->url);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    
    const char* proxy = g_config.enable_proxy ? g_config.proxy_url : NULL;
    
    // [Net] 使用更新后的 HttpGet，设置 20s 超时以适应大文件
    char* content = HttpGet(link->url, proxy, 20);
    
    if (content) {
        int added = 0;
        int original_len = (int)strlen(content);

        // 1. 尝试 Base64 解码
        char* decoded = Base64Decode(content);
        char* target = decoded ? decoded : content; // 如果解码失败，尝试原始内容
        
        // 2. 按行扫描
        char* ctx = NULL;
        char* line = strtok_s(target, "\n\r", &ctx);
        while (line) {
            // 简单清洗
            while (*line && (*line == ' ' || *line == '\t')) line++;
            
            // 识别协议
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

// ----------------------------------------------------------------
// 子任务 B: 测速
// ----------------------------------------------------------------

void SpeedTestWorker(void* data, int idx) {
    ProxyNode* node = (ProxyNode*)data;
    
    // 如果之前解析失败，跳过
    if (node->type == NODE_UNKNOWN && g_config.test_mode == TEST_MODE_TCP) {
        node->latency = -1;
        return;
    }
    
    double lat = -1;
    if (g_config.test_mode == TEST_MODE_TCP) {
        lat = SpeedTest_TcpPing(node->address, node->port, g_config.timeout);
    } else {
        // Sing-box 模式
        // 端口分配: Base + (idx % concurrency)
        // 注意：RunConcurrentTasks 传入的 idx 就是 0..concurrency-1
        lat = SpeedTest_Singbox(node->original_link, idx, g_config.timeout, g_config.test_url);
    }
    
    node->latency = lat;
    node->is_alive = (lat >= 0);
    
    if (lat >= 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[%dms] %s", (int)lat, node->remark);
        // PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(msg)); // 减少刷屏
    }
}


// ----------------------------------------------------------------
// 主聚合流程
// ----------------------------------------------------------------
unsigned int __stdcall ProcessThreadProc(void* arg) {
    ClearGlobalNodes();
    
    // 1. 获取 GUI 输入 (需要在主线程获取，或者通过 buffer 传入)
    // 这里的 arg 假设是主线程传入的输入缓冲区
    char* input_text = (char*)arg; 
    
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("=== 开始聚合处理 ==="));

    // 2. 解析输入：分离订阅链接和直接节点
    SubLink* sub_links = (SubLink*)malloc(100 * sizeof(SubLink)); // 限制 100 个订阅
    int sub_count = 0;
    
    char* ctx = NULL;
    char* line = strtok_s(input_text, "\n\r", &ctx);
    while (line) {
        // Trim
        while (*line && isspace((unsigned char)*line)) line++;
        
        if (strlen(line) > 0) {
            if (strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0) {
                if (sub_count < 100) {
                    strncpy(sub_links[sub_count].url, line, MAX_URL_LEN-1);
                    sub_count++;
                }
            } else {
                // 假设是节点
                AddNodeSafe(line);
            }
        }
        line = strtok_s(NULL, "\n\r", &ctx);
    }
    
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "直接节点: %d, 订阅链接: %d", g_node_count, sub_count);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    
    // 3. 并发下载订阅
    if (sub_count > 0) {
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("--- 开始下载订阅 ---"));
        RunConcurrentTasks(sub_links, sub_count, sizeof(SubLink), DownloadSubWorker, g_config.concurrency);
    }
    free(sub_links); // SubLink 结构体不再需要，节点已入 g_nodes
    
    // 检查停止
    if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) goto cleanup;

    snprintf(logBuf, sizeof(logBuf), "总节点数: %d", g_node_count);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    
    if (g_node_count == 0) {
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup("错误: 未找到任何节点。"));
        goto cleanup;
    }

    // 4. 并发测速
    if (g_config.enable_speedtest) {
        char modeStr[64];
        snprintf(modeStr, sizeof(modeStr), "--- 开始测速 (%s, 并发 %d) ---", 
            g_config.test_mode == TEST_MODE_TCP ? "TCP Ping" : "Sing-box", 
            g_config.concurrency);
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(modeStr));
        
        // 由于 g_nodes 在全局，直接对 g_nodes 进行操作
        // 注意：测速期间不要增删节点
        RunConcurrentTasks(g_nodes, g_node_count, sizeof(ProxyNode), SpeedTestWorker, g_config.concurrency);
        
        // 5. 排序 (按延迟升序，-1 放最后)
        // 简单的冒泡排序 (C qsort 也可以，但这里为了方便处理 -1)
        // 使用 qsort
        int NodeCompare(const void* a, const void* b) {
            ProxyNode* na = (ProxyNode*)a;
            ProxyNode* nb = (ProxyNode*)b;
            if (na->latency < 0 && nb->latency < 0) return 0;
            if (na->latency < 0) return 1; // a 无效，放后面
            if (nb->latency < 0) return -1;
            return (na->latency > nb->latency) ? 1 : -1;
        }
        qsort(g_nodes, g_node_count, sizeof(ProxyNode), NodeCompare);
        
        // 统计存活
        int alive = 0;
        for (int i=0; i<g_node_count; i++) if(g_nodes[i].latency >= 0) alive++;
        snprintf(logBuf, sizeof(logBuf), "测速完成。存活节点: %d", alive);
        PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(logBuf));
    }

    // 6. 生成结果 (Base64)
    // 拼接所有链接
    size_t bufSize = g_node_count * (MAX_URL_LEN + 2) + 1024;
    char* fullText = (char*)malloc(bufSize);
    if (fullText) {
        fullText[0] = 0;
        for (int i = 0; i < g_node_count; i++) {
            // 如果测速开启，仅保留存活节点？
            // Python 版逻辑：如果测速开启且有存活节点，只保留存活。如果没有存活，保留全部。
            // 这里简单策略：如果测速开启，只输出 latency >= 0 的
            if (g_config.enable_speedtest) {
                if (g_nodes[i].latency < 0) continue; 
            }
            strcat(fullText, g_nodes[i].original_link);
            strcat(fullText, "\n");
        }
        
        // 如果全挂了，或者没测速，输出全部
        if (strlen(fullText) == 0 && g_node_count > 0) {
             for (int i = 0; i < g_node_count; i++) {
                strcat(fullText, g_nodes[i].original_link);
                strcat(fullText, "\n");
            }
        }
        
        // Base64 编码
        char* b64Result = Base64Encode((unsigned char*)fullText, strlen(fullText));
        if (b64Result) {
            PostMessage(g_hMainWnd, WM_APP_PREVIEW, 0, (LPARAM)b64Result); // UI will free
        }
        free(fullText);
    }

cleanup:
    free(input_text); // 释放传入的 buffer
    PostMessage(g_hMainWnd, WM_APP_TASK_DONE, 1, 0); // 1=Process
    return 0;
}

// --- 启动接口 ---
void StartSearchTask() {
    if (hWorkerThread) CloseHandle(hWorkerThread);
    ResetEvent(g_hStopEvent);
    hWorkerThread = (HANDLE)_beginthreadex(NULL, 0, SearchThreadProc, NULL, 0, NULL);
}

void StartProcessTask() {
    if (hWorkerThread) CloseHandle(hWorkerThread);
    ResetEvent(g_hStopEvent);
    
    // 获取输入框内容
    // 由于不能在子线程调用 GetWindowText (跨线程 UI 操作有风险，虽然 GetWindowText 多数情况可以)
    // 最好在这里获取并传入
    // 借助 gui_main.c 提供的 GetSubsInputText
    // 假设最大 1MB 文本
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
    // 简单的保存文件对话框逻辑
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
