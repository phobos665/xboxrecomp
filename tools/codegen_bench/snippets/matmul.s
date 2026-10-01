# Synthetic guest code for tools/codegen_bench. Shaped like OutRun 2's sub_000F7487: 4x4 matrix product in packed SSE,
# 16 mulps + 12 addps, three stack arguments, ret 12.
.intel_syntax noprefix
.code32
.text
    mov eax, dword ptr [esp+8]
    mov ecx, dword ptr [esp+12]
    mov edx, dword ptr [esp+4]
    movaps xmm0, xmmword ptr [eax+0]
    shufps xmm0, xmm0, 0
    mulps xmm0, xmmword ptr [ecx+0]
    movaps xmm1, xmmword ptr [eax+0]
    shufps xmm1, xmm1, 85
    mulps xmm1, xmmword ptr [ecx+16]
    movaps xmm2, xmmword ptr [eax+0]
    shufps xmm2, xmm2, 170
    mulps xmm2, xmmword ptr [ecx+32]
    movaps xmm3, xmmword ptr [eax+0]
    shufps xmm3, xmm3, 255
    mulps xmm3, xmmword ptr [ecx+48]
    addps xmm0, xmm1
    addps xmm2, xmm3
    addps xmm0, xmm2
    movaps xmmword ptr [edx+0], xmm0
    movaps xmm0, xmmword ptr [eax+16]
    shufps xmm0, xmm0, 0
    mulps xmm0, xmmword ptr [ecx+0]
    movaps xmm1, xmmword ptr [eax+16]
    shufps xmm1, xmm1, 85
    mulps xmm1, xmmword ptr [ecx+16]
    movaps xmm2, xmmword ptr [eax+16]
    shufps xmm2, xmm2, 170
    mulps xmm2, xmmword ptr [ecx+32]
    movaps xmm3, xmmword ptr [eax+16]
    shufps xmm3, xmm3, 255
    mulps xmm3, xmmword ptr [ecx+48]
    addps xmm0, xmm1
    addps xmm2, xmm3
    addps xmm0, xmm2
    movaps xmmword ptr [edx+16], xmm0
    movaps xmm0, xmmword ptr [eax+32]
    shufps xmm0, xmm0, 0
    mulps xmm0, xmmword ptr [ecx+0]
    movaps xmm1, xmmword ptr [eax+32]
    shufps xmm1, xmm1, 85
    mulps xmm1, xmmword ptr [ecx+16]
    movaps xmm2, xmmword ptr [eax+32]
    shufps xmm2, xmm2, 170
    mulps xmm2, xmmword ptr [ecx+32]
    movaps xmm3, xmmword ptr [eax+32]
    shufps xmm3, xmm3, 255
    mulps xmm3, xmmword ptr [ecx+48]
    addps xmm0, xmm1
    addps xmm2, xmm3
    addps xmm0, xmm2
    movaps xmmword ptr [edx+32], xmm0
    movaps xmm0, xmmword ptr [eax+48]
    shufps xmm0, xmm0, 0
    mulps xmm0, xmmword ptr [ecx+0]
    movaps xmm1, xmmword ptr [eax+48]
    shufps xmm1, xmm1, 85
    mulps xmm1, xmmword ptr [ecx+16]
    movaps xmm2, xmmword ptr [eax+48]
    shufps xmm2, xmm2, 170
    mulps xmm2, xmmword ptr [ecx+32]
    movaps xmm3, xmmword ptr [eax+48]
    shufps xmm3, xmm3, 255
    mulps xmm3, xmmword ptr [ecx+48]
    addps xmm0, xmm1
    addps xmm2, xmm3
    addps xmm0, xmm2
    movaps xmmword ptr [edx+48], xmm0
    ret 12
