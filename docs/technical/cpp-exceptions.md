# C++ exceptions: the throw is not implemented, and it used to be silent

Found 20 September 2026, at the end of the chain that started with Outrun 2's
null `std::map`. It is the reason that title still does not render, and it
will stop any title that throws.

## What happens

MSVC compiles `throw x` into a call to `_CxxThrowException(&object, &throwinfo)`,
which never returns. Because it never returns, the compiler puts an `int3`
after it as a guard.

This runtime has no C++ exception support. The throw helper is lifted like any
other function, the call inside it does nothing useful, and control falls
through to the `int3`. The lifter used to emit that as a comment:

```c
    /* int3: debug-trap slide byte, stepped over */
```

So the throw returned. Execution carried on from the middle of a function that
was never meant to continue, with a stack nothing had unwound and callee-saved
registers nobody restored. Everything after that is wreckage, and none of it
mentions an exception.

## In Outrun 2

`sub_001C425B` ends its 250 lines with a tail jump into `sub_001BB79F`, which
is the throw:

```c
    PUSH32(esp, ecx);
    PUSH32(esp, 0x285FC0);      /* the _ThrowInfo */
    eax = ebp + -1;
    PUSH32(esp, eax);           /* the exception object, one byte */
    MEM8(ebp + -1) = 0x21;
    call sub_001838A6           /* _CxxThrowException(obj, info) */
    /* int3 */
```

The descriptor at 0x285FC0 decodes cleanly as a `_ThrowInfo`: attributes 0, no
unwind function, and a catchable-type array at 0x285FB8 holding one entry
whose `type_info` name is `.D`. In MSVC's mangling `.D` is **`char`**. The
object is one byte and it is set to 0x21.

So Outrun 2 throws `(char)0x21` during start-up. A one-byte sentinel like that
is a control-flow throw, not a crash: the title expects to catch it somewhere
and carry on. We neither throw nor catch it, so it does neither.

Everything downstream is explained by this. The heap walk that faults reading
0xFF5007E2 is `sub_000CD040` reading its argument from a stack slot that holds
a return address, because the stack was left where the throw abandoned it.
0xFF5007E2 is not a pointer at all; it is four bytes of instruction encoding,
which is why it appears 34 times in the image.

## What changed

The trap is still stepped over. A real `__debugbreak()` there kills the process
with `STATUS_BREAKPOINT`, exit code 3 and no message, which is how Wreckless
died after a full boot. But it now says so, once per address:

```
[INT3] lifted code reached a debug trap at 0x001BB7B5 and stepped over it.
       MSVC emits one after a call it thinks cannot return, so this is
       usually a C++ throw that this runtime did not unwind. Execution
       continues on a stack nothing cleaned up, so treat anything odd
       after this line as a consequence, not a new bug.
```

The `int3` after a kernel `int 0x2d` debug print is the kernel's slide byte and
is filtered out at run time, so it stays quiet. Padding between functions is
never executed and never reported. What is left is traps the title genuinely
reached.

## What it would take to support throws

Not started. The pieces, roughly in order:

1. **`_CxxThrowException` as an HLE entry point.** Identify it by signature —
   it is CRT boilerplate, so it is per-XDK, the same kind of problem as the
   SEH prologues in `docs/technical/eh-prolog-frames.md`.
2. **Walk the exception registration chain.** `fs:[0]` already holds it: the
   SEH prologue links a record on every function with a try block, and this
   runtime already models that well enough for the frame pointers to be right.
3. **Match the thrown type against each frame's catch table**, using the
   `_ThrowInfo` and the funclet tables MSVC emits.
4. **Transfer control to the catch block.** This is the hard part. Each lifted
   function is a real C function, so resuming inside one after abandoning the
   frames between means moving the native stack as well as the guest one. The
   lifter already has this problem for `longjmp` and solves it there; that code
   is the place to start.

Step 1 alone is worth doing before any of the rest, because it turns "the title
threw and we carried on" into "the title threw and we stopped", which is a
diagnosable failure instead of silent corruption.

## The general lesson

A throw that does nothing is worse than a throw that crashes. Three separate
investigations in this title — a null map pointer, garbage assembled from
overlapping stores, and a corrupt heap walk — were all the same missing
unwind, seen from three different distances. Wherever the runtime cannot do
something the title asked for, it should say so at the point of the request,
not let the consequences surface somewhere unrelated.
