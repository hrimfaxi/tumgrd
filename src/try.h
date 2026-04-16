#pragma once

#include <errno.h>

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#define _get_macro(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

// Returns _ret while printing error.
#define ret(...)  _get_macro(_0, ##__VA_ARGS__, _ret_fmt, _ret_fmt, _ret_fmt, _ret_fmt, _ret, )(__VA_ARGS__)
#define _ret(ret) return (ret)
#define _ret_fmt(ret, ...)                                                                                                     \
  ({                                                                                                                           \
    log_error(__VA_ARGS__);                                                                                                    \
    return (ret);                                                                                                              \
  })

// Jumps to `err_cleanup`, returning _ret while printing error.
//
// Requires `err_cleanup` label, `err` to be defined inside function scope, and `err` to be
// returned after cleanup.
#define err_cleanup(...)                                                                                                       \
  _get_macro(_0, ##__VA_ARGS__, _cleanup_fmt, _cleanup_fmt, _cleanup_fmt, _cleanup_fmt, _cleanup, )(__VA_ARGS__)
#define _cleanup(ret)                                                                                                          \
  ({                                                                                                                           \
    err = (ret);                                                                                                               \
    goto err_cleanup;                                                                                                          \
  })
#define _cleanup_fmt(ret, ...)                                                                                                 \
  ({                                                                                                                           \
    log_error(__VA_ARGS__);                                                                                                    \
    err = (ret);                                                                                                               \
    goto err_cleanup;                                                                                                          \
  })

#define _get_macro(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

// Tests int return value from a function. Used for functions that returns non-zero error.
#define try(expr, ...)                                                                                                         \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret < 0))                                                                                                    \
      ret(_ret, ##__VA_ARGS__);                                                                                                \
    _ret;                                                                                                                      \
  })

// `try` but `err_cleanup`.
#define try2(expr, ...)                                                                                                        \
  ({                                                                                                                           \
    long _ret = (expr);                                                                                                        \
    if (unlikely(_ret < 0))                                                                                                    \
      err_cleanup(_ret, ##__VA_ARGS__);                                                                                        \
    _ret;                                                                                                                      \
  })

// Similar to `try2_e`, but for function that returns a pointer.
#define try2_p(expr, ...)                                                                                                      \
  ({                                                                                                                           \
    void *_ptr = (expr);                                                                                                       \
    if (unlikely(!_ptr)) {                                                                                                     \
      long _ret = -ENOMEM;                                                                                                     \
      err_cleanup(_ret, ##__VA_ARGS__);                                                                                        \
    }                                                                                                                          \
    _ptr;                                                                                                                      \
  })

#define strret   strerror(-_ret)
#define strerrno strerror(errno)

// vim: set sw=2 ts=2 expandtab:
