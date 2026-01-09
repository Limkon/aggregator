#ifndef UTILS_NET_H
#define UTILS_NET_H
#include <stdbool.h>
char* HttpGet(const char* url, const char* proxy, int timeout_sec);
bool NetCheckConnection(const char* url, const char* proxy, int timeout);
void CleanupUtilsNet(); // 记得在 main.c 退出前调用
#endif
