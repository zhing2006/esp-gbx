"""Original illustrative artwork for UI previews; not a game ROM or test oracle."""

from PIL import Image, ImageDraw


def scene(system):
    """Create a small original scene at the emulated system's native size."""
    w, h = (240, 160) if system == "gba" else (160, 144)
    palette = (
        {"sky": "#B5E2E3", "cloud": "#F3EACB", "far": "#83B6A0",
         "hill": "#558C73", "grass": "#A0B875", "dark": "#315B51",
         "water": "#659EAF", "foam": "#BCE0D5", "road": "#D8C394",
         "wall": "#E9DDB9", "roof": "#AF634D", "sprite": "#7A4241"}
        if system != "gb" else
        {"sky": "#D7DFC2", "cloud": "#D7DFC2", "far": "#A1AD88",
         "hill": "#68745B", "grass": "#A1AD88", "dark": "#303F32",
         "water": "#68745B", "foam": "#D7DFC2", "road": "#D7DFC2",
         "wall": "#D7DFC2", "roof": "#68745B", "sprite": "#303F32"}
    )
    im = Image.new("RGB", (w, h), palette["sky"])
    d = ImageDraw.Draw(im)
    def rect(r, color):
        d.rectangle(r, fill=palette[color])
    horizon = h // 2 - 10
    for x, y in [(15, 23), (w-54, 16), (w//2-12, 36)]:
        rect((x, y, x+30, y+5), "cloud")
        rect((x+6, y-4, x+22, y+6), "cloud")
    for x in range(-25, w+60, 60):
        d.polygon([(x, horizon+10), (x+26, horizon-25), (x+59, horizon+10)], fill=palette["far"])
    d.polygon([(0,horizon+12), (28,horizon-4), (56,horizon+10),
               (86,horizon-9), (122,horizon+13), (w,horizon-1), (w,h), (0,h)], fill=palette["hill"])
    rect((0,horizon+19,w,h), "grass")
    river = w//2+12
    d.polygon([(river, horizon+19), (river+17,horizon+19), (river+8,h-45),
               (river+40,h), (river-2,h), (river-14,h-42)], fill=palette["water"])
    for y in range(horizon+26, h, 8):
        x = river + ((y*7)%13)-3
        rect((x,y,x+7,y), "foam")
    rect((0,h-33,w,h-22), "road")
    rect((river-18,h-36,river+29,h-20), "roof")
    for x in range(river-17,river+29,5):
        rect((x,h-36,x,h-20), "wall")
    for x, y in [(16,horizon+28), (w-25,horizon+23), (35,h-14), (w-15,h-6)]:
        rect((x-2,y-3,x+2,y+7), "roof")
        rect((x-8,y-13,x+8,y-2), "dark")
        rect((x-5,y-18,x+5,y-8), "hill")
        rect((x-3,y-18,x,y-16), "grass")
    hx, hy = w//4-8, horizon+26
    rect((hx,hy,hx+27,hy+25), "dark")
    rect((hx+2,hy+1,hx+25,hy+23), "wall")
    d.polygon([(hx-4,hy+2),(hx+13,hy-12),(hx+31,hy+2)], fill=palette["roof"])
    rect((hx+10,hy+12,hx+16,hy+25), "dark")
    rect((hx+4,hy+8,hx+8,hy+12), "water")
    px, py = w//2-17, h-30
    rect((px-3,py-9,px+3,py-6), "sprite")
    rect((px-2,py-5,px+2,py-2), "wall")
    rect((px-4,py-1,px+4,py+4), "sprite")
    rect((px-3,py+5,px-1,py+7), "dark")
    rect((px+1,py+5,px+3,py+7), "dark")
    for x in range(7,w-5,19):
        rect((x,h-9,x+1,h-7), "hill")
        rect((x+3,h-12,x+4,h-10), "hill")
    return im
