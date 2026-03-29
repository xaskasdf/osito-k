/*
 * OsitoK WASM — Type definitions (Emscripten-compatible)
 *
 * Thin redirect: re-exports arch/x86/include/types.h, which now contains
 * #ifdef __EMSCRIPTEN__ guards that replace all x86 asm with WASM no-ops.
 *
 * Having a separate WASM types.h in the -I path ensures that files resolved
 * via the include-path (e.g. common headers) land here first, then fall
 * through to x86/include/types.h — preventing double-definition conflicts.
 */

#ifndef OSITOK_WASM_TYPES_H
#define OSITOK_WASM_TYPES_H

#include "../../x86/include/types.h"

#endif /* OSITOK_WASM_TYPES_H */
