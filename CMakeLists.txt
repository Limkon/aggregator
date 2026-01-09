cmake_minimum_required(VERSION 3.10)

# 项目名称
project(ProxyAggregator C)

set(CMAKE_C_STANDARD 11)

# 查找依赖 (复用 MandalaECH 的 OpenSSL 用于 HTTPS/Hash)
find_package(OpenSSL REQUIRED)

# 源文件配置
set(SOURCES
    src/main.c
    src/cJSON.c
    src/gui_main.c
    src/gui_log.c
    src/aggregator_core.c
    src/aggregator_search.c
    src/parser_nodes.c
    src/speedtest_tcp.c
    src/speedtest_singbox.c
    src/utils_net.c
    src/utils_base64.c
)

set(RESOURCES resources/resource.rc)

# 构建 Windows GUI 程序
add_executable(${PROJECT_NAME} WIN32 ${SOURCES} ${RESOURCES})

# 头文件路径
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${OPENSSL_INCLUDE_DIR}
)

# 链接库 (参考 MandalaECH，加入 wininet 用于网络)
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${OPENSSL_LIBRARIES}
    ws2_32      # Winsock
    wininet     # HTTP 请求
    gdi32
    comctl32    # 通用控件
    user32
    shell32
)

# MinGW 静态链接配置 (保持 MandalaECH 风格)
if(MINGW)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static -static-libgcc -municode")
endif()
