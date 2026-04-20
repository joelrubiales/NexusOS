[BITS 16]
CPU X64
[ORG 0x7E00]
; LEGACY — INT 10h VESA. El kernel oficial enlaza en 1 MiB (Multiboot2); este stage2 saltaba a 0x10000.
; Conservado solo como referencia. Arranque: GRUB + boot.S.
; Tabla de páginas @ 0x70000 (mapa identidad 2 MiB).

%define PT                0x70000
%define BOOT_INFO         0x5000
%define TEMP_VBEINFO      0x6200

stage2:
    cli
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov sp,0x7C00
    sti

    mov di,modes_list

.probe_next:
    mov cx,[di]
    add di,2
    test cx,cx
    jz error_vesa_all

    mov bx,cx
    or  bx,0x4000
    mov ax,0x4F02
    int 0x10
    cmp ax,0x004F
    jne .probe_next

    push di
    xor ax,ax
    mov es,ax
    mov di,TEMP_VBEINFO
    mov ax,0x4F01
    int 0x10
    pop di
    cmp ax,0x004F
    jne .probe_next

    xor ax,ax
    mov es,ax
    cmp dword [es:TEMP_VBEINFO+0x28],strict dword 0
    je .probe_next
    cmp byte [es:TEMP_VBEINFO+0x19],strict byte 32
    jne .probe_next

    ; BootInfo @ 0x5000: magic, w, h, pitch, bpp, lfb_ptr (64-bit)
    mov dword [es:BOOT_INFO+0],  strict dword 0x1337BEEF
    movzx eax,word [es:TEMP_VBEINFO+0x12]
    mov dword [es:BOOT_INFO+4], eax
    movzx eax,word [es:TEMP_VBEINFO+0x14]
    mov dword [es:BOOT_INFO+8], eax
    movzx eax,word [es:TEMP_VBEINFO+0x32]
    test eax,eax
    jnz .pitch_ok
    movzx eax,word [es:TEMP_VBEINFO+0x10]
.pitch_ok:
    cmp eax,strict dword 4
    jb .probe_next
    mov dword [es:BOOT_INFO+12],eax
    mov dword [es:BOOT_INFO+16],strict dword 32
    mov eax,[es:TEMP_VBEINFO+0x28]
    mov dword [es:BOOT_INFO+20],eax
    mov dword [es:BOOT_INFO+24],strict dword 0

    jmp b_pm

modes_list:
    dw 0x143,0x141,0x140,0x118,0x115,0x112,0

error_vesa_all:
    mov ax,0xB800
    mov es,ax
    xor di,di
    mov word [es:di],0x0456
    cli
hang_vesa:
    hlt
    jmp hang_vesa

b_pm:
    cli
    lgdt [gd]
    mov eax,cr0
    or  al,1
    mov cr0,eax
    jmp 0x08:pm32

[BITS 32]
pm32:
    mov ax,0x10
    mov ds,ax
    mov ss,ax
    mov es,ax
    mov esp,0x90000
    mov edi,PT
    xor eax,eax
    mov ecx,6*4096/4
    rep stosd
    mov dword [PT],(PT+0x1000)|3
    mov dword [PT+4],0
    mov edi,PT+0x1000
    mov eax,(PT+0x2000)|3
    mov ecx,4
.lp:mov [edi],eax
    mov dword [edi+4],0
    add eax,0x1000
    add edi,8
    loop .lp
    mov edi,PT+0x2000
    xor ecx,ecx
.pd:mov eax,ecx
    shl eax,21
    or  eax,0x83
    stosd
    xor eax,eax
    stosd
    inc ecx
    cmp ecx,2048
    jb  .pd
    mov eax,cr4
    or  eax,1<<5
    mov cr4,eax
    mov eax,PT
    mov cr3,eax
    mov ecx,0xC0000080
    rdmsr
    or  eax,1<<8
    wrmsr
    mov eax,cr0
    or  eax,(1<<31)|1
    mov cr0,eax
    jmp 0x18:lm64

[BITS 64]
lm64:
    mov ax,0x10
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov rsp,0x90000
    mov rdi,strict qword BOOT_INFO
    mov rax,strict qword 0x10000
    jmp rax

gt:dq 0
    dw 0xFFFF,0,0x9A00,0x00CF
    dw 0xFFFF,0,0x9200,0x00CF
    dw 0,0,0x9A00,0x0020
ge:
gd:dw ge-gt-1
    dd gt
