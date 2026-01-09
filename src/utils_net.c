#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>

// OpenSSL 头文件
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

// --- 全局状态变量 ---
static SSL_CTX* g_ssl_ctx = NULL;
static volatile LONG s_ctxInitState = 0; 
static volatile LONG s_active_requests = 0; 
static volatile BOOL s_is_cleaning_up = FALSE; 

// --- 简单的 URL 解析结构 ---
typedef struct { 
    char scheme[16];
    char host[256]; 
    int port; 
    char path[2048]; 
} URL_COMPONENTS_SIMPLE;

// 辅助函数：解析 URL
static BOOL ParseUrl(const char* url, URL_COMPONENTS_SIMPLE* out) {
    if (!url || !out) return FALSE;
    memset(out, 0, sizeof(URL_COMPONENTS_SIMPLE));
    
    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) { 
        strcpy(out->scheme, "http");
        p += 7; 
        out->port = 80; 
    }
    else if (strncmp(p, "https://", 8) == 0) { 
        strcpy(out->scheme, "https");
        p += 8; 
        out->port = 443; 
    }
    else return FALSE;
    
    // 查找 Host 结束位置
    const char* slash = strchr(p, '/');
    int hostLen = slash ? (int)(slash - p) : (int)strlen(p);
    
    if (hostLen <= 0 || hostLen >= (int)sizeof(out->host)) return FALSE;
    
    strncpy(out->host, p, hostLen); 
    out->host[hostLen] = 0; 
    
    // 处理端口号
    char* colon = strchr(out->host, ':');
    if (colon) { 
        *colon = 0; 
        out->port = atoi(colon + 1); 
        if (out->port <= 0 || out->port > 65535) return FALSE;
    }
    
    // 处理路径
    if (slash) {
        if (strlen(slash) >= sizeof(out->path)) return FALSE;
        strcpy(out->path, slash);
    } else {
        strcpy(out->path, "/");
    }
    return TRUE;
}

// 辅助函数：初始化全局 SSL Context
static BOOL InitSSLContext() {
    if (g_ssl_ctx != NULL) return TRUE;

    if (InterlockedCompareExchange(&s_ctxInitState, 1, 0) == 0) {
        const SSL_METHOD* method = TLS_client_method();
        SSL_CTX* ctx = SSL_CTX_new(method);
        
        if (ctx) {
            // 加载系统默认证书库或指定文件
            if (SSL_CTX_load_verify_locations(ctx, "cacert.pem", NULL) != 1) {
                if (SSL_CTX_load_verify_locations(ctx, "resources/cacert.pem", NULL) != 1) {
                    // 找不到证书时，临时禁用验证以保证功能可用
                    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
                } else {
                    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
                }
            } else {
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
            }
            
            EnterCriticalSection(&g_dataLock);
            g_ssl_ctx = ctx;
            LeaveCriticalSection(&g_dataLock);
            InterlockedExchange(&s_ctxInitState, 2); 
            return TRUE;
        } else {
            InterlockedExchange(&s_ctxInitState, 0); 
            return FALSE;
        }
    } else {
        int wait_loops = 0;
        while (s_ctxInitState == 1 && wait_loops < 200) {
            Sleep(10);
            wait_loops++;
        }
        return (g_ssl_ctx != NULL);
    }
}

// 内部函数：单次请求，返回 Body 或 Location Header
static char* InternalRequest(const char* url, int timeout_sec, char* out_location, int loc_size) {
    if (s_is_cleaning_up) return NULL;
    
    SOCKET s = INVALID_SOCKET;
    SSL* ssl = NULL;
    struct addrinfo* res = NULL;
    char* buf = NULL;
    char* result = NULL;
    
    URL_COMPONENTS_SIMPLE u;
    if (!ParseUrl(url, &u)) return NULL;

    // 1. SSL 初始化 (仅 HTTPS 需要)
    BOOL is_https = (strcmp(u.scheme, "https") == 0);
    if (is_https && !InitSSLContext()) return NULL;

    // 2. DNS 解析
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", u.port);

    if (getaddrinfo(u.host, portStr, &hints, &res) != 0) return NULL;

    // 3. 建立连接
    struct addrinfo* ptr = NULL;
    for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        if (s_is_cleaning_up) break;
        s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        DWORD time_ms = timeout_sec * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&time_ms, sizeof(time_ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&time_ms, sizeof(time_ms));

        if (connect(s, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }

    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return NULL;
    }

    // 4. SSL 握手
    if (is_https) {
        ssl = SSL_new(g_ssl_ctx);
        if (!ssl) goto cleanup;

        SSL_set_fd(ssl, (int)s);
        SSL_set_tlsext_host_name(ssl, u.host); 
        if (SSL_connect(ssl) != 1) goto cleanup;
    }

    // 5. 发送请求
    char req[4096];
    snprintf(req, sizeof(req), 
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\r\n"
        "Connection: close\r\n"
        "\r\n", 
        u.path, u.host);

    int sent = is_https ? SSL_write(ssl, req, (int)strlen(req)) : send(s, req, (int)strlen(req), 0);
    if (sent <= 0) goto cleanup;

    // 6. 读取响应
    size_t total_cap = 65536; 
    size_t total_len = 0;
    buf = (char*)malloc(total_cap);
    if (!buf) goto cleanup;

    ULONGLONG tick_start = GetTickCount64();
    unsigned long on = 1;
    ioctlsocket(s, FIONBIO, &on); // 非阻塞

    while (!s_is_cleaning_up) {
        if (GetTickCount64() - tick_start > (ULONGLONG)(timeout_sec * 1000)) break;

        if (total_len >= total_cap - 1024) {
            size_t new_cap = total_cap * 2;
            if (new_cap > 10 * 1024 * 1024) break; 
            char* new_buf = (char*)realloc(buf, new_cap);
            if (!new_buf) break;
            buf = new_buf;
            total_cap = new_cap;
        }

        int n;
        if (is_https) n = SSL_read(ssl, buf + total_len, (int)(total_cap - total_len - 1));
        else n = recv(s, buf + total_len, (int)(total_cap - total_len - 1), 0);
        
        if (n > 0) {
            total_len += n;
        } else {
            if (is_https) {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) { Sleep(10); continue; }
            } else {
                if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(10); continue; }
            }
            if (n == 0) break; 
            break; 
        }
    }

    // 7. 解析响应
    if (buf && total_len > 0) {
        buf[total_len] = 0;
        
        // 检查状态码
        int status_code = 0;
        if (sscanf(buf, "HTTP/%*d.%*d %d", &status_code) == 1) {
            
            // 处理重定向 (301, 302, 307, 308)
            if (status_code == 301 || status_code == 302 || status_code == 307 || status_code == 308) {
                if (out_location) {
                    char* loc = strstr(buf, "\nLocation:");
                    if (!loc) loc = strstr(buf, "\nlocation:");
                    if (loc) {
                        loc += 10; // skip "\nLocation:"
                        while (*loc == ' ' || *loc == '\t') loc++;
                        char* end = strchr(loc, '\r');
                        if (!end) end = strchr(loc, '\n');
                        if (end) {
                            int len = (int)(end - loc);
                            if (len >= loc_size) len = loc_size - 1;
                            strncpy(out_location, loc, len);
                            out_location[len] = 0;
                        }
                    }
                }
            } 
            // 处理成功 (200)
            else if (status_code == 200) {
                char* body_start = strstr(buf, "\r\n\r\n");
                if (body_start) {
                    body_start += 4;
                    size_t content_len = total_len - (body_start - buf);
                    result = (char*)malloc(content_len + 1);
                    if (result) {
                        memcpy(result, body_start, content_len);
                        result[content_len] = 0;
                    }
                }
            }
        }
    }

cleanup:
    if (buf) free(buf);
    if (res) freeaddrinfo(res);
    if (ssl) SSL_free(ssl);
    if (s != INVALID_SOCKET) closesocket(s);
    return result;
}

// 核心函数：执行 GET 请求 (支持自动重定向)
char* HttpGet(const char* url, const char* proxy, int timeout_sec) {
    if (s_is_cleaning_up) return NULL;
    InterlockedIncrement(&s_active_requests);

    char current_url[MAX_URL_LEN];
    strncpy(current_url, url, sizeof(current_url) - 1);
    char* final_result = NULL;
    
    int redirects = 0;
    while (redirects < 5) { // 最多允许 5 次重定向
        char location[MAX_URL_LEN] = {0};
        
        // 发起请求
        char* result = InternalRequest(current_url, timeout_sec, location, sizeof(location));
        
        // 如果有结果 (200 OK)，直接返回
        if (result) {
            final_result = result;
            break;
        }
        
        // 如果没有结果，但有 Location (重定向)
        if (location[0] != 0) {
            // 处理相对路径
            if (strncmp(location, "http", 4) != 0) {
                // 简单的相对路径拼接 (仅支持以 / 开头的绝对路径)
                if (location[0] == '/') {
                    URL_COMPONENTS_SIMPLE u;
                    ParseUrl(current_url, &u);
                    snprintf(current_url, sizeof(current_url), "%s://%s%s", u.scheme, u.host, location);
                } else {
                    // 复杂相对路径暂不支持，直接失败
                    break;
                }
            } else {
                strncpy(current_url, location, sizeof(current_url) - 1);
            }
            redirects++;
            continue; // 重试新地址
        } else {
            // 既无结果也无重定向，失败
            break;
        }
    }
    
    InterlockedDecrement(&s_active_requests);
    return final_result;
}

// 连通性测试
bool NetCheckConnection(const char* url, const char* proxy, int timeout) {
    char* res = HttpGet(url, proxy, timeout);
    if (res) {
        free(res);
        return true;
    }
    return false;
}

// 资源清理
void CleanupUtilsNet() {
    s_is_cleaning_up = TRUE;
    for (int i = 0; i < 200; i++) { 
        if (InterlockedCompareExchange(&s_active_requests, 0, 0) == 0) break;
        Sleep(10);
    }
    EnterCriticalSection(&g_dataLock);
    if (g_ssl_ctx) {
        SSL_CTX_free(g_ssl_ctx);
        g_ssl_ctx = NULL;
    }
    LeaveCriticalSection(&g_dataLock);
}
