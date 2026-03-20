/*
 * OsitoK x86-64 — Git-Compatible Version Control
 *
 * Standard git object model (SHA-1 + zlib) stored on OsitoFS.
 * Uses path-style filenames within OsitoFS flat namespace:
 *   .git/HEAD, .git/objects/ab/cdef..., .git/refs/heads/master
 *
 * All object formats are byte-compatible with standard git.
 */

#ifndef OSITOK_GIT_H
#define OSITOK_GIT_H

#include "../include/types.h"

/* SHA-1 hash (20 bytes binary, 40 chars hex) */
#define GIT_SHA1_RAW   20
#define GIT_SHA1_HEX   40

/* Object types */
#define GIT_OBJ_BLOB   3
#define GIT_OBJ_TREE   2
#define GIT_OBJ_COMMIT 1
#define GIT_OBJ_TAG    4

/* Index entry */
typedef struct {
    uint8_t  sha1[GIT_SHA1_RAW];
    uint32_t mode;
    char     name[64];
} git_index_entry_t;

/* Index (staging area) */
#define GIT_MAX_INDEX  256

typedef struct {
    git_index_entry_t entries[GIT_MAX_INDEX];
    int count;
} git_index_t;

/* ── Git commands (called from shell) ───────────────────────── */

/* git init — create .git/ structure */
int git_init(void);

/* git add <file> — stage a file */
int git_add(const char *filename);

/* git commit -m "message" — create commit from index */
int git_commit(const char *message);

/* git log — show commit history */
int git_log(void);

/* git status — show working tree status */
int git_status(void);

/* git diff — show unstaged changes */
int git_diff(void);

/* git branch [name] — list or create branch */
int git_branch(const char *name);

/* git checkout <branch> — switch branch */
int git_checkout(const char *branch);

/* ── Utility ────────────────────────────────────────────────── */

/* Convert 20-byte SHA-1 to 40-char hex string */
void git_sha1_to_hex(const uint8_t sha1[20], char hex[41]);

/* Convert 40-char hex string to 20-byte SHA-1 */
int git_hex_to_sha1(const char *hex, uint8_t sha1[20]);

#endif /* OSITOK_GIT_H */
