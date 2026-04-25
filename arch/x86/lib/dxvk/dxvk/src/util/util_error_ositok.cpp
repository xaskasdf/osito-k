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
     * translation isn't a silent crash. Caller passes a fixed string
     * "DXVK throw" today; future waves can pass actual error text. */
    if (msg) puts(msg);
    exit(1);
    for (;;) { }  /* unreachable; placates [[noreturn]] under -O0 */
  }

}
