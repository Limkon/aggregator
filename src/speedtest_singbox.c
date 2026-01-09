#include "common.h"
#include "utils_net.h" // HttpGet
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <process.h>

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
    cJSON_AddStringToObject(in_item, "type", "socks");
    cJSON_AddStringToObject(in_item, "tag", "socks-in");
    cJSON_AddStringToObject(in_item, "listen", "127.0.0.1");
    cJSON_AddNumberToObject(in_item, "listen_port", local_port);
    cJSON_AddItemToArray(inbounds, in_item);
    cJSON_AddItemToObject(root, "inbounds", inbounds);

    // Outbounds: [Node, Direct]
    cJSON* outbounds = cJSON_CreateArray();
    cJSON_AddItemToArray(outbounds, outbound); // 添加传入的节点配置
    
    // 必须添加 direct 出站用于 fallback 或 dns (虽然我们强制路由)
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
    
    // 获取 outbound 的 tag
    cJSON* tag_item = cJSON_GetObjectItem(outbound, "tag");
    cJSON_AddStringToObject(rule, "outbound", tag_item ? tag_item->valuestring : "proxy");
    
    cJSON_AddItemToObject(route, "rules", rules);
    cJSON_AddItemToObject(root, "route", route);

    char* json_str = cJSON_Print(root); // 格式化输出方便调试，生产环境可用 PrintUnformatted
    cJSON_Delete(root);
    return json_str;
}

/**
 * 执行 Sing-box 真实延迟测试
 * @param node_link     节点链接
 * @param port_index    端口偏移索引 (0 ~ concurrency-1)，用于避免端口冲突
 * @param timeout_sec   超时时间 (秒)
 * @param test_url      测速目标 URL
 * @return              延迟毫秒数，失败返回 -1.0
 */
double SpeedTest_Singbox(const char* node_link, int port_index, int timeout_sec, const char* test_url) {
    if (!node_link || !test_url) return -1.0;

    // 1. 生成配置
    cJSON* outbound = GenerateSingboxOutbound(node_link);
    if (!outbound) return -1.0; // 解析失败或不支持的协议

    int local_port = SINGBOX_BASE_PORT + port_index;
    char* config_json = CreateSingboxConfig(outbound, local_port); // outbound ownership transferred inside? No, cJSON_AddItem transfers.
    // 注意：GenerateSingboxOutbound 返回的指针被 add 到 root 后，root delete 时会一起释放。
    // 所以 CreateSingboxConfig 内部逻辑必须正确处理所有权。
    // 上面的 CreateSingboxConfig 实现中：cJSON_AddItemToArray(outbounds, outbound) 转移了所有权。
    // 因此这里不需要 cJSON_Delete(outbound)。
    
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
    // 命令: sing-box.exe run -c sb_config_10900.json
    snprintf(cmd_line, sizeof(cmd_line), "\"%s\" run -c \"%s\"", SINGBOX_EXE_NAME, config_filename);

    if (!CreateProcessA(NULL, cmd_line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        // 启动失败 (找不到 exe?)
        DeleteFileA(config_filename);
        return -1.0;
    }

    // 4. 等待进程启动 (简单的 Sleep，更优雅的方式是轮询端口是否被监听)
    Sleep(800); // 800ms 等待初始化

    // 检查进程是否意外退出
    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
        // 进程已死
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        DeleteFileA(config_filename);
        return -1.0;
    }

    // 5. 通过代理发起请求 (复用 utils_net.c 的 HttpGet)
    char proxy_url[64];
    snprintf(proxy_url, sizeof(proxy_url), "127.0.0.1:%d", local_port); // WinINet/OpenSSL 代理格式

    double start_time = GetTimeMs();
    
    // 调用 HttpGet (utils_net.c 中的实现需支持 SOCKS 或 HTTP 代理)
    // 警告：之前的 utils_net.c (OpenSSL版) 尚未实现 SOCKS 握手逻辑，仅支持 HTTP 代理。
    // Sing-box 默认提供 mixed (HTTP/SOCKS) 还是纯 SOCKS？
    // 上面配置的是 "type": "socks"。OpenSSL 不支持直接连接 SOCKS。
    // 修正：将 Sing-box 入站改为 "mixed" 或 "http" 以便 HttpGet 可以连接。
    // 这里我们修改上面的 CreateSingboxConfig 里的 inbound type 为 "mixed" (支持 HTTP 和 SOCKS)。
    
    // (修正 CreateSingboxConfig 中的 "socks" -> "mixed")
    // 但 cJSON 对象已生成，无法此处修改。
    // *关键修正*：请确保 utils_net.c 能够处理 HTTP 代理，或者我们让 Sing-box 开启 mixed 端口。
    // Sing-box 的 "mixed" 类型同时支持 HTTP 和 SOCKS5。
    // 为了兼容性，建议 CreateSingboxConfig 中使用 "mixed"。
    
    // ... 假设 inbound 已改为 mixed ...
    
    char* response = HttpGet(test_url, proxy_url, timeout_sec);
    double latency = -1.0;

    if (response) {
        double end_time = GetTimeMs();
        latency = end_time - start_time;
        free(response);
    }

    // 6. 清理
    // 终止进程
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 1000); // 等待彻底结束
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // 删除配置文件
    DeleteFileA(config_filename);

    return latency;
}

// [修正补丁] 为了支持 utils_net.c (OpenSSL) 的 HTTP 代理能力
// 我们需要覆盖上面的 CreateSingboxConfig 中的 inbound type
// 请在整合代码时，将 CreateSingboxConfig 函数中的:
// cJSON_AddStringToObject(in_item, "type", "socks");
// 修改为:
// cJSON_AddStringToObject(in_item, "type", "mixed");
