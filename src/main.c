#include "gui.h"
#include "common.h"

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow) {
    // 1. 初始化 Winsock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // 2. 初始化 Common Controls (用于 GUI)
    INITCOMMONCONTROLSEX ic = {sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&ic);

    // 3. 创建主窗口 (代码在 gui_main.c)
    CreateMainWindow(hInst, nShow);

    // 4. 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    WSACleanup();
    return 0;
}
