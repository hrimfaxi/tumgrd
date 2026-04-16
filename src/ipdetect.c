#include "ipdetect.h"
#include "tumgrd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void tumgrd_trim(char *s) {
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

static int tumgrd_is_ipv4(const char *s) {
  struct in_addr addr4;
  return s && inet_pton(AF_INET, s, &addr4) == 1;
}

static int tumgrd_is_ipv6(const char *s) {
  struct in6_addr addr6;
  return s && inet_pton(AF_INET6, s, &addr6) == 1;
}

static int tumgrd_ip_family_from_version(const char *ip_version) {
  if (!ip_version || ip_version[0] == '\0') {
    return 0;
  }

  if (strcmp(ip_version, "4") == 0 || strcmp(ip_version, "ipv4") == 0) {
    return 4;
  }

  if (strcmp(ip_version, "6") == 0 || strcmp(ip_version, "ipv6") == 0) {
    return 6;
  }

  return 0;
}

static int tumgrd_extract_ip(const char *text, const char *ip_version, char *out, size_t out_len) {
  char  buf[512];
  char *p;
  int   family;

  if (!text || !out || out_len == 0) {
    return -1;
  }

  snprintf(buf, sizeof(buf), "%s", text);
  tumgrd_trim(buf);

  family = tumgrd_ip_family_from_version(ip_version);

  /*
   * 最常见情况：响应体本身就是一个 IP
   */
  if ((family == 0 || family == 4) && tumgrd_is_ipv4(buf)) {
    snprintf(out, out_len, "%s", buf);
    return 0;
  }

  if ((family == 0 || family == 6) && tumgrd_is_ipv6(buf)) {
    snprintf(out, out_len, "%s", buf);
    return 0;
  }

  /*
   * 容错：从文本里按空白/换行/引号/尖括号切 token 抽 IP
   */
  p = buf;
  while (*p) {
    char   token[128];
    size_t n = 0;

    while (*p && (isspace((unsigned char) *p) || *p == '"' || *p == '\'' || *p == '<' || *p == '>' || *p == ',' || *p == ';')) {
      p++;
    }

    if (!*p) {
      break;
    }

    while (*p && !isspace((unsigned char) *p) && *p != '"' && *p != '\'' && *p != '<' && *p != '>' && *p != ',' && *p != ';') {
      if (n + 1 < sizeof(token)) {
        token[n++] = *p;
      }
      p++;
    }

    token[n] = '\0';

    if (token[0] == '\0') {
      continue;
    }

    if ((family == 0 || family == 4) && tumgrd_is_ipv4(token)) {
      snprintf(out, out_len, "%s", token);
      return 0;
    }

    if ((family == 0 || family == 6) && tumgrd_is_ipv6(token)) {
      snprintf(out, out_len, "%s", token);
      return 0;
    }
  }

  return -1;
}

static void tumgrd_build_url(const char *url, char *buf, size_t buf_len) {
  if (!buf || buf_len == 0) {
    return;
  }

  if (!url || url[0] == '\0') {
    url = TUMGRD_DEFAULT_IP_CHECK_URL;
  }

  if (strstr(url, "://")) {
    snprintf(buf, buf_len, "%s", url);
  } else {
    snprintf(buf, buf_len, "http://%s", url);
  }
}

static int tumgrd_exec_capture(char *const argv[], char *out, size_t out_len) {
  int     pipefd[2];
  pid_t   pid;
  ssize_t nread;
  size_t  used = 0;
  int     status;

  if (!argv || !argv[0] || !out || out_len == 0) {
    return -1;
  }

  out[0] = '\0';

  if (pipe(pipefd) != 0) {
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    close(pipefd[0]);

    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    execvp(argv[0], argv);
    _exit(127);
  }

  close(pipefd[1]);

  while ((nread = read(pipefd[0], out + used, out_len - 1 - used)) > 0) {
    used += (size_t) nread;
    if (used >= out_len - 1) {
      break;
    }
  }

  out[used] = '\0';
  close(pipefd[0]);

  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return -1;
  }

  return 0;
}

int tumgrd_detect_public_ip(const char *url, const char *ip_version, char *out, size_t out_len) {
  char final_url[256];
  char buf[512];
  int  rc;

  if (!out || out_len == 0) {
    return -1;
  }

  out[0] = '\0';
  tumgrd_build_url(url, final_url, sizeof(final_url));

  /*
   * 优先 OpenWrt 常见的 uclient-fetch
   */
  {
    char *argv[] = {"uclient-fetch", "-qO-", final_url, NULL};

    buf[0] = '\0';
    rc     = tumgrd_exec_capture(argv, buf, sizeof(buf));
    if (rc == 0 && tumgrd_extract_ip(buf, ip_version, out, out_len) == 0) {
      return 0;
    }
  }

  /*
   * 回退到 wget
   */
  {
    char *argv[] = {"wget", "-qO-", final_url, NULL};

    buf[0] = '\0';
    rc     = tumgrd_exec_capture(argv, buf, sizeof(buf));
    if (rc == 0 && tumgrd_extract_ip(buf, ip_version, out, out_len) == 0) {
      return 0;
    }
  }

  fprintf(stderr, "[ipdetect] failed url=%s ip_version=%s\n", final_url, ip_version ? ip_version : "");

  return -1;
}
