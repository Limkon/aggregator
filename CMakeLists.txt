cmake_minimum_required(VERSION 3.15)
project(ProxyAggregator LANGUAGES C)

# --- 1. 配置构建选项 ---
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 支持 Windows Unicode 字符集
add_definitions(-DUNICODE -D_UNICODE)

# --- 2. 查找依赖库 ---
find_package(OpenSSL REQUIRED)

# --- 3. 设置文件 ---
include_directories(include)
include_directories(${OPENSSL_INCLUDE_DIR})

# 源文件列表
set(SOURCES
    src/main.c
    src/gui_main.c
    src/aggregator_core.c
    src/aggregator_search.c
    src/parser_nodes.c
    src/speedtest_singbox.c
    src/speedtest_tcp.c
    src/utils_base64.c
    src/utils_net.c
    src/cJSON.c
)

set(RESOURCES resources/resource.rc)

# --- 4. 创建目标 ---
# WIN32 标记它是 GUI 程序
add_executable(ProxyAggregator WIN32 ${SOURCES} ${RESOURCES})

# --- 5. 链接配置 ---
# 关键修复：MinGW 使用 wWinMain 需要 -municode
if(MINGW)
    target_link_options(ProxyAggregator PRIVATE -municode)
endif()

# 链接库
target_link_libraries(ProxyAggregator
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32      # Winsock 2
    crypt32     # Windows Crypto API
    comctl32    # Common Controls (GUI)
)
