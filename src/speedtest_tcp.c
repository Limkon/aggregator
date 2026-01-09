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
 * @param timeout_sec   超时时间 (秒)
 * @return              延迟毫秒数，失败或超时返回 -1.0
 */
double SpeedTest_TcpPing(const char* address, int port, int timeout_sec) {
    if (!address || port <= 0 || port > 65535) return -1.0;

    struct addrinfo hints = {0};
    struct addrinfo* res = NULL;
    SOCKET sock = INVALID_SOCKET;
    double start_time, end_time, latency = -1.0;
    int result;

    // 1. DNS 解析 (可能会阻塞，但在工作线程中执行无妨)
    hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(address, port_str, &hints, &res) != 0) {
        return -1.0; // 解析失败
    }

    // 2. 遍历解析结果尝试连接
    struct addrinfo* ptr = NULL;
    for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        // 设置非阻塞模式
        unsigned long on = 1;
        if (ioctlsocket(sock, FIONBIO, &on) != 0) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }

        // 开始计时
        start_time = GetTimeMs();

        // 发起连接
        result = connect(sock, ptr->ai_addr, (int)ptr->ai_addrlen);

        if (result == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                // 真正的错误
                closesocket(sock);
                sock = INVALID_SOCKET;
                continue;
            }
        }

        // 使用 select 等待连接完成 (实现超时控制)
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);

        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        // select 返回 > 0 表示有事件，socket 可写即表示连接成功
        int sel_res = select(0, NULL, &write_fds, NULL, &tv);
        
        if (sel_res > 0) {
            // 还需要检查 SO_ERROR 确认是否真的成功
            int error = 0;
            int len = sizeof(error);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&error, &len) == 0) {
                if (error == 0) {
                    // 连接成功！
                    end_time = GetTimeMs();
                    latency = end_time - start_time;
                    closesocket(sock);
                    break; // 跳出循环，返回结果
                }
            }
        }

        // 失败或超时
        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    if (res) freeaddrinfo(res);

    return latency;
}
