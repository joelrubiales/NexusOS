; void user_iretq_enter(uint64_t rip, uint64_t rsp, uint64_t rflags);
; SysV: RDI=rip, RSI=rsp, RDX=rflags
; Marco IRETQ (64-bit): RIP, CS, RFLAGS, RSP, SS (RSP actual ring-0 apunta al RIP).

DEFAULT REL

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

global user_iretq_enter

user_iretq_enter:
    cli
    push 0x2B              ; SS  (datos usuario)
    push rsi               ; RSP usuario
    push rdx               ; RFLAGS
    push 0x33              ; CS  (código 64 usuario, GDT 0x30|RPL3)
    push rdi               ; RIP
    iretq
