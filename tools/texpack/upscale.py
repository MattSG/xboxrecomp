"""Upscale dumped textures (fonts) for a RECOMP_TEX_PACK replacement pack.

    1. run with RECOMP_TEX_DUMP=texdump, visit the menus / HUD you care about
    2. copy the font atlases you want out of texdump (keep the file names --
       they are the texture's content hash)
    3. python tools/texpack/upscale.py texdump-picked texpack --scale 4
    4. run with RECOMP_TEX_PACK=texpack

Colour is resampled premultiplied (no dark fringes where the glyph meets
transparency). Alpha is resampled smoothly and then its edge is steepened to
--edge output pixels (colour edges too: font atlases keep coverage in
colour), so a glyph's outline comes out as a clean curve rather
than either blocks or blur. --edge 0 keeps the plain smooth alpha (for soft,
glow-like images).

Needs Pillow only. python upscale.py --selftest checks the maths.
"""
import argparse
import os
import sys

from PIL import Image, ImageChops, ImageMath


def upscale(im, scale, edge):
    im = im.convert("RGBA")
    size = (im.width * scale, im.height * scale)
    r, g, b, a = im.split()
    a_up = a.resize(size, Image.BICUBIC)
    rgb = [ImageChops.multiply(c, a).resize(size, Image.LANCZOS) for c in (r, g, b)]
    rgb = [ImageMath.lambda_eval(
        lambda x: x["convert"](x["min"](x["c"] * 255 / x["max"](x["a"], 1), 255), "L"),
        c=c, a=a_up) for c in rgb]
    if edge > 0:
        k = scale / edge          # transition: one source texel -> `edge` pixels
        lut = [max(0, min(255, round((v - 127.5) * k + 127.5))) for v in range(256)]
        # font atlases keep coverage in colour too (fill in blue, outline
        # opaque black), so the fill/outline edge is steepened like alpha's
        a_up = a_up.point(lut)
        rgb = [c.point(lut) for c in rgb]
    return Image.merge("RGBA", rgb + [a_up])


def selftest():
    # an opaque white diagonal stair on transparent black, 8x8
    src = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
    for y in range(8):
        for x in range(y + 1):
            src.putpixel((x, y), (255, 255, 255, 255))
    out = upscale(src, 4, 1.5)
    assert out.size == (32, 32)
    assert out.getpixel((2, 30))[3] == 255           # deep inside: opaque
    assert out.getpixel((30, 2))[3] == 0             # far outside: clear
    assert out.getpixel((2, 30))[:3] == (255, 255, 255)   # no dark fringe inside
    alphas = [out.getpixel((x, y))[3] for y in range(32) for x in range(32)]
    partial = sum(0 < v < 255 for v in alphas)
    assert 0 < partial < 32 * 4, partial              # a thin AA edge, not a blur
    soft = upscale(src, 4, 0)
    assert sum(0 < soft.getpixel((x, y))[3] < 255 for y in range(32) for x in range(32)) > partial
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?", help="folder (or one .png) of dumped textures")
    ap.add_argument("dst", nargs="?", help="pack folder to write")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--edge", type=float, default=1.5, help="alpha edge width in output pixels; 0 = smooth")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.src or not args.dst:
        ap.error("src and dst are required")
    files = [args.src] if os.path.isfile(args.src) else [
        os.path.join(args.src, f) for f in sorted(os.listdir(args.src)) if f.lower().endswith(".png")]
    os.makedirs(args.dst, exist_ok=True)
    for f in files:
        out = os.path.join(args.dst, os.path.basename(f))
        upscale(Image.open(f), args.scale, args.edge).save(out)
        print(out)


if __name__ == "__main__":
    sys.exit(main())
