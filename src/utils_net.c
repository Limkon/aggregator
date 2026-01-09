/* src/utils_net.c */
#include "common.h"
#include "utils_net.h"
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

// --- URL 解析结构 ---
typedef struct {
    char scheme[16];
    char host[256];
    int port;
    char path[2048];
} URL_COMPONENTS_SIMPLE;

static BOOL ParseUrl(const char* url, URL_COMPONENTS_SIMPLE* out) {
    if (!url || !out) return FALSE;
    memset(out, 0, sizeof(URL_COMPONENTS_SIMPLE));

    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) {
        strcpy_s(out->scheme, sizeof(out->scheme), "http");
        p += 7;
        out->port = 80;
    }
    else if (strncmp(p, "https://", 8) == 0) {
        strcpy_s(out->scheme, sizeof(out->scheme), "https");
        p += 8;
        out->port = 443;
    }
    else {
        // 尝试容错，如果没写协议头，默认 HTTP 或根据端口判断? 
        // 这里简单处理：假设直接是 host:port 或 host
        strcpy_s(out->scheme, sizeof(out->scheme), "http");
        // p 保持不变
    }

    const char* slash = strchr(p, '/');
    int hostLen = slash ? (int)(slash - p) : (int)strlen(p);

    if (hostLen <= 0 || hostLen >= (int)sizeof(out->host)) return FALSE;

    strncpy(out->host, p, hostLen);
    out->host[hostLen] = 0;

    char* colon = strchr(out->host, ':');
    if (colon) {
        *colon = 0;
        out->port = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535) return FALSE;
    }

    if (slash) {
        if (strlen(slash) >= sizeof(out->path)) return FALSE;
        strcpy_s(out->path, sizeof(out->path), slash);
    }
    else {
        strcpy_s(out->path, sizeof(out->path), "/");
    }
    return TRUE;
}

// 初始化 SSL (保持不变)
static BOOL InitSSLContext() {
    if (g_ssl_ctx != NULL) return TRUE;

    if (InterlockedCompareExchange(&s_ctxInitState, 1, 0) == 0) {
        if (s_is_cleaning_up) {
            InterlockedExchange(&s_ctxInitState, 0);
            return FALSE;
        }

        const SSL_METHOD* method = TLS_client_method();
        SSL_CTX* ctx = SSL_CTX_new(method);

        if (ctx) {
            if (SSL_CTX_load_verify_locations(ctx, "cacert.pem", NULL) != 1) {
                if (SSL_CTX_load_verify_locations(ctx, "resources/cacert.pem", NULL) != 1) {
                    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
                } else {
                    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
                }
            } else {
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
            }

            EnterCriticalSection(&g_dataLock);
            if (!s_is_cleaning_up) {
                g_ssl_ctx = ctx;
                InterlockedExchange(&s_ctxInitState, 2);
            } else {
                SSL_CTX_free(ctx);
                InterlockedExchange(&s_ctxInitState, 0);
            }
            LeaveCriticalSection(&g_dataLock);
            return (g_ssl_ctx != NULL);
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

// [New] 建立 TCP 连接 (支持通过 HTTP 代理隧道)
// 返回连接好的 socket，若失败返回 INVALID_SOCKET
static SOCKET ConnectWithProxy(const char* host, int port, const char* proxy_url, int timeout_ms) {
    SOCKET s = INVALID_SOCKET;
    struct addrinfo hints = { 0 };
    struct addrinfo* res = NULL;
    char portStr[16];
    
    char targetHost[256];
    int targetPort;
    
    // 1. 解析目标地址
    // 如果有代理，我们连接代理的 IP；如果没有，连接目标的 IP
    URL_COMPONENTS_SIMPLE proxyInfo;
    BOOL useProxy = (proxy_url && strlen(proxy_url) > 0 && ParseUrl(proxy_url, &proxyInfo));
    
    if (useProxy) {
        strncpy(targetHost, proxyInfo.host, sizeof(targetHost)-1);
        targetPort = proxyInfo.port;
    } else {
        strncpy(targetHost, host, sizeof(targetHost)-1);
        targetPort = port;
    }

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portStr, sizeof(portStr), "%d", targetPort);

    if (getaddrinfo(targetHost, portStr, &hints, &res) != 0) return INVALID_SOCKET;

    struct addrinfo* ptr = NULL;
    for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        if (s_is_cleaning_up) break;
        s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        // 设置连接超时
        DWORD conn_timeout = (timeout_ms > 3000) ? 3000 : timeout_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&conn_timeout, sizeof(conn_timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&conn_timeout, sizeof(conn_timeout));

        if (connect(s, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    // 2. 如果使用代理，发送 HTTP CONNECT 指令建立隧道
    if (useProxy) {
        char connectReq[512];
        snprintf(connectReq, sizeof(connectReq), 
            "CONNECT %s:%d HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Proxy-Connection: Keep-Alive\r\n\r\n",
            host, port, host, port);
        
        if (send(s, connectReq, (int)strlen(connectReq), 0) <= 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        // 读取代理响应 (简易处理，等待 200 Connection Established)
        char respBuf[1024];
        int n = recv(s, respBuf, sizeof(respBuf)-1, 0);
        if (n <= 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        respBuf[n] = 0;
        
        if (!strstr(respBuf, " 200 ")) { // 检查 HTTP 200
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    return s;
}

// 核心请求函数 (已更新：支持 Proxy 参数)
static char* InternalRequest(const char* url, int timeout_ms, const char* auth_token, const char* proxy, char* out_location, int loc_size) {
    if (s_is_cleaning_up) return NULL;
    InterlockedIncrement(&s_active_requests);

    char* result = NULL;
    SOCKET s = INVALID_SOCKET;
    SSL* ssl = NULL;
    char* buf = NULL;

    URL_COMPONENTS_SIMPLE u;
    if (!ParseUrl(url, &u)) goto cleanup;

    BOOL is_https = (strcmp(u.scheme, "https") == 0);
    if (is_https && !InitSSLContext()) goto cleanup;

    // [Update] 使用支持代理的连接函数
    s = ConnectWithProxy(u.host, u.port, proxy, timeout_ms);
    if (s == INVALID_SOCKET) goto cleanup;

    if (s_is_cleaning_up) goto cleanup;

    // SSL 握手
    if (is_https) {
        ssl = SSL_new(g_ssl_ctx);
        if (!ssl) goto cleanup;
        SSL_set_fd(ssl, (int)s);
        SSL_set_tlsext_host_name(ssl, u.host);
        if (SSL_connect(ssl) != 1) goto cleanup;
    }

    // 构造请求
    char extra_headers[512] = {0};
    if (auth_token && strlen(auth_token) > 0) {
        snprintf(extra_headers, sizeof(extra_headers), "Authorization: Bearer %s\r\n", auth_token);
    }

    char req[4096];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Mandala-Client/1.0 (Compatible; Aggregator)\r\n"
        "Accept: text/plain, application/json, */*\r\n"
        "%s"
        "Connection: close\r\n\r\n",
        u.path, u.host, extra_headers);

    int req_len = (int)strlen(req);
    int written = is_https ? SSL_write(ssl, req, req_len) : send(s, req, req_len, 0);
    if (written <= 0) goto cleanup;

    // 准备读取 (非阻塞)
    unsigned long on = 1;
    ioctlsocket(s, FIONBIO, &on);

    size_t total_cap = 65536;
    size_t total_len = 0;
    buf = (char*)malloc(total_cap);
    if (!buf) goto cleanup;

    ULONGLONG tick_start = GetTickCount64();

    while (buf && !s_is_cleaning_up) {
        ULONGLONG now = GetTickCount64();
        if (now - tick_start > (ULONGLONG)timeout_ms) break;

        if (total_len >= total_cap - 1024) {
            size_t new_cap = total_cap * 2;
            if (new_cap > 8 * 1024 * 1024) break; 
            char* new_buf = (char*)realloc(buf, new_cap);
            if (!new_buf) break;
            buf = new_buf;
            total_cap = new_cap;
        }

        int n;
        if (is_https) {
            n = SSL_read(ssl, buf + total_len, (int)(total_cap - total_len - 1));
        } else {
            n = recv(s, buf + total_len, (int)(total_cap - total_len - 1), 0);
        }

        if (n > 0) {
            total_len += n;
        } else {
            int should_retry = 0;
            if (is_https) {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) should_retry = 1;
            } else {
                if (WSAGetLastError() == WSAEWOULDBLOCK) should_retry = 1;
            }

            if (should_retry) {
                long long remaining = (long long)timeout_ms - (long long)(GetTickCount64() - tick_start);
                if (remaining <= 0) break;

                fd_set r_fds;
                FD_ZERO(&r_fds);
                FD_SET(s, &r_fds);
                struct timeval tv;
                tv.tv_sec = (long)(remaining / 1000);
                tv.tv_usec = (long)((remaining % 1000) * 1000);
                if (tv.tv_sec > 0 || tv.tv_usec > 100000) { tv.tv_sec = 0; tv.tv_usec = 100000; }
                
                select(0, &r_fds, NULL, NULL, &tv);
                continue;
            }
            if (n == 0) break; // EOF
            break; // Error
        }
    }

    if (buf && total_len > 0 && !s_is_cleaning_up) {
        buf[total_len] = 0;
        int status_code = 0;
        if (sscanf(buf, "HTTP/%*d.%*d %d", &status_code) == 1) {
            if (status_code >= 300 && status_code < 400 && out_location) {
                char* loc = strstr(buf, "\nLocation:");
                if (!loc) loc = strstr(buf, "\nlocation:");
                if (loc) {
                    loc += 10;
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
    if (ssl) SSL_free(ssl);
    if (s != INVALID_SOCKET) closesocket(s);
    InterlockedDecrement(&s_active_requests);
    return result;
}

// 通用 HTTP 请求处理（含重定向 + 代理传递）
static char* PerformHttpRequest(const char* url, int timeout_sec, const char* token, const char* proxy) {
    if (s_is_cleaning_up) return NULL;
    
    char current_url[MAX_URL_LEN];
    strncpy(current_url, url, sizeof(current_url) - 1);
    char* final_result = NULL;

    int redirects = 0;
    while (redirects < 5 && !s_is_cleaning_up) {
        char location[MAX_URL_LEN] = { 0 };
        // [Update] 传入 Proxy 参数
        char* result = InternalRequest(current_url, timeout_sec * 1000, token, proxy, location, sizeof(location));

        if (result) {
            final_result = result;
            break;
        }

        if (location[0] != 0) {
            if (strncmp(location, "http", 4) != 0) {
                if (location[0] == '/') {
                    URL_COMPONENTS_SIMPLE u;
                    ParseUrl(current_url, &u);
                    snprintf(current_url, sizeof(current_url), "%s://%s%s", u.scheme, u.host, location);
                }
                else break;
            }
            else {
                strncpy(current_url, location, sizeof(current_url) - 1);
            }
            redirects++;
            continue;
        }
        else {
            break;
        }
    }

    return final_result;
}

// 接口 1: 普通下载 (现已支持代理)
char* HttpGet(const char* url, const char* proxy, int timeout_sec) {
    return PerformHttpRequest(url, timeout_sec, NULL, proxy);
}

// 接口 2: 带 Token 的请求 (不使用代理或需手动透传，这里暂时为 NULL)
char* NetRequest(const char* url, const char* token) {
    // 搜索通常不需要代理，或者可以扩展
    return PerformHttpRequest(url, 15, token, NULL);
}

// 接口 3: 连通性检查
bool NetCheckConnection(const char* url, const char* proxy, int timeout) {
    char* res = HttpGet(url, proxy, timeout);
    if (res) {
        free(res);
        return true;
    }
    return false;
}

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
    InterlockedExchange(&s_ctxInitState, 0);
    LeaveCriticalSection(&g_dataLock);
}
