/* src/utils_net.c */
#include "common.h"
#include "utils_net.h"
#include "gui.h" 
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
#include <openssl/pem.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

// --- 外部变量 ---
extern HWND g_hMainWnd;

// --- 日志辅助 ---
static void NetLog(const char* fmt, ...) {
    if (!g_hMainWnd) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    PostMessage(g_hMainWnd, WM_APP_LOG, 0, (LPARAM)_strdup(buf));
}

// --- 全局变量 ---
static SSL_CTX* g_ssl_ctx = NULL;
static volatile LONG s_ctxInitState = 0; 
static volatile LONG s_active_requests = 0;
static volatile BOOL s_is_cleaning_up = FALSE;

// --- [核心功能] 从 EXE 资源段加载证书 ---
static BOOL LoadCacertFromResource(SSL_CTX* ctx) {
    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(2), RT_RCDATA);
    if (!hRes) {
        NetLog("[Net] 错误: 未能在 EXE 资源中找到证书 (ID=2)");
        return FALSE;
    }

    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return FALSE;

    void* pData = LockResource(hData);
    DWORD size = SizeofResource(NULL, hRes);
    
    if (!pData || size == 0) return FALSE;

    NetLog("[Net] 从 EXE 资源加载证书 (%d bytes)...", size);

    BIO* cbio = BIO_new_mem_buf(pData, size);
    if (!cbio) {
        NetLog("[Net] BIO 创建失败");
        return FALSE;
    }

    X509_STORE* store = SSL_CTX_get_cert_store(ctx);
    if (!store) {
        BIO_free(cbio);
        return FALSE;
    }

    X509* cert = NULL;
    int count = 0;
    
    while ((cert = PEM_read_bio_X509(cbio, NULL, 0, NULL)) != NULL) {
        if (X509_STORE_add_cert(store, cert)) {
            count++;
        }
        X509_free(cert);
    }
    
    unsigned long err = ERR_peek_last_error();
    if (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE) {
        ERR_clear_error();
    }

    BIO_free(cbio);
    
    NetLog("[Net] 成功导入 %d 个根证书", count);
    return (count > 0);
}

// --- URL 解析 ---
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
        strcpy_s(out->scheme, sizeof(out->scheme), "http");
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

// --- 初始化 SSL ---
static BOOL InitSSLContext() {
    if (g_ssl_ctx != NULL) return TRUE;

    if (InterlockedCompareExchange(&s_ctxInitState, 1, 0) == 0) {
        if (s_is_cleaning_up) {
            InterlockedExchange(&s_ctxInitState, 0);
            return FALSE;
        }

        NetLog("[Net] 初始化 SSL 上下文 (单文件模式)...");
        const SSL_METHOD* method = TLS_client_method();
        SSL_CTX* ctx = SSL_CTX_new(method);

        if (ctx) {
            BOOL certLoaded = FALSE;

            if (SSL_CTX_load_verify_locations(ctx, "cacert.pem", NULL) == 1) {
                NetLog("[Net] 加载外部 cacert.pem 成功");
                certLoaded = TRUE;
            } 
            else if (SSL_CTX_load_verify_locations(ctx, "resources/cacert.pem", NULL) == 1) {
                NetLog("[Net] 加载外部 resources/cacert.pem 成功");
                certLoaded = TRUE;
            }

            if (!certLoaded) {
                if (LoadCacertFromResource(ctx)) {
                    certLoaded = TRUE;
                }
            }

            if (certLoaded) {
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
            } else {
                NetLog("[Net] 警告: 无法加载任何证书，禁用 SSL 验证 (不安全)");
                SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
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
            NetLog("[Net] 错误: SSL_CTX_new 失败");
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

// --- 连接代理逻辑 (毫秒级超时) ---
static SOCKET ConnectWithProxy(const char* host, int port, const char* proxy_url, int timeout_ms) {
    SOCKET s = INVALID_SOCKET;
    struct addrinfo hints = { 0 };
    struct addrinfo* res = NULL;
    char portStr[16];
    
    char targetHost[256];
    int targetPort;
    
    URL_COMPONENTS_SIMPLE proxyInfo;
    BOOL useProxy = (proxy_url && strlen(proxy_url) > 0 && ParseUrl(proxy_url, &proxyInfo));
    
    if (useProxy) {
        strncpy(targetHost, proxyInfo.host, sizeof(targetHost)-1);
        targetPort = proxyInfo.port;
        // NetLog("[Net] 连接代理: %s:%d -> %s", targetHost, targetPort, host); // 减少日志刷屏
    } else {
        strncpy(targetHost, host, sizeof(targetHost)-1);
        targetPort = port;
    }

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portStr, sizeof(portStr), "%d", targetPort);

    if (getaddrinfo(targetHost, portStr, &hints, &res) != 0) {
        NetLog("[Net] DNS 解析失败: %s", targetHost);
        return INVALID_SOCKET;
    }

    struct addrinfo* ptr = NULL;
    for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        if (s_is_cleaning_up) break;
        s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        // [Fix] 设置套接字超时 (毫秒)
        DWORD conn_timeout = (DWORD)timeout_ms; 
        
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&conn_timeout, sizeof(conn_timeout));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&conn_timeout, sizeof(conn_timeout));

        if (connect(s, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) break;
        
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET) {
        // NetLog("[Net] TCP 连接失败");
        return INVALID_SOCKET;
    }

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

        char respBuf[1024];
        int n = recv(s, respBuf, sizeof(respBuf)-1, 0);
        if (n <= 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        respBuf[n] = 0;
        
        if (!strstr(respBuf, " 200 ")) {
            char* crlf = strstr(respBuf, "\r\n");
            if(crlf) *crlf = 0;
            NetLog("[Net] 代理握手拒绝: %s", respBuf);
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    return s;
}

// --- 请求逻辑 ---
static char* InternalRequest(const char* url, int timeout_ms, const char* auth_token, const char* proxy, char* out_location, int loc_size) {
    if (s_is_cleaning_up) return NULL;
    InterlockedIncrement(&s_active_requests);

    char* result = NULL;
    SOCKET s = INVALID_SOCKET;
    SSL* ssl = NULL;
    char* buf = NULL;

    URL_COMPONENTS_SIMPLE u;
    if (!ParseUrl(url, &u)) {
        goto cleanup;
    }

    BOOL is_https = (strcmp(u.scheme, "https") == 0);
    if (is_https && !InitSSLContext()) goto cleanup;

    s = ConnectWithProxy(u.host, u.port, proxy, timeout_ms);
    if (s == INVALID_SOCKET) goto cleanup;

    if (s_is_cleaning_up) goto cleanup;

    if (is_https) {
        ssl = SSL_new(g_ssl_ctx);
        if (!ssl) goto cleanup;
        SSL_set_fd(ssl, (int)s);
        SSL_set_tlsext_host_name(ssl, u.host);
        
        if (SSL_connect(ssl) != 1) {
            NetLog("[Net] SSL 握手失败: %s", u.host);
            goto cleanup;
        }
    }

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

    unsigned long on = 1;
    ioctlsocket(s, FIONBIO, &on);

    size_t total_cap = 65536;
    size_t total_len = 0;
    buf = (char*)malloc(total_cap);
    if (!buf) goto cleanup;

    ULONGLONG tick_start = GetTickCount64();

    while (buf && !s_is_cleaning_up) {
        ULONGLONG now = GetTickCount64();
        if (now - tick_start > (ULONGLONG)timeout_ms) {
            // NetLog("[Net] 数据传输超时: %s", u.host);
            break;
        }

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
                if (tv.tv_sec > 0 || tv.tv_usec > 50000) { tv.tv_sec = 0; tv.tv_usec = 50000; }
                select(0, &r_fds, NULL, NULL, &tv);
                continue;
            }
            if (n == 0) break; 
            break; 
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
            } else {
                // NetLog("[Net] HTTP 错误: %d (%s)", status_code, u.host);
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

// [修改] 参数改为 timeout_ms
static char* PerformHttpRequest(const char* url, int timeout_ms, const char* token, const char* proxy) {
    if (s_is_cleaning_up) return NULL;
    
    char current_url[MAX_URL_LEN];
    strncpy(current_url, url, sizeof(current_url) - 1);
    char* final_result = NULL;

    int redirects = 0;
    while (redirects < 5 && !s_is_cleaning_up) {
        char location[MAX_URL_LEN] = { 0 };
        // [修改] 传递 timeout_ms，不再乘以 1000
        char* result = InternalRequest(current_url, timeout_ms, token, proxy, location, sizeof(location));

        if (result) {
            final_result = result;
            break;
        }

        if (location[0] != 0) {
            NetLog("[Net] 重定向 -> %s", location);
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

// [修改] 参数改为 timeout_ms
char* HttpGet(const char* url, const char* proxy, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 15000;
    return PerformHttpRequest(url, timeout_ms, NULL, proxy);
}

// 保持兼容旧接口
char* NetRequest(const char* url, const char* token) {
    return PerformHttpRequest(url, 15000, token, NULL); // 默认 15000ms
}

// [修改] 参数改为 timeout_ms
char* NetRequestWithProxy(const char* url, const char* token, const char* proxy, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 15000;
    return PerformHttpRequest(url, timeout_ms, token, proxy);
}

bool NetCheckConnection(const char* url, const char* proxy, int timeout_ms) {
    char* res = HttpGet(url, proxy, timeout_ms);
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
