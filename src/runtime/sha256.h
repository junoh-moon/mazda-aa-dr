#ifndef MX5_SHA256_H
#define MX5_SHA256_H
#include <stddef.h>
void mx5_sha256_bytes(const void *data, size_t len, char hex[65]);
bool mx5_verify_file_sha256(const char *path, const char *expected);
#endif
