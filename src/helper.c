#include "helper.h"
#include "log.h"
#include "try.h"

#include <uci.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static int parse_u32(const char *input, uint32_t *out_u32) {
  char         *endptr = NULL;
  unsigned long ulong_val;

  errno     = 0; // 复位errno： strtoul函数本身不保证成功时设置errno=0
  ulong_val = strtoul(input, &endptr, 0);
  if (endptr == input || *endptr || errno == ERANGE || ulong_val > UINT32_MAX) {
    // log_error("Invalid u32: %s", input);
    return -EINVAL;
  }

  *out_u32 = (typeof(*out_u32)) ulong_val;
  return 0;
}

int parse_interval(const char *input, uint32_t *out_interval) {
  int err = parse_u32(input, out_interval);

  if (err || *out_interval < 10 || *out_interval > 3600) {
    log_error("Invalid interval: %s", input);
    return -EINVAL;
  }

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

static int _mkdir_p(const char *dir, mode_t mode) {
  char   tmp[PATH_MAX];
  size_t len;
  char  *p;

  if (!dir || dir[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  len = strlen(dir);
  if (len >= sizeof(tmp)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  copy_string(tmp, sizeof(tmp), dir);

  if (tmp[len - 1] == '/') {
    tmp[len - 1] = '\0';
  }

  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        return -1;
      }
      *p = '/';
    }
  }

  if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
    return -1;
  }

  return 0;
}

int ensure_parent_dir(const char *path) {
  char  dir[PATH_MAX];
  char *slash;

  if (!path || path[0] == '\0') {
    errno = EINVAL;
    return -1;
  }

  if (strlen(path) >= sizeof(dir)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  copy_string(dir, sizeof(dir), path);

  slash = strrchr(dir, '/');
  if (!slash) {
    return 0;
  }

  if (slash == dir) {
    return 0;
  }

  *slash = '\0';

  return _mkdir_p(dir, 0755);
}

int check_wan_interface(const char *ifname, bool *is_wan) {
  struct uci_context *ctx = NULL;
  struct uci_ptr      ptr = {};
  char                query[] = "network.wan.device";
  int                 err     = 0;

  if (!is_wan)
    return -EINVAL;

  *is_wan = false;
  ctx = try2_p(uci_alloc_context());
  try2(uci_lookup_ptr(ctx, &ptr, query, true) == UCI_OK ? 0 : -ENOENT);

  if (ptr.o && is_wan) {
    if (ptr.o->type == UCI_TYPE_STRING) {
      if (ifname && streq(ptr.o->v.string, ifname)) {
        *is_wan = true;
      }
    } else if (ptr.o->type == UCI_TYPE_LIST) {
      struct uci_element *e;
      uci_foreach_element(&ptr.o->v.list, e) {
        if (ifname && streq(e->name, ifname)) {
          *is_wan = true;
          break;
        }
      }
    }
  }

err_cleanup:
  if (ctx) {
    uci_free_context(ctx);
    ctx = NULL;
  }

  return err;
}

// vim: set sw=2 ts=2 et:
