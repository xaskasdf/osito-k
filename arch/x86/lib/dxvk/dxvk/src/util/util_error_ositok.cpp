/* util_error_ositok.cpp -- OsitoK W5.1 DxvkError::abort_ositok impl.
 *
 * The upstream DxvkError class throws; on OsitoK we build with
 * -fno-exceptions and rewrite `throw` statements via a macro in
 * dxvk_include.h / dxbc_include.h to call this helper instead.
 *
 * Bound into libdxvk_util.a so every TU picking up util_error.h can
 * link against a single implementation. Forwards to exit(1) via the
 * OsitoK libc at runtime; treat it as equivalent to std::terminate.
 */

#include "util_error.h"

extern "C" void exit(int);
extern "C" int  puts(const char *);

namespace dxvk {

  [[noreturn]] void DxvkError::abort_ositok(const char *msg) {
    /* W5.1-fix: emit msg to serial before exiting so failed shader
     * translation isn't a silent crash. W5.2: throw rewrites now pass
     * the actual constructor argument, so `msg` is the real error text
     * (or a str::format() result via the std::string overload). */
    if (msg) puts(msg);
    exit(1);
    for (;;) { }  /* unreachable; placates [[noreturn]] under -O0 */
  }

  [[noreturn]] void DxvkError::abort_ositok(const std::string& msg) {
    abort_ositok(msg.c_str());
  }

  [[noreturn]] void DxvkError::abort_ositok() {
    abort_ositok("DXVK abort (no message)");
  }

}
