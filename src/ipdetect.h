#pragma once

#include <stddef.h>

int detect_public_ip(const char *url, const char *ip_version, char *out, size_t out_len);
