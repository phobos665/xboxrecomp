# Synthetic guest code for tools/codegen_bench. A call-heavy loop: one direct call per iteration (callee.s), cdecl.
.intel_syntax noprefix
.code32
.text
    push esi
    push edi
    mov esi, dword ptr [esp+12]
    mov edi, dword ptr [esp+16]
loop_top:
    push esi
    call 0x00090100
    add esp, 4
    add dword ptr [edi], eax
    add esi, 12
    dec dword ptr [esp+20]
    jnz loop_top
    pop edi
    pop esi
    ret
