# OsitoK — Filesystem Support Roadmap

## Estado actual

| Filesystem | Modo | Status | Archivo |
|------------|------|--------|---------|
| **OsitoFS v2** | R/W | Completo (9 host tools) | `fs/ositofs2.c` |
| **FAT32** | R/W | Completo (LFN, create, delete) | `fs/fat32.c` |
| **tmpfs** | R/W | Completo (RAM, 128 files, 64MB) | `fs/tmpfs.c` |
| **ext2** | R/O | Completo (inodes, indirect blocks) | `fs/ext2.c` |
| **ISO 9660** | R/O | Completo (PVD, dirs, file read) | `fs/iso9660.c` |

## VFS Mount Points

```
/          → OsitoFS v2 (primary, R/W)
/tmp/      → tmpfs (RAM, R/W)
/fat/      → FAT32 partition (R/W)
/ext2/     → ext2/ext3 partition (R/O)
/iso/      → ISO 9660 image (R/O)
/dev/*     → Virtual devices (null, zero, urandom, console)
/proc/*    → Process info (maps, status)
```

---

## Roadmap: Filesystems por implementar

### Tier 1: Alta prioridad (lectura de discos comunes)

#### ext4 (Read-Only)
- **Por qué**: Formato default de Linux desde ~2008. Necesario para leer particiones de cualquier distro moderna.
- **Diferencia con ext2**: Extents (árbol de bloques contiguos en vez de indirect blocks), flex_bg (grupos flexibles), checksums opcionales.
- **Trabajo**:
  - Extender `ext2.c` con detección de extent flag (`EXT4_EXTENTS_FL = 0x80000`)
  - Parsear extent tree: `struct ext4_extent_header` + `ext4_extent` (12 bytes cada uno)
  - Leaf extent: `{block, len, start_hi, start_lo}` → rango contiguo de bloques
  - Interno: `{block, leaf_lo, leaf_hi}` → puntero a bloque con más extents
  - `ext2_get_block()` → si extent flag, usar extent tree en vez de indirect blocks
- **Estimación**: ~200 líneas extra en ext2.c
- **Archivo**: `fs/ext2.c` (extensión, no archivo nuevo)

#### NTFS (Read-Only)
- **Por qué**: Formato de Windows. Necesario para leer discos/USB de Windows, particiones dual-boot.
- **Complejidad**: ALTA. NTFS es un filesystem basado en base de datos (MFT).
- **Trabajo**:
  - Boot sector → MFT location (cluster del $MFT)
  - MFT entry parsing (1024 bytes/entry): header, attribute list
  - Resident vs non-resident attributes: $DATA inline vs cluster runs
  - Run list decoding: `{header_byte, length_bytes, offset_bytes}` variable-length encoding
  - Directory index: $INDEX_ROOT (B+ tree) + $INDEX_ALLOCATION
  - Filename attribute ($FILE_NAME): Unicode UTF-16LE
  - NO: compression, encryption, sparse files, reparse points (first pass)
- **Estimación**: ~800-1200 líneas. El run list decoder y B+ tree son los más complejos.
- **Archivo**: `fs/ntfs.c` (nuevo)

### Tier 2: Media prioridad (ecosistema extendido)

#### exFAT (Read-Only → R/W)
- **Por qué**: Formato default de USB sticks >32GB y tarjetas SD. Más simple que NTFS.
- **Trabajo**:
  - Boot sector → FAT offset, cluster heap offset, root directory cluster
  - Directory entries: tipo+longitud (stream extension, filename extension)
  - Allocation bitmap (en vez de escanear FAT para free clusters)
  - No LFN slots como FAT32 — nombres nativos Unicode
- **Estimación**: ~400 líneas
- **Archivo**: `fs/exfat.c` (nuevo)

#### HFS+ (Read-Only)
- **Por qué**: Formato de macOS (pre-APFS). Útil para leer discos de Mac.
- **Trabajo**:
  - Volume header (sector 2, 512 bytes)
  - Catalog B-tree: key = (parent CNID, name), record = file/folder info
  - Extent overflow B-tree (archivos con >8 extents)
  - Fork data: extents en volume header + overflow
  - Unicode filename normalization (NFD)
- **Estimación**: ~600 líneas
- **Archivo**: `fs/hfsplus.c` (nuevo)

#### Btrfs (Read-Only)
- **Por qué**: Filesystem default de Fedora/openSUSE. Copy-on-write, snapshots, checksums.
- **Complejidad**: ALTA. Btrfs usa B-trees para todo.
- **Trabajo**:
  - Superblock (offset 0x10000) → root tree, chunk tree
  - Chunk tree: logical→physical address mapping
  - FS tree: directory items, inode items, extent data
  - B-tree traversal: key = (objectid, type, offset), item headers
  - Inline vs regular extents
- **Estimación**: ~1000 líneas
- **Archivo**: `fs/btrfs.c` (nuevo)

### Tier 3: Baja prioridad (especializado)

#### APFS (Read-Only)
- **Por qué**: Formato actual de macOS/iOS.
- **Complejidad**: MUY ALTA. Containers, volumes, copy-on-write, encryption.
- **Trabajo**: Container superblock → volume superblock → B-tree catalog
- **Estimación**: ~1500 líneas
- **Nota**: Documentación oficial de Apple limitada; requiere reverse engineering

#### ZFS (Read-Only)
- **Por qué**: Usado en FreeBSD, illumos, TrueNAS. Checksums, RAID, dedup.
- **Complejidad**: EXTREMA. ZFS es un filesystem + volume manager.
- **Trabajo**: Uberblock → MOS → DSL → ZAP → ZPL
- **Estimación**: ~2000+ líneas
- **Nota**: Pocas implementaciones read-only existen fuera de OpenZFS

#### UDF (Read-Only)
- **Por qué**: Formato de DVD-Video y Blu-ray.
- **Trabajo**: Anchor Volume Descriptor → partition map → FID chain
- **Estimación**: ~500 líneas
- **Archivo**: `fs/udf.c` (nuevo)

#### SquashFS (Read-Only)
- **Por qué**: Usado en Linux live CDs, snap packages, embedded.
- **Trabajo**: Superblock → inode table → directory table → fragment table. Decompresión (zlib/lz4/zstd).
- **Estimación**: ~600 líneas (requiere decompressor)
- **Archivo**: `fs/squashfs.c` (nuevo)

### Tier 4: Mainframe / Legacy (académico)

#### z/FS (IBM z/OS)
- **Por qué**: Filesystem POSIX de z/OS mainframes. Interesante académicamente.
- **Diseño**: Aggregate → filesystem → POSIX hierarchy. Fixed-block architecture (FBA).
- **Trabajo**: Aggregate label → filesystem descriptor → POSIX directory blocks
- **Complejidad**: Documentación IBM disponible (SA23-2280). Block sizes de 1KB/2KB/4KB/8KB.
- **Estimación**: ~800 líneas (si la documentación es precisa)
- **Nota**: Requiere emular acceso a bloques FBA sobre NVMe/AHCI

#### VSAM (IBM z/OS)
- **Por qué**: Método de acceso (no filesystem) central de z/OS. KSDS/ESDS/RRDS.
- **Diseño**: Catalog → cluster → data/index components
- **Complejidad**: ALTA. VSAM no es un filesystem sino un método de acceso a registros.
- **Nota**: Más relevante como curiosidad académica que como funcionalidad práctica

#### PDSE/PDS (IBM z/OS)
- **Por qué**: Partitioned datasets — la forma clásica de organizar código en z/OS.
- **Diseño**: Directory block (256 bytes/entry) + member data (variable-length records)
- **Estimación**: ~400 líneas
- **Nota**: Interesante para demo de interoperabilidad mainframe

#### OS/400 - IFS (IBM i)
- **Por qué**: Filesystem integrado de IBM i (AS/400).
- **Diseño**: Objetos del sistema operativo, no bloques en disco. Single-level storage.
- **Nota**: Prácticamente imposible de implementar fuera de IBM i — el filesystem ES el OS.

---

## Prioridad de implementación recomendada

1. **ext4 read-only** — extensión de ext2.c existente, máximo impacto
2. **NTFS read-only** — acceso a particiones Windows, alta demanda
3. **exFAT** — USB sticks modernos, relativamente simple
4. **UDF** — DVDs, completaría soporte de medios ópticos
5. **SquashFS** — útil para live images y distribución
6. **HFS+/Btrfs** — ecosistema completo
7. **APFS/ZFS** — desafío técnico interesante
8. **z/FS y familia IBM** — proyecto académico de largo plazo
