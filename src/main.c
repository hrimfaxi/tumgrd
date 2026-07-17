#include "db.h"
#include "helper.h"
#include "ipdetect.h"
#include "log.h"
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

#define DEFAULT_SOCKET_PATH NULL
#define DEFAULT_LOG_LEVEL   "info"
#define DEFAULT_INTERVAL    60

static void config_init(struct tumgrd_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->db_path       = TUMGRD_DB_PATH;
  cfg->socket_path   = DEFAULT_SOCKET_PATH;
  cfg->log_level     = DEFAULT_LOG_LEVEL;
  cfg->interval_sec  = DEFAULT_INTERVAL;
  cfg->enable_xor    = false;
  cfg->use_client_ip = true;
  cfg->fwmark        = TUMGRD_IPDETECT_FWMARK;
}

static void usage(FILE *out, const char *prog) {
  fprintf(out,
          "Usage: %s [options]\n"
          "\n"
          "Options:\n"
          "  -d, --database PATH      sqlite database path (default: %s)\n"
          "  -i, --interval SEC       monitor interval seconds (10-3600) (default: %d)\n"
          "  -s, --socket PATH        unix socket path (default: %s)\n"
          "      --log-level LEVEL    log level: debug|info|warn|error (default: %s)\n"
          "      --enable-xor         enable automatic XOR key generation for new nodes\n"
          "      --disable-xor        disable automatic XOR key generation (default)\n"
          "      --use-client-ip      use @client_ip@ placeholder (default)\n"
          "      --no-client-ip       use detected IP instead of @client_ip@\n"
          "      --fwmark NUM         SO_MARK value for IP detection (0-255, default: %d)\n"
          "  -h, --help               show this help\n",
          prog, TUMGRD_DB_PATH, DEFAULT_INTERVAL, nonempty_or_default(DEFAULT_SOCKET_PATH, "null"), DEFAULT_LOG_LEVEL,
          TUMGRD_IPDETECT_FWMARK);
}

static int parse_args(int argc, char **argv, struct tumgrd_config *cfg) {
  int c;
  int err;

  static const struct option long_opts[] = {{"database", required_argument, NULL, 'd'},
                                            {"interval", required_argument, NULL, 'i'},
                                            {"socket", required_argument, NULL, 's'},
                                            {"log-level", required_argument, NULL, 1},
                                            {"help", no_argument, NULL, 'h'},
                                            {"enable-xor", no_argument, NULL, 2},
                                            {"disable-xor", no_argument, NULL, 3},
                                            {"use-client-ip", no_argument, NULL, 5},
                                            {"no-client-ip", no_argument, NULL, 6},
                                            {"fwmark", required_argument, NULL, 4},
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
      cfg->log_level = optarg;
      break;
    case 2:
      cfg->enable_xor = true;
      break;
    case 3:
      cfg->enable_xor = false;
      break;
    case 4: {
      char *endptr = NULL;
      long  val    = strtol(optarg, &endptr, 10);
      if (!endptr || *endptr || endptr == optarg || val < 0 || val > 255) {
        log_error("invalid fwmark: %s (must be 0-255)", optarg);
        err = -1;
        goto err_cleanup;
      }
      cfg->fwmark = (int) val;
    } break;
    case 5:
      cfg->use_client_ip = true;
      break;
    case 6:
      cfg->use_client_ip = false;
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

static void signal_handler(int signo) {
  (void) signo;
  uloop_end();
}

static int parse_log_level(const char *s, int *out) {
  if (!s || !out || s[0] == '\0') {
    return -1;
  }

  if (streqcase(s, "error")) {
    *out = LOG_ERROR;
    return 0;
  }
  if (streqcase(s, "warn")) {
    *out = LOG_WARN;
    return 0;
  }
  if (streqcase(s, "info")) {
    *out = LOG_INFO;
    return 0;
  }
  if (streqcase(s, "debug")) {
    *out = LOG_DEBUG;
    return 0;
  }
  if (streqcase(s, "trace")) {
    *out = LOG_TRACE;
    return 0;
  }

  return -1;
}

int main(int argc, char **argv) {
  int               err       = -1;
  bool              db_opened = false;
  struct tumgrd_ctx ctx       = {};

  config_init(&ctx.cfg);
  err = parse_args(argc, argv, &ctx.cfg);
  if (err > 0) {
    return 0;
  }
  try2(err, "[main] parse_args failed");
  try2(parse_log_level(ctx.cfg.log_level, &log_verbosity), "invalid log level: %s (expected: error, warn, info, debug, trace)",
       nonempty_or_default(ctx.cfg.log_level, "(null)"));

  try2(uloop_init(), "[main] uloop_init failed");
  set_ipdetect_fwmark(ctx.cfg.fwmark);

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGPIPE, SIG_IGN);

  try2(tumgrd_db_open(&ctx.db, ctx.cfg.db_path), "[main] open db failed: %s", ctx.cfg.db_path);
  db_opened = true;
  try2(tumgrd_db_init_schema(&ctx.db), "[main] init schema failed");
  try2(tumgrd_ubus_init(&ctx), "[main] tumgrd_ubus_init failed");

  log_info("[main] starting tumgrd: db=%s socket=%s interval=%d log_level=%s%s", nonempty_or_default(ctx.cfg.db_path, "(null)"),
           nonempty_or_default(ctx.cfg.socket_path, "(default)"), ctx.cfg.interval_sec,
           nonempty_or_default(ctx.cfg.log_level, "(null)"), ctx.cfg.enable_xor ? " xor-enabled" : "");

  uloop_run();

  err = 0;

err_cleanup:
  tumgrd_ubus_cleanup(&ctx);

  uloop_timeout_cancel(&ctx.startup_reconcile_timer);
  uloop_timeout_cancel(&ctx.periodic_reconcile_timer);

  if (db_opened) {
    tumgrd_db_close(&ctx.db);
    db_opened = false;
  }

  uloop_done();
  return err;
}

// vim: set sw=2 ts=2 et:
