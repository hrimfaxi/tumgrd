#include "ipdetect.h"
#include "helper.h"
#include "log.h"
#include "try.h"
#include "tumgrd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int is_ipv4(const char *s) {
  struct in_addr addr4;
  return s && inet_pton(AF_INET, s, &addr4) == 1;
}

static int is_ipv6(const char *s) {
  struct in6_addr addr6;
  return s && inet_pton(AF_INET6, s, &addr6) == 1;
}

static int ip_family_from_version(const char *ip_version) {
  if (!ip_version || ip_version[0] == '\0') {
    return 0;
  }

  if (streqcase(ip_version, "ipv4")) {
    return 4;
  }

  if (streqcase(ip_version, "ipv6")) {
    return 6;
  }

  return 0;
}

static int extract_ip(const char *text, const char *ip_version, char *out, size_t out_len) {
  char  buf[512];
  char *p;
  int   family;

  if (!text || !out || out_len == 0) {
    return -1;
  }

  copy_string(buf, sizeof(buf), text);
  trim_inplace(buf);

  family = ip_family_from_version(ip_version);

  /*
   * 最常见情况：响应体本身就是一个 IP
   */
  if ((family == 0 || family == 4) && is_ipv4(buf)) {
    snprintf(out, out_len, "%s", buf);
    return 0;
  }

  if ((family == 0 || family == 6) && is_ipv6(buf)) {
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

    if ((family == 0 || family == 4) && is_ipv4(token)) {
      copy_string(out, out_len, token);
      return 0;
    }

    if ((family == 0 || family == 6) && is_ipv6(token)) {
      copy_string(out, out_len, token);
      return 0;
    }
  }

  return -1;
}

static void build_url(const char *url, char *buf, size_t buf_len) {
  if (!buf || buf_len == 0) {
    return;
  }

  if (!url || url[0] == '\0') {
    url = TUMGRD_DEFAULT_IP_CHECK_URL;
  }

  if (strstr(url, "://")) {
    copy_string(buf, buf_len, url);
  } else {
    snprintf(buf, buf_len, "http://%s", url);
  }
}

static int exec_capture(char *const argv[], char *out, size_t out_len) {
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

/*
 * 通过连接外部 IPv6 主机获取本机 WAN IPv6 地址
 * 原理：connect() 后 getsockname() 返回的是内核选路后的源地址
 * 这个地址是能出网的全球单播地址，而非 fe80::/10 等链路本地地址
 */
static int detect_ipv6_wan_by_connect(const char *host, int port, char *out, size_t out_len) {
  struct addrinfo         hints, *res, *rp;
  int                     sockfd = -1;
  int                     err;
  struct sockaddr_storage local_addr;
  socklen_t               addr_len = sizeof(local_addr);
  void                   *addr_ptr = NULL;
  const char             *addr_str = NULL;
  char                    port_str[6];
  int                     gai_ret;

  if (!out || out_len == 0) {
    return -1;
  }

  out[0] = '\0';

  if (!host || host[0] == '\0') {
    host = "ipv6.baidu.com";
  }
  if (port <= 0 || port > 65535) {
    port = 80;
  }

  snprintf(port_str, sizeof(port_str), "%d", port);

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET6;    /* 强制 IPv6 */
  hints.ai_socktype = SOCK_STREAM; /* TCP */

  gai_ret = getaddrinfo(host, port_str, &hints, &res);
  if (gai_ret != 0) {
    log_error("[ipdetect] getaddrinfo failed for %s:%d: %s", host, port, gai_strerror(gai_ret));
    err = -1;
    goto err_cleanup;
  }

  /* 尝试连接第一个可用的地址 */
  for (rp = res; rp != NULL; rp = rp->ai_next) {
    sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sockfd < 0) {
      continue;
    }

    /* 非阻塞连接也可以，但这里用阻塞更简单 */
    if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break; /* 连接成功 */
    }

    close(sockfd);
    sockfd = -1;
  }

  if (sockfd < 0) {
    err = -1;
    log_error("[ipdetect] failed to connect to %s:%d", host, port);
    goto err_cleanup;
  }

  try2(getsockname(sockfd, (struct sockaddr *) &local_addr, &addr_len), "[ipdetect] getsockname failed");

  if (local_addr.ss_family != AF_INET6) {
    log_error("[ipdetect] unexpected address family: %d", (int) local_addr.ss_family);
    err = -1;
    goto err_cleanup;
  }

  struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) &local_addr;
  addr_ptr                  = &sin6->sin6_addr;

  addr_str = inet_ntop(AF_INET6, addr_ptr, out, out_len);
  if (!addr_str) {
    log_error("[ipdetect] inet_ntop failed");
    err = -1;
    goto err_cleanup;
  }

  /* 过滤掉链路本地地址（理论上不应该出现，但做个保险） */
  if (strncasecmp(out, "fe80:", 5) == 0 || strncasecmp(out, "fe80::", 6) == 0) {
    log_error("[ipdetect] got link-local address %s, not a WAN address", out);
    err = -1;
    goto err_cleanup;
  }

  err = 0;

err_cleanup:
  if (sockfd >= 0) {
    close(sockfd);
    sockfd = -1;
  }
  if (res) {
    freeaddrinfo(res);
  }

  return err;
}

int detect_public_ip(const char *url, const char *ip_version, char *out, size_t out_len) {
  char final_url[256];
  int  err;

  if (!out || out_len == 0) {
    return -1;
  }

  out[0] = '\0';
  build_url(url, final_url, sizeof(final_url));

  /*
   * 如果是 IPv6 请求且没有指定外部检测 URL，
   * 优先使用本地连接探测方法（无需外部 HTTP 服务）
   */
  int family = ip_family_from_version(ip_version);
  if (family == 6 && (!url || url[0] == '\0' || streq(url, TUMGRD_DEFAULT_IP_CHECK_URL))) {
    err = detect_ipv6_wan_by_connect("ipv6.baidu.com", 80, out, out_len);
    if (err == 0) {
      return 0;
    }
    log_info("[ipdetect] IPv6 connect method failed, fallback to HTTP check");
  }

  {
    char buf[512];

    const char *bins[] = {
      "uclient-fetch",
      "wget",
    };

    char *argv[] = {NULL, "-qO-", (char *) final_url, NULL};

    for (int i = 0; i < 2; i++) {
      buf[0]  = '\0';
      argv[0] = (char *) bins[i];

      err = exec_capture(argv, buf, sizeof(buf));
      if (err == 0 && extract_ip(buf, ip_version, out, out_len) == 0) {
        return 0;
      }
    }
  }

  log_error("[ipdetect] failed url=%s ip_version=%s", final_url, ip_version ? ip_version : "");
  return -1;
}

// vim: set sw=2 ts=2 et:
