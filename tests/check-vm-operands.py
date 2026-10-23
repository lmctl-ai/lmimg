#!/usr/bin/env python3
"""Reject bytecode loops whose operands have the wrong runtime kind.

These programs cannot be emitted by lmscript.  They exercise the public v5
bytecode loader with hand-built programs so the host's VM checks remain
regression-tested independently of the compiler.
"""

from __future__ import annotations

import hashlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def u16(value: int) -> bytes:
    return struct.pack(">H", value)


def i32(value: int) -> bytes:
    return struct.pack(">i", value)


def u32(value: int) -> bytes:
    return struct.pack(">I", value)


def bytecode(schema: bytes, constants: list[str], locals_: list[tuple[str, bool]], code: bytes) -> bytes:
    out = bytearray(b"DSLB" + u16(5) + hashlib.sha256(schema).digest())
    out += u32(len(constants))
    for value in constants:
        encoded = value.encode("utf-8")
        out += b"\x01" + u32(len(encoded)) + encoded
    out += u16(0)  # imports
    out += u16(0)  # contexts
    out += u16(len(locals_))
    for type_name, hidden in locals_:
        out += u32(constants.index(type_name)) + bytes((1 if hidden else 0,))
    out += u16(2)  # measured stack depth
    out += u32(len(code)) + code
    out += u32(0)  # line table
    return bytes(out)


def while_with_int_condition(schema: bytes) -> bytes:
    # WHILE_BEGIN, PUSH_INT(1), WHILE_TEST(exit), LOOP_BACK(head), HALT.
    # WHILE_TEST's condition is intentionally an int, not a bool.
    code = bytearray(b"\x34\x01" + struct.pack(">q", 1))
    code += b"\x35" + i32(5)       # 15 + 5 -> HALT
    code += b"\x36" + i32(-20)     # 20 - 20 -> WHILE_BEGIN
    code += b"\x11"
    return bytecode(schema, [], [], bytes(code))


def for_range_with_strings(schema: bytes, bad_begin: bool, bad_finish: bool) -> bytes:
    # FOR_RANGE requires two hidden int locals, but the stack supplies strings.
    # The runtime must reject those values before reading their integer fields.
    constants = ["int", "begin", "end"]
    code = bytearray(b"\x02" + u32(1) if bad_begin else b"\x01" + struct.pack(">q", 0))
    code += b"\x02" + u32(2) if bad_finish else b"\x01" + struct.pack(">q", 1)
    code += b"\x2f" + u16(0) + u16(1) + i32(7)  # exit after FOR_NEXT
    code += b"\x31" + u16(1) + i32(-7)             # back to FOR_RANGE body
    code += b"\x11"
    return bytecode(schema, constants, [("int", True), ("int", True)], bytes(code))


def run(host: Path, payload: bytes, expected: str, directory: Path) -> None:
    path = directory / "malformed-loop.dslb"
    path.write_bytes(payload)
    result = subprocess.run([str(host), "--dsl", str(path)], text=True, capture_output=True, timeout=30)
    if result.returncode != 3:
        raise SystemExit(f"{expected}: expected exit 3, got {result.returncode}; stdout={result.stdout!r} stderr={result.stderr!r}")
    if expected not in result.stdout:
        raise SystemExit(f"{expected}: missing diagnostic in stdout: {result.stdout!r}")
    if result.stderr.count("\n") != 1:
        raise SystemExit(f"{expected}: expected one stderr diagnostic, got {result.stderr!r}")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    host = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "lmimg"
    schema_path = root / "schema" / "lmimg.dsl.json"
    schema = schema_path.read_bytes()
    with tempfile.TemporaryDirectory(prefix="lmimg-vm-operands-") as temp:
        directory = Path(temp)
        run(host, while_with_int_condition(schema), "condition must be bool", directory)
        run(host, for_range_with_strings(schema, True, False), "FOR_RANGE operands must be int", directory)
        run(host, for_range_with_strings(schema, False, True), "FOR_RANGE operands must be int", directory)
        run(host, for_range_with_strings(schema, True, True), "FOR_RANGE operands must be int", directory)
    print("malformed loop operands rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
