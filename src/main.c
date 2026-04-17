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
#define DEFAULT_LOG_FORMAT                "text"
#define DEFAULT_LOG_LEVEL                 "info"
#define DEFAULT_INTERVAL                  60

static struct tumgrd_ctx    g_ctx;
static struct ubus_context *g_ubus = NULL;
static struct uloop_timeout g_reconcile_timer;

static void tumgrd_config_init(struct tumgrd_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->db_path      = TUMGRD_DB_PATH;
  cfg->socket_path  = DEFAULT_SOCKET_PATH;
  cfg->client_bin   = DEFAULT_CLIENT_BIN;
  cfg->log_format   = DEFAULT_LOG_FORMAT;
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
          "      --log-format FMT     log format: text|json (default: %s)\n"
          "      --log-level LEVEL    log level: debug|info|warn|error (default: %s)\n"
          "  -h, --help               show this help\n",
          prog, TUMGRD_DB_PATH, DEFAULT_INTERVAL, DEFAULT_SOCKET_PATH == NULL ? "null" : DEFAULT_SOCKET_PATH,
          DEFAULT_CLIENT_BIN, DEFAULT_LOG_FORMAT, DEFAULT_LOG_LEVEL);
}

static bool parse_interval(const char *s, int *out) {
  char *end = NULL;
  long  v   = strtol(s, &end, 10);

  if (!s || s[0] == '\0' || !end || *end != '\0') {
    return false;
  }
  if (v < 10 || v > 3600) {
    return false;
  }

  *out = (int) v;
  return true;
}

static int parse_args(int argc, char **argv, struct tumgrd_config *cfg) {
  int c;

  static const struct option long_opts[] = {{"database", required_argument, NULL, 'd'},
                                            {"interval", required_argument, NULL, 'i'},
                                            {"socket", required_argument, NULL, 's'},
                                            {"client-bin", required_argument, NULL, 1},
                                            {"log-format", required_argument, NULL, 2},
                                            {"log-level", required_argument, NULL, 3},
                                            {"help", no_argument, NULL, 'h'},
                                            {0, 0, 0, 0}};

  while ((c = getopt_long(argc, argv, "d:i:s:h", long_opts, NULL)) != -1) {
    switch (c) {
    case 'd':
      cfg->db_path = optarg;
      break;
    case 'i':
      if (!parse_interval(optarg, &cfg->interval_sec)) {
        fprintf(stderr, "invalid interval: %s\n", optarg);
        return -1;
      }
      break;
    case 's':
      cfg->socket_path = optarg;
      break;
    case 1:
      cfg->client_bin = optarg;
      break;
    case 2:
      cfg->log_format = optarg;
      break;
    case 3:
      cfg->log_level = optarg;
      break;
    case 'h':
      usage(stdout, argv[0]);
      return 1;
    default:
      usage(stderr, argv[0]);
      return -1;
    }
  }

  if (optind < argc) {
    fprintf(stderr, "unexpected positional argument: %s\n", argv[optind]);
    usage(stderr, argv[0]);
    return -1;
  }

  return 0;
}

static void tumgrd_reconcile_timer_cb(struct uloop_timeout *t) {
  (void) t;
  (void) tumgrd_reconcile_all(&g_ctx.db, true);
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
  struct tumgrd_config cfg;
  int                  err       = -1;
  bool                 db_opened = false;

  tumgrd_config_init(&cfg);

  err = parse_args(argc, argv, &cfg);
  if (err > 0) {
    return 0;
  }
  try2(err, "[main] parse_args failed");

  try2(parse_log_level(cfg.log_level, &log_verbosity), "invalid log level: %s (expected: error, warn, info, debug, trace)\n",
       cfg.log_level ? cfg.log_level : "(null)");

  memset(&g_ctx, 0, sizeof(g_ctx));
  g_ctx.cfg = cfg;
  memset(&g_reconcile_timer, 0, sizeof(g_reconcile_timer));

  try2(uloop_init(), "[main] uloop_init failed");

  signal(SIGINT, tumgrd_signal_handler);
  signal(SIGTERM, tumgrd_signal_handler);

  try2(tumgrd_db_open(&g_ctx.db, cfg.db_path), "[main] open db failed: %s", cfg.db_path);
  db_opened = true;
  try2(tumgrd_db_init_schema(&g_ctx.db), "[main] init schema failed");

  g_ubus = try2_p(ubus_connect(cfg.socket_path), "[main] ubus_connect failed");
  ubus_add_uloop(g_ubus);

  try2(tumgrd_ubus_init(g_ubus, &g_ctx), "[main] tumgrd_ubus_init failed");

  g_reconcile_timer.cb = tumgrd_reconcile_timer_cb;
  uloop_timeout_set(&g_reconcile_timer, TUMGRD_STARTUP_RECONCILE_DELAY_MS);

  log_info("[main] starting tumgrd: db=%s socket=%s interval=%d log_level=%s log_format=%s client_bin=%s",
           cfg.db_path ? cfg.db_path : "(null)", cfg.socket_path ? cfg.socket_path : "(default)", cfg.interval_sec,
           cfg.log_level ? cfg.log_level : "(null)", cfg.log_format ? cfg.log_format : "(null)",
           cfg.client_bin ? cfg.client_bin : "(null)");

  uloop_run();

  err = 0;

err_cleanup:
  tumgrd_ubus_cleanup();
  if (g_ubus) {
    ubus_free(g_ubus);
    g_ubus = NULL;
  }

  uloop_timeout_cancel(&g_reconcile_timer);

  if (db_opened) {
    tumgrd_db_close(&g_ctx.db);
    db_opened = false;
  }

  uloop_done();
  return err;
}

// vim: set sw=2 ts=2 et:
