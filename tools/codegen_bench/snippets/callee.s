# Synthetic guest code for tools/codegen_bench. The leaf caller.s calls: three loads through an argument pointer.
.intel_syntax noprefix
.code32
.text
    mov ecx, dword ptr [esp+4]
    mov eax, dword ptr [ecx]
    add eax, dword ptr [ecx+4]
    imul eax, dword ptr [ecx+8]
    ret
