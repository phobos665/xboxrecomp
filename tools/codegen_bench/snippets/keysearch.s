# Synthetic guest code for tools/codegen_bench. Shaped like OutRun 2's sub_00011000: binary search over keyframes with
# comiss/jb, then a scalar-SSE linear interpolation. ret 16.
.intel_syntax noprefix
.code32
.text
    push ebx
    push esi
    push edi
    mov esi, dword ptr [esp+16]
    mov edi, dword ptr [esp+20]
    movss xmm4, dword ptr [esp+24]
    xor ebx, ebx
    lea edx, [edi-1]
loop_top:
    cmp ebx, edx
    jge done
    lea eax, [ebx+edx+1]
    sar eax, 1
    mov ecx, eax
    shl ecx, 4
    add ecx, esi
    add ecx, 16
    comiss xmm4, dword ptr [ecx-16]
    jb below
    mov ebx, eax
    jmp loop_top
below:
    lea edx, [eax-1]
    jmp loop_top
done:
    mov ecx, ebx
    shl ecx, 4
    add ecx, esi
    movss xmm0, dword ptr [ecx+16]
    subss xmm0, dword ptr [ecx]
    movss xmm1, xmm4
    subss xmm1, dword ptr [ecx]
    divss xmm1, xmm0
    mov eax, dword ptr [esp+28]
    movss xmm2, dword ptr [ecx+20]
    subss xmm2, dword ptr [ecx+4]
    mulss xmm2, xmm1
    addss xmm2, dword ptr [ecx+4]
    movss dword ptr [eax], xmm2
    movss xmm2, dword ptr [ecx+24]
    subss xmm2, dword ptr [ecx+8]
    mulss xmm2, xmm1
    addss xmm2, dword ptr [ecx+8]
    movss dword ptr [eax+4], xmm2
    movss xmm2, dword ptr [ecx+28]
    subss xmm2, dword ptr [ecx+12]
    mulss xmm2, xmm1
    addss xmm2, dword ptr [ecx+12]
    movss dword ptr [eax+8], xmm2
    pop edi
    pop esi
    pop ebx
    ret 16
