#!/usr/bin/env python3
"""
elf2efi.py — Convert gnu-efi ELF shared object to PE32+ EFI application.

Replaces `objcopy --target=efi-app-x86_64` for macOS where that target
is unavailable. Produces a valid UEFI PE32+ application from the ELF
produced by GNU ld with the gnu-efi linker script.

Strategy: copy all ALLOC sections into the PE image preserving their
virtual addresses relative to the ELF base. Create .text (code) and
.data (everything else) PE sections. .reloc gets its own section.

Usage: python3 elf2efi.py input.so output.efi
"""
import struct, sys

SECTION_ALIGNMENT = 0x1000
FILE_ALIGNMENT = 0x200
IMAGE_FILE_MACHINE_AMD64 = 0x8664
PE32PLUS_MAGIC = 0x020B
EFI_APPLICATION = 10

SHF_ALLOC = 0x2
SHF_WRITE = 0x1
SHF_EXECINSTR = 0x4


def align_up(val, a):
    return (val + a - 1) & ~(a - 1)


def read_elf(path):
    with open(path, 'rb') as f:
        data = f.read()

    assert data[:4] == b'\x7fELF' and data[4] == 2, "Need 64-bit ELF"

    (e_type, e_machine, _, e_entry, e_phoff, e_shoff,
     _, _, e_phentsize, e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)

    # Read section headers
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        vals = struct.unpack_from('<IIQQQQIIqq', data, off)
        secs.append({
            'name_off': vals[0], 'type': vals[1], 'flags': vals[2],
            'addr': vals[3], 'offset': vals[4], 'size': vals[5],
        })

    # Get section names
    st = secs[e_shstrndx]
    strtab = data[st['offset']:st['offset'] + st['size']]
    for s in secs:
        end = strtab.index(b'\0', s['name_off'])
        s['name'] = strtab[s['name_off']:end].decode('ascii')

    # Collect allocatable sections
    alloc = [s for s in secs if (s['flags'] & SHF_ALLOC) and s['size'] > 0]
    alloc.sort(key=lambda s: s['addr'])

    return data, e_entry, alloc


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input.so output.efi", file=sys.stderr)
        sys.exit(1)

    elf_data, entry, alloc_secs = read_elf(sys.argv[1])

    # Find the loaded range
    load_start = alloc_secs[0]['addr']
    last = alloc_secs[-1]
    load_end = last['addr'] + last['size']

    # Create the flat image of all loaded content
    image_size_raw = load_end - load_start
    image = bytearray(image_size_raw)

    # Copy sections into the flat image
    for s in alloc_secs:
        off = s['addr'] - load_start
        if s['type'] != 8:  # Not SHT_NOBITS (bss)
            sdata = elf_data[s['offset']:s['offset'] + s['size']]
            image[off:off + len(sdata)] = sdata

    print(f"ELF: entry=0x{entry:x}, loaded=0x{load_start:x}-0x{load_end:x} "
          f"({image_size_raw} bytes), {len(alloc_secs)} sections")

    # Find .reloc section for PE base relocations
    reloc_sec = None
    for s in alloc_secs:
        if s['name'] == '.reloc':
            reloc_sec = s
            break

    # Build PE sections:
    # We'll create one flat section containing everything, called ".flat"
    # This is the simplest approach that preserves all section contents
    # at their correct relative addresses.

    # PE layout:
    # [headers] [.flat section (entire loaded image)]
    num_pe_sections = 1
    dos_size = 64
    pe_sig_size = 4
    coff_size = 20
    opt_size = 112 + 16 * 8  # 240 bytes (PE32+ opt header + 16 data dirs)
    sec_hdrs_size = num_pe_sections * 40
    headers_total = align_up(dos_size + pe_sig_size + coff_size + opt_size + sec_hdrs_size, FILE_ALIGNMENT)

    # The flat section RVA — must be aligned to SECTION_ALIGNMENT
    flat_rva = align_up(headers_total, SECTION_ALIGNMENT)
    flat_vsize = align_up(image_size_raw, SECTION_ALIGNMENT)
    flat_raw_size = align_up(image_size_raw, FILE_ALIGNMENT)
    flat_file_off = headers_total

    pe_image_size = flat_rva + flat_vsize

    # Entry point RVA: the ELF entry is relative to the ELF base (load_start).
    # In the PE, the flat section starts at flat_rva, so:
    entry_rva = flat_rva + (entry - load_start)

    # Relocation directory (if .reloc exists)
    # The .reloc section from gnu-efi contains PE base relocations with RVAs
    # relative to ELF base. We need to adjust them by flat_rva since we moved
    # the image to start at flat_rva in the PE.
    reloc_rva = 0
    reloc_size = 0
    if reloc_sec:
        reloc_rva = flat_rva + (reloc_sec['addr'] - load_start)
        reloc_size = reloc_sec['size']
        # Patch relocation page RVAs in the image
        reloc_off_in_image = reloc_sec['addr'] - load_start
        pos = reloc_off_in_image
        while pos < reloc_off_in_image + reloc_size:
            page_rva, block_size = struct.unpack_from('<II', image, pos)
            if block_size == 0:
                break
            # Adjust page RVA by flat_rva offset
            struct.pack_into('<I', image, pos, page_rva + flat_rva)
            pos += block_size

    # ── Build PE ──────────────────────────────────────────

    pe = bytearray()

    # DOS header (64 bytes)
    dos = bytearray(64)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 64)  # e_lfanew
    pe += dos

    # PE signature
    pe += b'PE\x00\x00'

    # COFF header (20 bytes)
    pe += struct.pack('<HHIIIHH',
        IMAGE_FILE_MACHINE_AMD64,
        num_pe_sections,
        0, 0, 0,  # timestamp, symtab ptr, sym count
        opt_size,
        0x002E  # EXECUTABLE | LINE_NUMS_STRIPPED | LOCAL_SYMS_STRIPPED | LARGE_ADDRESS_AWARE (NO RELOCS_STRIPPED)
    )

    # Optional header PE32+ (240 bytes)
    opt = bytearray(opt_size)
    struct.pack_into('<H', opt, 0, PE32PLUS_MAGIC)
    struct.pack_into('<BB', opt, 2, 2, 26)  # linker version
    struct.pack_into('<I', opt, 4, image_size_raw)  # SizeOfCode
    struct.pack_into('<I', opt, 8, 0)   # SizeOfInitializedData
    struct.pack_into('<I', opt, 12, 0)  # SizeOfUninitializedData
    struct.pack_into('<I', opt, 16, entry_rva)
    struct.pack_into('<I', opt, 20, flat_rva)  # BaseOfCode
    struct.pack_into('<Q', opt, 24, 0)  # ImageBase
    struct.pack_into('<I', opt, 32, SECTION_ALIGNMENT)
    struct.pack_into('<I', opt, 36, FILE_ALIGNMENT)
    struct.pack_into('<I', opt, 56, pe_image_size)  # SizeOfImage
    struct.pack_into('<I', opt, 60, headers_total)  # SizeOfHeaders
    struct.pack_into('<H', opt, 68, EFI_APPLICATION)  # Subsystem
    struct.pack_into('<I', opt, 108, 16)  # NumberOfRvaAndSizes

    # Data directory 5 = Base Relocation Table
    if reloc_rva:
        struct.pack_into('<II', opt, 112 + 5 * 8, reloc_rva, reloc_size)

    pe += opt

    # Section header: .flat (contains entire loaded image)
    sec_hdr = bytearray(40)
    sec_hdr[0:5] = b'.flat'
    struct.pack_into('<I', sec_hdr, 8, image_size_raw)   # VirtualSize
    struct.pack_into('<I', sec_hdr, 12, flat_rva)         # VirtualAddress
    struct.pack_into('<I', sec_hdr, 16, flat_raw_size)    # SizeOfRawData
    struct.pack_into('<I', sec_hdr, 20, flat_file_off)    # PointerToRawData
    struct.pack_into('<I', sec_hdr, 36, 0xE0000060)       # CODE|INIT_DATA|READ|WRITE|EXECUTE
    pe += sec_hdr

    # Pad headers
    pe += b'\0' * (headers_total - len(pe))

    # Section data
    pe += image
    pe += b'\0' * (flat_raw_size - len(image))

    with open(sys.argv[2], 'wb') as f:
        f.write(pe)

    print(f"PE: {len(pe)} bytes, entry RVA=0x{entry_rva:x}, "
          f".flat @ RVA 0x{flat_rva:x} ({image_size_raw} bytes)")
    if reloc_rva:
        print(f"  .reloc @ RVA 0x{reloc_rva:x} ({reloc_size} bytes)")


if __name__ == '__main__':
    main()
