#!/usr/bin/env python3
"""
gen_initrd.py — Generador de initrd.tar para NexusOS.

Crea:
  background.bmp     - Fondo de escritorio 256×192 (degradado espacial).
  icons/0.bmp        - Icono "Carpeta"   32×32
  icons/1.bmp        - Icono "Terminal"  32×32
  icons/2.bmp        - Icono "Navegador" 32×32
  icons/3.bmp        - Icono "Sistema"   32×32
  icons/4.bmp        - Icono "Info"      32×32
  icons/5.bmp        - Icono "Apagar"    32×32

Uso:
  python3 gen_initrd.py [output.tar]
  (Por defecto: iso/boot/initrd.tar)
"""

import sys, os, struct, math, tarfile, io

# ─── BMP 32-bit top-down ───────────────────────────────────────────────────
def make_bmp32(width: int, height: int, pixels_argb: list[int]) -> bytes:
    """
    pixels_argb: lista de ints 0xAARRGGBB, fila superior primero (top-down).
    Genera un BMP 32-bit con BITMAPINFOHEADER y height negativo (top-down).
    En memoria cada píxel se guarda como [B,G,R,A] (little-endian), que es
    exactamente el formato que el kernel lee como uint32 0xAARRGGBB. ✓
    """
    assert len(pixels_argb) == width * height, "pixel count mismatch"

    # Pixel data: cada píxel como [B,G,R,A]
    pd = bytearray(width * height * 4)
    for i, c in enumerate(pixels_argb):
        b  = c        & 0xFF
        g  = (c >> 8) & 0xFF
        r  = (c >>16) & 0xFF
        a  = (c >>24) & 0xFF
        pd[i*4+0] = b
        pd[i*4+1] = g
        pd[i*4+2] = r
        pd[i*4+3] = a

    file_size = 14 + 40 + len(pd)
    # File header
    fh = struct.pack('<2sIHHI', b'BM', file_size, 0, 0, 54)
    # BITMAPINFOHEADER (height negativo = top-down)
    dib = struct.pack('<IiiHHIIiiII',
        40, width, -height, 1, 32, 0, len(pd), 2835, 2835, 0, 0)
    return bytes(fh) + bytes(dib) + bytes(pd)


# ─── Paleta de colores del SO ──────────────────────────────────────────────
def rgb(r: int, g: int, b: int, a: int = 255) -> int:
    return (a << 24) | (r << 16) | (g << 8) | b

def lerp(a: int, b: int, t: float) -> int:
    return int(a + (b - a) * t)

def lerp_color(c0: int, c1: int, t: float) -> int:
    r = lerp((c0>>16)&0xFF, (c1>>16)&0xFF, t)
    g = lerp((c0>>8 )&0xFF, (c1>>8 )&0xFF, t)
    b = lerp( c0     &0xFF,  c1     &0xFF, t)
    return rgb(r, g, b)


# ─── Wallpaper 256×192 ────────────────────────────────────────────────────
def gen_wallpaper(w=256, h=192) -> bytes:
    """
    Degradado espacial: azul marino oscuro (arriba) → púrpura profundo (abajo).
    Estrellas semi-aleatorias para textura.
    """
    TOP   = rgb(8, 18, 52)
    BOT   = rgb(72, 34, 96)
    pixels = []
    # Pseudo-random sin módulo import (LCG simple)
    seed = 0xACE1
    def prng() -> int:
        nonlocal seed
        seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
        return seed

    for y in range(h):
        ty = y / (h - 1)
        for x in range(w):
            tx = x / (w - 1)
            # Degradado vertical + leve variación diagonal
            t = ty * 0.85 + tx * 0.15
            c = lerp_color(TOP, BOT, t)
            # Estrellas: 1 cada ~256 píxeles
            n = prng()
            if (n & 0xFF) == 0:
                br = 180 + ((n >> 8) & 75)
                c = rgb(br, br, br + ((n >> 16) & 20))
            pixels.append(c)

    return make_bmp32(w, h, pixels)


# ─── Generadores de iconos 32×32 ──────────────────────────────────────────
def icon_blank(c_fill: int) -> list[int]:
    return [c_fill] * (32 * 32)

def set_rect(px, x0, y0, x1, y1, c, w=32):
    for y in range(y0, y1):
        for x in range(x0, x1):
            if 0 <= x < w and 0 <= y < w:
                px[y * w + x] = c

def set_circle(px, cx, cy, r, c, w=32):
    for y in range(w):
        for x in range(w):
            if (x - cx)**2 + (y - cy)**2 <= r*r:
                px[y * w + x] = c

def make_icon_transparent() -> list[int]:
    return [rgb(0, 0, 0, 0)] * (32 * 32)

# 0 → Carpeta (amarillo-naranja)
def gen_icon_folder() -> bytes:
    px = make_icon_transparent()
    BODY  = rgb(220, 170,  40)
    TAB   = rgb(240, 200,  80)
    LINE  = rgb(180, 130,  20)
    # cuerpo de la carpeta
    set_rect(px,  2, 12, 30, 28, BODY)
    # pestaña superior
    set_rect(px,  2,  8, 14, 12, TAB)
    # borde inferior sutil
    set_rect(px,  2, 27, 30, 28, LINE)
    # líneas interiores simulando documentos
    set_rect(px,  6, 16, 26, 17, rgb(240, 220, 120, 160))
    set_rect(px,  6, 20, 26, 21, rgb(240, 220, 120, 120))
    return make_bmp32(32, 32, px)

# 1 → Terminal (negro con >_ )
def gen_icon_terminal() -> bytes:
    px = make_icon_transparent()
    BG     = rgb(18,  22,  38)
    BORDER = rgb(60,  70, 110)
    GREEN  = rgb(80, 220,  80)
    # fondo redondeado (simplificado: rect)
    set_rect(px,  2,  2, 30, 30, BG)
    set_rect(px,  2,  2, 30,  3, BORDER)
    set_rect(px,  2, 29, 30, 30, BORDER)
    set_rect(px,  2,  2,  3, 30, BORDER)
    set_rect(px, 29,  2, 30, 30, BORDER)
    # ">" prompt
    for i in range(5):
        set_rect(px,  6+i, 14-i, 7+i, 15-i, GREEN)
        set_rect(px,  6+i, 16+i, 7+i, 17+i, GREEN)
    # "_" cursor
    set_rect(px, 13, 19, 20, 21, GREEN)
    return make_bmp32(32, 32, px)

# 2 → Navegador (globo terráqueo azul)
def gen_icon_globe() -> bytes:
    px = make_icon_transparent()
    OCEAN = rgb( 30, 100, 200)
    LAND  = rgb( 60, 170,  60)
    GRID  = rgb( 20,  70, 160)
    # océano (círculo)
    set_circle(px, 16, 16, 13, OCEAN)
    # continentes (rectángulos simplificados)
    set_rect(px,  8, 10, 15, 18, LAND)
    set_rect(px, 18, 14, 25, 22, LAND)
    set_rect(px, 12,  8, 20, 12, LAND)
    # líneas de longitud/latitud
    set_rect(px,  3, 15, 29, 16, GRID)
    set_rect(px, 15,  3, 16, 29, GRID)
    return make_bmp32(32, 32, px)

# 3 → Sistema / Configuración (engranaje gris)
def gen_icon_system() -> bytes:
    px = make_icon_transparent()
    GEAR = rgb(150, 155, 170)
    DARK = rgb( 50,  55,  65)
    HUB  = rgb(190, 195, 210)
    # cuadrado exterior con muescas
    set_rect(px,  8,  2, 24,  8, GEAR)
    set_rect(px,  8, 24, 24, 30, GEAR)
    set_rect(px,  2,  8,  8, 24, GEAR)
    set_rect(px, 24,  8, 30, 24, GEAR)
    set_rect(px, 10, 10, 22, 22, GEAR)
    # agujero central
    set_circle(px, 16, 16, 5, DARK)
    # aro interno del agujero
    set_circle(px, 16, 16, 3, HUB)
    return make_bmp32(32, 32, px)

# 4 → Información (círculo violeta con "i")
def gen_icon_info() -> bytes:
    px = make_icon_transparent()
    RING = rgb(130,  70, 210)
    FILL = rgb(160, 100, 240)
    WHITE= rgb(240, 240, 255)
    set_circle(px, 16, 16, 13, RING)
    set_circle(px, 16, 16, 11, FILL)
    # punto de la "i"
    set_rect(px, 15,  9, 18, 12, WHITE)
    # palo de la "i"
    set_rect(px, 15, 14, 18, 24, WHITE)
    return make_bmp32(32, 32, px)

# 5 → Apagar (rojo con símbolo de power)
def gen_icon_power() -> bytes:
    px = make_icon_transparent()
    RED   = rgb(200,  55,  55)
    DKRED = rgb(150,  35,  35)
    WHITE = rgb(240, 240, 255)
    set_circle(px, 16, 18, 12, DKRED)
    set_circle(px, 16, 18, 10, RED)
    # arco superior abierto (arco de circunferencia simplificado)
    for angle_deg in range(40, 141):
        rad = math.radians(angle_deg - 90)
        for r in range(9, 13):
            ix = int(16 + r * math.cos(rad))
            iy = int(18 + r * math.sin(rad))
            if 0 <= ix < 32 and 0 <= iy < 32:
                px[iy * 32 + ix] = WHITE
    # línea vertical superior
    set_rect(px, 15,  5, 18, 17, WHITE)
    return make_bmp32(32, 32, px)


# ─── Archivos de sistema falsos para el instalador ────────────────────────
#
# Generan datos pseudo-aleatorios de tamaño realista para que el instalador
# tarde ~7 s al iterar el TAR a 2 KiB/frame × 30 fps ≈ 60 KiB/s.
# Total de payload de sistema: ~200 KiB + ~215 KiB (BMPs) = ~415 KiB.

def fake_bin(size: int) -> bytes:
    """Relleno cíclico 0-255; generación O(n/256), sin módulos externos."""
    chunk = bytes(range(256))
    return (chunk * (size // 256)) + bytes(range(size % 256))


SYSTEM_FILES: list[tuple[str, bytes]] = [
    # bin/
    ("bin/bash",                fake_bin(20_480)),
    ("bin/ls",                  fake_bin(8_192)),
    ("bin/cat",                 fake_bin(5_120)),
    ("bin/grep",                fake_bin(10_240)),
    ("bin/cp",                  fake_bin(6_144)),
    ("bin/mv",                  fake_bin(6_144)),
    ("bin/rm",                  fake_bin(5_120)),
    ("bin/mkdir",               fake_bin(4_096)),
    ("bin/chmod",               fake_bin(4_096)),
    ("bin/chown",               fake_bin(4_096)),
    # lib/
    ("lib/libc.so.6",           fake_bin(49_152)),
    ("lib/libm.so.6",           fake_bin(24_576)),
    ("lib/libpthread.so.0",     fake_bin(12_288)),
    ("lib/ld-linux-x86-64.so.2",fake_bin(8_192)),
    # etc/
    ("etc/fstab",
        b"# /etc/fstab\n"
        b"/dev/sda1  /      ext4  defaults  1 1\n"
        b"/dev/sda2  swap   swap  defaults  0 0\n"
        b"tmpfs      /tmp   tmpfs defaults  0 0\n"),
    ("etc/hostname",            b"nexusos\n"),
    ("etc/passwd",
        b"root:x:0:0:root:/root:/bin/nexusbash\n"
        b"user:x:1000:1000:User:/home/user:/bin/nexusbash\n"
        b"nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n"),
    ("etc/shadow",
        b"root:*:19000:0:99999:7:::\n"
        b"user:*:19000:0:99999:7:::\n"),
    ("etc/nexus_config",
        b"hostname=nexusos\n"
        b"user=user\n"
        b"lang=en_US.UTF-8\n"
        b"timezone=UTC+0\n"
        b"keyboard=us\n"
        b"shell=/bin/nexusbash\n"
        b"arch=x86_64\n"
        b"edition=gaming\n"
        b"desktop=nexus-desktop\n"
        b"version=3.0.0\n"),
    ("etc/grub/grub.cfg",
        b"set default=0\nset timeout=5\n"
        b'menuentry "NexusOS 3.0" {\n'
        b"  multiboot2 /boot/kernel.bin\n"
        b"  module2    /boot/initrd.tar\n"
        b"}\n"),
    # usr/
    ("usr/bin/python3",         fake_bin(24_576)),
    ("usr/bin/nano",            fake_bin(16_384)),
    ("usr/lib/nexus/core.so",   fake_bin(20_480)),
    # boot/
    ("boot/vmlinuz-nexus",      fake_bin(36_864)),
    ("boot/System.map",         fake_bin(4_096)),
    # var/
    ("var/log/install.log",
        b"[NexusOS Installer]\n"
        b"version=3.0.0\n"
        b"status=complete\n"),
]


# ─── Empaquetar en TAR ─────────────────────────────────────────────────────
def add_to_tar(tf: tarfile.TarFile, name: str, data: bytes):
    buf = io.BytesIO(data)
    info = tarfile.TarInfo(name=name)
    info.size = len(data)
    tf.addfile(info, buf)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "iso/boot/initrd.tar"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    icon_generators = [
        gen_icon_folder,
        gen_icon_terminal,
        gen_icon_globe,
        gen_icon_system,
        gen_icon_info,
        gen_icon_power,
    ]

    # USTAR (format=0) garantiza cabeceras de 512 bytes compatibles con el
    # parser de vfs.c. El formato PAX (default Python 3.8+) añade bloques
    # extendidos que complican el walker innecesariamente para archivos pequeños.
    with tarfile.open(out, "w:", format=tarfile.USTAR_FORMAT) as tf:
        # ── Archivos de sistema (aparecen primero en el log del instalador) ──
        total_sys = 0
        for name, data in SYSTEM_FILES:
            print(f"  → {name} ({len(data)} B) ...", end=" ", flush=True)
            add_to_tar(tf, name, data)
            total_sys += len(data)
            print("OK")

        # ── Assets gráficos del escritorio ──────────────────────────────────
        print(f"  → background.bmp (256×192) ...", end=" ", flush=True)
        add_to_tar(tf, "background.bmp", gen_wallpaper(256, 192))
        print("OK")

        for i, gen_fn in enumerate(icon_generators):
            name = f"icons/{i}.bmp"
            print(f"  → {name} (32×32) ...", end=" ", flush=True)
            add_to_tar(tf, name, gen_fn())
            print("OK")

    stat = os.stat(out)
    print(f"\nInitrd: {out}  ({stat.st_size // 1024} KiB)")
    print(f"  Sistema: {total_sys // 1024} KiB de archivos de sistema")


if __name__ == "__main__":
    main()
