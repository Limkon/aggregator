#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h> // 新增: 修复 unknown type name 'uint32_t'

// Base64 索引表
static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 辅助：查找字符索引
static int pos(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/**
 * Base64 编码
 * @param data      输入数据
 * @param input_len 输入长度
 * @return          返回动态分配的编码字符串 (需 free)
 */
char* Base64Encode(const unsigned char* data, size_t input_len) {
    size_t output_len = 4 * ((input_len + 2) / 3);
    char* encoded_data = (char*)malloc(output_len + 1);
    if (encoded_data == NULL) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i < input_len;) {
        uint32_t octet_a = i < input_len ? data[i++] : 0;
        uint32_t octet_b = i < input_len ? data[i++] : 0;
        uint32_t octet_c = i < input_len ? data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }

    // 处理 Padding
    int mod = input_len % 3;
    if (mod == 1) {
        encoded_data[output_len - 1] = '=';
        encoded_data[output_len - 2] = '=';
    } else if (mod == 2) {
        encoded_data[output_len - 1] = '=';
    }

    encoded_data[output_len] = '\0';
    return encoded_data;
}

/**
 * 安全的 Base64 解码 (自动处理 URL-Safe 字符和 Padding)
 * @param input     输入字符串
 * @return          返回动态分配的解码字符串 (需 free)，失败返回 NULL
 */
char* Base64Decode(const char* input) {
    if (!input) return NULL;

    size_t len = strlen(input);
    if (len == 0) return _strdup("");

    // 1. 预处理：复制并规范化 (处理 URL-Safe 字符)
    char* clean_input = (char*)malloc(len + 4); // +4 for padding safety
    if (!clean_input) return NULL;

    size_t clean_len = 0;
    for (size_t i = 0; i < len; i++) {
        char c = input[i];
        if (c == '-' || c == '_') {
            clean_input[clean_len++] = (c == '-') ? '+' : '/';
        } else if (isalnum((unsigned char)c) || c == '+' || c == '/') {
            clean_input[clean_len++] = c;
        }
        // 忽略非 Base64 字符（除了 =），这比 Python 的 ignore errors 更宽松
    }

    // 2. 补全 Padding
    while (clean_len % 4 != 0) {
        clean_input[clean_len++] = '=';
    }
    clean_input[clean_len] = '\0';

    // 3. 解码
    size_t output_len = clean_len / 4 * 3;
    char* decoded_data = (char*)malloc(output_len + 1);
    if (decoded_data == NULL) {
        free(clean_input);
        return NULL;
    }

    size_t i, j;
    for (i = 0, j = 0; i < clean_len;) {
        int sextext_a = clean_input[i] == '=' ? 0 & i++ : pos(clean_input[i++]);
        int sextext_b = clean_input[i] == '=' ? 0 & i++ : pos(clean_input[i++]);
        int sextext_c = clean_input[i] == '=' ? 0 & i++ : pos(clean_input[i++]);
        int sextext_d = clean_input[i] == '=' ? 0 & i++ : pos(clean_input[i++]);

        if (sextext_a == -1 || sextext_b == -1 || sextext_c == -1 || sextext_d == -1) {
            // 包含非法字符，解码失败
            free(clean_input);
            free(decoded_data);
            return NULL;
        }

        uint32_t triple = (sextext_a << 3 * 6) + (sextext_b << 2 * 6) + (sextext_c << 1 * 6) + (sextext_d << 0 * 6);

        if (j < output_len) decoded_data[j++] = (triple >> 2 * 8) & 0xFF;
        if (j < output_len) decoded_data[j++] = (triple >> 1 * 8) & 0xFF;
        if (j < output_len) decoded_data[j++] = (triple >> 0 * 8) & 0xFF;
    }

    decoded_data[j] = '\0'; // 确保字符串结尾
    free(clean_input);

    // 去除结尾可能的无效空字符（由于 padding 计算导致）
    // 在处理二进制数据时需谨慎，但此处主要用于处理文本订阅
    return decoded_data;
}
