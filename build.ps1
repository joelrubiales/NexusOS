$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Find-Nasm {
    $cmd = Get-Command nasm -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Path }
    $fb = "C:\Users\rubio\AppData\Local\bin\NASM\nasm.exe"
    if (Test-Path $fb) { return $fb }
    throw "NASM no encontrado."
}

$nasm = Find-Nasm

Write-Host "== NexusOS — arranque oficial: GRUB Multiboot2 (ELF en 1 MiB) =="
Write-Host "   Este script ya NO genera disquete MBR (boot.asm eliminado)."
Write-Host "   En Windows, genera la ISO con WSL, por ejemplo:"
Write-Host '   wsl -e bash -lc "cd /mnt/c/Users/rubio/Desktop/NexusOS && make clean && make"'
Write-Host ""
Write-Host "   Opcional: compilar solo objetos PE si necesitas depurar (sin ISO):"
Write-Host ""

$CFLAGS = @(
    "-m64", "-O2", "-std=gnu11",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-fno-pie", "-fno-pic", "-fno-asynchronous-unwind-tables",
    "-mno-red-zone", "-mcmodel=large", "-mgeneral-regs-only",
    "-Wall", "-Wextra"
)

Write-Host "[1/3] Limpiando objetos PE de prueba..."
Remove-Item -ErrorAction SilentlyContinue *.o, kernel.tmp, kernel.bin, kernel.elf -Force

Write-Host "[2/3] gcc -c (C + boot.S)..."
$cs = @(
    "kernel.c", "idt.c", "pantalla.c", "teclado.c", "pci.c", "nic.c", "vga.c", "gfx.c", "gui.c",
    "font8x8.c", "mouse.c", "memory.c", "paging.c", "kmalloc.c", "disk.c", "multitasking.c", "vesa.c", "window.c",
    "desktop.c", "mouse_gui.c", "apps.c", "top_panel.c", "dock_icons.c", "icons_data.c"
)
& gcc @CFLAGS -c @cs
if ($LASTEXITCODE -ne 0) { throw "gcc C fallo." }
& gcc @CFLAGS -c boot.S -o boot.o
if ($LASTEXITCODE -ne 0) { throw "gcc boot.S fallo (¿GNU as disponible?)." }

Write-Host "[3/3] NASM isr/task_switch..."
& $nasm -f win64 isr.asm -o isr.o
if ($LASTEXITCODE -ne 0) { throw "NASM isr.asm fallo." }
& $nasm -f win64 task_switch.asm -o task_switch.o
if ($LASTEXITCODE -ne 0) { throw "NASM task_switch fallo." }

$ldElf = Get-Command "x86_64-w64-mingw32-ld" -ErrorAction SilentlyContinue
if (-not $ldElf) { $ldElf = Get-Command "ld" -ErrorAction SilentlyContinue }

if ($ldElf) {
    $objs = @(
        "boot.o", "kernel.o", "idt.o", "isr.o", "task_switch.o", "pantalla.o", "teclado.o",
        "pci.o", "nic.o", "vga.o", "gfx.o", "gui.o", "font8x8.o", "mouse.o", "memory.o", "paging.o", "kmalloc.o",
        "disk.o", "multitasking.o", "vesa.o", "window.o", "desktop.o", "mouse_gui.o", "apps.o", "top_panel.o",
        "dock_icons.o", "icons_data.o"
    )
    & $ldElf.Path -m elf_x86_64 -T linker.ld -nostdlib -static -o kernel.elf @objs 2>$null
    if ($LASTEXITCODE -eq 0 -and (Test-Path kernel.elf)) {
        Write-Host "   kernel.elf generado OK. Copia a WSL y ejecuta: grub-mkrescue ..."
    } else {
        Write-Host "   ld -m elf_x86_64 no disponible o fallo — usa WSL/Linux: make  →  NexusOS.iso"
    }
} else {
    Write-Host "   No se encontro ld ELF; usa WSL: make"
}

Write-Host ""
Write-Host "=== Fin (objetos .o listos; ISO = make en WSL/Linux) ==="
