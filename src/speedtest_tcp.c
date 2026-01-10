/* src/speedtest_tcp.c */
#include "common.h"
#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// --- 内部辅助：高精度计时器 (返回毫秒) ---
static double GetTimeMs() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}

/**
 * 执行 TCP Ping 测速
 * @param address       目标地址 (IP 或域名)
 * @param port          目标端口
 * @param timeout_ms    超时时间 (毫秒)
 * @return              延迟毫秒数，失败或超时返回 -1.0
 */
double SpeedTest_TcpPing(const char* address, int port, int timeout_ms) {
    if (!address || port <= 0 || port > 65535 || strlen(address) == 0) return -1.0;

    struct addrinfo hints = {0};
    struct addrinfo* res = NULL;
    SOCKET sock = INVALID_SOCKET;
    double start_time, end_time, latency = -1.0;

    // [优化] 如果地址已经是 IP 字符串，直接使用，跳过 getaddrinfo 的 DNS 查询
    // 这能极大减少“无反应”的情况，特别是面对大量被污染域名时
    unsigned long ip_addr = inet_addr(address);
    if (ip_addr != INADDR_NONE) {
        // 是 IPv4 地址
        struct sockaddr_in target;
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = ip_addr;
        target.sin_port = htons(port);
        
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return -1.0;

        unsigned long on = 1;
        ioctlsocket(sock, FIONBIO, &on);

        start_time = GetTimeMs();
        int conn_res = connect(sock, (struct sockaddr*)&target, sizeof(target));
        
        // --- 统一的 select 等待逻辑 (IP直连) ---
        if (conn_res == 0) {
            end_time = GetTimeMs();
            latency = end_time - start_time;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set write_fds, except_fds;
            FD_ZERO(&write_fds); FD_ZERO(&except_fds);
            FD_SET(sock, &write_fds); FD_SET(sock, &except_fds);
            
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            if (select(0, NULL, &write_fds, &except_fds, &tv) > 0) {
                if (FD_ISSET(sock, &write_fds)) {
                    latency = GetTimeMs() - start_time;
                }
            }
        }
        closesocket(sock);
        return latency;
    }

    // --- 如果不是 IP，必须走 DNS 解析 (可能会慢) ---
    hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(address, port_str, &hints, &res) != 0) {
        return -1.0; // 解析失败
    }

    struct addrinfo* ptr = NULL;
    for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        unsigned long on = 1;
        ioctlsocket(sock, FIONBIO, &on);

        start_time = GetTimeMs();

        // 发起连接
        if (connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) {
            end_time = GetTimeMs();
            latency = end_time - start_time;
            closesocket(sock);
            break; 
        }

        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }

        fd_set write_fds, except_fds;
        FD_ZERO(&write_fds); FD_ZERO(&except_fds);
        FD_SET(sock, &write_fds); FD_SET(sock, &except_fds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &write_fds, &except_fds, &tv) > 0) {
             if (FD_ISSET(sock, &write_fds)) {
                // 进一步检查 SO_ERROR 确认
                int error = 0;
                int len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == 0 && error == 0) {
                    end_time = GetTimeMs();
                    latency = end_time - start_time;
                    closesocket(sock);
                    break; 
                }
             }
        }

        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    if (res) freeaddrinfo(res);
    return latency;
}
