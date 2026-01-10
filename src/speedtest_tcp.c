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

// 简单的日志辅助 (需要 aggregator_core.c 暴露或自己实现，这里为了解耦暂不使用)
// 只要返回 -1.0，上层自然会过滤掉

/**
 * 执行 TCP Ping 测速
 */
double SpeedTest_TcpPing(const char* address, int port, int timeout_ms) {
    if (!address || port <= 0 || port > 65535 || strlen(address) == 0) return -1.0;

    struct addrinfo hints = {0};
    struct addrinfo* res = NULL;
    SOCKET sock = INVALID_SOCKET;
    double start_time, end_time, latency = -1.0;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    // [注意] getaddrinfo 可能会阻塞。如果 address 是被墙的域名，这里可能卡很久
    if (getaddrinfo(address, port_str, &hints, &res) != 0) {
        return -1.0; 
    }

    struct addrinfo* ptr = NULL;
    for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        unsigned long on = 1;
        ioctlsocket(sock, FIONBIO, &on);

        start_time = GetTimeMs();

        if (connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) {
            // 立即连接成功 (极少见)
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
        FD_ZERO(&write_fds);
        FD_ZERO(&except_fds);
        FD_SET(sock, &write_fds);
        FD_SET(sock, &except_fds); // [修复] 监听异常集合

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int sel_res = select(0, NULL, &write_fds, &except_fds, &tv);
        
        if (sel_res > 0) {
            // 检查是否有异常 (连接被拒绝/RST)
            if (FD_ISSET(sock, &except_fds)) {
                // 连接失败
            }
            else if (FD_ISSET(sock, &write_fds)) {
                // 可写，进一步检查 SO_ERROR
                int error = 0;
                int len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == 0) {
                    if (error == 0) {
                        end_time = GetTimeMs();
                        latency = end_time - start_time;
                        closesocket(sock);
                        break; // 成功！
                    }
                }
            }
        }

        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    if (res) freeaddrinfo(res);
    return latency;
}
