# Mortal Kombat: Deadly Alliance — the loader is a hand-rolled coroutine

20 September 2026. XDK 4721, title `0x4D57000C`, entry `0x0015ADAC`. It lifts
8406 functions with none failed, boots through XAPI, input and DirectSound
init, builds its heaps and creates a D3D device. It never draws a frame,
because its asset loader runs on its own stack and this runtime cannot follow
a guest that switches stacks.

This is not a discovery gap, a signature gap or a missing kernel ordinal. It
is a capability the toolkit does not have.

---

## What the title does

`sub_000E0A50` is the loading-screen object's pump, vtable slot `+0x18`:

```
sub_000E0A50:
    mov  eax, [0x321208]        ; the loading-screen object
    mov  [eax+0x10], ebp        ; save this context's callee-saved registers
    mov  [eax+0x14], ebx
    mov  [eax+0x18], esi
    mov  [eax+0x1c], edi
    pop  dword ptr [0x2d4d30]   ; take its OWN return address off the stack
    mov  [eax+0x44], esp        ; save this context's stack pointer
    ...
    mov  esp, [0x2d4d44]        ; switch to the other context's stack
    mov  ss,  [0x2d4d60]
    mov  ebp, [0x2d4d54]
    jmp  dword ptr [0x2d4d34]   ; resume it, mid-function
```

and `sub_000E05D0` is the other half — it pops its own return address into
`[0x2d4d34]`, records `esp` and `ebp` in `[0x2d4d44]` / `[0x2d4d54]`, and the
family of functions around it (`0x000E0583`, `0x000E05C6`, `0x000E066C`,
`0x000E07F5`, `0x000E08BE`, `0x000E09D7`, `0x000E0A3C`, `0x000E0AAE`) all end
with `jmp [0x2d4d34]` instead of `ret`. `sub_000E08E0` does the mirror
restore and returns normally.

Two stacks, switched by hand, with control transferred by jumping to a saved
address. A fiber, written in assembly, inside the title.

## Why it stops us

The continuation it jumps to is `0x000E0535`, which is **inside**
`sub_000E0350` (`0x000E0350..0x000E0547`), not at its start. Lifted functions
are host C functions, and the dispatch table maps guest function *entries* to
them. There is no way to enter a lifted function in the middle, so the jump is
refused, the pump returns having done nothing, and the wait loop in
`sub_001205B0` spins forever on a state that can never advance.

`mov ss, word ptr [0x2d4d60]` is worth noting separately: the title reloads
the stack segment. The Xbox runs flat in ring 0 so it is a no-op in effect,
but it is a reminder that this code is not playing by the ABI at all.

## What does not fix it

**Seeding.** `0x000E0535` is mid-function — `tools.seed_from_log` rejects it
correctly, with "restores ebp without saving it: mid-function". Three rounds
went into seeding before the diagnostic distinguished a refused jump from a
refused call, which is why that distinction now exists (see below).

**Taking the other wait path.** `sub_001205B0` waits two ways: with the
loading-screen object present it uses the coroutine pump, and without it an
ordinary spin/Sleep/pump loop that needs no stack switching. Forcing the
second path (`RECOMP_MK_PLAIN_WAIT=1`, a title-local switch) does not help —
the refusals continue at the same rate, because the coroutine is how the
loader runs, not an optional decoration on the wait.

## What would fix it

In rough order of cost.

1. **Alternate entry points.** Let the recompiler emit, for a function that
   is the target of a mid-function indirect jump, an additional entry that
   jumps straight to that label, and register it in the dispatch table. This
   makes the transfer expressible. It does not by itself make it *correct*:
   the host C stack still unwinds on its own schedule while the guest's does
   not, so a switch that never comes back leaks a host frame per switch.

2. **Host fibers.** Map each guest context to a host fiber and make the
   stack-switch idiom a runtime call. Correct, and needs the idiom to be
   recognised — which is per-title work unless the pattern is detected
   generically (`pop [mem]` of a return address is a strong signal).

3. **A dispatch loop instead of C calls.** The general answer: lift basic
   blocks and drive them from a loop keyed on the guest program counter, so
   any address is a legal destination. This is what makes arbitrary guest
   control flow work, and it is a large change that interacts with the
   register-model item already in the plan.

None of these is a Mortal Kombat fix. A title that hand-rolls a loading
coroutine is not rare, and the same machinery answers `longjmp`, unrecovered
switch arms and any other jump into the middle of a function.

## The diagnostic change this produced

The runtime reported both of these the same way:

```
[ICALL] Failed to resolve VA 0x0019423E ...
[ICALL] Failed to resolve VA 0x000E0535 ...
```

They are different problems. The first is a call to an address nothing
identified — a discovery gap, answered by seeding. The second is a jump, and
seeding cannot answer it. The dispatch macros now publish which form was
refused and the log says so:

```
[ICALL] unresolved call target 0x0019423E -- 1 time(s) (total calls: 35072)
  callers: 0x0018415B <- 0x001A2472 <- ...
[ICALL] unresolved jump target 0x000E0535 -- 1 time(s) (total calls: 73191)
  a jump, not a call: if this address is inside a function rather than at its
  start, the guest is doing its own control flow (a coroutine, a longjmp, or a
  switch arm) and seeding it as a function will not help
  callers: 0x002E6D50 <- 0x000E031C <- ...
```

## How far it gets, and what was fixed on the way

Two toolkit bugs were found reaching this point, both of which affected every
title, not just this one.

**`NtFreeVirtualMemory` never freed anything.** The allocating bridge hands
out guest VAs from `xbox_HeapAlloc`; the freeing side called the host path,
which does `VirtualFree` on that address. Guest RAM is a `CreateFileMapping`
view and `VirtualFree` cannot release memory inside one, so it failed every
time and returned `STATUS_UNSUCCESSFUL`. The XDK's `RtlFreeHeap` frees a large
block through `NtFreeVirtualMemory` and reports FALSE when it fails, so the
CRT's `free()` silently did nothing for any block big enough to take that
path. MKDA sizes its heaps by allocating 29 MB, freeing it and allocating it
again; the second allocation returned NULL and every heap it built was empty.
Its own log said so, once the title's logger was hooked:

```
==> allocated SystemHeap size: 29695.88 K
Out of RAM                                    x17
==>> allocated OVERFLOW_HEAP size:   0.00 K
```

**`recomp_manual.c` could not read the guest registers.** `g_eax` and `g_esp`
are defined `__declspec(thread)` and were declared there as plain externs.
That links, and reads zero. The file whose job is to explain a refused
indirect call had no access to the guest stack pointer, which is why its log
could never name a caller.

## Reading the title's own diagnostics

MKDA has a logger at `sub_00157650`: `vsprintf` into a fixed buffer at
`0x002F4890`, then an emit that goes nowhere here. Hooking it after the format
and printing the buffer turns every diagnosis the title makes about itself
into a line on stderr, and is what identified the heap failure in one run
after a day of reading disassembly. Worth looking for in any title — the
shape is a function that ends in `vsprintf` to a fixed address.
