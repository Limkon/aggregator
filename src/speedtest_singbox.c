#include "common.h"
#include "utils_net.h" // HttpGet
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <process.h>
#include <io.h> // 用于 _access

// 外部声明：从 parser_nodes.c 获取 Sing-box 出站配置生成器
extern cJSON* GenerateSingboxOutbound(const char* link);

// 辅助：高精度计时 (复用)
static double GetTimeMs() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}

// 辅助：生成完整 Sing-box 配置字符串
static char* CreateSingboxConfig(cJSON* outbound, int local_port) {
    if (!outbound) return NULL;

    cJSON* root = cJSON_CreateObject();
    
    // Log: 减少噪音
    cJSON* log = cJSON_CreateObject();
    cJSON_AddStringToObject(log, "level", "error");
    cJSON_AddItemToObject(root, "log", log);

    // Inbounds: SOCKS 入站
    cJSON* inbounds = cJSON_CreateArray();
    cJSON* in_item = cJSON_CreateObject();
    // [修正] 改为 mixed 以支持 HTTP 代理请求
    cJSON_AddStringToObject(in_item, "type", "mixed");
    cJSON_AddStringToObject(in_item, "tag", "socks-in");
    cJSON_AddStringToObject(in_item, "listen", "127.0.0.1");
    cJSON_AddNumberToObject(in_item, "listen_port", local_port);
    cJSON_AddItemToArray(inbounds, in_item);
    cJSON_AddItemToObject(root, "inbounds", inbounds);

    // Outbounds: [Node, Direct]
    cJSON* outbounds = cJSON_CreateArray();
    cJSON_AddItemToArray(outbounds, outbound); 
    
    cJSON* direct = cJSON_CreateObject();
    cJSON_AddStringToObject(direct, "type", "direct");
    cJSON_AddStringToObject(direct, "tag", "direct");
    cJSON_AddItemToArray(outbounds, direct);
    
    cJSON_AddItemToObject(root, "outbounds", outbounds);

    // Route: 强制所有流量走节点
    cJSON* route = cJSON_CreateObject();
    cJSON* rules = cJSON_CreateArray();
    cJSON* rule = cJSON_CreateObject();
    cJSON_AddItemToArray(rules, rule);
    
    cJSON* inbound_list = cJSON_CreateArray();
    cJSON_AddItemToArray(inbound_list, cJSON_CreateString("socks-in"));
    cJSON_AddItemToObject(rule, "inbound", inbound_list);
    
    cJSON* tag_item = cJSON_GetObjectItem(outbound, "tag");
    cJSON_AddStringToObject(rule, "outbound", tag_item ? tag_item->valuestring : "proxy");
    
    cJSON_AddItemToObject(route, "rules", rules);
    cJSON_AddItemToObject(root, "route", route);

    char* json_str = cJSON_Print(root); 
    cJSON_Delete(root);
    return json_str;
}

/**
 * 执行 Sing-box 真实延迟测试
 * @param node_link     节点链接
 * @param port_index    端口偏移索引
 * @param timeout_ms    超时时间 (毫秒) [修改]
 * @param test_url      测速目标 URL
 * @return              延迟毫秒数，失败返回 -1.0
 */
double SpeedTest_Singbox(const char* node_link, int port_index, int timeout_ms, const char* test_url) {
    if (!node_link || !test_url) return -1.0;

    // 检查 sing-box.exe 是否存在
    if (_access(SINGBOX_EXE_NAME, 0) != 0) {
        return -1.0;
    }

    // 1. 生成配置
    cJSON* outbound = GenerateSingboxOutbound(node_link);
    if (!outbound) return -1.0;

    int local_port = SINGBOX_BASE_PORT + port_index;
    char* config_json = CreateSingboxConfig(outbound, local_port); 
    
    if (!config_json) return -1.0;

    // 2. 写入临时文件
    char config_filename[MAX_PATH];
    snprintf(config_filename, sizeof(config_filename), "sb_config_%d.json", local_port);
    
    FILE* fp = fopen(config_filename, "w");
    if (!fp) {
        free(config_json);
        return -1.0;
    }
    fputs(config_json, fp);
    fclose(fp);
    free(config_json);

    // 3. 启动 Sing-box 进程
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // 隐藏窗口
    ZeroMemory(&pi, sizeof(pi));

    char cmd_line[1024];
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\" run -c \"%s\"", SINGBOX_EXE_NAME, config_filename);

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        DeleteFileA(config_filename);
        return -1.0;
    }

    // 4. 等待进程启动
    Sleep(800); 

    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        DeleteFileA(config_filename);
        return -1.0;
    }

    // 5. 通过代理发起请求
    char proxy_url[64];
    snprintf(proxy_url, sizeof(proxy_url), "127.0.0.1:%d", local_port); 

    double start_time = GetTimeMs();
    
    // [修改] 传入 timeout_ms，utils_net 已经改为接收毫秒
    char* response = HttpGet(test_url, proxy_url, timeout_ms);
    double latency = -1.0;

    if (response) {
        double end_time = GetTimeMs();
        latency = end_time - start_time;
        free(response);
    }

    // 6. 清理
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 1000); 
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    DeleteFileA(config_filename);

    return latency;
}
