# OsitoFS v2 — Filesystem Specification

## Overview

OsitoFS v2 is a write-once, contiguous-block filesystem optimized for NVMe DMA
and AI model storage. Designed for bare-metal environments with no OS overhead.

- **Block size**: 1 MB (1,048,576 bytes) — optimal for NVMe sequential I/O
- **Metadata overhead**: 4 MB (blocks 0-3) regardless of device size
- **Max files**: 4,096
- **Max blocks**: 262,144 (256 TB theoretical)
- **Write-once**: Files are immutable after creation

## On-Disk Layout

```
Block 0: Superblock (512 bytes used, rest zero-padded)
Block 1: File Table (4096 entries × 256 bytes = 1 MB)
Block 2: Block CRC Table (262144 × uint32 = 1 MB)
Block 3: Layer Index Table (512 slots × 2048 bytes = 1 MB)
Block 4..N: Data blocks (contiguous, first-fit allocation)
```

## Superblock (Block 0)

512 bytes, packed. Rest of block 0 is zero.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | magic | `0x4F534632` ("OSF2") |
| 0x04 | 4 | version | `2` |
| 0x08 | 4 | block_size | `1048576` (1 MB) |
| 0x0C | 4 | total_blocks | Total blocks on device |
| 0x10 | 4 | used_blocks | Blocks in use |
| 0x14 | 4 | file_count | Number of valid files |
| 0x18 | 4 | next_data_block | Next free data block |
| 0x1C | 16 | uuid | Filesystem UUID (v4) |
| 0x2C | 32 | label | Human-readable label (null-terminated) |
| 0x4C | 8 | create_time | Unix timestamp |
| 0x54 | 4 | crc32 | CRC32 of superblock (field zeroed for calc) |
| 0x58 | 424 | reserved | Zero-padded to 512 bytes |

## File Table Entry (Block 1)

4096 entries × 256 bytes each = exactly 1 MB block.

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 64 | name | Filename (null-terminated) |
| 0x40 | 8 | size | File size in bytes |
| 0x48 | 4 | start_block | First data block number |
| 0x4C | 4 | block_count | Number of contiguous blocks |
| 0x50 | 4 | crc32 | CRC32 of entire file data |
| 0x54 | 4 | flags | `VALID=1`, `GGUF=2`, `RAW=4` |
| 0x58 | 4 | quant_type | Quantization type (if GGUF) |
| 0x5C | 4 | num_layers | Transformer layer count |
| 0x60 | 4 | hidden_size | Hidden dimension |
| 0x64 | 4 | vocab_size | Vocabulary size |
| 0x68 | 4 | head_count | Attention heads |
| 0x6C | 4 | kv_head_count | KV heads (GQA) |
| 0x70 | 4 | context_length | Max context length |
| 0x74 | 128 | model_name | Model identifier string |
| 0xF4 | 2 | layer_index_slot | Layer Index slot (0xFFFF = none) |
| 0xF6 | 10 | reserved | Zero-padded to 256 bytes |

## Block CRC Table (Block 2)

262,144 × uint32_t = exactly 1 MB. One CRC32 per data block.

## Layer Index Table (Block 3)

512 slots × 2048 bytes = exactly 1 MB.

Each slot:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | num_layers | Number of layers |
| 0x04 | 4 | reserved | - |
| 0x08 | 2040 | layer_offset[255] | Byte offset from file start to each layer |

## CRC32

IEEE 802.3 polynomial `0xEDB88320` (reflected). Same as zlib/gzip.
The `osfs2_crc32()` function is defined in the shared header.

## Quantization Types

| Value | Name | Description |
|-------|------|-------------|
| 0 | NONE | Unknown/unset |
| 1 | F32 | 32-bit float |
| 2 | F16 | 16-bit float |
| 3 | Q8_0 | 8-bit quantization |
| 4 | Q4_0 | 4-bit quantization |
| 5 | Q4_1 | 4-bit quantization v2 |
| 6 | Q5_0 | 5-bit quantization |
| 7 | Q5_1 | 5-bit quantization v2 |
| 8 | Q2_K | 2-bit K-quant |
| 9 | Q3_K | 3-bit K-quant |
| 10 | Q4_K | 4-bit K-quant |
| 11 | Q5_K | 5-bit K-quant |
| 12 | Q6_K | 6-bit K-quant |
| 13 | IQ2_XXS | Importance Q 2-bit |
| 14 | IQ3_XXS | Importance Q 3-bit |

## Host Tools

Built from `tools/ositofs/`:

- **mkfs.ositofs** `<device> [--label name]` — Format with OsitoFS v2
- **ositofs-write** `<device> <file> [--name name]` — Write file (auto-detects GGUF)
- **ositofs-ls** `<device>` — List files with model info
- **ositofs-info** `<device>` — Show filesystem info

All tools use `O_DIRECT` for block-aligned I/O on raw devices.
