; x86-64: cambio de pila (callee-saved según SysV x64)

DEFAULT REL
section .text

global cpu_switch

; void cpu_switch(unsigned long long *old_sp_out, unsigned long long new_sp);
cpu_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov rax, rsp
    mov [rdi], rax
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
