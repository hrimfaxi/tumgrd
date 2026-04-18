#include "db.h"
#include "log.h"
#include "reconcile.h"
#include "try.h"
#include "tumgrd.h"
#include "ubus_if.h"

#include <libubox/uloop.h>
#include <libubus.h>

#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TUMGRD_STARTUP_RECONCILE_DELAY_MS 3000
#define DEFAULT_SOCKET_PATH               NULL
#define DEFAULT_CLIENT_BIN                "tuctl_client"
#define DEFAULT_LOG_LEVEL                 "info"
#define DEFAULT_INTERVAL                  60

static void tumgrd_startup_reconcile_timer_cb(struct uloop_timeout *t);
static void tumgrd_periodic_reconcile_timer_cb(struct uloop_timeout *t);

static struct tumgrd_ctx    g_ctx;
static struct uloop_timeout g_startup_reconcile_timer = {
  .cb = tumgrd_startup_reconcile_timer_cb,
};

static struct uloop_timeout g_periodic_reconcile_timer = {
  .cb = tumgrd_periodic_reconcile_timer_cb,
};

static void tumgrd_config_init(struct tumgrd_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->db_path      = TUMGRD_DB_PATH;
  cfg->socket_path  = DEFAULT_SOCKET_PATH;
  cfg->client_bin   = DEFAULT_CLIENT_BIN;
  cfg->log_level    = DEFAULT_LOG_LEVEL;
  cfg->interval_sec = DEFAULT_INTERVAL;
}

static void usage(FILE *out, const char *prog) {
  fprintf(out,
          "Usage: %s [options]\n"
          "\n"
          "Options:\n"
          "  -d, --database PATH      sqlite database path (default: %s)\n"
          "  -i, --interval SEC       monitor interval seconds (10-3600) (default: %d)\n"
          "  -s, --socket PATH        unix socket path (default: %s)\n"
          "      --client-bin PATH    path to tuctl_client binary (default: %s)\n"
          "      --log-level LEVEL    log level: debug|info|warn|error (default: %s)\n"
          "  -h, --help               show this help\n",
          prog, TUMGRD_DB_PATH, DEFAULT_INTERVAL, nonempty_or_default(DEFAULT_SOCKET_PATH, "null"), DEFAULT_CLIENT_BIN,
          DEFAULT_LOG_LEVEL);
}

static int parse_interval(const char *s, int *out) {
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

static int parse_args(int argc, char **argv, struct tumgrd_config *cfg) {
  int c;
  int err;

  static const struct option long_opts[] = {{"database", required_argument, NULL, 'd'},
                                            {"interval", required_argument, NULL, 'i'},
                                            {"socket", required_argument, NULL, 's'},
                                            {"client-bin", required_argument, NULL, 1},
                                            {"log-level", required_argument, NULL, 2},
                                            {"help", no_argument, NULL, 'h'},
                                            {0, 0, 0, 0}};

  while ((c = getopt_long(argc, argv, "d:i:s:h", long_opts, NULL)) != -1) {
    switch (c) {
    case 'd':
      cfg->db_path = optarg;
      break;
    case 'i':
      try2(parse_interval(optarg, &cfg->interval_sec), "invalid interval: %s", optarg);
      break;
    case 's':
      cfg->socket_path = optarg;
      break;
    case 1:
      cfg->client_bin = optarg;
      break;
    case 2:
      cfg->log_level = optarg;
      break;
    case 'h':
      usage(stdout, argv[0]);
      err = 1;
      goto err_cleanup;
    default:
      goto usage;
    }
  }

  if (optind < argc) {
    log_error("unexpected positional argument: %s", argv[optind]);
  usage:
    usage(stderr, argv[0]);
    err = -1;
    goto err_cleanup;
  }

  err = 0;
err_cleanup:
  return err;
}

static void tumgrd_startup_reconcile_timer_cb(struct uloop_timeout *t) {
  (void) t;
  (void) tumgrd_reconcile_all(&g_ctx.db, true);
}

static void tumgrd_periodic_reconcile_timer_cb(struct uloop_timeout *t) {
  (void) tumgrd_reconcile_all(&g_ctx.db, false);
  uloop_timeout_set(t, g_ctx.cfg.interval_sec * 1000);
}

static void tumgrd_signal_handler(int signo) {
  (void) signo;
  uloop_end();
}

static int parse_log_level(const char *s, int *out) {
  if (!s || !out || s[0] == '\0') {
    return -1;
  }

  if (strcasecmp(s, "error") == 0) {
    *out = LOG_ERROR;
    return 0;
  }
  if (strcasecmp(s, "warn") == 0) {
    *out = LOG_WARN;
    return 0;
  }
  if (strcasecmp(s, "info") == 0) {
    *out = LOG_INFO;
    return 0;
  }
  if (strcasecmp(s, "debug") == 0) {
    *out = LOG_DEBUG;
    return 0;
  }
  if (strcasecmp(s, "trace") == 0) {
    *out = LOG_TRACE;
    return 0;
  }

  return -1;
}

int main(int argc, char **argv) {
  int  err       = -1;
  bool db_opened = false;

  tumgrd_config_init(&g_ctx.cfg);

  err = parse_args(argc, argv, &g_ctx.cfg);
  if (err > 0) {
    return 0;
  }
  try2(err, "[main] parse_args failed");
  try2(parse_log_level(g_ctx.cfg.log_level, &log_verbosity),
       "invalid log level: %s (expected: error, warn, info, debug, trace)", nonempty_or_default(g_ctx.cfg.log_level, "(null)"));

  try2(uloop_init(), "[main] uloop_init failed");

  signal(SIGINT, tumgrd_signal_handler);
  signal(SIGTERM, tumgrd_signal_handler);

  try2(tumgrd_db_open(&g_ctx.db, g_ctx.cfg.db_path), "[main] open db failed: %s", g_ctx.cfg.db_path);
  db_opened = true;
  try2(tumgrd_db_init_schema(&g_ctx.db), "[main] init schema failed");
  try2(tumgrd_ubus_init(&g_ctx), "[main] tumgrd_ubus_init failed");

  uloop_timeout_set(&g_startup_reconcile_timer, TUMGRD_STARTUP_RECONCILE_DELAY_MS);
  uloop_timeout_set(&g_periodic_reconcile_timer, g_ctx.cfg.interval_sec * 1000);
  log_info("[main] starting tumgrd: db=%s socket=%s interval=%d log_level=%s client_bin=%s",
           nonempty_or_default(g_ctx.cfg.db_path, "(null)"), nonempty_or_default(g_ctx.cfg.socket_path, "(default)"),
           g_ctx.cfg.interval_sec, nonempty_or_default(g_ctx.cfg.log_level, "(null)"),
           nonempty_or_default(g_ctx.cfg.client_bin, "(null)"));

  uloop_run();

  err = 0;

err_cleanup:
  tumgrd_ubus_cleanup(&g_ctx);

  uloop_timeout_cancel(&g_startup_reconcile_timer);
  uloop_timeout_cancel(&g_periodic_reconcile_timer);

  if (db_opened) {
    tumgrd_db_close(&g_ctx.db);
    db_opened = false;
  }

  uloop_done();
  return err;
}

// vim: set sw=2 ts=2 et:
