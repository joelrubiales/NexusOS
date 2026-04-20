; ── sched_switch.asm : sched_timer_handler ──────────────────────────────
; Handler preventivo para IRQ0 (PIT, vector 32).
;
; Al entrar, la CPU ya ha empujado automáticamente (orden baja→alta addr):
;   SS | RSP | RFLAGS | CS | RIP      (5 × 8 = 40 bytes)
;
; Empujamos los 15 GPR en este orden (de forma que los pops al final sean
; el inverso y dejen los registros en su valor original):
;   push rax, rcx, rdx, rbx, rsi, rdi, rbp, r8-r15   (15 × 8 = 120 bytes)
;
; Total en pila antes del call: 160 bytes. Como el ABI SysV x86-64
; requiere RSP ≡ 0 (mod 16) en el punto de CALL, y 160 % 16 = 0,
; la pila está alineada correctamente.
;
; Prototipo C:
;   uint64_t sched_tick(uint64_t current_rsp);
;   → argumento en RDI, valor de retorno en RAX
;
; El handler pasa RSP como current_rsp y cambia RSP al valor devuelto.
; Si no hay cambio de tarea, RAX = RDI (mismo RSP).

DEFAULT REL
section .text

extern sched_tick
global sched_timer_handler

sched_timer_handler:
    ; ── Guardar todos los GPR del contexto interrumpido ──────────────
    push rax
    push rcx
    push rdx
    push rbx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; ── Llamar al scheduler C ─────────────────────────────────────────
    ; RDI = current_rsp (puntero al marco completo en la pila actual).
    ; RAX = nuevo RSP (puede ser el mismo si no hay cambio de tarea).
    mov  rdi, rsp
    call sched_tick
    mov  rsp, rax       ; cambiar a la pila de la siguiente tarea

    ; ── Restaurar GPR de la nueva tarea (o de la misma si no hubo switch) ──
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    pop rdx
    pop rcx
    pop rax

    ; ── Volver a la tarea (nueva o actual): iretq restaura RIP, CS, RFLAGS, RSP, SS ──
    iretq
