# Synthetic guest code for tools/codegen_bench. An x87 dot-product loop: the stack lives in TLS (g_fp_stack/g_fp_top).
.intel_syntax noprefix
.code32
.text
    mov eax, dword ptr [esp+4]
    mov ecx, dword ptr [esp+8]
    fldz
loop_top:
    fld dword ptr [eax]
    fmul dword ptr [eax+4]
    faddp st(1), st(0)
    add eax, 8
    dec ecx
    jnz loop_top
    fsqrt
    mov eax, dword ptr [esp+12]
    fstp dword ptr [eax]
    ret
