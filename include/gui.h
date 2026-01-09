#ifndef GUI_H
#define GUI_H

#include <windows.h>
#include <stdbool.h>

// --- GUI 接口函数声明 ---

/**
 * 创建并显示应用程序主窗口
 * * @param hInstance 应用程序实例句柄
 * @param nShow     窗口显示命令 (如 SW_SHOWNORMAL)
 * @return bool     成功返回 true，失败返回 false
 */
bool CreateMainWindow(HINSTANCE hInstance, int nShow);

/**
 * 获取主窗口句柄 (全局访问器)
 */
HWND GetMainWindowHandle();

// --- 控件操作辅助函数 (供其他模块调用) ---

/**
 * 向日志区域追加文本 (线程安全，内部使用 PostMessage 或直接操作)
 * 注意：实际实现通常在 WndProc 处理 WM_APP_LOG 消息中完成
 */
void GuiAppendLog(const char* message);

#endif // GUI_H
