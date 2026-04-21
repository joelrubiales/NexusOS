; Trampoline en una sola sección: smp_params queda en VMA 0x8FE0 vía relleno.

        CPU     X64

        SECTION .text
        GLOBAL  trampoline_entry
        GLOBAL  smp_params

        BITS    16
trampoline_entry:
        cli
        xor     ax, ax
        mov     ds, ax
        lgdt    [gdtr16]

        mov     eax, dword [smp_params + 4]
        mov     cr3, eax

        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax

        jmp     0x08:pm32

        BITS    32
pm32:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     esp, 0x7C00

        mov     ecx, 0xC0000080
        rdmsr
        or      eax, (1 << 8) | (1 << 11)
        wrmsr

        mov     eax, cr4
        or      eax, 1 << 5
        mov     cr4, eax

        mov     eax, cr0
        or      eax, 1 << 31
        mov     cr0, eax

        jmp     0x18:lm64

        BITS    64
lm64:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        xor     ax, ax
        mov     fs, ax
        mov     gs, ax

        mov     rax, qword [smp_params + 8]
        mov     rsp, rax
        mov     rax, qword [smp_params + 16]
        jmp     rax

gdtr16:
        dw      gdt_end - gdt0 - 1
        dd      gdt0

gdt0:
        dq      0
        dq      0x00CF9A000000FFFF
        dq      0x00CF92000000FFFF
        dq      0x00209A0000000000
gdt_end:

        times (0x8FE0 - 0x8000 - ($ - trampoline_entry)) db 0

smp_params:
        dd      0
        dd      0
        dq      0
        dq      0

        times (0x9000 - 0x8000 - ($ - trampoline_entry)) db 0
