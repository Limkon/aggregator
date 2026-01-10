/* src/parser_nodes.c */
#include "common.h"
#include "cJSON.h"
#include "utils_base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// --- 内部辅助结构：拆解后的 URL ---
typedef struct {
    char scheme[32];
    char username[128];
    char password[128];
    char host[256];
    int port;
    char path[512];
    char query[1024];
    char fragment[256];
} ParsedUrl;

// --- 辅助：修正 URL-Safe Base64 字符串 ---
// 将 '-'->'+', '_'->'/'，并补齐 '='
static char* FixUrlSafeBase64(const char* input) {
    if (!input) return NULL;
    size_t len = strlen(input);
    size_t new_len = len + 4; // 预留 padding 空间
    char* output = (char*)malloc(new_len);
    if (!output) return NULL;

    strcpy(output, input);
    for (size_t i = 0; i < len; i++) {
        if (output[i] == '-') output[i] = '+';
        else if (output[i] == '_') output[i] = '/';
    }

    // Padding
    while (strlen(output) % 4 != 0) {
        strcat(output, "=");
    }
    return output;
}

// --- 辅助函数：URL 解码 (%xx) ---
static void UrlDecode(char* dst, const char* src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = 0;
}

// --- 辅助函数：解析 Query 参数 ---
// 在 query 字符串中查找 key=value
static void GetQueryParam(const char* query, const char* key, char* out_val, int max_len) {
    if (!query || !key || !out_val) return;
    out_val[0] = 0;
    
    char key_eq[128];
    snprintf(key_eq, sizeof(key_eq), "%s=", key);
    
    const char* p = strstr(query, key_eq);
    // 必须匹配行首或&之后
    while (p) {
        if (p == query || *(p - 1) == '&' || *(p - 1) == '?') {
            p += strlen(key_eq);
            const char* end = strchr(p, '&');
            int len = end ? (int)(end - p) : (int)strlen(p);
            if (len >= max_len) len = max_len - 1;
            strncpy(out_val, p, len);
            out_val[len] = 0;
            
            // 需要 URL Decode 参数值
            char temp[1024];
            strncpy(temp, out_val, sizeof(temp));
            UrlDecode(out_val, temp);
            return;
        }
        p = strstr(p + 1, key_eq);
    }
}

// --- 辅助函数：通用 URL 解析器 ---
static int ParseGenericUrl(const char* link, ParsedUrl* out) {
    memset(out, 0, sizeof(ParsedUrl));
    char buffer[2048];
    strncpy(buffer, link, sizeof(buffer) - 1);
    
    // Scheme
    char* p = strstr(buffer, "://");
    if (!p) return 0;
    *p = 0;
    strncpy(out->scheme, buffer, sizeof(out->scheme) - 1);
    char* rest = p + 3;
    
    // Fragment (#)
    char* frag = strchr(rest, '#');
    if (frag) {
        *frag = 0;
        strncpy(out->fragment, frag + 1, sizeof(out->fragment) - 1);
        // Decode fragment (tag)
        char temp[256]; strcpy(temp, out->fragment);
        UrlDecode(out->fragment, temp);
    }
    
    // Query (?)
    char* q = strchr(rest, '?');
    if (q) {
        *q = 0;
        strncpy(out->query, q + 1, sizeof(out->query) - 1);
    }
    
    // Path (/)
    char* path = strchr(rest, '/');
    if (path) {
        *path = 0; // 暂时截断 Host 部分
        strncpy(out->path, path + 1, sizeof(out->path) - 1); // skip /
    }
    
    // UserInfo (@)
    char* at = strchr(rest, '@');
    if (at) {
        *at = 0;
        char* user_pass = rest;
        rest = at + 1; // Host starts after @
        
        char* colon = strchr(user_pass, ':');
        if (colon) {
            *colon = 0;
            strncpy(out->username, user_pass, sizeof(out->username) - 1);
            strncpy(out->password, colon + 1, sizeof(out->password) - 1);
        } else {
            strncpy(out->username, user_pass, sizeof(out->username) - 1);
        }
    }
    
    // Host:Port
    // Handle IPv6 [::1]:80
    if (rest[0] == '[') {
        char* close_bracket = strchr(rest, ']');
        if (close_bracket) {
            *close_bracket = 0;
            strncpy(out->host, rest + 1, sizeof(out->host) - 1);
            if (*(close_bracket + 1) == ':') {
                out->port = atoi(close_bracket + 2);
            }
        }
    } else {
        char* colon = strchr(rest, ':');
        if (colon) {
            *colon = 0;
            strncpy(out->host, rest, sizeof(out->host) - 1);
            out->port = atoi(colon + 1);
        } else {
            strncpy(out->host, rest, sizeof(out->host) - 1);
            // Default ports
            if (strcasecmp(out->scheme, "http") == 0) out->port = 80;
            else if (strcasecmp(out->scheme, "https") == 0) out->port = 443;
        }
    }
    return 1;
}

// ======================================================================================
// 1. 基础解析：提取地址和端口 (用于 TCP Ping)
// ======================================================================================

void ParseNodeBasic(const char* link, ProxyNode* node) {
    if (!link || !node) return;
    
    // 默认值
    node->type = NODE_UNKNOWN;
    node->address[0] = 0;
    node->port = 0;
    node->remark[0] = 0;
    
    // --- VMess ---
    if (strncasecmp(link, "vmess://", 8) == 0) {
        node->type = NODE_VMESS;
        char* b64_raw = (char*)link + 8;
        // [修复] Base64 预处理
        char* b64_fixed = FixUrlSafeBase64(b64_raw);
        char* json_str = Base64Decode(b64_fixed ? b64_fixed : b64_raw);
        if (b64_fixed) free(b64_fixed);

        if (json_str) {
            cJSON* root = cJSON_Parse(json_str);
            if (root) {
                cJSON* add = cJSON_GetObjectItem(root, "add");
                cJSON* port = cJSON_GetObjectItem(root, "port");
                cJSON* ps = cJSON_GetObjectItem(root, "ps");
                
                if (add && add->valuestring) strncpy(node->address, add->valuestring, MAX_HOST_LEN - 1);
                
                if (port) {
                    if (cJSON_IsNumber(port)) node->port = port->valueint;
                    else if (cJSON_IsString(port)) node->port = atoi(port->valuestring);
                }
                
                if (ps && ps->valuestring) strncpy(node->remark, ps->valuestring, MAX_REMARK_LEN - 1);
                
                cJSON_Delete(root);
            }
            free(json_str);
        }
    }
    // --- VLESS / Trojan / Hysteria2 ---
    else if (strncasecmp(link, "vless://", 8) == 0 ||
             strncasecmp(link, "trojan://", 9) == 0 ||
             strncasecmp(link, "hysteria2://", 12) == 0 ||
             strncasecmp(link, "hy2://", 6) == 0) {
        
        if (strncasecmp(link, "vless", 5) == 0) node->type = NODE_VLESS;
        else if (strncasecmp(link, "trojan", 6) == 0) node->type = NODE_TROJAN;
        else node->type = NODE_HYSTERIA2;

        ParsedUrl u;
        if (ParseGenericUrl(link, &u)) {
            strncpy(node->address, u.host, MAX_HOST_LEN - 1);
            node->port = u.port;
            if (u.fragment[0]) strncpy(node->remark, u.fragment, MAX_REMARK_LEN - 1);
        }
    }
    // --- Shadowsocks (SS) ---
    else if (strncasecmp(link, "ss://", 5) == 0) {
        node->type = NODE_SS;
        ParsedUrl u;
        // SS 可能是 SIP002 (ss://user:pass@host:port) 或 Legacy (ss://base64)
        if (strchr(link, '@')) {
             if (ParseGenericUrl(link, &u)) {
                strncpy(node->address, u.host, MAX_HOST_LEN - 1);
                node->port = u.port;
                if (u.fragment[0]) strncpy(node->remark, u.fragment, MAX_REMARK_LEN - 1);
            }
        } else {
            // Base64 format: ss://BASE64(#tag)
            const char* b64_part = link + 5;
            char* tag_ptr = strchr(b64_part, '#');
            if (tag_ptr) {
                strncpy(node->remark, tag_ptr + 1, MAX_REMARK_LEN - 1);
                UrlDecode(node->remark, node->remark);
                // 临时截断以解码
                *tag_ptr = 0; 
            }
            
            // [修复] 处理 SS Base64
            char* b64_fixed = FixUrlSafeBase64(b64_part);
            char* decoded = Base64Decode(b64_fixed ? b64_fixed : b64_part);
            if (b64_fixed) free(b64_fixed);
            
            if (tag_ptr) *tag_ptr = '#'; // 恢复
            
            if (decoded) {
                // Decoded: method:pass@host:port
                char dummy_link[2048];
                snprintf(dummy_link, sizeof(dummy_link), "ss://%s", decoded);
                if (ParseGenericUrl(dummy_link, &u)) {
                    strncpy(node->address, u.host, MAX_HOST_LEN - 1);
                    node->port = u.port;
                }
                free(decoded);
            }
        }
    }
}

// ======================================================================================
// 2. 高级解析：生成 Sing-box Outbound JSON
// ======================================================================================

cJSON* GenerateSingboxOutbound(const char* link) {
    if (!link) return NULL;
    
    // 初始化
    cJSON* outbound = cJSON_CreateObject();
    
    // ---------------- VMESS ----------------
    if (strncasecmp(link, "vmess://", 8) == 0) {
        char* b64_raw = (char*)link + 8;
        // [修复] Base64 预处理
        char* b64_fixed = FixUrlSafeBase64(b64_raw);
        char* json_str = Base64Decode(b64_fixed ? b64_fixed : b64_raw);
        if (b64_fixed) free(b64_fixed);
        
        if (!json_str) { cJSON_Delete(outbound); return NULL; }
        
        cJSON* src = cJSON_Parse(json_str);
        if (!src) { free(json_str); cJSON_Delete(outbound); return NULL; }

        cJSON_AddStringToObject(outbound, "type", "vmess");
        
        cJSON* add = cJSON_GetObjectItem(src, "add");
        cJSON* port = cJSON_GetObjectItem(src, "port");
        cJSON* id = cJSON_GetObjectItem(src, "id");
        cJSON* aid = cJSON_GetObjectItem(src, "aid");
        cJSON* scy = cJSON_GetObjectItem(src, "scy");
        cJSON* net = cJSON_GetObjectItem(src, "net");
        cJSON* path = cJSON_GetObjectItem(src, "path");
        cJSON* host = cJSON_GetObjectItem(src, "host");
        cJSON* tls = cJSON_GetObjectItem(src, "tls");
        cJSON* sni = cJSON_GetObjectItem(src, "sni"); 
        
        if (add) cJSON_AddStringToObject(outbound, "server", add->valuestring);
        if (port) {
             if(cJSON_IsNumber(port)) cJSON_AddNumberToObject(outbound, "server_port", port->valueint);
             else cJSON_AddNumberToObject(outbound, "server_port", atoi(port->valuestring));
        }
        if (id) cJSON_AddStringToObject(outbound, "uuid", id->valuestring);
        if (aid) cJSON_AddNumberToObject(outbound, "alter_id", cJSON_IsNumber(aid) ? aid->valueint : atoi(aid->valuestring));
        else cJSON_AddNumberToObject(outbound, "alter_id", 0);
        
        cJSON_AddStringToObject(outbound, "security", (scy && scy->valuestring) ? scy->valuestring : "auto");

        // Transport
        char* net_type = (net && net->valuestring) ? net->valuestring : "tcp";
        
        if (strcasecmp(net_type, "ws") == 0) {
            cJSON* transport = cJSON_CreateObject();
            cJSON_AddStringToObject(transport, "type", "ws");
            cJSON_AddStringToObject(transport, "path", (path && path->valuestring) ? path->valuestring : "/");
            
            if (host && host->valuestring && strlen(host->valuestring) > 0) {
                cJSON* headers = cJSON_CreateObject();
                cJSON_AddStringToObject(headers, "Host", host->valuestring);
                cJSON_AddItemToObject(transport, "headers", headers);
            }
            cJSON_AddItemToObject(outbound, "transport", transport);
        }
        else if (strcasecmp(net_type, "grpc") == 0) {
            cJSON* transport = cJSON_CreateObject();
            cJSON_AddStringToObject(transport, "type", "grpc");
            cJSON_AddStringToObject(transport, "service_name", (path && path->valuestring) ? path->valuestring : "");
            cJSON_AddItemToObject(outbound, "transport", transport);
        }

        // TLS
        if ((tls && tls->valuestring && strcasecmp(tls->valuestring, "tls") == 0) || 
             (port && (port->valueint == 443))) {
            
            cJSON* tls_conf = cJSON_CreateObject();
            cJSON_AddBoolToObject(tls_conf, "enabled", cJSON_True);
            
            char* server_name = NULL;
            if (sni && sni->valuestring && strlen(sni->valuestring)>0) server_name = sni->valuestring;
            else if (host && host->valuestring && strlen(host->valuestring)>0) server_name = host->valuestring;
            else if (add && add->valuestring) server_name = add->valuestring;
            
            if (server_name) cJSON_AddStringToObject(tls_conf, "server_name", server_name);
            cJSON_AddBoolToObject(tls_conf, "insecure", cJSON_True); 
            cJSON_AddItemToObject(outbound, "tls", tls_conf);
        }

        cJSON_Delete(src);
        free(json_str);
    }
    // ---------------- VLESS / TROJAN ----------------
    else if (strncasecmp(link, "vless://", 8) == 0 || strncasecmp(link, "trojan://", 9) == 0) {
        ParsedUrl u;
        if (!ParseGenericUrl(link, &u)) { cJSON_Delete(outbound); return NULL; }
        
        int is_vless = (strncasecmp(link, "vless", 5) == 0);
        cJSON_AddStringToObject(outbound, "type", is_vless ? "vless" : "trojan");
        cJSON_AddStringToObject(outbound, "server", u.host);
        cJSON_AddNumberToObject(outbound, "server_port", u.port);
        
        if (is_vless) cJSON_AddStringToObject(outbound, "uuid", u.username);
        else cJSON_AddStringToObject(outbound, "password", u.username); 

        // Query Params
        char security[32] = {0}; GetQueryParam(u.query, "security", security, 32);
        char type[32] = {0}; GetQueryParam(u.query, "type", type, 32);
        char sni[128] = {0}; GetQueryParam(u.query, "sni", sni, 128);
        char pbk[128] = {0}; GetQueryParam(u.query, "pbk", pbk, 128);
        char sid[64] = {0}; GetQueryParam(u.query, "sid", sid, 64);
        char path[256] = {0}; GetQueryParam(u.query, "path", path, 256);
        char host[128] = {0}; GetQueryParam(u.query, "host", host, 128);
        char serviceName[128] = {0}; GetQueryParam(u.query, "serviceName", serviceName, 128);
        char flow[64] = {0}; GetQueryParam(u.query, "flow", flow, 64);

        if (is_vless && strlen(flow) > 0) cJSON_AddStringToObject(outbound, "flow", flow);

        // TLS / Reality
        if (strcmp(security, "tls") == 0 || strcmp(security, "reality") == 0) {
            cJSON* tls_conf = cJSON_CreateObject();
            cJSON_AddBoolToObject(tls_conf, "enabled", cJSON_True);
            cJSON_AddStringToObject(tls_conf, "server_name", strlen(sni) > 0 ? sni : u.host);
            cJSON_AddBoolToObject(tls_conf, "insecure", cJSON_True);
            
            if (strcmp(security, "reality") == 0) {
                cJSON* reality = cJSON_CreateObject();
                cJSON_AddBoolToObject(reality, "enabled", cJSON_True);
                cJSON_AddStringToObject(reality, "public_key", pbk);
                cJSON_AddStringToObject(reality, "short_id", sid);
                cJSON_AddItemToObject(tls_conf, "reality", reality);
            }
            cJSON_AddItemToObject(outbound, "tls", tls_conf);
        }

        // Transport
        if (strcmp(type, "ws") == 0) {
            cJSON* transport = cJSON_CreateObject();
            cJSON_AddStringToObject(transport, "type", "ws");
            cJSON_AddStringToObject(transport, "path", strlen(path) > 0 ? path : "/");
            if (strlen(host) > 0) {
                cJSON* headers = cJSON_CreateObject();
                cJSON_AddStringToObject(headers, "Host", host);
                cJSON_AddItemToObject(transport, "headers", headers);
            }
            cJSON_AddItemToObject(outbound, "transport", transport);
        } else if (strcmp(type, "grpc") == 0) {
            cJSON* transport = cJSON_CreateObject();
            cJSON_AddStringToObject(transport, "type", "grpc");
            cJSON_AddStringToObject(transport, "service_name", serviceName);
            cJSON_AddItemToObject(outbound, "transport", transport);
        }
    }
    // ---------------- HYSTERIA 2 ----------------
    else if (strncasecmp(link, "hysteria2://", 12) == 0 || strncasecmp(link, "hy2://", 6) == 0) {
        ParsedUrl u;
        if (!ParseGenericUrl(link, &u)) { cJSON_Delete(outbound); return NULL; }

        cJSON_AddStringToObject(outbound, "type", "hysteria2");
        cJSON_AddStringToObject(outbound, "server", u.host);
        cJSON_AddNumberToObject(outbound, "server_port", u.port);
        cJSON_AddStringToObject(outbound, "password", u.username); // Auth payload

        char sni[128] = {0}; GetQueryParam(u.query, "sni", sni, 128);
        char obfs[64] = {0}; GetQueryParam(u.query, "obfs", obfs, 64);
        char obfs_pass[128] = {0}; GetQueryParam(u.query, "obfs-password", obfs_pass, 128);
        
        cJSON* tls_conf = cJSON_CreateObject();
        cJSON_AddBoolToObject(tls_conf, "enabled", cJSON_True);
        cJSON_AddStringToObject(tls_conf, "server_name", strlen(sni) > 0 ? sni : u.host);
        cJSON_AddBoolToObject(tls_conf, "insecure", cJSON_True);
        cJSON_AddItemToObject(outbound, "tls", tls_conf);

        if (strcmp(obfs, "salamander") == 0) {
            cJSON* obfs_conf = cJSON_CreateObject();
            cJSON_AddStringToObject(obfs_conf, "type", "salamander");
            cJSON_AddStringToObject(obfs_conf, "password", obfs_pass);
            cJSON_AddItemToObject(outbound, "obfs", obfs_conf);
        }
    }
    // ---------------- SS ----------------
    else if (strncasecmp(link, "ss://", 5) == 0) {
        // [修复] 增加对 Base64 SS 的支持，不仅支持 SIP002
        ParsedUrl u;
        char* link_to_use = (char*)link;
        char buffer[2048]; // Decode buffer

        if (!strchr(link, '@')) {
             // Try base64
             const char* b64_raw = link + 5;
             char* tag = strchr(b64_raw, '#');
             if(tag) *tag = 0;
             
             char* b64_fixed = FixUrlSafeBase64(b64_raw);
             char* decoded = Base64Decode(b64_fixed ? b64_fixed : b64_raw);
             if (b64_fixed) free(b64_fixed);
             
             if(tag) *tag = '#';
             
             if(decoded) {
                 snprintf(buffer, sizeof(buffer), "ss://%s", decoded);
                 free(decoded);
                 link_to_use = buffer;
             } else {
                 cJSON_Delete(outbound); return NULL;
             }
        }
        
        if (ParseGenericUrl(link_to_use, &u)) {
             cJSON_AddStringToObject(outbound, "type", "shadowsocks");
             cJSON_AddStringToObject(outbound, "server", u.host);
             cJSON_AddNumberToObject(outbound, "server_port", u.port);
             cJSON_AddStringToObject(outbound, "method", u.username);
             cJSON_AddStringToObject(outbound, "password", u.password);
        } else {
             cJSON_Delete(outbound); return NULL;
        }
    }
    else {
        cJSON_Delete(outbound);
        return NULL;
    }

    // Common Tag
    cJSON_AddStringToObject(outbound, "tag", "proxy");

    return outbound;
}
