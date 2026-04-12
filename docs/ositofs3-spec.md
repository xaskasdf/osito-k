# OsitoFS v3 Specification

## Overview
OsitoFS v3 is the next-generation native filesystem for OsitoK, designed to overcome the limitations of the contiguous, flat layout of v2. It introduces a modern architecture based on inodes and extents, enabling full POSIX-compliant hierarchical directories, sparse files, dynamic growth, and symbolic links, all while maintaining high performance for NVMe DMA transfers.

## Key Features
- **Hierarchical Directories**: True tree structure `/usr/bin/gcc` instead of flat names.
- **Extent-Based Allocation**: Files are composed of extents (start block + count), reducing fragmentation and allowing files to grow.
- **Inodes**: Metadata separation from directory entries (dentries).
- **Symlinks**: First-class support for symbolic links.
- **VFS Translation Layer**: Seamless path homogenization depending on process personality (POSIX, Win32, DOS).

## Disk Layout (1MB Block Size)
- **Block 0**: Superblock (Metadata about the entire filesystem)
- **Block 1**: Inode Bitmap (Tracks free/used inodes)
- **Block 2**: Block Bitmap (Tracks free/used data blocks)
- **Block 3..K**: Inode Table (Array of `osfs3_inode_t`)
- **Block K+1..N**: Data Blocks (Directory contents and File data)

## Data Structures

### Superblock
Contains magic number, version, block size, total blocks, inode counts, free counts, UUID, and the root inode number.

### Inodes (`osfs3_inode_t`)
Represents a filesystem object (file, directory, symlink).
- `mode`: File type and permissions (e.g., S_IFDIR, S_IFREG, S_IFLNK).
- `size`: Size in bytes.
- `nlink`: Number of hard links.
- `uid`, `gid`: Ownership.
- `atime`, `mtime`, `ctime`: Timestamps.
- `extents`: Array of `osfs3_extent_t` pointing to data blocks.
- `flags`: OSFS3 specific flags.

### Extents (`osfs3_extent_t`)
Defines a contiguous run of blocks.
- `start_block`: Starting logical block number.
- `block_count`: Number of contiguous blocks in this extent.

### Directory Entries (`osfs3_dentry_t`)
Stored inside the data blocks of a directory inode.
- `inode`: Target inode number.
- `name_len`: Length of the name.
- `type`: File type (DT_DIR, DT_REG, etc.).
- `name`: Variable length name string.

## VFS Integration
The Virtual File System (VFS) layer abstracts the underlying filesystem.
- `namei()`: The core path resolution function, translating string paths to inodes, handling symlinks and mount points.
- **Homogenization**:
  - `POSIX`: Strict case-sensitive `/a/b/c`.
  - `Win32`: Translates `C:\a\b` to `/a/b` and performs case-insensitive lookups.
  - `DOS`: Similar to Win32, potentially with 8.3 truncation if required by legacy APIs.
