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
static volatile LONG s_ctxInitState = 0; // 0=Uninit, 1=Initializing, 2=Done
static volatile LONG s_active_requests = 0; // 活跃请求计数
static volatile BOOL s_is_cleaning_up = FALSE; // 清理标志

// --- 简单的 URL 解析结构 ---
typedef struct { 
    char host[256]; 
    int port; 
    char path[2048]; 
} URL_COMPONENTS_SIMPLE;

// 辅助函数：解析 URL
static BOOL ParseUrl(const char* url, URL_COMPONENTS_SIMPLE* out) {
    if (!url || !out) return FALSE;
    memset(out, 0, sizeof(URL_COMPONENTS_SIMPLE));
    
    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) { p += 7; out->port = 80; }
    else if (strncmp(p, "https://", 8) == 0) { p += 8; out->port = 443; }
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

// 辅助函数：初始化全局 SSL Context (单例模式)
static BOOL InitSSLContext() {
    // 双重检查锁定 (Double-Checked Locking) 优化
    if (g_ssl_ctx != NULL) return TRUE;

    if (InterlockedCompareExchange(&s_ctxInitState, 1, 0) == 0) {
        // 获取了初始化锁
        const SSL_METHOD* method = TLS_client_method();
        SSL_CTX* ctx = SSL_CTX_new(method);
        
        if (ctx) {
            // 尝试加载证书 (优先当前目录，其次 resources 目录)
            if (SSL_CTX_load_verify_locations(ctx, "cacert.pem", NULL) != 1) {
                if (SSL_CTX_load_verify_locations(ctx, "resources/cacert.pem", NULL) != 1) {
                    // 如果找不到证书，为了保证可用性，临时禁用验证并打印警告
                    // GuiAppendLog("[Net] Warning: cacert.pem not found. SSL verification disabled.");
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
            
            InterlockedExchange(&s_ctxInitState, 2); // 标记初始化完成
            return TRUE;
        } else {
            InterlockedExchange(&s_ctxInitState, 0); // 初始化失败
            return FALSE;
        }
    } else {
        // 其他线程正在初始化，等待完成
        int wait_loops = 0;
        while (s_ctxInitState == 1 && wait_loops < 200) {
            Sleep(10);
            wait_loops++;
        }
        return (g_ssl_ctx != NULL);
    }
}

// 核心函数：执行 HTTPS GET 请求
// 对应 MandalaECH 的 InternalHttpsGet
char* HttpGet(const char* url, const char* proxy, int timeout_sec) {
    if (s_is_cleaning_up) return NULL;
    InterlockedIncrement(&s_active_requests);

    char* result = NULL;
    SOCKET s = INVALID_SOCKET;
    SSL* ssl = NULL;
    struct addrinfo* res = NULL;
    char* buf = NULL;
    
    // 1. 解析 URL
    URL_COMPONENTS_SIMPLE u;
    if (!ParseUrl(url, &u)) goto cleanup;

    // 2. 初始化 SSL 环境
    if (!InitSSLContext()) goto cleanup;

    // 3. DNS 解析
    // 注意：如果有代理设置，这里逻辑需要调整 (MandalaECH 此处暂未处理 HTTP 代理链)
    // 为保持与 MandalaECH 一致性，暂不实现 HTTP Tunnel 代理逻辑，仅直连
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", u.port);

    if (getaddrinfo(u.host, portStr, &hints, &res) != 0) goto cleanup;

    // 4. 建立 TCP 连接
    struct addrinfo* ptr = NULL;
    for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        if (s_is_cleaning_up) break;
        s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        // 设置超时
        DWORD time_ms = timeout_sec * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&time_ms, sizeof(time_ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&time_ms, sizeof(time_ms));

        if (connect(s, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }

    if (s == INVALID_SOCKET) goto cleanup;

    // 5. 建立 SSL 握手
    ssl = SSL_new(g_ssl_ctx);
    if (!ssl) goto cleanup;

    SSL_set_fd(ssl, (int)s);
    SSL_set_tlsext_host_name(ssl, u.host); // SNI 支持

    if (SSL_connect(ssl) != 1) {
        // Handshake failed
        goto cleanup;
    }

    // 6. 发送 HTTP 请求
    char req[4096];
    snprintf(req, sizeof(req), 
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: ProxyAggregator/1.0\r\n"
        "Connection: close\r\n"
        "\r\n", 
        u.path, u.host);

    if (SSL_write(ssl, req, (int)strlen(req)) <= 0) goto cleanup;

    // 7. 读取响应
    size_t total_cap = 65536; // 64KB 初始容量
    size_t total_len = 0;
    buf = (char*)malloc(total_cap);
    if (!buf) goto cleanup;

    ULONGLONG tick_start = GetTickCount64();
    
    // 设置非阻塞以便自行控制超时逻辑 (MandalaECH 逻辑)
    unsigned long on = 1;
    ioctlsocket(s, FIONBIO, &on);

    while (!s_is_cleaning_up) {
        // 超时检查
        if (GetTickCount64() - tick_start > (ULONGLONG)(timeout_sec * 1000)) break;

        // 扩容检查
        if (total_len >= total_cap - 1024) {
            size_t new_cap = total_cap * 2;
            if (new_cap > 10 * 1024 * 1024) break; // 最大 10MB
            char* new_buf = (char*)realloc(buf, new_cap);
            if (!new_buf) break;
            buf = new_buf;
            total_cap = new_cap;
        }

        int n = SSL_read(ssl, buf + total_len, (int)(total_cap - total_len - 1));
        
        if (n > 0) {
            total_len += n;
        } else {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                Sleep(10); // 简单等待
                continue;
            }
            if (n == 0) break; // EOF (连接关闭)
            break; // Error
        }
    }

    // 8. 解析 HTTP Body (去除 Header)
    if (buf && total_len > 0) {
        buf[total_len] = 0;
        // 简单判断 HTTP 200
        if (strstr(buf, "HTTP/1.1 200") || strstr(buf, "HTTP/1.0 200")) {
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

cleanup:
    if (buf) free(buf);
    if (res) freeaddrinfo(res);
    if (ssl) SSL_free(ssl);
    if (s != INVALID_SOCKET) closesocket(s);
    InterlockedDecrement(&s_active_requests);
    return result;
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
    
    // 等待活跃请求结束
    for (int i = 0; i < 200; i++) { // 最多等待 2秒
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
