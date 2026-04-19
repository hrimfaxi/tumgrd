#pragma once

#include <stdbool.h>

int  parse_interval(const char *s, int *out);
bool streqcase(const char *a, const char *b);
bool streq(const char *a, const char *b);
void trim_inplace(char *s);

// vim: set sw=2 ts=2 et:
