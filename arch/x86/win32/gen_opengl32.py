#!/usr/bin/env python3
"""Generate typed Win32/OpenGL ABI wrappers for Mesa's exported GL API."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
DEFAULT_XML = (
    REPO / "arch/x86/lib/opengl/mesa/src/mapi/glapi/registry/gl.xml"
)
DEFAULT_MODULE = (
    REPO / "arch/x86/build/lib/opengl-module/libGL-osito.so"
)
DEFAULT_OUTPUT = Path(__file__).with_name("opengl32_generated.inc")

FLOAT_TYPES = {"GLfloat", "GLclampf"}
DOUBLE_TYPES = {"GLdouble", "GLclampd"}
SIGNED_64_TYPES = {"GLint64", "GLint64EXT"}
UNSIGNED_64_TYPES = {"GLuint64", "GLuint64EXT"}
POINTER_SIZE_TYPES = {
    "GLintptr",
    "GLintptrARB",
    "GLsizeiptr",
    "GLsizeiptrARB",
    "GLvdpauSurfaceNV",
}
POINTER_TYPEDEFS = {
    "GLsync",
    "GLeglClientBufferEXT",
    "GLeglImageOES",
    "GLDEBUGPROC",
    "GLDEBUGPROCARB",
    "GLDEBUGPROCKHR",
    "GLDEBUGPROCAMD",
    "GLVULKANPROCNV",
}
SIGNED_32_TYPES = {
    "GLbyte",
    "GLshort",
    "GLint",
    "GLsizei",
    "GLfixed",
    "GLclampx",
}


@dataclass(frozen=True)
class Param:
    declaration: str
    name: str
    ctype: str


@dataclass(frozen=True)
class Command:
    name: str
    return_type: str
    params: tuple[Param, ...]
    core_11: bool


def normalize_c(text: str) -> str:
    text = re.sub(r"\s+", " ", text).strip()
    text = text.replace(" * ", " *").replace("* ", "*")
    return text


def element_text(element: ET.Element) -> str:
    return normalize_c("".join(element.itertext()))


def parse_registry(path: Path) -> dict[str, Command]:
    root = ET.parse(path).getroot()
    core_11: set[str] = set()
    for feature in root.findall("feature"):
        if feature.get("api") != "gl":
            continue
        try:
            version = tuple(int(part) for part in feature.get("number", "0").split("."))
        except ValueError:
            continue
        if version > (1, 1):
            continue
        for require in feature.findall("require"):
            api = require.get("api")
            if api and api != "gl":
                continue
            core_11.update(
                item.get("name", "") for item in require.findall("command")
            )

    commands: dict[str, Command] = {}
    for node in root.findall("./commands/command"):
        proto = node.find("proto")
        if proto is None:
            continue
        name = proto.findtext("name")
        if not name:
            continue
        proto_text = element_text(proto)
        name_pos = proto_text.rfind(name)
        if name_pos < 0:
            raise ValueError(f"cannot parse return type for {name}")
        return_type = normalize_c(proto_text[:name_pos])

        params: list[Param] = []
        for param in node.findall("param"):
            param_name = param.findtext("name")
            if not param_name:
                raise ValueError(f"unnamed parameter in {name}")
            declaration = element_text(param)
            ctype = param.findtext("ptype") or ""
            params.append(Param(declaration, param_name, ctype))

        commands[name] = Command(
            name=name,
            return_type=return_type,
            params=tuple(params),
            core_11=name in core_11,
        )
    return commands


def module_symbols(path: Path, nm: str) -> set[str]:
    result = subprocess.run(
        [nm, "-D", "--defined-only", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    symbols: set[str] = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if fields and fields[-1].startswith("gl"):
            symbols.add(fields[-1])
    return symbols


def is_pointer(param: Param) -> bool:
    return (
        "*" in param.declaration
        or "[" in param.declaration
        or param.ctype in POINTER_TYPEDEFS
    )


def param_slots(param: Param) -> int:
    if is_pointer(param):
        return 1
    if param.ctype in DOUBLE_TYPES | SIGNED_64_TYPES | UNSIGNED_64_TYPES:
        return 2
    return 1


def pointer_cast_type(param: Param) -> str:
    pos = param.declaration.rfind(param.name)
    if pos < 0:
        raise ValueError(f"cannot derive pointer type from {param.declaration}")
    prefix = param.declaration[:pos].strip()
    suffix = param.declaration[pos + len(param.name):].strip()
    if suffix.startswith("["):
        prefix = f"{prefix} *"
    return normalize_c(prefix)


def decode_param(param: Param, slot: int) -> tuple[str, int]:
    low = f"s{slot}"
    if is_pointer(param):
        return (
            f"({pointer_cast_type(param)})(ULONG_PTR)(uint32_t){low}",
            1,
        )
    if param.ctype in FLOAT_TYPES:
        return f"opengl_float_from_slot({low})", 1
    if param.ctype in DOUBLE_TYPES:
        return f"opengl_double_from_slots({low}, s{slot + 1})", 2
    if param.ctype in SIGNED_64_TYPES:
        return (
            f"({param.ctype})(int64_t)opengl_u64_from_slots({low}, s{slot + 1})",
            2,
        )
    if param.ctype in UNSIGNED_64_TYPES:
        return f"({param.ctype})opengl_u64_from_slots({low}, s{slot + 1})", 2
    if param.ctype in POINTER_SIZE_TYPES:
        return f"({param.ctype})(int32_t)(uint32_t){low}", 1
    if param.ctype in SIGNED_32_TYPES:
        return f"({param.ctype})(int32_t)(uint32_t){low}", 1
    cast = param.ctype or "uint32_t"
    return f"({cast})(uint32_t){low}", 1


def return_is_pointer(command: Command) -> bool:
    return "*" in command.return_type or command.return_type in POINTER_TYPEDEFS


def emit_command(command: Command, symbol_index: int) -> str:
    params_decl = ", ".join(p.declaration for p in command.params) or "void"
    args = ", ".join(p.name for p in command.params)
    native_type = f"ogl_native_{command.name}_t"
    lines = [
        f"typedef {command.return_type} (OGL_SYSV_ABI *{native_type})({params_decl});",
        f"static {command.return_type} WINAPI ogl_wrap_{command.name}({params_decl})",
        "{",
        f"    {native_type} native = ({native_type})",
        f"        opengl_native_symbol({symbol_index}, \"{command.name}\");",
        "    if (!native) {",
    ]
    if command.return_type == "void":
        lines.append("        return;")
    else:
        lines.append(f"        return ({command.return_type})0;")
    lines.extend(["    }"])
    if command.return_type == "void":
        lines.append(f"    native({args});" if args else "    native();")
    else:
        lines.append(f"    return native({args});" if args else "    return native();")
    lines.append("}")

    slot_count = sum(param_slots(param) for param in command.params)
    slots_decl = ", ".join(f"uint64_t s{i}" for i in range(slot_count)) or "void"
    decoded: list[str] = []
    slot = 0
    for param in command.params:
        expression, consumed = decode_param(param, slot)
        decoded.append(expression)
        slot += consumed
    decoded_args = ", ".join(decoded)

    lines.extend([
        "",
        f"static uint64_t WINAPI ogl_compat_{command.name}({slots_decl})",
        "{",
    ])
    call = f"ogl_wrap_{command.name}({decoded_args})" if decoded_args else f"ogl_wrap_{command.name}()"
    if command.return_type == "void":
        lines.extend([f"    {call};", "    return 0;"])
    else:
        lines.append(f"    {command.return_type} result = {call};")
        if command.name in {"glGetString", "glGetStringi"}:
            lines.append(
                "    return (uint64_t)(ULONG_PTR)"
                "opengl_compat_string((const char *)result);"
            )
        elif return_is_pointer(command):
            lines.append("    return (uint64_t)(ULONG_PTR)result;")
        elif command.return_type in FLOAT_TYPES | DOUBLE_TYPES:
            raise ValueError(
                f"PE32 floating-point return is not implemented: {command.name}"
            )
        else:
            lines.append("    return (uint64_t)result;")
    lines.append("}")
    return "\n".join(lines)


def generate(commands: list[Command]) -> str:
    max_slots = max(
        (sum(param_slots(param) for param in command.params) for command in commands),
        default=0,
    )
    lines = [
        "/* Generated by gen_opengl32.py from Khronos gl.xml and Mesa exports. */",
        "/* Do not edit by hand. */",
        f"#define OGL_GENERATED_MAX_SLOTS {max_slots}",
        "",
    ]
    for symbol_index, command in enumerate(commands):
        lines.append(emit_command(command, symbol_index))
        lines.append("")

    lines.extend([
        "static const OGL_GENERATED_EXPORT ogl_generated_exports[] = {",
    ])
    for command in commands:
        slots = sum(param_slots(param) for param in command.params)
        core = "TRUE" if command.core_11 else "FALSE"
        lines.append(
            "    { WX_STD(\"%s\", ogl_wrap_%s, %d), "
            "(PVOID)ogl_compat_%s, %s },"
            % (command.name, command.name, slots, command.name, core)
        )
    lines.extend(["};", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xml", type=Path, default=DEFAULT_XML)
    parser.add_argument("--module", type=Path, default=DEFAULT_MODULE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()

    registry = parse_registry(args.xml)
    exported = module_symbols(args.module, args.nm)
    missing = sorted(exported - registry.keys())
    if missing:
        print("Mesa exports absent from gl.xml:", ", ".join(missing), file=sys.stderr)
        return 1

    selected = [registry[name] for name in sorted(exported)]
    output = generate(selected)
    args.output.write_text(output, encoding="ascii", newline="\n")
    max_slots = max(
        (sum(param_slots(param) for param in command.params) for command in selected),
        default=0,
    )
    print(
        f"generated {len(selected)} wrappers in {args.output} "
        f"(max PE32 slots: {max_slots})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
