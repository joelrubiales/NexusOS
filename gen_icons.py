# Generates icons_embed.c — run: python gen_icons.py > icons_embed.c (optional)
W = 32

def pack(r, g, b, a):
    return ((a & 255) << 24) | ((r & 255) << 16) | ((g & 255) << 8) | (b & 255)

def globe():
    o = []
    for y in range(W):
        for x in range(W):
            cx, cy = 15.5, 15.5
            dx, dy = x - cx, y - cy
            d = (dx * dx + dy * dy) ** 0.5
            if d > 15.2:
                o.append(pack(0, 0, 0, 0))
                continue
            t = d / 15.2
            r = int(25 + 80 * (1 - t))
            g = int(90 + 100 * t)
            b = int(200 + 40 * (1 - t))
            if ((x - 10) ** 2 + (y - 12) ** 2) < 36 or ((x - 20) ** 2 + (y - 18) ** 2) < 30:
                r, g, b = 70, 160, 90
            a = 255 if d < 14.5 else int(255 * max(0.0, (15.2 - d) / 0.7))
            o.append(pack(r, g, b, a))
    return o

def folder():
    o = []
    for y in range(W):
        for x in range(W):
            tab = y >= 6 and y < 12 and x >= 4 and x < 22
            body = y >= 10 and y < 26 and x >= 4 and x < 28
            if not (tab or body):
                o.append(pack(0, 0, 0, 0))
                continue
            if tab:
                r, g, b = 255, 200, 80
            else:
                r, g, b = 70, 130, 230
                if x > 6 and x < 26 and y > 14 and y < 24:
                    r, g, b = 90, 150, 245
            edge = min(x - 4, 28 - x, y - 10, 26 - y)
            a = 255 if edge > 2 else max(0, (edge * 128) // 2)
            o.append(pack(r, g, b, min(255, a + 100)))
    return o

def terminal():
    o = []
    for y in range(W):
        for x in range(W):
            if x < 3 or x > 28 or y < 3 or y > 28:
                o.append(pack(0, 0, 0, 0))
                continue
            r, g, b = 22, 24, 32
            if x < 5 or x > 26 or y < 6 or y > 25:
                r, g, b = 40, 42, 55
            if x >= 7 and x <= 24 and y >= 9 and y <= 22:
                r, g, b = 12, 14, 20
            if y >= 14 and y <= 17 and x >= 9 and x <= 20:
                r, g, b = 60, 220, 120
            o.append(pack(r, g, b, 255))
    return o

if __name__ == "__main__":
    for name, fn in [("icon_rgba_globe", globe), ("icon_rgba_folder", folder), ("icon_rgba_terminal", terminal)]:
        arr = fn()
        print("static const unsigned int %s[%d] = {" % (name, len(arr)))
        for i in range(0, len(arr), 8):
            print("  " + ", ".join("0x%08X" % v for v in arr[i : i + 8]) + ",")
        print("};")
