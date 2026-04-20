; ISR x86-64 (win64 COFF) — marco alineado; iretq. -mno-red-zone en C evita choque con red zone.

DEFAULT REL
section .text

extern timer_body
extern keyboard_body
extern mouse_body
extern irq_stub_body

global timer_handler
global keyboard_handler
global mouse_handler
global irq_stub
global isr_halt

%macro ISR_NOERR 1
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    sub  rsp, 32
    call %1
    add  rsp, 32
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rax
    iretq
%endmacro

timer_handler:
    ISR_NOERR timer_body

keyboard_handler:
    ISR_NOERR keyboard_body

mouse_handler:
    ISR_NOERR mouse_body

irq_stub:
    ISR_NOERR irq_stub_body

isr_halt:
    cli
.loop:
    hlt
    jmp .loop
