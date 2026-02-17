#!/usr/bin/env python3
"""
OsitoVM Assembler — Two-pass assembler for OsitoVM bytecode.

Syntax:
    ; comment
    label:
      push8 72
      push16 1000
      push32 80000000
      syscall 0
      jmp label
      load 0
      store 1
      halt

Usage: py tools/asm.py input.asm output.vm
"""

import sys
import struct

# Opcodes
OPCODES = {
    'nop':     (0x00, 'none'),
    'halt':    (0x01, 'none'),
    'push8':   (0x10, 'u8'),
    'push16':  (0x11, 'u16'),
    'push32':  (0x12, 'u32'),
    'dup':     (0x13, 'none'),
    'drop':    (0x14, 'none'),
    'swap':    (0x15, 'none'),
    'over':    (0x16, 'none'),
    'rot':     (0x17, 'none'),
    'add':     (0x20, 'none'),
    'sub':     (0x21, 'none'),
    'mul':     (0x22, 'none'),
    'div':     (0x23, 'none'),
    'mod':     (0x24, 'none'),
    'neg':     (0x25, 'none'),
    'and':     (0x28, 'none'),
    'or':      (0x29, 'none'),
    'xor':     (0x2A, 'none'),
    'not':     (0x2B, 'none'),
    'shl':     (0x2C, 'none'),
    'shr':     (0x2D, 'none'),
    'eq':      (0x30, 'none'),
    'ne':      (0x31, 'none'),
    'lt':      (0x32, 'none'),
    'gt':      (0x33, 'none'),
    'le':      (0x34, 'none'),
    'ge':      (0x35, 'none'),
    'jmp':     (0x40, 'label'),
    'jz':      (0x41, 'label'),
    'jnz':     (0x42, 'label'),
    'call':    (0x43, 'label'),
    'ret':     (0x44, 'none'),
    'load':    (0x50, 'u8'),
    'store':   (0x51, 'u8'),
    'syscall': (0x60, 'u8'),
}

# Syscall aliases: mnemonic -> (opcode=0x60, 'u8') with implicit operand
SYSCALL_ALIASES = {
    'putc':         0,
    'getc':         1,
    'put_dec':      2,
    'put_hex':      3,
    'yield':        4,
    'delay':        5,
    'ticks':        6,
    'gpio_mode':    7,
    'gpio_write':   8,
    'gpio_read':    9,
    'gpio_toggle':  10,
    'input_poll':   11,
    'input_state':  12,
    'adc_read':     13,
    'fb_clear':     14,
    'fb_pixel':     15,
    'fb_line':      16,
    'fb_flush':     17,
}

def parse_int(s):
    """Parse an integer literal (decimal or 0x hex)."""
    s = s.strip()
    if s.startswith('0x') or s.startswith('0X'):
        return int(s, 16)
    return int(s)

def instruction_size(op_type):
    """Size in bytes of opcode + operand."""
    if op_type == 'none':
        return 1
    elif op_type == 'u8':
        return 2
    elif op_type in ('u16', 'label'):
        return 3
    elif op_type == 'u32':
        return 5
    return 1

def assemble(lines):
    """Two-pass assembler. Returns bytecode bytes."""
    # Pass 1: collect labels and compute offsets
    labels = {}
    instructions = []  # (line_num, mnemonic, operand_str, offset)
    offset = 0

    for line_num, raw_line in enumerate(lines, 1):
        line = raw_line.split(';')[0].strip()
        if not line:
            continue

        # Label definition
        if line.endswith(':'):
            label = line[:-1].strip()
            if label in labels:
                print(f"error: line {line_num}: duplicate label '{label}'", file=sys.stderr)
                sys.exit(1)
            labels[label] = offset
            continue

        # Instruction
        parts = line.split(None, 1)
        mnem = parts[0].lower()
        operand = parts[1].strip() if len(parts) > 1 else None

        # Expand syscall aliases: e.g. "input_poll" -> "syscall 11"
        if mnem in SYSCALL_ALIASES:
            operand = str(SYSCALL_ALIASES[mnem])
            mnem = 'syscall'

        if mnem not in OPCODES:
            print(f"error: line {line_num}: unknown instruction '{mnem}'", file=sys.stderr)
            sys.exit(1)

        opcode, op_type = OPCODES[mnem]
        instructions.append((line_num, mnem, operand, offset))
        offset += instruction_size(op_type)

    # Pass 2: emit bytecode
    bytecode = bytearray()

    for line_num, mnem, operand, insn_offset in instructions:
        opcode, op_type = OPCODES[mnem]
        bytecode.append(opcode)

        if op_type == 'none':
            if operand is not None:
                print(f"warning: line {line_num}: '{mnem}' takes no operand, ignoring", file=sys.stderr)

        elif op_type == 'u8':
            if operand is None:
                print(f"error: line {line_num}: '{mnem}' requires u8 operand", file=sys.stderr)
                sys.exit(1)
            val = parse_int(operand)
            if val < 0 or val > 255:
                print(f"error: line {line_num}: u8 operand out of range: {val}", file=sys.stderr)
                sys.exit(1)
            bytecode.append(val)

        elif op_type == 'u16':
            if operand is None:
                print(f"error: line {line_num}: '{mnem}' requires u16 operand", file=sys.stderr)
                sys.exit(1)
            val = parse_int(operand)
            if val < 0 or val > 65535:
                print(f"error: line {line_num}: u16 operand out of range: {val}", file=sys.stderr)
                sys.exit(1)
            bytecode.extend(struct.pack('<H', val))

        elif op_type == 'u32':
            if operand is None:
                print(f"error: line {line_num}: '{mnem}' requires u32 operand", file=sys.stderr)
                sys.exit(1)
            val = parse_int(operand)
            bytecode.extend(struct.pack('<I', val & 0xFFFFFFFF))

        elif op_type == 'label':
            if operand is None:
                print(f"error: line {line_num}: '{mnem}' requires label operand", file=sys.stderr)
                sys.exit(1)
            target_label = operand.strip()

            # Allow numeric literal offsets too
            if target_label.lstrip('-').isdigit() or target_label.startswith('0x'):
                rel = parse_int(target_label)
            else:
                if target_label not in labels:
                    print(f"error: line {line_num}: undefined label '{target_label}'", file=sys.stderr)
                    sys.exit(1)
                target_offset = labels[target_label]
                # Relative offset from the position of the i16 operand
                # The operand is at insn_offset + 1 (after the opcode byte)
                operand_pos = insn_offset + 1
                rel = target_offset - operand_pos

            if rel < -32768 or rel > 32767:
                print(f"error: line {line_num}: jump offset out of range: {rel}", file=sys.stderr)
                sys.exit(1)
            bytecode.extend(struct.pack('<h', rel))

    return bytes(bytecode)

def make_header(num_locals=0):
    """Build 8-byte OSVM header."""
    header = bytearray()
    header.extend(struct.pack('<I', 0x4D56534F))  # Magic "OSVM"
    header.append(1)    # Version
    header.append(0)    # Flags
    header.append(num_locals & 0xFF)
    header.append(0)    # Reserved
    return bytes(header)

def main():
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} input.asm output.vm", file=sys.stderr)
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2]

    with open(input_file, 'r') as f:
        lines = f.readlines()

    bytecode = assemble(lines)
    header = make_header()

    with open(output_file, 'wb') as f:
        f.write(header)
        f.write(bytecode)

    print(f"assembled: {len(bytecode)} bytes bytecode, {len(header) + len(bytecode)} bytes total -> {output_file}")

if __name__ == '__main__':
    main()
