#!/usr/bin/env python3
"""
Break at a guest address in xemu and report the registers.

The reference-oracle step: when the recompiled build reaches a function with a
value that looks wrong, this reads what the real game had there.

    python3 scripts/xemu_probe.py --addr 0x000B9030

Requires xemu started with its GDB stub open:

    xemu.exe -s          # stub on localhost:1234
    xemu.exe -s -S       # ... and wait for a connection before booting

Safety, per docs/technical/xemu-debugging.md: the stub will not accept a
breakpoint while the guest is running, so this halts first; and a breakpoint
left behind is an int3 in the guest's code, so this removes every one it set
even when interrupted.
"""

import argparse
import socket
import struct
import sys
import time

# Order of the 16 general registers in an i386 "g" reply.
I386_REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
             "eip", "eflags", "cs", "ss", "ds", "es", "fs", "gs"]


class GdbError(RuntimeError):
    pass


class Gdb:
    """Just enough GDB Remote Serial Protocol to halt, break and read."""

    def __init__(self, host, port, timeout):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.breakpoints = set()

    # -- wire format -----------------------------------------------------

    def _send(self, payload: str) -> None:
        checksum = sum(payload.encode()) & 0xFF
        self.sock.sendall(f"${payload}#{checksum:02x}".encode())

    def _read_packet(self, timeout=None) -> str:
        if timeout is not None:
            self.sock.settimeout(timeout)
        buf = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                raise GdbError("timed out waiting for the stub")
            if not chunk:
                raise GdbError("stub closed the connection")
            buf += chunk
            # Skip the '+' / '-' acknowledgements the stub interleaves.
            if b"$" in buf and b"#" in buf:
                start = buf.index(b"$") + 1
                end = buf.index(b"#", start)
                if len(buf) >= end + 3:
                    return buf[start:end].decode(errors="replace")

    def command(self, payload: str, timeout=None) -> str:
        self._send(payload)
        reply = self._read_packet(timeout)
        self.sock.sendall(b"+")
        return reply

    # -- operations ------------------------------------------------------

    def halt(self) -> str:
        """Interrupt the guest. Required before touching breakpoints."""
        self.sock.sendall(b"\x03")
        time.sleep(0.3)
        try:
            return self._read_packet(timeout=3.0)
        except GdbError:
            return ""       # already stopped

    def set_breakpoint(self, addr: int, hardware: bool = False) -> None:
        kind = "Z1" if hardware else "Z0"
        reply = self.command(f"{kind},{addr:x},1")
        if reply != "OK":
            raise GdbError(f"stub refused a breakpoint at 0x{addr:08X}: "
                           f"{reply!r} (is the address mapped yet?)")
        self.breakpoints.add((addr, hardware))

    def clear_breakpoint(self, addr: int, hardware: bool = False) -> None:
        self.command(f"{'z1' if hardware else 'z0'},{addr:x},1")
        self.breakpoints.discard((addr, hardware))

    def clear_all(self) -> None:
        for addr, hardware in list(self.breakpoints):
            try:
                self.clear_breakpoint(addr, hardware)
            except Exception:                       # noqa: BLE001
                print(f"  WARNING: could not remove the breakpoint at "
                      f"0x{addr:08X}; it may still be set in the guest",
                      file=sys.stderr)

    def continue_until_stop(self, timeout: float) -> str:
        self._send("c")
        return self._read_packet(timeout)

    def registers(self) -> dict:
        raw = self.command("g")
        if raw.startswith("E") and len(raw) <= 3:
            raise GdbError(f"register read failed: {raw}")
        values = {}
        for i, name in enumerate(I386_REGS):
            field = raw[i * 8:(i + 1) * 8]
            if len(field) < 8:
                break
            values[name] = struct.unpack("<I", bytes.fromhex(field))[0]
        return values

    def read_memory(self, addr: int, length: int):
        raw = self.command(f"m{addr:x},{length:x}")
        if raw.startswith("E") and len(raw) <= 3:
            return None                             # unmapped
        try:
            return bytes.fromhex(raw)
        except ValueError:
            return None

    def close(self) -> None:
        try:
            self.sock.close()
        except Exception:                           # noqa: BLE001
            pass


def describe(addr: int) -> str:
    """Rough guess at what a guest address is, for orientation."""
    if addr == 0:
        return "null"
    if 0x00010000 <= addr < 0x00600000:
        return "image (code/data)"
    if 0x00780000 <= addr < 0x00F80000:
        return "guest stack"
    if 0x00F80000 <= addr < 0x04000000:
        return "heap"
    if 0x80000000 <= addr < 0x84000000:
        return "contiguous window"
    if 0x84000000 <= addr < 0xF0000000:
        return "ABOVE the contiguous window"
    if 0xFD000000 <= addr < 0xFE000000:
        return "NV2A registers"
    return "unmapped / unknown"


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--addr", required=True, type=lambda s: int(s, 0),
                    help="Guest VA to break on, e.g. 0x000B9030")
    ap.add_argument("--hits", type=int, default=1,
                    help="How many times to stop there (default 1)")
    ap.add_argument("--deref", metavar="REG", default="ecx",
                    help="Register to treat as a pointer and sample "
                         "(default ecx, the thiscall receiver)")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0,
                    help="Also read this offset from the --deref pointer")
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--wait", type=float, default=120.0,
                    help="Seconds to wait for the breakpoint (default 120)")
    ap.add_argument("--read", action="append", default=[], metavar="VA",
                    type=lambda s: int(s, 0),
                    help="Also dump the 4-byte word at this absolute guest "
                         "address when the breakpoint hits. Repeatable -- use "
                         "it for the function-pointer slots a call goes "
                         "through, which is what a runtime-populated table "
                         "looks like from the outside.")
    ap.add_argument("--peek", action="store_true",
                    help="Do not break: just halt, dump 16 bytes at --addr, "
                         "and resume. Use this first to confirm the game is "
                         "actually loaded there.")
    ap.add_argument("--hw", action="store_true",
                    help="Use a hardware breakpoint (Z1) instead of Z0")
    args = ap.parse_args()

    print(f"connecting to {args.host}:{args.port} ...")
    try:
        gdb = Gdb(args.host, args.port, timeout=10.0)
    except OSError as exc:
        print(f"error: could not connect: {exc}\n"
              f"       is xemu running with -s ?", file=sys.stderr)
        return 1

    try:
        print("halting the guest ...")
        gdb.halt()

        code = gdb.read_memory(args.addr, 16)
        loaded = code is not None and set(code) not in ({0}, {0xFF})
        if loaded:
            print(f"  bytes at 0x{args.addr:08X}: {code.hex(chr(32))}")
        else:
            # With xemu -S the guest has not executed yet, so the image is
            # not in memory. That is expected and is exactly when a
            # hardware breakpoint is the right tool: it watches the address
            # rather than patching the code, so it survives the loader
            # writing the section in later.
            print(f"  0x{args.addr:08X} is not loaded yet")
            if not args.hw:
                print("  xemu is still in the BIOS or dashboard. Either let")
                print("  the game boot and retry, or start xemu with -S and")
                print("  pass --hw to break before the image is loaded.")
                return 1
            print("  --hw given: setting a hardware breakpoint anyway.")

        if args.read:
            print("  absolute reads:")
            for target in args.read:
                word = gdb.read_memory(target, 4)
                if word is None:
                    print(f"    [0x{target:08X}] NOT READABLE")
                else:
                    value = struct.unpack("<I", word)[0]
                    print(f"    [0x{target:08X}] = 0x{value:08X}   {describe(value)}")

        if args.peek:
            print("peek only, not breaking.")
            return 0

        print(f"setting a {'hardware' if args.hw else 'software'} breakpoint "
              f"at 0x{args.addr:08X} ...")
        gdb.set_breakpoint(args.addr, hardware=args.hw)

        for hit in range(1, args.hits + 1):
            print(f"\nrunning (waiting up to {args.wait:.0f}s for hit "
                  f"{hit}/{args.hits}) ...")
            try:
                gdb.continue_until_stop(timeout=args.wait)
            except GdbError:
                print("  never reached it. Either the game does not run this "
                      "path yet, or it needs more play time.")
                break

            regs = gdb.registers()
            print(f"--- hit {hit} at 0x{args.addr:08X} "
                  f"(eip=0x{regs.get('eip', 0):08X}) ---")
            for name in ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"):
                value = regs.get(name)
                if value is None:
                    continue
                print(f"  {name} = 0x{value:08X}   {describe(value)}")

            for target in args.read:
                word = gdb.read_memory(target, 4)
                if word is None:
                    print(f"  [0x{target:08X}] NOT READABLE")
                else:
                    value = struct.unpack("<I", word)[0]
                    print(f"  [0x{target:08X}] = 0x{value:08X}   {describe(value)}")

            pointer = regs.get(args.deref)
            if pointer:
                head = gdb.read_memory(pointer, 32)
                if head is None:
                    print(f"  [{args.deref}] is not readable - "
                          f"the guest does not have that mapped either")
                else:
                    words = struct.unpack("<8I", head)
                    print(f"  [{args.deref}] first 8 words: "
                          + " ".join(f"{w:08X}" for w in words))
                if args.offset:
                    field = gdb.read_memory(pointer + args.offset, 4)
                    target = pointer + args.offset
                    if field is None:
                        print(f"  [{args.deref}+0x{args.offset:X}] "
                              f"(0x{target:08X}) is NOT readable")
                    else:
                        print(f"  [{args.deref}+0x{args.offset:X}] "
                              f"(0x{target:08X}) = "
                              f"0x{struct.unpack('<I', field)[0]:08X}")
    except GdbError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        print("\nremoving breakpoints and resuming ...")
        gdb.clear_all()
        try:
            gdb._send("c")
        except Exception:                           # noqa: BLE001
            pass
        gdb.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
