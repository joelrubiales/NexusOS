; LEGACY — MBR + INT 13h (no usado con GRUB/Multiboot2).
; Sustituido por boot.S + GRUB. Conservado solo como referencia o `make legacy-img`.
[BITS 16]
[ORG 0x7C00]
; MBR 512 B: carga boot2 @ 0x7E00.
;
; Cadena gráfica (boot2_legacy_nasm.asm):
;   • INT 10h 4F02/4F01: prueba modos 118h/115h/112h (1024×768, 800×600, 640×480) 32 bpp LFB.
;   • BootInfo @ 0x5000: magic 0x1337BEEF, w, h, pitch, bpp, lfb_ptr (64-bit).
;   • Fallo VESA: 'V' roja en 0xB8000.
;   • Long mode: RDI = 0x5000 → kernel_main(BootInfo*); mapa identidad 2 MiB cubre LFB típico.
;
%define SPT  18
%define HDS  2
%define KSEC 200
%define BOOT2_SECTORS 4
%define KERNEL_FIRST_SECTOR 5

start:
    cli
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov sp,0x7C00
    sti
    mov [drv],dl

    mov di,0x5000
    mov cx,8
    rep stosd
    mov di,0x500
    mov cx,16
    rep stosw

    mov ax,0x07C0
    mov es,ax
    mov bx,0x0200
    mov ah,0x02
    mov al,BOOT2_SECTORS
    mov ch,0
    mov cl,2
    mov dh,0
    mov dl,[drv]
    int 0x13
    jc .hg

    mov ax,0x1000
    mov es,ax
    xor bx,bx
    mov bp,KERNEL_FIRST_SECTOR
    mov si,KSEC
.rl:cmp si,0
    je .go2
    mov ax,bp
    xor dx,dx
    mov cx,SPT*HDS
    div cx
    push ax
    mov ax,dx
    xor dx,dx
    push bx
    mov bx,SPT
    div bx
    pop bx
    mov dh,al
    mov cl,dl
    inc cl
    pop ax
    mov ch,al
    mov ah,2
    mov al,1
    mov dl,[drv]
    push es
    int 0x13
    pop es
    jc .hg
    mov ax,es
    add ax,0x20
    mov es,ax
    inc bp
    dec si
    jmp .rl
.go2:
    jmp 0x0000:0x7E00
.hg:cli
    hlt

drv:db 0
times 510-($-$$) db 0
dw 0xAA55
