cmake_minimum_required(VERSION 3.10)

# 项目名称
project(ProxyAggregator C)

# --- 1. 基础编译设置 ---
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- 2. 查找依赖库 ---
# 查找 OpenSSL (用于 HTTPS 和哈希计算)
find_package(OpenSSL REQUIRED)

# --- 3. 源文件配置 ---
# 按功能模块组织源文件
set(SOURCES
    # 入口与 GUI 界面
    src/main.c
    src/gui_main.c
    src/gui_log.c
    
    # 聚合器核心逻辑
    src/aggregator_core.c    # 线程调度与任务管理
    src/aggregator_search.c  # GitHub 搜索逻辑
    
    # 协议解析与测速模块
    src/parser_nodes.c       # 节点链接解析 (vmess/vless/ss等)
    src/speedtest_tcp.c      # TCP Ping 测速
    src/speedtest_singbox.c  # Sing-box 进程调用
    
    # 通用工具模块
    src/utils_net.c          # 网络请求 (WinINet 封装)
    src/utils_base64.c       # Base64 编解码
    src/cJSON.c              # JSON 解析库
)

# 资源文件 (图标、版本信息等)
set(RESOURCES
    resources/resource.rc
)

# --- 4. 构建目标 ---
# 创建 Windows GUI 可执行文件 (WIN32 参数会自动处理 -mwindows)
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${RESOURCES})

# 强制输出文件名
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "ProxyAggregator")

# --- 5. 目标属性配置 ---

# [Include] 设置头文件搜索路径
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/resources
    ${OPENSSL_INCLUDE_DIR}
)

# [Defines] 预处理器定义
target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS # 忽略 scanf 等不安全警告
)

# [Options] 编译选项 - 增强兼容性
# 仅在 GCC/Clang (MinGW) 下强制 UTF-8 源码解析，防止中文乱码
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
        "-finput-charset=UTF-8"
    )
endif()

# [Libs] 链接依赖库
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OPENSSL_LIBRARIES}
    ws2_32      # Winsock 网络库
    wininet     # Windows HTTP/HTTPS 客户端库
    gdi32       # 图形设备接口
    comctl32    # 通用控件库 (ListView, ProgressBar等)
    user32      # 用户界面库
    shell32     # Shell API
    crypt32     # 加密 API
    ole32       # COM 组件支持
    shlwapi     # Shell 轻量级实用工具
)

# --- 6. 平台特定配置 (MinGW) ---
if(MINGW)
    # [优化] MinGW 下的静态链接与入口配置
    # -static: 链接静态库，避免依赖 DLL
    # -static-libgcc: 静态链接 GCC 运行时
    # -municode: 使用 wWinMain 作为入口点
    set(MINGW_LINK_FLAGS "-static -static-libgcc -municode")
    
    # 如果是 Release 模式，自动剥离符号表以减小体积
    if(CMAKE_BUILD_TYPE MATCHES "Release")
        set(MINGW_LINK_FLAGS "${MINGW_LINK_FLAGS} -s")
    endif()

    # 将链接标志应用于目标
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "${MINGW_LINK_FLAGS}")
endif()

# --- 7. 构建信息摘要 ---
message(STATUS "=========================================")
message(STATUS "  Project Name:      ${PROJECT_NAME}")
message(STATUS "  Build Type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  OpenSSL Include:   ${OPENSSL_INCLUDE_DIR}")
message(STATUS "  Output Name:       ProxyAggregator.exe")
message(STATUS "=========================================")
