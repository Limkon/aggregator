#include "parser.h"
#include "utils_base64.h"
#include <stdio.h>
#include <string.h>

// 对应 Python: _parse_node_link_for_tcp
bool ParseNodeForTcp(const char* link, char* out_addr, int* out_port) {
    if (strncmp(link, "vmess://", 8) == 0) {
        // 解码 Base64 -> 解析 JSON -> 获取 add 和 port
        char* b64 = (char*)link + 8;
        char* json_str = Base64Decode(b64); // 需实现 Base64Decode
        if (!json_str) return false;

        cJSON* json = cJSON_Parse(json_str);
        if (json) {
            cJSON* add = cJSON_GetObjectItem(json, "add");
            cJSON* port = cJSON_GetObjectItem(json, "port");
            if (add && port) {
                strcpy(out_addr, add->valuestring);
                // port 可能是数字或字符串，需处理
                *out_port = cJSON_IsNumber(port) ? port->valueint : atoi(port->valuestring);
                cJSON_Delete(json);
                free(json_str);
                return true;
            }
            cJSON_Delete(json);
        }
        free(json_str);
    } 
    else if (strncmp(link, "ss://", 5) == 0) {
        // C 语言手动解析 ss://user:pass@host:port
        // 使用 strchr, strrchr 定位 @ 和 :
        // ... (此处需大量字符串指针操作代码)
    }
    // ... 其他协议实现
    return false;
}
