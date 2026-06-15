#include "ipdetect.h"
#include "helper.h"
#include "log.h"
#include "try.h"
#include "tumgrd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* 全局 SO_MARK 值，默认 2，由 main 通过 set_ipdetect_fwmark() 设置 */
static int g_ipdetect_fwmark = TUMGRD_IPDETECT_FWMARK;

void set_ipdetect_fwmark(int mark) {
  g_ipdetect_fwmark = mark;
}

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

/*
 * 通过连接外部 IPv6 主机获取本机 WAN IPv6 地址
 * 原理：connect() 后 getsockname() 返回的是内核选路后的源地址
 * 这个地址是能出网的全球单播地址，而非 fe80::/10 等链路本地地址
 */
static int detect_ipv6_wan_by_connect(const char *host, int port, char *out, size_t out_len) {
  struct addrinfo         hints, *res = NULL, *rp = NULL;
  int                     sockfd = -1;
  int                     err;
  struct sockaddr_storage local_addr;
  socklen_t               addr_len = sizeof(local_addr);
  void                   *addr_ptr = NULL;
  const char             *addr_str = NULL;
  char                    port_str[16];
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
    if (sockfd < 0)
      continue;

    /* 打上 fwmark 避免被透明代理劫持 */
    if (setsockopt(sockfd, SOL_SOCKET, SO_MARK, &g_ipdetect_fwmark, sizeof(g_ipdetect_fwmark)) < 0) {
      log_error("[ipdetect] SO_MARK failed (IPv6 connect): %s", strerror(errno));
    }

    // ---- 非阻塞 connect 实现超时 ----
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sockfd, rp->ai_addr, rp->ai_addrlen);
    if (ret < 0 && errno != EINPROGRESS) {
      close(sockfd);
      sockfd = -1;
      continue;
    }

    if (ret < 0 && errno == EINPROGRESS) {
      fd_set         wfds;
      struct timeval tv;

      FD_ZERO(&wfds);
      FD_SET(sockfd, &wfds);
      tv.tv_sec  = 3;
      tv.tv_usec = 0;

      ret = select(sockfd + 1, NULL, &wfds, NULL, &tv);
      if (ret <= 0) {
        close(sockfd);
        sockfd = -1;
        continue;
      }

      int       so_error;
      socklen_t len = sizeof(so_error);
      getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
      if (so_error != 0) {
        close(sockfd);
        sockfd = -1;
        continue;
      }
    }

    // 连接成功，恢复阻塞模式（可选）
    fcntl(sockfd, F_SETFL, flags);
    break;
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
  if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
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

/**
 * 解析 URL，提取 scheme、host、port、path。
 * 仅支持 http 协议，遇到 https 或其他 scheme 返回 -1。
 * 支持 IPv6 字面量 [addr] 格式。
 * 若 URL 为空，则返回 -1。
 */
static int parse_url_host_path(const char *url, char *host, size_t host_size, int *port, char *path, size_t path_size) {
  const char *p = url;
  const char *host_start, *host_end;
  const char *port_start = NULL;
  const char *path_start;
  size_t      host_len, path_len;
  long        port_num;

  if (!url || url[0] == '\0')
    return -1;

  /* 检查 scheme */
  if (strncmp(p, "http://", 7) == 0) {
    p += 7;
  } else if (strncmp(p, "https://", 8) == 0) {
    log_error("[ipdetect] HTTPS URL not supported: %s", url);
    return -1;
  }

  /* 处理 IPv6 字面量 */
  if (*p == '[') {
    host_start              = p + 1;
    const char *bracket_end = strchr(host_start, ']');
    if (!bracket_end) {
      log_error("[ipdetect] missing ']' in IPv6 URL: %s", url);
      return -1;
    }
    host_end = bracket_end;
    p        = bracket_end + 1;
  } else {
    host_start = p;
    host_end   = host_start;
    while (*host_end != '\0' && *host_end != '/' && *host_end != ':') {
      host_end++;
    }
    p = host_end;
  }

  host_len = (size_t) (host_end - host_start);
  if (host_len >= host_size) {
    log_error("[ipdetect] host too long in URL: %s", url);
    return -1;
  }
  if (host_len == 0) {
    log_error("[ipdetect] empty host in URL: %s", url);
    return -1;
  }
  memcpy(host, host_start, host_len);
  host[host_len] = '\0';

  /* 解析端口 */
  if (*p == ':') {
    p++;
    port_start = p;
    while (isdigit((unsigned char) *p))
      p++;
    if (p == port_start) {
      log_error("[ipdetect] invalid port in URL: %s", url);
      return -1;
    }
    port_num = strtol(port_start, NULL, 10);
    if (port_num < 1 || port_num > 65535) {
      log_error("[ipdetect] port out of range in URL: %s", url);
      return -1;
    }
    *port = (int) port_num;
  } else {
    *port = TUMGRD_DEFAULT_IP_CHECK_PORT;
  }

  /* 解析 path */
  if (*p == '/') {
    path_start = p;
  } else if (*p == '\0') {
    path_start = "/";
  } else {
    log_error("[ipdetect] unexpected char after host:port in URL: %s", url);
    return -1;
  }

  path_len = strlen(path_start);
  if (path_len >= path_size) {
    log_error("[ipdetect] path too long in URL: %s", url);
    return -1;
  }
  strcpy(path, path_start);

  return 0;
}

/**
 * 内部 HTTP GET 请求，打上 SO_MARK=2 绕过 xtp-rs。
 * 只支持 HTTP/1.0，端口由参数指定。
 * 会遍历 getaddrinfo 结果重试连接。
 * 读取响应直到 EOF 或缓冲区满，自动跳过 HTTP 头部，只保留 body。
 */
static int http_get_with_mark(const char *host, int port, const char *path, char *out, size_t out_len) {
  int             ret = -1, sock = -1;
  struct addrinfo hints, *res, *rp;
  char            service[8];
  char            request[512];
  char            response[4096] = {0};
  size_t          total          = 0;
  ssize_t         n;
  size_t          req_len, sent;

  if (!host || !path || !out || out_len == 0)
    return -1;

  snprintf(service, sizeof(service), "%d", port);

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(host, service, &hints, &res) != 0) {
    log_error("[ipdetect] getaddrinfo failed for %s:%d", host, port);
    return -1;
  }

  for (rp = res; rp != NULL; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock < 0)
      continue;

    if (setsockopt(sock, SOL_SOCKET, SO_MARK, &g_ipdetect_fwmark, sizeof(g_ipdetect_fwmark)) < 0) {
      log_error("[ipdetect] SO_MARK failed: %s (continuing without mark)", strerror(errno));
    }

    if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
      break;

    close(sock);
    sock = -1;
  }

  if (sock < 0) {
    log_error("[ipdetect] connect to %s:%d failed", host, port);
    freeaddrinfo(res);
    return -1;
  }
  freeaddrinfo(res);
  res = NULL;

  /* 构造 HTTP 请求 */
  int n_req = snprintf(request, sizeof(request),
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "User-Agent: Wget/1.21.4\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       path, host);
  if (n_req < 0 || (size_t) n_req >= sizeof(request)) {
    log_error("[ipdetect] HTTP request too long for %s:%d%s", host, port, path);
    goto out;
  }
  req_len = (size_t) n_req;

  /* 确保请求完整发送 */
  sent = 0;
  while (sent < req_len) {
    ssize_t m = send(sock, request + sent, req_len - sent, 0);
    if (m < 0) {
      if (errno == EINTR)
        continue;
      log_error("[ipdetect] send failed: %s", strerror(errno));
      goto out;
    }
    if (m == 0) {
      log_error("[ipdetect] send returned 0");
      goto out;
    }
    sent += (size_t) m;
  }

  /* 循环读取响应 */
  while (total < sizeof(response) - 1) {
    n = recv(sock, response + total, sizeof(response) - 1 - total, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      log_error("[ipdetect] recv failed: %s", strerror(errno));
      goto out;
    }
    if (n == 0)
      break;
    total += n;
  }
  response[total] = '\0';
  if (total == sizeof(response) - 1) {
    log_error("[ipdetect] response truncated (buffer full)");
  }

  /* 检查状态码 */
  if (strncmp(response, "HTTP/", 5) != 0) {
    log_error("[ipdetect] invalid HTTP response");
    goto out;
  }
  const char *status_line = response + 5;
  while (*status_line && *status_line != ' ')
    status_line++;
  while (*status_line == ' ')
    status_line++;
  if (strncmp(status_line, "200", 3) != 0) {
    log_error("[ipdetect] HTTP status not 200");
    goto out;
  }

  /* 寻找 header 结束 */
  char *body_start = strstr(response, "\r\n\r\n");
  if (!body_start) {
    body_start = strstr(response, "\n\n");
    if (body_start)
      body_start += 2;
    else {
      log_error("[ipdetect] no header end found");
      goto out;
    }
  } else {
    body_start += 4;
  }

  size_t body_len = total - (body_start - response);
  if (body_len == 0) {
    log_error("[ipdetect] empty body");
    goto out;
  }
  if (body_len >= out_len)
    body_len = out_len - 1;
  memcpy(out, body_start, body_len);
  out[body_len] = '\0';
  ret           = 0;

out:
  if (sock >= 0)
    close(sock);
  return ret;
}

int detect_public_ip(const char *url, const char *ip_version, char *out, size_t out_len) {
  int err;

  if (!out || out_len == 0) {
    return -1;
  }

  out[0] = '\0';

  /* 解析 URL，获取 host、port、path */
  char host[128] = TUMGRD_DEFAULT_IP_CHECK_HOST;
  int  port      = TUMGRD_DEFAULT_IP_CHECK_PORT;
  char path[256] = TUMGRD_DEFAULT_IP_CHECK_PATH;

  if (url && url[0] != '\0') {
    if (parse_url_host_path(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
      return -1; /* URL 非法则直接失败，不静默回退 */
  }

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

  /* 使用内部 HTTP 请求 */
  {
    char buf[512];
    err = http_get_with_mark(host, port, path, buf, sizeof(buf));
    if (err == 0 && extract_ip(buf, ip_version, out, out_len) == 0) {
      return 0;
    }
  }

  log_error("[ipdetect] failed host=%s port=%d path=%s ip_version=%s", host, port, path, ip_version ? ip_version : "");
  return -1;
}

// vim: set sw=2 ts=2 et:
