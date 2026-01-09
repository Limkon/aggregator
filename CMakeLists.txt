cmake_minimum_required(VERSION 3.15)
project(ProxyAggregator LANGUAGES C)

# --- 1. 配置构建选项 ---
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 支持 Windows Unicode 字符集
add_definitions(-DUNICODE -D_UNICODE)

# --- 2. 查找依赖库 ---
# 查找 OpenSSL (用于 utils_net.c)
find_package(OpenSSL REQUIRED)

# --- 3. 设置文件 ---
# 头文件路径
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

# 资源文件 (图标等)
set(RESOURCES resources/resource.rc)

# --- 4. 创建目标 ---
# WIN32 选项告诉编译器这是一个 GUI 程序，而不是控制台程序 (即入口是 WinMain)
add_executable(ProxyAggregator WIN32 ${SOURCES} ${RESOURCES})

# --- 5. 链接库 ---
target_link_libraries(ProxyAggregator
    OpenSSL::SSL
    OpenSSL::Crypto
    ws2_32      # Winsock 2
    crypt32     # Windows Crypto API
    comctl32    # Common Controls (用于 GUI)
)

# 如果使用 MSVC，可能需要手动复制 DLL 或进行其他设置，但在 GitHub Actions 中通常环境已就绪
