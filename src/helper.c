#include "helper.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int parse_interval(const char *s, int *out) {
  char *end = NULL;
  long  v   = strtol(s, &end, 10);

  if (!s || s[0] == '\0' || !end || *end != '\0') {
    return -1;
  }
  if (v < 10 || v > 3600) {
    return -1;
  }

  *out = (int) v;
  return 0;
}

bool streqcase(const char *a, const char *b) {
  return strcasecmp(a, b) == 0;
}

bool streq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

void trim_inplace(char *s) {
  char *start;
  char *end;

  if (!s || s[0] == '\0') {
    return;
  }

  start = s;
  while (*start && isspace((unsigned char) *start)) {
    start++;
  }

  if (start != s) {
    memmove(s, start, strlen(start) + 1);
  }

  end = s + strlen(s);
  while (end > s && isspace((unsigned char) end[-1])) {
    end--;
  }
  *end = '\0';
}

void log_trimmed(const char *tag, const char *script) {
  if (!script) {
    log_info("%s: (null)", tag);
    return;
  }

  char *tmp = strdup(script);

  if (!tmp) {
    log_error("%s: strdup failed", tag);
    return;
  }

  trim_inplace(tmp);
  log_info("%s: %s", tag, tmp);
  free(tmp);
}

void copy_string(char *dst, size_t dst_len, const char *src) {
  if (!dst || dst_len == 0) {
    return;
  }

  if (!src) {
    dst[0] = '\0';
    return;
  }

  snprintf(dst, dst_len, "%s", src);
}

const char *nonempty_or_default(const char *input, const char *def_str) {
  if (!input || input[0] == '\0') {
    return def_str;
  }
  return input;
}

const char *nonempty_or_null(const char *input) {
  return nonempty_or_default(input, NULL);
}

// vim: set sw=2 ts=2 et:
