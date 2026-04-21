; Entrada SYSCALL x86-64 (MSR LSTAR). ABI Linux: RAX=nr, RDI,RSI,RDX,R10,R8,R9.
; RCX=user RIP, R11=user RFLAGS (enmascarados por IA32_FMASK).

DEFAULT REL

section .note.GNU-stack noalloc noexec nowrite progbits

section .bss
align 16
syscall_stack_bottom:
    resb 16384
syscall_stack_top:

section .text

global syscall_entry
extern syscall_dispatch_c

syscall_entry:
    cli
    mov r12, rsp
    mov rsp, syscall_stack_top
    push r12
    push rcx
    push r11

    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rax

    sti
    mov rdi, rsp
    call syscall_dispatch_c
    cli

    add rsp, 8 * 7

    pop r11
    pop rcx
    pop r12
    mov rsp, r12
    sysret
