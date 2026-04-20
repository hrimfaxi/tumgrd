#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TUMGRD_FREE(p)                                                                                                         \
  do {                                                                                                                         \
    free(p);                                                                                                                   \
    p = NULL;                                                                                                                  \
  } while (0);

int         parse_interval(const char *s, uint32_t *out);
bool        streqcase(const char *a, const char *b);
bool        streq(const char *a, const char *b);
void        trim_inplace(char *s);
void        log_trimmed(const char *tag, const char *script);
void        copy_string(char *dst, size_t dst_len, const char *src);
const char *nonempty_or_default(const char *input, const char *def_str);
const char *nonempty_or_null(const char *s);
int         ensure_parent_dir(const char *path);
int         check_wan_interface(const char *ifname, bool *is_wan);

// vim: set sw=2 ts=2 et:
