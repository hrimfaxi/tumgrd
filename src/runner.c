#include "runner.h"
#include "helper.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TUMGRD_TUCTL_CLIENT_BIN "tuctl_client"
#define TUMGRD_KTUCTL_BIN       "ktuctl"

static void tumgrd_copy_string(char *dst, size_t dst_len, const char *src) {
  if (!dst || dst_len == 0) {
    return;
  }

  if (!src) {
    dst[0] = '\0';
    return;
  }

  snprintf(dst, dst_len, "%s", src);
}

static void tumgrd_trim_inplace(char *s) {
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

static void tumgrd_sanitize_comment(const char *src, char *dst, size_t dst_len) {
  size_t i;
  size_t j = 0;

  if (!dst || dst_len == 0) {
    return;
  }

  if (!src) {
    dst[0] = '\0';
    return;
  }

  for (i = 0; src[i] != '\0' && j + 1 < dst_len; i++) {
    char c = src[i];
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
    dst[j++] = c;
  }

  dst[j] = '\0';
  tumgrd_trim_inplace(dst);
}

static const char *tumgrd_ip_version_flag(const char *ip_version) {
  if (!ip_version || ip_version[0] == '\0') {
    return NULL;
  }

  if (streq(ip_version, "4") || streq(ip_version, "ipv4") || streq(ip_version, "-4")) {
    return "-4";
  }

  if (streq(ip_version, "6") || streq(ip_version, "ipv6") || streq(ip_version, "-6")) {
    return "-6";
  }

  return NULL;
}

static int tumgrd_read_all_fd(int fd, char *buf, size_t buf_len) {
  ssize_t nread;
  size_t  used = 0;

  if (!buf || buf_len == 0) {
    return -1;
  }

  buf[0] = '\0';

  while ((nread = read(fd, buf + used, buf_len - 1 - used)) > 0) {
    used += (size_t) nread;
    if (used >= buf_len - 1) {
      break;
    }
  }

  buf[used] = '\0';

  if (nread < 0) {
    return -1;
  }

  return 0;
}

static void tumgrd_log_argv(const char *tag, char *const argv[]) {
  int i;

  fprintf(stderr, "[runner] %s exec:", tag ? tag : "cmd");
  for (i = 0; argv && argv[i]; i++) {
    fprintf(stderr, " %s", argv[i]);
  }
  fprintf(stderr, "\n");
}

static int tumgrd_exec_with_stdio(char *const argv[], const char *stdin_data, int has_memlimit, int memlimit, char *stdout_buf,
                                  size_t stdout_buf_len, char *stderr_buf, size_t stderr_buf_len) {
  int   stdin_pipe[2]  = {-1, -1};
  int   stdout_pipe[2] = {-1, -1};
  int   stderr_pipe[2] = {-1, -1};
  pid_t pid;
  int   status;
  char  memlimit_env[64];

  if (!argv || !argv[0]) {
    return -1;
  }

  if (stdout_buf && stdout_buf_len > 0) {
    stdout_buf[0] = '\0';
  }
  if (stderr_buf && stderr_buf_len > 0) {
    stderr_buf[0] = '\0';
  }

  if (pipe(stdin_pipe) != 0) {
    return -1;
  }

  if (pipe(stdout_pipe) != 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    return -1;
  }

  if (pipe(stderr_pipe) != 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    return -1;
  }

  if (pid == 0) {
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    if (dup2(stdin_pipe[0], STDIN_FILENO) < 0) {
      _exit(127);
    }
    if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    if (dup2(stderr_pipe[1], STDERR_FILENO) < 0) {
      _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    if (has_memlimit) {
      snprintf(memlimit_env, sizeof(memlimit_env), "%d", memlimit);
      setenv("TUTUICMPTUNNEL_PWHASH_MEMLIMIT", memlimit_env, 1);
    }

    execvp(argv[0], argv);
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  if (stdin_data && stdin_data[0] != '\0') {
    size_t total   = strlen(stdin_data);
    size_t written = 0;

    while (written < total) {
      ssize_t n = write(stdin_pipe[1], stdin_data + written, total - written);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      written += (size_t) n;
    }
  }
  close(stdin_pipe[1]);

  if (stdout_buf && stdout_buf_len > 0) {
    tumgrd_read_all_fd(stdout_pipe[0], stdout_buf, stdout_buf_len);
  }
  close(stdout_pipe[0]);

  if (stderr_buf && stderr_buf_len > 0) {
    tumgrd_read_all_fd(stderr_pipe[0], stderr_buf, stderr_buf_len);
  }
  close(stderr_pipe[0]);

  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return -1;
  }

  return 0;
}

static int tumgrd_run_tuctl_script(const struct tumgrd_node *node, const char *script, const char *success_marker) {
  char        server_port[16];
  char       *argv[16];
  int         argc = 0;
  const char *ip_flag;
  char        stdout_buf[2048];
  char        stderr_buf[2048];
  int         rc;

  if (!node || !script) {
    return -1;
  }

  ip_flag = tumgrd_ip_version_flag(node->ip_version);
  snprintf(server_port, sizeof(server_port), "%d", node->server_port);

  argv[argc++] = TUMGRD_TUCTL_CLIENT_BIN;

  if (ip_flag) {
    argv[argc++] = (char *) ip_flag;
  }

  argv[argc++] = "max-retries";
  argv[argc++] = "5";
  argv[argc++] = "psk";
  argv[argc++] = (char *) node->psk;
  argv[argc++] = "server";
  argv[argc++] = (char *) node->server_host;
  argv[argc++] = "server-port";
  argv[argc++] = server_port;
  argv[argc++] = "script";
  argv[argc++] = "-";
  argv[argc]   = NULL;

  tumgrd_log_argv("tuctl_client", argv);

  rc = tumgrd_exec_with_stdio(argv, script, node->has_memlimit, node->memlimit, stdout_buf, sizeof(stdout_buf), stderr_buf,
                              sizeof(stderr_buf));

  tumgrd_trim_inplace(stdout_buf);
  tumgrd_trim_inplace(stderr_buf);
  log_info("[runner] tuctl_client uid=%s server=%s:%d client_port=%d rc=%d stdout=%s stderr=%s", node->uid, node->server_host,
           node->server_port, node->client_port, rc, stdout_buf, stderr_buf);

  if (rc != 0) {
    return -1;
  }

  if (success_marker && success_marker[0] != '\0') {
    if (!strstr(stdout_buf, success_marker) && !strstr(stderr_buf, success_marker)) {
      log_error("[runner] success marker missing: marker=%s", success_marker);
      return -1;
    }
  }

  return 0;
}

static int tumgrd_run_ktuctl(char *const argv[]) {
  char stdout_buf[1024];
  char stderr_buf[1024];
  int  rc;

  tumgrd_log_argv("ktuctl", argv);

  rc = tumgrd_exec_with_stdio(argv, NULL, 0, 0, stdout_buf, sizeof(stdout_buf), stderr_buf, sizeof(stderr_buf));

  tumgrd_trim_inplace(stdout_buf);
  tumgrd_trim_inplace(stderr_buf);
  log_info("[runner] ktuctl rc=%d stdout=%s stderr=%s", rc, stdout_buf, stderr_buf);

  return rc;
}

int tumgrd_runner_server_add(const struct tumgrd_node *node, const char *current_ip) {
  char comment[256];
  char script[1024];

  if (!node || !current_ip || current_ip[0] == '\0') {
    return -1;
  }

  tumgrd_sanitize_comment(node->client_comment, comment, sizeof(comment));

  if (comment[0] != '\0') {
    snprintf(script, sizeof(script), "server-add uid %s port %d address %s comment %s\n", node->uid, node->client_port,
             current_ip, comment);
  } else {
    snprintf(script, sizeof(script), "server-add uid %s port %d address %s\n", node->uid, node->client_port, current_ip);
  }

  {
    char *temp = strdup(script);
    if (temp) {
      tumgrd_trim_inplace(temp);
      log_info("[runner] server add stdin: %s", temp);
    }
    free(temp);
  }
  return tumgrd_run_tuctl_script(node, script, "server updated:");
}

int tumgrd_runner_server_del(const struct tumgrd_node *node) {
  char script[512];

  if (!node) {
    return -1;
  }

  snprintf(script, sizeof(script), "server-del uid %s\n", node->uid);

  {
    char *temp = strdup(script);
    if (temp) {
      tumgrd_trim_inplace(temp);
      log_info("[runner] server del stdin: %s", temp);
    }
    free(temp);
  }

  return tumgrd_run_tuctl_script(node, script, "server deleted:");
}

int tumgrd_runner_client_add(const struct tumgrd_node *node) {
  char        client_port[16];
  char       *argv[16];
  int         argc = 0;
  const char *ip_flag;

  if (!node) {
    return -1;
  }

  ip_flag = tumgrd_ip_version_flag(node->ip_version);
  snprintf(client_port, sizeof(client_port), "%d", node->client_port);

  argv[argc++] = TUMGRD_KTUCTL_BIN;
  if (ip_flag) {
    argv[argc++] = (char *) ip_flag;
  }
  argv[argc++] = "client-add";
  argv[argc++] = "address";
  argv[argc++] = (char *) node->server_host;
  argv[argc++] = "port";
  argv[argc++] = client_port;
  argv[argc++] = "uid";
  argv[argc++] = (char *) node->uid;

  if (node->description[0] != '\0') {
    argv[argc++] = "comment";
    argv[argc++] = (char *) node->description;
  }

  argv[argc] = NULL;

  return tumgrd_run_ktuctl(argv);
}

int tumgrd_runner_client_del(const struct tumgrd_node *node) {
  char       *argv[16];
  int         argc = 0;
  const char *ip_flag;

  if (!node) {
    return -1;
  }

  ip_flag = tumgrd_ip_version_flag(node->ip_version);

  argv[argc++] = TUMGRD_KTUCTL_BIN;
  if (ip_flag) {
    argv[argc++] = (char *) ip_flag;
  }
  argv[argc++] = "client-del";
  argv[argc++] = "address";
  argv[argc++] = (char *) node->server_host;
  argv[argc++] = "uid";
  argv[argc++] = (char *) node->uid;
  argv[argc]   = NULL;

  return tumgrd_run_ktuctl(argv);
}

int tumgrd_runner_reset_local_client(const struct tumgrd_node *node) {
  // client-del 失败忽略
  (void) tumgrd_runner_client_del(node);
  return tumgrd_runner_client_add(node);
}

// vim: set sw=2 ts=2 et:
