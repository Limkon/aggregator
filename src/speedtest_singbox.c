/* src/speedtest_singbox.c */
#include "common.h"
#include "utils_net.h" // HttpGet
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <process.h>
#include <io.h> 

// 外部声明
extern cJSON* GenerateSingboxOutbound(const char* link);

// 辅助：高精度计时
static double GetTimeMs() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// 辅助：检测本地端口是否已监听 (智能等待核心)
static BOOL WaitForLocalPort(int port, int max_wait_ms) {
    int elapsed = 0;
    while (elapsed < max_wait_ms) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock != INVALID_SOCKET) {
            struct sockaddr_in target;
            target.sin_family = AF_INET;
            target.sin_addr.s_addr = inet_addr("127.0.0.1");
            target.sin_port = htons(port);

            // 尝试连接
            if (connect(sock, (struct sockaddr*)&target, sizeof(target)) == 0) {
                closesocket(sock);
                return TRUE; // 连接成功，说明 sing-box 已经准备好了
            }
            closesocket(sock);
        }
        Sleep(50); // 每 50ms 轮询一次
        elapsed += 50;
    }
    return FALSE;
}

// 辅助：生成 Sing-box 配置
static char* CreateSingboxConfig(cJSON* outbound, int local_port) {
    if (!outbound) return NULL;

    cJSON* root = cJSON_CreateObject();
    
    // Log: 禁止输出日志以提升性能
    cJSON* log = cJSON_CreateObject();
    cJSON_AddStringToObject(log, "level", "panic"); // 只记录严重错误
    cJSON_AddItemToObject(root, "log", log);

    // [新增] DNS: 防止本地 DNS 污染导致连不上节点域名
    // 强制使用 AliDNS 解析节点地址，并走 Direct 出站
    cJSON* dns = cJSON_CreateObject();
    cJSON* servers = cJSON_CreateArray();
    cJSON* srv = cJSON_CreateObject();
    cJSON_AddStringToObject(srv, "tag", "local_dns");
    cJSON_AddStringToObject(srv, "address", "223.5.5.5");
    cJSON_AddStringToObject(srv, "detour", "direct");
    cJSON_AddItemToArray(servers, srv);
    cJSON_AddItemToObject(dns, "servers", servers);
    cJSON_AddItemToObject(root, "dns", dns);

    // Inbounds: Mixed
    cJSON* inbounds = cJSON_CreateArray();
    cJSON* in_item = cJSON_CreateObject();
    cJSON_AddStringToObject(in_item, "type", "mixed");
    cJSON_AddStringToObject(in_item, "tag", "mixed-in");
    cJSON_AddStringToObject(in_item, "listen", "127.0.0.1");
    cJSON_AddNumberToObject(in_item, "listen_port", local_port);
    cJSON_AddItemToArray(inbounds, in_item);
    cJSON_AddItemToObject(root, "inbounds", inbounds);

    // Outbounds
    cJSON* outbounds = cJSON_CreateArray();
    cJSON_AddItemToArray(outbounds, outbound); 
    
    cJSON* direct = cJSON_CreateObject();
    cJSON_AddStringToObject(direct, "type", "direct");
    cJSON_AddStringToObject(direct, "tag", "direct");
    cJSON_AddItemToArray(outbounds, direct);
    
    cJSON_AddItemToObject(root, "outbounds", outbounds);

    // Route
    cJSON* route = cJSON_CreateObject();
    cJSON* rules = cJSON_CreateArray();
    
    // 规则1: DNS 流量走 direct (针对 local_dns)
    cJSON* rule_dns = cJSON_CreateObject();
    cJSON_AddStringToObject(rule_dns, "outbound", "direct");
    cJSON_AddStringToObject(rule_dns, "port", "53");
    cJSON_AddItemToArray(rules, rule_dns);

    // 规则2: 所有入站流量走代理节点
    cJSON* rule = cJSON_CreateObject();
    cJSON* inbound_list = cJSON_CreateArray();
    cJSON_AddItemToArray(inbound_list, cJSON_CreateString("mixed-in"));
    cJSON_AddItemToObject(rule, "inbound", inbound_list);
    
    cJSON* tag_item = cJSON_GetObjectItem(outbound, "tag");
    cJSON_AddStringToObject(rule, "outbound", tag_item ? tag_item->valuestring : "proxy");
    cJSON_AddItemToArray(rules, rule);
    
    cJSON_AddItemToObject(route, "rules", rules);
    cJSON_AddItemToObject(root, "route", route);

    char* json_str = cJSON_PrintUnformatted(root); // 压缩格式，减少 I/O
    cJSON_Delete(root);
    return json_str;
}

/**
 * 执行 Sing-box 真实延迟测试
 */
double SpeedTest_Singbox(const char* node_link, int port_index, int timeout_ms, const char* test_url) {
    if (!node_link || !test_url) return -1.0;

    // 检查文件
    if (_access(SINGBOX_EXE_NAME, 0) != 0) return -1.0;

    // 1. 生成配置
    cJSON* outbound = GenerateSingboxOutbound(node_link);
    if (!outbound) return -1.0;

    int local_port = SINGBOX_BASE_PORT + port_index;
    char* config_json = CreateSingboxConfig(outbound, local_port); 
    if (!config_json) return -1.0;

    // 2. 写入文件
    char config_filename[MAX_PATH];
    snprintf(config_filename, sizeof(config_filename), "sb_%d.json", local_port);
    
    FILE* fp = fopen(config_filename, "w");
    if (!fp) { free(config_json); return -1.0; }
    fputs(config_json, fp);
    fclose(fp);
    free(config_json);

    // 3. 启动进程
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    char cmd_line[1024];
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\" run -c \"%s\"", SINGBOX_EXE_NAME, config_filename);

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        DeleteFileA(config_filename);
        return -1.0;
    }

    // 4. [核心优化] 智能等待端口就绪 (最多等 2.5 秒)
    // 只有端口通了，才开始测速，否则直接判负
    double latency = -1.0;
    if (WaitForLocalPort(local_port, 2500)) {
        // 5. 发起测速请求
        char proxy_url[64];
        snprintf(proxy_url, sizeof(proxy_url), "127.0.0.1:%d", local_port); 

        double start_time = GetTimeMs();
        char* response = HttpGet(test_url, proxy_url, timeout_ms); // 这里的 timeout_ms 是 HTTP 请求的超时

        if (response) {
            double end_time = GetTimeMs();
            latency = end_time - start_time;
            free(response);
        }
    }

    // 6. 清理
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 500); // 快速等待结束
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    DeleteFileA(config_filename);

    return latency;
}
