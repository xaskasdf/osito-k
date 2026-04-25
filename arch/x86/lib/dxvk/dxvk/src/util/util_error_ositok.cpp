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

namespace dxvk {

  [[noreturn]] void DxvkError::abort_ositok(const char *msg) {
    (void)msg;
    exit(1);
    for (;;) { }  /* unreachable; placates [[noreturn]] under -O0 */
  }

}
