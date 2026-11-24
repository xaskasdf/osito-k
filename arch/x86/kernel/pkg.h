/*
 * OsitoK x86-64 — Native package manager (`pkg`)
 *
 * Fetches binaries on-demand over HTTPS from naranjositos.tech, verifies
 * SHA-256 against a catalog, installs by kind (bin / lib / sysroot), and runs
 * them. Mirrors the WASM pkg manager + osito-a lazy package load.
 *
 * Catalog: https://wasm.naranjositos.tech/k/x86_64/pkg/catalog.json
 */
#ifndef OSITOK_PKG_H
#define OSITOK_PKG_H

#include "../include/types.h"

/* Output sink — the shell passes its sh_puts so pkg output reaches the
 * terminal (and honors redirection). pkg.c stays decoupled from the shell. */
typedef void (*pkg_out_fn)(const char *s);

/* `pkg <subcommand> [args]`. Returns 0 on success, -1 on error. */
int pkg_cmd(int argc, char **argv, pkg_out_fn out);

/* Lazy-load hook for the shell's unknown-command path (shell.c).
 * If `cmd` resolves to a kind:bin package in the locally-cached catalog,
 * auto-install (with deps) and exec it with the original argv.
 * Returns 1 if it handled the command (the package ran), 0 if `cmd` is not a
 * known package (caller should print "Unknown command"). Never touches the
 * network unless a cached catalog already names `cmd`. */
int pkg_lazy_try(const char *cmd, int argc, char **argv, pkg_out_fn out);

#endif /* OSITOK_PKG_H */
