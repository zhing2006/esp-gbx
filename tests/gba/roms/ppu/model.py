"""Expected frames of the GBX PPU test scenes, computed from GBATEK.

This is a line-by-line model of the GBA PPU written from GBATEK alone, used to
check the reference frames of the scenes in scenes.py: a reference frame from
the reference emulator is accepted only when it equals the frame computed
here. Each rule cites the GBATEK section it comes from.

The model covers only what the scenes use. It rejects scene settings whose
result GBATEK leaves open (see Scene.check), so that a scene never depends on
a guess:
- per-line writes to the affine reference point while PB or PD is not zero,
  because GBATEK "LCD I/O BG Rotation/Scaling" does not say whether the
  write or the end-of-line increment comes first;
- overlapping OBJs whose priority to BG decreases with the OAM index, which
  GBATEK "LCD OBJ - OAM Attributes" calls garbage;
- OBJ mosaic on OBJs that are not aligned to the mosaic grid;
- color effects whose result needs rounding (see blend()).

Frames are lists of 160 rows of 240 BGR555 values.
"""

WHITE = 0x7FFF

# GBATEK "LCD OBJ - OAM Attributes", OBJ Attribute 1: size by shape and size.
OBJ_SIZES = {
    0: ((8, 8), (16, 16), (32, 32), (64, 64)),
    1: ((16, 8), (32, 8), (32, 16), (64, 32)),
    2: ((8, 16), (8, 32), (16, 32), (32, 64)),
}


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s28(v):
    v &= 0x0FFFFFFF
    return v - 0x10000000 if v & 0x08000000 else v


class Regs:
    """I/O register state as halfwords, addressed by offset from 4000000h."""

    def __init__(self, io):
        self.h = {}
        for addr, (value, width) in io.items():
            self.write(addr, value, width)

    def write(self, addr, value, width):
        off = addr - 0x04000000
        self.h[off] = value & 0xFFFF
        if width == 4:
            self.h[off + 2] = (value >> 16) & 0xFFFF

    def copy(self):
        r = Regs({})
        r.h = dict(self.h)
        return r

    def __getitem__(self, off):
        return self.h.get(off, 0)

    def word(self, off):
        return self[off] | (self[off + 2] << 16)


def u16(mem, addr):
    return mem[addr] | (mem[addr + 1] << 8)


def text_bg(scene, regs, n, line):
    """GBATEK "LCD I/O BG Control", "LCD I/O BG Scrolling", "LCD VRAM BG
    Screen Data Format (BG Map)", "LCD VRAM Character Data"."""
    cnt = regs[0x08 + 2 * n]
    hofs, vofs = regs[0x10 + 4 * n] & 0x1FF, regs[0x12 + 4 * n] & 0x1FF
    char_base, screen_base = ((cnt >> 2) & 3) * 0x4000, ((cnt >> 8) & 31) * 0x800
    color256, size = (cnt >> 7) & 1, cnt >> 14
    width, height = (256, 512, 256, 512)[size], (256, 256, 512, 512)[size]
    mh, mv = mosaic_bg(regs) if cnt & 0x40 else (1, 1)
    y = line - line % mv
    out = [None] * 240
    vram = scene.vram
    for x in range(240):
        mx = x - x % mh
        px, py = (mx + hofs) % width, (y + vofs) % height
        # SC0-SC3 (GBATEK "LCD I/O BG Control"): side by side for 512x256,
        # stacked for 256x512, 2x2 for 512x512.
        block = (0, px // 256, py // 256, px // 256 + 2 * (py // 256))[size]
        entry = u16(vram, screen_base + block * 0x800 + ((py % 256) // 8) * 64 + ((px % 256) // 8) * 2)
        tile, tx, ty = entry & 0x3FF, px % 8, py % 8
        if entry & 0x400:
            tx = 7 - tx
        if entry & 0x800:
            ty = 7 - ty
        if color256:
            addr = char_base + tile * 64 + ty * 8 + tx
            scene.check_bg_tile(addr)
            index = vram[addr]
        else:
            addr = char_base + tile * 32 + ty * 4 + tx // 2
            scene.check_bg_tile(addr)
            index = (vram[addr] >> (4 * (tx & 1))) & 15
            if index:
                index += (entry >> 12) * 16
        if index:
            out[x] = u16(scene.pram, index * 2)
    return out


def mosaic_bg(regs):
    """GBATEK "LCD I/O Mosaic Function"."""
    m = regs[0x4C]
    return (m & 15) + 1, ((m >> 4) & 15) + 1


def affine_bg(scene, regs, n, ref, mode, line, mosaic_ref):
    """GBATEK "LCD I/O BG Rotation/Scaling", "LCD VRAM Bitmap BG Modes"."""
    cnt = regs[0x08 + 2 * n]
    base = 0x20 + 0x10 * (n - 2)
    pa, pc = s16(regs[base]), s16(regs[base + 4])
    mh, mv = mosaic_bg(regs) if cnt & 0x40 else (1, 1)
    refx, refy = mosaic_ref if mv > 1 else ref
    out = [None] * 240
    vram, dispcnt = scene.vram, regs[0]
    frame = 0xA000 if (dispcnt & 0x10) and mode in (4, 5) else 0
    if mode in (1, 2):
        size = 128 << (cnt >> 14)
        char_base, screen_base = ((cnt >> 2) & 3) * 0x4000, ((cnt >> 8) & 31) * 0x800
        wrap = cnt & 0x2000
    for x in range(240):
        mx = x - x % mh
        tx, ty = (refx + pa * mx) >> 8, (refy + pc * mx) >> 8
        if mode in (1, 2):
            if wrap:
                tx, ty = tx % size, ty % size
            elif not (0 <= tx < size and 0 <= ty < size):
                continue
            tile = vram[screen_base + (ty // 8) * (size // 8) + tx // 8]
            addr = char_base + tile * 64 + (ty % 8) * 8 + tx % 8
            scene.check_bg_tile(addr)
            index = vram[addr]
            if index:
                out[x] = u16(scene.pram, index * 2)
        elif mode == 3:
            if 0 <= tx < 240 and 0 <= ty < 160:
                out[x] = u16(vram, (ty * 240 + tx) * 2) & 0x7FFF
        elif mode == 4:
            if 0 <= tx < 240 and 0 <= ty < 160:
                index = vram[frame + ty * 240 + tx]
                if index:
                    out[x] = u16(scene.pram, index * 2)
        elif mode == 5:
            if 0 <= tx < 160 and 0 <= ty < 128:
                out[x] = u16(vram, frame + (ty * 160 + tx) * 2) & 0x7FFF
    return out


def obj_line(scene, regs, line):
    """GBATEK "LCD OBJ - Overview", "LCD OBJ - OAM Attributes", "LCD OBJ - OAM
    Rotation/Scaling Parameters", "LCD OBJ - VRAM Character (Tile) Mapping".

    Returns per-pixel (color, priority, semi-transparent) or None, and the OBJ
    window mask. OBJs are drawn in OAM order and the first opaque pixel stays,
    which is the order GBATEK gives (OBJ0 above OBJ1-127); Scene.check rejects
    overlaps where a later OBJ has a higher priority to BG. Rendering stops at
    the first OBJ on the line whose cycles no longer fit the budget.
    """
    dispcnt = regs[0]
    budget = 954 if dispcnt & 0x20 else 1210
    one_dim = dispcnt & 0x40
    bitmap = (dispcnt & 7) >= 3
    omh, omv = ((regs[0x4C] >> 8) & 15) + 1, ((regs[0x4C] >> 12) & 15) + 1
    pixels = [None] * 240
    window = [False] * 240
    used = 0
    oam, vram = scene.oam, scene.vram
    for i in range(128):
        a0, a1, a2 = u16(oam, i * 8), u16(oam, i * 8 + 2), u16(oam, i * 8 + 4)
        affine = a0 & 0x100
        if not affine and a0 & 0x200:
            continue
        w, h = OBJ_SIZES[(a0 >> 14) & 3][a1 >> 14]
        bw, bh = (w * 2, h * 2) if affine and a0 & 0x200 else (w, h)
        y = a0 & 0xFF
        mosaic = a0 & 0x1000
        sy = line - line % omv if mosaic else line
        dy = (sy - y) & 0xFF
        if (line - y) & 0xFF >= bh:
            continue
        cost = 10 + 2 * bw if affine else w
        if used + cost > budget:
            break
        used += cost
        tile_name = a2 & 0x3FF
        if bitmap and tile_name < 512:
            continue
        color256 = a0 & 0x2000
        mode = (a0 >> 10) & 3
        x = a1 & 0x1FF
        if affine:
            group = (a1 >> 9) & 31
            pa, pb, pc, pd = (s16(u16(oam, group * 32 + 6 + 8 * k)) for k in range(4))
        for i_x in range(bw):
            sx = (x + i_x) & 0x1FF
            if sx >= 240:
                continue
            ix = i_x
            if mosaic:
                ix = (sx - sx % omh - x) & 0x1FF
            if affine:
                cx, cy = bw // 2, bh // 2
                tx = ((pa * (ix - cx) + pb * (dy - cy)) >> 8) + w // 2
                ty = ((pc * (ix - cx) + pd * (dy - cy)) >> 8) + h // 2
                if not (0 <= tx < w and 0 <= ty < h):
                    continue
            else:
                tx, ty = ix, dy
                if a1 & 0x1000:
                    tx = w - 1 - tx
                if a1 & 0x2000:
                    ty = h - 1 - ty
            if color256:
                if one_dim:
                    tile = tile_name + ((ty // 8) * (w // 8) + tx // 8) * 2
                else:
                    tile = (tile_name & ~1) + (ty // 8) * 32 + (tx // 8) * 2
                addr = 0x10000 + (tile & 0x3FF) * 32 + (ty % 8) * 8 + tx % 8
                index = vram[addr]
                color = u16(scene.pram, 0x200 + index * 2) if index else None
            else:
                if one_dim:
                    tile = tile_name + (ty // 8) * (w // 8) + tx // 8
                else:
                    tile = tile_name + (ty // 8) * 32 + tx // 8
                addr = 0x10000 + (tile & 0x3FF) * 32 + (ty % 8) * 4 + (tx % 8) // 2
                index = (vram[addr] >> (4 * (tx & 1))) & 15
                color = u16(scene.pram, 0x200 + ((a2 >> 12) * 16 + index) * 2) if index else None
            if color is None:
                continue
            if mode == 2:
                window[sx] = True
            elif pixels[sx] is None:
                pixels[sx] = (color, (a2 >> 10) & 3, mode == 1)
    return pixels, window


def in_window(regs, n, x, line):
    """GBATEK "LCD I/O Window Feature" gives X1..X2-1 and Y1..Y2-1, and reads
    X1 > X2 as X2 = 240. Tonc ("Graphic effects", 13.3.1) and gbadoc
    ("Windowing") say the window wraps around instead: [0, X2) and [X1,
    240), likewise for Y. The model follows Tonc and gbadoc, as do
    NanoBoyAdvance and mGBA; the user confirmed this in task 1-2."""
    hreg, vreg = regs[0x40 + 2 * n], regs[0x44 + 2 * n]
    x1, x2 = hreg >> 8, hreg & 0xFF
    y1, y2 = vreg >> 8, vreg & 0xFF
    inside_x = x1 <= x < x2 if x1 <= x2 else x >= x1 or x < x2
    inside_y = y1 <= line < y2 if y1 <= y2 else line >= y1 or line < y2
    return inside_x and inside_y


def blend(a, b, eva, evb):
    """GBATEK "LCD I/O Color Special Effects": I = MIN(31, I1st*EVA + I2nd*EVB)
    with the coefficients in 1/16 units. GBATEK does not say how the result
    is rounded, and no hardware measurement was found, so the scenes only
    use colors and coefficients whose results are whole numbers."""
    out = 0
    for shift in (0, 5, 10):
        c = ((a >> shift) & 31) * eva + ((b >> shift) & 31) * evb
        if c % 16:
            raise ValueError(f'alpha blend of {a:#06x} and {b:#06x} by {eva}/{evb} needs rounding')
        out |= min(31, c >> 4) << shift
    return out


def brighten(a, evy, up):
    """GBATEK "LCD I/O Color Special Effects": I + (31-I)*EVY or I - I*EVY,
    EVY in 1/16 units; whole results only, as for blend()."""
    out = 0
    for shift in (0, 5, 10):
        c = (a >> shift) & 31
        step = (31 - c) * evy if up else c * evy
        if step % 16:
            raise ValueError(f'brightness of {a:#06x} by {evy} needs rounding')
        out |= (c + step // 16 if up else c - step // 16) << shift
    return out


def render_line(scene, regs, line, refs, mosaic_refs):
    dispcnt = regs[0]
    if dispcnt & 0x80:
        return [WHITE] * 240
    mode = dispcnt & 7
    layers = {}
    valid = {0: (0, 1, 2, 3), 1: (0, 1, 2), 2: (2, 3), 3: (2,), 4: (2,), 5: (2,)}[mode]
    for n in valid:
        if not dispcnt & (0x100 << n):
            continue
        if mode == 0 or (mode == 1 and n < 2):
            layers[n] = text_bg(scene, regs, n, line)
        else:
            layers[n] = affine_bg(scene, regs, n, refs[n], mode, line, mosaic_refs[n])
    objs, objwin = obj_line(scene, regs, line) if dispcnt & 0x1000 else ([None] * 240, [False] * 240)
    any_window = dispcnt & 0xE000
    winin, winout = regs[0x48], regs[0x4A]
    bldcnt, bldalpha, bldy = regs[0x50], regs[0x52], regs[0x54]
    eva, evb = min(16, bldalpha & 31), min(16, (bldalpha >> 8) & 31)
    evy = min(16, bldy & 31)
    effect = (bldcnt >> 6) & 3
    backdrop = u16(scene.pram, 0) & 0x7FFF
    out = []
    for x in range(240):
        mask = 0x3F
        if any_window:
            if dispcnt & 0x2000 and in_window(regs, 0, x, line):
                mask = winin & 0x3F
            elif dispcnt & 0x4000 and in_window(regs, 1, x, line):
                mask = (winin >> 8) & 0x3F
            elif dispcnt & 0x8000 and objwin[x]:
                mask = (winout >> 8) & 0x3F
            else:
                mask = winout & 0x3F
        # GBATEK "LCD I/O BG Control" and "LCD OBJ - OAM Attributes",
        # Priority: lower value first, OBJ above a BG of the same priority,
        # BG0 above BG3.
        stack = []
        if objs[x] is not None and mask & 0x10:
            color, prio, semi = objs[x]
            stack.append((prio, 0, 4, color, semi))
        for n, pixels in layers.items():
            if pixels[x] is not None and mask & (1 << n):
                stack.append((regs[0x08 + 2 * n] & 3, 1, n, pixels[x], False))
        stack.sort()
        stack.append((4, 2, 5, backdrop, False))
        top, second = stack[0], stack[1] if len(stack) > 1 else None
        color = top[3]
        first_target = bldcnt & (1 << top[2])
        second_target = second is not None and bldcnt & (0x100 << second[2])
        if top[4] and second_target:
            # Semi-transparent OBJ: always alpha blended with a 2nd target
            # below it (GBATEK "LCD I/O Color Special Effects").
            color = blend(color, second[3], eva, evb)
        elif mask & 0x20 and first_target:
            if effect == 1 and second_target:
                color = blend(color, second[3], eva, evb)
            elif effect in (2, 3):
                color = brighten(color, evy, effect == 2)
        out.append(color)
    return out


def render(scene):
    """Renders the frame shown in every frame after the scene is loaded."""
    scene.check()
    frame = []
    tables = scene.line_values()
    regs_by_line = []
    regs = Regs(scene.io)
    regs.write(0x04000000, scene.dispcnt, 2)
    for line in range(160):
        r = regs.copy()
        for addr, width, values in tables[line]:
            r.write(addr, values, width)
        regs_by_line.append(r)
    # Internal reference points (GBATEK "LCD I/O BG Rotation/Scaling"):
    # copied from BGxX/BGxY at V-Blank and on every write, incremented by
    # PB/PD after each line.
    refs = {}
    for n in (2, 3):
        base = 0x28 + 0x10 * (n - 2)
        refs[n] = [s28(regs_by_line[0].word(base)), s28(regs_by_line[0].word(base + 4))]
    written = scene.reference_writes()
    mosaic_start = {n: tuple(refs[n]) for n in (2, 3)}
    for line in range(160):
        r = regs_by_line[line]
        for n in (2, 3):
            base = 0x28 + 0x10 * (n - 2)
            if line and (n, line) in written:
                refs[n] = [s28(r.word(base)), s28(r.word(base + 4))]
            mv = mosaic_bg(r)[1] if r[0x08 + 2 * n] & 0x40 else 1
            if line % mv == 0:
                mosaic_start[n] = tuple(refs[n])
        frame.append(render_line(scene, r, line, {n: tuple(refs[n]) for n in (2, 3)}, mosaic_start))
        for n in (2, 3):
            pbase = 0x20 + 0x10 * (n - 2)
            refs[n][0] += s16(r[pbase + 2])
            refs[n][1] += s16(r[pbase + 6])
    return frame
