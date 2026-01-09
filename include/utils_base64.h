#ifndef UTILS_BASE64_H
#define UTILS_BASE64_H
#include <stddef.h> // for size_t
char* Base64Encode(const unsigned char* data, size_t input_len);
char* Base64Decode(const char* input);
#endif
