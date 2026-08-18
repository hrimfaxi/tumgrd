#include "runner.h"
#include "helper.h"
#include "log.h"
#include "try.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* glibc 仅在 _DEFAULT_SOURCE 下声明 environ，musl 无条件声明；显式 extern 保证两边一致 */
extern char **environ;

#define TUMGRD_TUCTL_CLIENT_BIN "tuctl_client"
#define TUMGRD_KTUCTL_BIN       "ktuctl"
#define TUMGRD_MEMLIMIT_ENV     "TUTUICMPTUNNEL_PWHASH_MEMLIMIT"
#define TUMGRD_MEMLIMIT_ENV_EQ  TUMGRD_MEMLIMIT_ENV "="

static void sanitize_comment(const char *src, char *dst, size_t dst_len) {
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
  trim_inplace(dst);
}

static const char *ip_version_flag(const char *ip_version) {
  if (!ip_version || ip_version[0] == '\0') {
    return NULL;
  }

  if (streqcase(ip_version, "4") || streqcase(ip_version, "ipv4") || streqcase(ip_version, "-4")) {
    return "-4";
  }

  if (streqcase(ip_version, "6") || streqcase(ip_version, "ipv6") || streqcase(ip_version, "-6")) {
    return "-6";
  }

  return NULL;
}

static int read_all_fd(int fd, char *buf, size_t buf_len) {
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

static void log_argv(const char *tag, char *const argv[]) {
  char buf[2048] = {0};
  int  off       = 0;
  int  i;
  int  sensitive_next = 0;

  if (!argv)
    return;

  off = snprintf(buf, sizeof(buf), "[runner] %s exec:", tag ? tag : "cmd");
  for (i = 0; argv[i] && off < (int) sizeof(buf) - 32; i++) {
    if (sensitive_next) {
      off += snprintf(buf + off, sizeof(buf) - off, " ****");
      sensitive_next = 0;
    } else {
      off += snprintf(buf + off, sizeof(buf) - off, " %s", argv[i]);
    }
    if (strcasecmp(argv[i], "psk") == 0 || strcasecmp(argv[i], "xor") == 0) {
      sensitive_next = 1;
    }
  }
  log_debug("%s", buf);
}

static int set_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);

  if (flags < 0) {
    return -1;
  }

  return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* 父进程标准 fd 被关闭时 pipe() 可能返回 0/1/2，adddup2(fd, fd) 会退化；
 * 把管道端提升到 >=3 并加 FD_CLOEXEC，避免依赖具体 libc 对等 fd 的处理。
 * F_DUPFD_CLOEXEC 自 Linux 2.6.24 起可用，musl/glibc 均提供该 fcntl 命令。 */
static int prepare_pipe_end(int *fd) {
  int newfd;

  if (*fd < STDERR_FILENO + 1) {
    newfd = fcntl(*fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (newfd < 0) {
      return -1;
    }
    close(*fd);
    *fd = newfd;
    return 0;
  }

  return set_cloexec(*fd);
}

static int exec_with_stdio(char *const argv[], const char *stdin_data, int has_memlimit, int memlimit, char *stdout_buf,
                           size_t stdout_buf_len, char *stderr_buf, size_t stderr_buf_len) {
  int                        stdin_pipe[2]  = {-1, -1};
  int                        stdout_pipe[2] = {-1, -1};
  int                        stderr_pipe[2] = {-1, -1};
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t          attr;
  sigset_t                   sigdef;
  pid_t                      pid = -1;
  int                        status;
  int                        spawn_err;
  int                        envp_count = 0;
  int                        i;
  char                     **envp_storage = NULL;
  char                      *memlimit_env = NULL;
  char                     **envp         = environ;
  char                       memlimit_str[64];
  bool                       fa_ready   = false;
  bool                       attr_ready = false;
  int                        err        = -1;

  if (!argv || !argv[0]) {
    return -1;
  }

  if (stdout_buf && stdout_buf_len > 0) {
    stdout_buf[0] = '\0';
  }
  if (stderr_buf && stderr_buf_len > 0) {
    stderr_buf[0] = '\0';
  }

  /* 管道端提升到 >=3 且带 FD_CLOEXEC。POSIX 规定 file action 按加入顺序执行，
   * 之后才关闭仍带 FD_CLOEXEC 的 fd：adddup2 先复制到 0/1/2（新 fd 清除
   * FD_CLOEXEC），原始管道端随后在 exec 时自动关闭，因此无需 addclose。 */
  try2(pipe(stdin_pipe), "pipe stdin failed: %s", strerrno);
  try2(prepare_pipe_end(&stdin_pipe[0]), "prepare stdin_pipe[0] failed: %s", strerrno);
  try2(prepare_pipe_end(&stdin_pipe[1]), "prepare stdin_pipe[1] failed: %s", strerrno);
  try2(pipe(stdout_pipe), "pipe stdout failed: %s", strerrno);
  try2(prepare_pipe_end(&stdout_pipe[0]), "prepare stdout_pipe[0] failed: %s", strerrno);
  try2(prepare_pipe_end(&stdout_pipe[1]), "prepare stdout_pipe[1] failed: %s", strerrno);
  try2(pipe(stderr_pipe), "pipe stderr failed: %s", strerrno);
  try2(prepare_pipe_end(&stderr_pipe[0]), "prepare stderr_pipe[0] failed: %s", strerrno);
  try2(prepare_pipe_end(&stderr_pipe[1]), "prepare stderr_pipe[1] failed: %s", strerrno);

  /* posix_spawn 系列失败返回正错误码，取负匹配 try2 的 <0 约定 */
  try2(-posix_spawn_file_actions_init(&fa), "posix_spawn_file_actions_init failed: %s", strret);
  fa_ready = true;
  try2(-posix_spawn_file_actions_adddup2(&fa, stdin_pipe[0], STDIN_FILENO), "adddup2 stdin failed: %s", strret);
  try2(-posix_spawn_file_actions_adddup2(&fa, stdout_pipe[1], STDOUT_FILENO), "adddup2 stdout failed: %s", strret);
  try2(-posix_spawn_file_actions_adddup2(&fa, stderr_pipe[1], STDERR_FILENO), "adddup2 stderr failed: %s", strret);

  try2(-posix_spawnattr_init(&attr), "posix_spawnattr_init failed: %s", strret);
  attr_ready = true;
  sigemptyset(&sigdef);
  sigaddset(&sigdef, SIGPIPE);
  /* 父进程忽略 SIGPIPE，exec 不重置被忽略的信号；用 SETSIGDEF 让子进程恢复默认行为 */
  try2(-posix_spawnattr_setsigdefault(&attr, &sigdef), "setsigdefault failed: %s", strret);
  try2(-posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF), "setflags failed: %s", strret);

  /* posix_spawn 没有子进程代码可跑，环境变量需显式构造（父进程环境 + memlimit）。
   * 过滤父环境中已有的同名变量，等价于旧的 setenv(..., overwrite=1)。 */
  if (has_memlimit) {
    size_t prefix_len = strlen(TUMGRD_MEMLIMIT_ENV_EQ);
    int    out        = 0;

    while (environ[envp_count]) {
      envp_count++;
    }
    envp_storage = calloc((size_t) envp_count + 2, sizeof(char *));
    try2_p(envp_storage, "calloc envp failed");
    for (i = 0; i < envp_count; i++) {
      if (strncmp(environ[i], TUMGRD_MEMLIMIT_ENV_EQ, prefix_len) != 0) {
        envp_storage[out++] = environ[i];
      }
    }
    snprintf(memlimit_str, sizeof(memlimit_str), TUMGRD_MEMLIMIT_ENV_EQ "%u", (unsigned int) memlimit);
    memlimit_env = strdup(memlimit_str);
    try2_p(memlimit_env, "strdup memlimit env failed");
    envp_storage[out++] = memlimit_env;
    envp_storage[out]   = NULL;
    envp                = envp_storage;
  }

  spawn_err = posix_spawnp(&pid, argv[0], &fa, &attr, argv, envp);
  if (spawn_err != 0) {
    err_cleanup(-1, "posix_spawnp %s failed: %s", argv[0], strerror(spawn_err));
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  stdin_pipe[0]  = -1;
  stdout_pipe[1] = -1;
  stderr_pipe[1] = -1;

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
  stdin_pipe[1] = -1;

  if (stdout_buf && stdout_buf_len > 0) {
    if (read_all_fd(stdout_pipe[0], stdout_buf, stdout_buf_len) != 0) {
      log_warn("[runner] failed to read stdout from %s: %s", argv[0], strerror(errno));
    }
  }
  close(stdout_pipe[0]);
  stdout_pipe[0] = -1;

  if (stderr_buf && stderr_buf_len > 0) {
    if (read_all_fd(stderr_pipe[0], stderr_buf, stderr_buf_len) != 0) {
      log_warn("[runner] failed to read stderr from %s: %s", argv[0], strerror(errno));
    }
  }
  close(stderr_pipe[0]);
  stderr_pipe[0] = -1;

  for (;;) {
    pid_t waited = waitpid(pid, &status, 0);
    if (waited == pid) {
      break;
    }
    if (waited < 0 && errno != EINTR) {
      err = -1;
      goto err_cleanup;
    }
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    err = -1;
    goto err_cleanup;
  }

  err = 0;

err_cleanup:
  if (fa_ready) {
    posix_spawn_file_actions_destroy(&fa);
  }
  if (attr_ready) {
    posix_spawnattr_destroy(&attr);
  }
  TUMGRD_FREE(memlimit_env);
  TUMGRD_FREE(envp_storage);
  if (stdin_pipe[0] >= 0) {
    close(stdin_pipe[0]);
  }
  if (stdin_pipe[1] >= 0) {
    close(stdin_pipe[1]);
  }
  if (stdout_pipe[0] >= 0) {
    close(stdout_pipe[0]);
  }
  if (stdout_pipe[1] >= 0) {
    close(stdout_pipe[1]);
  }
  if (stderr_pipe[0] >= 0) {
    close(stderr_pipe[0]);
  }
  if (stderr_pipe[1] >= 0) {
    close(stderr_pipe[1]);
  }
  return err;
}

static int run_tuctl_script(const struct tumgrd_node *node, const char *script, const char *success_marker) {
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

  ip_flag = ip_version_flag(node->ip_version);
  snprintf(server_port, sizeof(server_port), "%d", node->server_port);

  argv[argc++] = TUMGRD_TUCTL_CLIENT_BIN;

  if (ip_flag) {
    argv[argc++] = (char *) ip_flag;
  }

  argv[argc++] = "max-retries";
  argv[argc++] = "1";
  argv[argc++] = "psk";
  argv[argc++] = (char *) node->psk;
  argv[argc++] = "server";
  argv[argc++] = (char *) node->server_host;
  argv[argc++] = "server-port";
  argv[argc++] = server_port;
  argv[argc++] = "script";
  argv[argc++] = "-";
  argv[argc]   = NULL;

  log_argv("tuctl_client", argv);

  rc = exec_with_stdio(argv, script, node->has_memlimit, node->memlimit, stdout_buf, sizeof(stdout_buf), stderr_buf,
                       sizeof(stderr_buf));

  trim_inplace(stdout_buf);
  trim_inplace(stderr_buf);
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

static int run_ktuctl(char *const argv[]) {
  char stdout_buf[1024];
  char stderr_buf[1024];
  int  rc;

  log_argv("ktuctl", argv);

  rc = exec_with_stdio(argv, NULL, 0, 0, stdout_buf, sizeof(stdout_buf), stderr_buf, sizeof(stderr_buf));

  trim_inplace(stdout_buf);
  trim_inplace(stderr_buf);
  log_info("[runner] ktuctl rc=%d stdout=%s stderr=%s", rc, stdout_buf, stderr_buf);

  return rc;
}

int tumgrd_runner_server_add(const struct tumgrd_node *node, const struct tumgrd_config *cfg, const char *current_ip) {
  if (!node || !current_ip || current_ip[0] == '\0') {
    return -1;
  }

  int   err                 = -1;
  char *script              = NULL;
  char  comment[256]        = {0};
  char  comment_suffix[280] = {0};
  char  xor_suffix[140]     = {0};

  sanitize_comment(node->client_comment, comment, sizeof(comment));
  if (comment[0] != '\0') {
    snprintf(comment_suffix, sizeof(comment_suffix), " comment %s", comment);
  }

  if (node->xor_key[0] != '\0') {
    snprintf(xor_suffix, sizeof(xor_suffix), " xor %s", node->xor_key);
  }

  if (cfg && cfg->use_client_ip) {
    /* 先尝试 @client_ip@ 占位符，由服务端解析客户端地址 */
    try2(asprintf(&script, "server-add uid %s port %d address @client_ip@%s%s\n", node->uid, node->client_port, xor_suffix,
                  comment_suffix),
         "asprintf");
    log_trimmed("[runner] server add stdin (client_ip placeholder)", script);
    if (run_tuctl_script(node, script, "server updated:") == 0) {
      TUMGRD_FREE(script);
      return 0;
    }

    /* 回退：使用检测到的实际 IP（兼容旧版 tuctl_server） */
    log_info("[runner] @client_ip@ not supported, falling back to detected ip=%s uid=%s", current_ip, node->uid);
    TUMGRD_FREE(script);
  }

  try2(asprintf(&script, "server-add uid %s port %d address %s%s%s\n", node->uid, node->client_port, current_ip, xor_suffix,
                comment_suffix),
       "asprintf");
  log_trimmed("[runner] server add stdin (detected ip)", script);
  try2(run_tuctl_script(node, script, "server updated:"));
  err = 0;

err_cleanup:
  TUMGRD_FREE(script);
  return err;
}

int tumgrd_runner_server_del(const struct tumgrd_node *node) {
  if (!node) {
    return -1;
  }

  int   err    = -1;
  char *script = NULL;

  try2(asprintf(&script, "server-del uid %s\n", node->uid), "asprintf");
  log_trimmed("[runner] server del stdin", script);
  try2(run_tuctl_script(node, script, "server deleted:"));
  err = 0;

err_cleanup:
  TUMGRD_FREE(script);
  return err;
}

int tumgrd_runner_client_add(const struct tumgrd_node *node) {
  char        client_port[16] = {};
  char       *argv[16];
  int         argc = 0;
  const char *ip_flag;

  if (!node) {
    return -1;
  }

  ip_flag = ip_version_flag(node->ip_version);
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

  if (node->xor_key[0] != '\0') {
    argv[argc++] = "xor";
    argv[argc++] = (char *) node->xor_key;
  }

  argv[argc] = NULL;

  return run_ktuctl(argv);
}

int tumgrd_runner_client_del(const struct tumgrd_node *node) {
  char       *argv[16];
  int         argc = 0;
  const char *ip_flag;

  if (!node) {
    return -1;
  }

  ip_flag = ip_version_flag(node->ip_version);

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

  return run_ktuctl(argv);
}

int tumgrd_runner_reset_local_client(const struct tumgrd_node *node) {
  // client-del 失败忽略
  (void) tumgrd_runner_client_del(node);
  return tumgrd_runner_client_add(node);
}

// vim: set sw=2 ts=2 et:
