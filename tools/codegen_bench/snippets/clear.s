# Synthetic guest code for tools/codegen_bench. Shaped like OutRun 2's sub_000A5230 (docs/technical/lifted-code-quality-review.md):
# a buffer clear by rep stosd + rep stosb, then a 32-dword constant clear.
.intel_syntax noprefix
.code32
.text
    mov edx, dword ptr [0x4e7e34]
    mov ecx, dword ptr [0x4e7e38]
    push edi
    mov edi, dword ptr [edx]
    mov edx, ecx
    shr ecx, 2
    xor eax, eax
    rep stosd
    mov ecx, edx
    and ecx, 3
    rep stosb
    mov edi, 0x4e7f00
    mov ecx, 32
    rep stosd
    pop edi
    ret
