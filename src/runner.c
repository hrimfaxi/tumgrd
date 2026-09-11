#include "runner.h"
#include "helper.h"
#include "log.h"
#include "try.h"
#include "tumgrd.h"

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
#include <time.h>
#include <unistd.h>

/* glibc 仅在 _DEFAULT_SOURCE 下声明 environ，musl 无条件声明；显式 extern 保证两边一致 */
extern char **environ;

#define TUMGRD_TUCTL_CLIENT_BIN "tuctl_client"
#define TUMGRD_KTUCTL_BIN       "ktuctl"
#define TUMGRD_MEMLIMIT_ENV     "TUTUICMPTUNNEL_PWHASH_MEMLIMIT"
#define TUMGRD_MEMLIMIT_ENV_EQ  TUMGRD_MEMLIMIT_ENV "="

/*
 * 子进程 deadline。
 *
 * exec_with_stdio() 整段（spawn + 写 stdin + 读 stdout/stderr + waitpid）超过
 * TUMGRD_SUBPROC_TIMEOUT_S 还没结束，说明 tuctl_client/ktuctl 卡住了。单线程
 * daemon 此时正阻塞在 read()/write()/waitpid() 上，回不到 reconcile 代码，所以用
 * alarm 打断当前 syscall：handler 只置标志，被中断的 syscall 返回 EINTR，普通控制流
 * 在检查点里 kill 掉子进程组并 reap，然后 exec_with_stdio() 返回 -1 → reconcile 把
 * 该节点标记为 ERROR，下一周期由 was_error 自愈重试。不做自杀：重启整个 daemon 会把
 * 爆炸半径从一个节点扩大到所有节点（且 procd 的 respawn_retry 用尽后会彻底停服务）。
 *
 * handler 里**不做 kill**，只置标志：在信号处理器里按 pid 发信号，只能用"可能已经被
 * reap、甚至已被内核复用"的 pid，任何"先 reap 再清 pid"的同步都无法消除 syscall 返回
 * 与进入 sigprocmask 之间的窗口。把 kill 放进普通控制流后，"发信号"与"reap 该 pid"
 * 永远在同一处、顺序明确 —— 已 reap 的路径只做超时分类、绝不再发信号。这样也就不需要
 * 用 sig_atomic_t 存 pid（它不保证有符号，-1 哨兵在无符号 ABI 上会变成 UINT_MAX）。
 *
 * 为什么用 alarm 而不是 poll：管道读没法用 SO_RCVTIMEO 限时（对 pipe 调
 * setsockopt 返回 ENOTSOCK），而 alarm 不关心进程卡在哪个 syscall 上。
 *
 * 已知覆盖不到的窗口：musl 的 posix_spawn 走 vfork（CLONE_VFORK），父进程等的是
 * wait_for_completion_killable —— 只有致命信号能打断，被 handler 捕获的 SIGALRM
 * 不行，且那时 handler 根本不会运行。若卡在这一段，只能等它自己返回（本地二进制
 * exec 是微秒级；真卡住说明文件系统有问题）。spawn 返回后的检查点仍能识别这期间
 * 到期的 deadline。
 *
 * 前提契约：exec_with_stdio() 对它 spawn 的子进程拥有**独占 waitpid 所有权** —— 从
 * posix_spawnp 成功返回到自己回收之间，不能让别处 reap 它，否则 watchdog_check(pid)
 * 有可能按一个已被内核复用的 pid/pgid 发信号。当前成立的依据：
 *   1. 这一段同步跑在同一个 uloop 回调内；libubox 的 uloop_handle_processes()
 *      （内部 waitpid(-1, WNOHANG)）只在 uloop_run 每轮迭代的开头执行，回调进行中
 *      不会运行；
 *   2. 本仓库的 waitpid 只有 runner.c 这两处（正常回收 + 看门狗超时回收），没有
 *      SIGCHLD handler，也没有 uloop_process_add；
 *   3. musl 的 posix_spawn 只在子进程 exec 失败时自己 reap，那种情况下它返回错误，
 *      我们不会再用这个 pid。
 * 若将来把子进程等待改成异步/延后（例如 uloop_process_add、把 reap 挪到下一轮
 * 事件循环），这条契约即失效，必须重新审视 watchdog_check(pid) 的安全性。
 */
static volatile sig_atomic_t g_watchdog_timed_out = 0;
static bool                  g_watchdog_usable    = false;

static void watchdog_alarm_handler(int signo) {
  int saved_errno = errno;

  (void) signo;

  g_watchdog_timed_out = 1;

  /* alarm 是一次性的：重新武装，保证之后每个阻塞 syscall 都能被打断并回到检查点，
   * 否则"deadline 已到期、后面还有阻塞读"仍会永久卡住 */
  alarm(TUMGRD_SUBPROC_TIMEOUT_RETRY_S);

  errno = saved_errno;
}

/* SIGALRM/alarm() 是进程级全局资源，本模块独占：别处不要再装 SIGALRM handler 或调用
 * alarm()，否则会互相覆盖。main.c 只装了 SIGINT/SIGTERM/SIGPIPE，libubox 只用
 * SIGINT/SIGTERM/SIGCHLD/SIGPIPE，因此当前无冲突；保留原有 disposition 也不必要。
 *
 * 同一独占性的另一半是"屏蔽"：别处不要长期屏蔽 SIGALRM。arm/disarm 只用 sigprocmask
 * 保护自己的状态切换、并原样恢复调用方的 mask，所以调用方若把它屏蔽着调进 runner，
 * 定时器照跑而 handler 永不执行，disarm 还会把那个 pending 信号当垃圾丢掉 ——
 * deadline 静默失效且零日志。exec 期间 SIGALRM 必须可投递，这是默认状态。 */
void tumgrd_runner_install_watchdog(void) {
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = watchdog_alarm_handler;
  sigemptyset(&sa.sa_mask);
  /* 必须显式 sigaction + sa_flags = 0，不能用 signal()：
   * - musl 的 signal() 硬设 SA_RESTART（src/signal/signal.c），被中断的 read()/
   *   waitpid() 会被内核自动重启，deadline 形同虚设（实测：SA_RESTART 下阻塞 read
   *   一直不返回，sa_flags=0 时返回 -1/EINTR）；
   * - glibc 的 signal() 语义随 feature-test macro 变：实测 gnu11 下是 SA_RESTART，
   *   -D_POSIX_C_SOURCE 下是 SysV 语义（含 SA_RESETHAND）—— 后者会在第一次投递后把
   *   handler 重置为默认动作，下一次超时会直接杀掉 daemon。
   * 信号屏蔽由 watchdog_arm/disarm 自己管，不需要 SA_NODEFER。 */
  sa.sa_flags = 0;
  if (sigaction(SIGALRM, &sa, NULL) < 0) {
    /* 装不上就绝不 arm：否则 alarm() 到期会走默认动作，反而把 daemon 杀掉 */
    log_error("[runner] install SIGALRM watchdog failed: %s (subprocess deadline disabled)", strerror(errno));
    g_watchdog_usable = false;
    return;
  }
  g_watchdog_usable = true;
}

/* 屏蔽 SIGALRM：保护 watchdog 状态切换 */
static void watchdog_mask_signal(sigset_t *old) {
  sigset_t set;

  sigemptyset(&set);
  sigaddset(&set, SIGALRM);
  sigprocmask(SIG_BLOCK, &set, old);
}

/* 丢弃已产生但未投递的 SIGALRM。alarm(0) 只取消定时器，撤不掉已经 pending 的信号，
 * 否则它会在下一轮 invocation 里把 g_watchdog_timed_out 误置为 1，让那个子进程被白杀。 */
static void watchdog_discard_pending(void) {
  sigset_t        set;
  struct timespec zero = {0, 0};

  sigemptyset(&set);
  sigaddset(&set, SIGALRM);
  while (sigtimedwait(&set, NULL, &zero) >= 0) {
  }
}

/* 开始计时（覆盖 spawn → 写 stdin → 读两个管道 → waitpid）*/
static void watchdog_arm(void) {
  sigset_t old;

  watchdog_mask_signal(&old);
  alarm(0);
  watchdog_discard_pending();
  g_watchdog_timed_out = 0;
  if (g_watchdog_usable) {
    alarm(TUMGRD_SUBPROC_TIMEOUT_S);
  }
  sigprocmask(SIG_SETMASK, &old, NULL);
}

/* 停止计时。故意不清 g_watchdog_timed_out：调用方要靠它区分"超时"和"普通失败" */
static void watchdog_disarm(void) {
  sigset_t old;

  watchdog_mask_signal(&old);
  alarm(0);
  watchdog_discard_pending();
  sigprocmask(SIG_SETMASK, &old, NULL);
}

/* 检查点：deadline 到期则强杀子进程组并 reap，返回 -1 让调用方 try2 → err_cleanup。
 * pid 必须是"尚未 reap"的子进程 pid（子进程是组长，pid == pgid，kill(-pid) 连后代一起收）；
 * 已经 reap 过就传 -1：只做超时分类，绝不再对可能被复用的 pid 发信号。 */
static int watchdog_check(pid_t pid) {
  if (!g_watchdog_timed_out) {
    return 0;
  }

  if (pid > 0) {
    (void) kill(-pid, SIGKILL);

    while (waitpid(pid, NULL, 0) < 0) {
      if (errno == EINTR) {
        /* 又被 alarm 打断 = 进程组没收掉（理论上仅当 SETPGROUP 没生效，pgid 不存在）。
         * 补一枪直接打子进程：能走到这里说明 waitpid 没报 ECHILD，子进程还存在
         * （存活或僵尸，僵尸会直接返回），因此 pid 不可能已被内核复用，发信号是安全的。
         * 不加这一枪，这个循环就会每 5s 被 EINTR 一次而永远转下去 —— 看门狗自己成了挂死点 */
        (void) kill(pid, SIGKILL);
        continue;
      }
      /* ECHILD = child 被别处 reap（违反上面的独占契约），其它错误也值得暴露出来 */
      log_warn("[runner] waitpid(%ld) after watchdog kill failed: %s", (long) pid, strerror(errno));
      break;
    }
  }

  return -1;
}

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

/*
 * 读到 EOF / 缓冲区满 / 出错为止。
 *
 * 本函数对 EINTR 的处理是 runner 看门狗的生存前提，改之前先看文件开头那段说明：
 * deadline 到期时正是靠 read() 返回 EINTR 才走出这里，再由调用方的检查点收子进程。
 * 因此"超时就立刻放弃"必须显式保留 —— 若改成教科书式的 EINTR 无条件重试，卡住的
 * 子进程会把父进程永久锁在本函数里（handler 每 5s 重装一次 alarm，read 每次都返回
 * EINTR 再被重试），检查点永远到不了，看门狗静默失效且没有任何日志。
 */
static int read_all_fd(int fd, char *buf, size_t buf_len) {
  ssize_t nread = 0;
  size_t  used  = 0;
  int     err   = 0;

  if (!buf || buf_len == 0) {
    return -1;
  }

  buf[0] = '\0';

  for (;;) {
    nread = read(fd, buf + used, buf_len - 1 - used);

    if (nread == 0) {
      break; /* EOF：子进程关掉了写端 */
    }

    if (nread < 0) {
      if (errno == EINTR && !g_watchdog_timed_out) {
        continue; /* 与 deadline 无关的中断：重试 */
      }
      err = -1; /* deadline 到期（或真错误）：放弃，交给检查点杀进程组 */
      break;
    }

    used += (size_t) nread;
    if (used >= buf_len - 1) {
      break; /* 缓冲区满 */
    }
  }

  buf[used] = '\0';

  return err;
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

  /* 开始计时：覆盖 spawn、写 stdin、读两个管道、waitpid 的全过程。
   * 此时子进程还不存在，若 deadline 在此之前到期，handler 只置标志（无进程可杀），
   * spawn 返回后的检查点会识别到。 */
  watchdog_arm();

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
  /* 子进程独立成一个进程组（pgroup=0 ⇒ pgid 取子进程 pid）：超时时 kill(-pid) 连后代一起收，
   * 避免后代继承 stdout/stderr 管道后父进程永远读不到 EOF */
  try2(-posix_spawnattr_setpgroup(&attr, 0), "setpgroup failed: %s", strret);
  try2(-posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP), "setflags failed: %s", strret);

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

  /* spawn 期间到期的 deadline 在这里补判（那时还没有 pid 可杀） */
  try2(watchdog_check(pid), "[runner] subprocess %s exceeded %d s deadline during spawn, killed", argv[0],
       TUMGRD_SUBPROC_TIMEOUT_S);

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
      ssize_t n;

      /* deadline 到期就不要再进 write：管道写满时内核先塞满 64KB 才在信号上收手，
       * 那次 write 返回的是已写字节数（正数）而不是 EINTR，光看 errno 发现不了超时，
       * 要等 handler 重装的 alarm 再响一次才被打断 —— 白等一个 retry 间隔。
       * 实测（host harness，deadline 2s / retry 4s，1MB stdin + 不读 stdin 的子进程）：
       * 没有这一句时整个 exec 花 6000ms 才返回，加上之后 2001ms。 */
      if (g_watchdog_timed_out) {
        break;
      }

      n = write(stdin_pipe[1], stdin_data + written, total - written);
      if (n < 0) {
        if (errno == EINTR && !g_watchdog_timed_out) {
          continue;
        }
        break;
      }
      written += (size_t) n;
    }
  }
  close(stdin_pipe[1]);
  stdin_pipe[1] = -1;

  /* stdin 阶段到期（子进程不读 stdin，把管道写满）就在这里收掉进程组：后面的 drain
   * 才能立刻拿到 EOF，不必再等一次 retry alarm。写循环因 EPIPE 退出时 flag 为 0，
   * 这里是空操作。 */
  try2(watchdog_check(pid), "[runner] subprocess %s exceeded %d s deadline (stdin), killed", argv[0], TUMGRD_SUBPROC_TIMEOUT_S);

  if (stdout_buf && stdout_buf_len > 0) {
    if (read_all_fd(stdout_pipe[0], stdout_buf, stdout_buf_len) != 0) {
      log_warn("[runner] failed to read stdout from %s: %s", argv[0], strerror(errno));
    }
  }
  close(stdout_pipe[0]);
  stdout_pipe[0] = -1;

  /* 到期就立刻收子进程组：后续 drain 才拿得到 EOF，不必再等一次 retry alarm */
  try2(watchdog_check(pid), "[runner] subprocess %s exceeded %d s deadline, killed", argv[0], TUMGRD_SUBPROC_TIMEOUT_S);

  if (stderr_buf && stderr_buf_len > 0) {
    if (read_all_fd(stderr_pipe[0], stderr_buf, stderr_buf_len) != 0) {
      log_warn("[runner] failed to read stderr from %s: %s", argv[0], strerror(errno));
    }
  }
  close(stderr_pipe[0]);
  stderr_pipe[0] = -1;

  try2(watchdog_check(pid), "[runner] subprocess %s exceeded %d s deadline, killed", argv[0], TUMGRD_SUBPROC_TIMEOUT_S);

  for (;;) {
    pid_t waited = waitpid(pid, &status, 0);
    if (waited == pid) {
      break;
    }
    if (waited < 0) {
      if (errno == EINTR) {
        /* deadline 到期时不能吞掉：否则会被当成普通失败甚至成功。
         * 此处 pid 尚未 reap，可以安全地杀进程组并回收 */
        if (g_watchdog_timed_out) {
          (void) watchdog_check(pid);
          err_cleanup(-1, "[runner] subprocess %s exceeded %d s deadline (waitpid)", argv[0], TUMGRD_SUBPROC_TIMEOUT_S);
        }
        continue;
      }
      /* ECHILD = 已被别处 reap（违反上面的独占契约，值得留日志）；其余错误状态不明，
       * 一律失败返回（不再对 pid 发信号）*/
      if (errno == ECHILD) {
        log_warn("[runner] child %ld was reaped elsewhere, watchdog ownership violated", (long) pid);
      }
      err = -1;
      goto err_cleanup;
    }
  }

  /* 子进程已回收（可能正是被看门狗 SIGKILL 的）：超时必须归类为超时。
   * 传 -1：pid 已不存在，绝不能再对它（可能被复用）发信号 */
  try2(watchdog_check(-1), "[runner] subprocess %s exceeded %d s deadline", argv[0], TUMGRD_SUBPROC_TIMEOUT_S);

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    err = -1;
    goto err_cleanup;
  }

  err = 0;

err_cleanup:
  /* 下面 disarm 之后定时器已取消、pending 已丢弃，就不会再有人置位 flag：清 flag 放在它
   * 之后才是安全的，放在它之前等于和刚好到期的 deadline 赛跑。 */
  watchdog_disarm();

  /* 成功路径不能把残留的超时标志带出去：deadline 可能恰好在最后一次检查与 disarm
   * 之间到期，此时 err 已是 0，而 reset_local_client 这类调用方只看标志、不看返回值，
   * 会把这次成功误判成超时。失败路径的标志要故意留着（调用方靠它区分超时与普通失败） */
  if (err == 0) {
    g_watchdog_timed_out = 0;
  }

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

    /* 子进程超时不等于"服务端不支持 @client_ip@"，别再花一个 deadline 重试 */
    if (g_watchdog_timed_out) {
      TUMGRD_FREE(script);
      err_cleanup(-1, "[runner] subprocess timed out, skipping detected-ip fallback uid=%s", node->uid);
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
  // client-del 失败忽略；但子进程超时说明它卡死了，不再继续 client-add
  (void) tumgrd_runner_client_del(node);
  if (g_watchdog_timed_out) {
    return -1;
  }
  return tumgrd_runner_client_add(node);
}

// vim: set sw=2 ts=2 et:
