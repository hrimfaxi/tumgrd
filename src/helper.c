#include "helper.h"

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

// vim: set sw=2 ts=2 et:
