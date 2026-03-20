/*
 * OsitoK x86-64 — Minimal zlib (DEFLATE inflate + deflate)
 *
 * For git object compression/decompression.
 * Inflate: full DEFLATE (stored, fixed, dynamic Huffman).
 * Deflate: fixed Huffman with LZ77 hash chain.
 */

#ifndef OSITOK_ZLIB_H
#define OSITOK_ZLIB_H

#include "../include/types.h"

/* Compress src into dst (zlib format: header + deflate + adler32).
 * dst must be pre-allocated (worst case: src_len + src_len/1000 + 32).
 * On success, *dst_len is set to compressed size. Returns 0 or -1. */
int zlib_deflate(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst, uint32_t *dst_len);

/* Decompress zlib-format src into dst.
 * *dst_len is input: max output size; output: actual decompressed size.
 * Returns 0 on success, -1 on format error, -2 on overflow, -3 on checksum. */
int zlib_inflate(const uint8_t *src, uint32_t src_len,
                 uint8_t *dst, uint32_t *dst_len);

/* Raw adler32 checksum */
uint32_t zlib_adler32(const uint8_t *data, uint32_t len);

#endif /* OSITOK_ZLIB_H */
