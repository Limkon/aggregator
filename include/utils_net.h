#ifndef UTILS_NET_H
#define UTILS_NET_H

#include <stdbool.h>

// 基础 HTTP GET 请求，支持超时
char* HttpGet(const char* url, const char* proxy, int timeout);

// 带认证 Token 的请求 (主要用于 GitHub API)
char* NetRequest(const char* url, const char* token);

// 检查网络连通性
bool NetCheckConnection(const char* url, const char* proxy, int timeout);

// 清理网络库资源 (SSL上下文等)
void CleanupUtilsNet();

#endif // UTILS_NET_H
