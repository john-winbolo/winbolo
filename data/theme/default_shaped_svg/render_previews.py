"""Render comparison previews for the four sample tiles."""

from pathlib import Path
import io
import subprocess

from PIL import Image, ImageDraw, ImageFont

from pixel_to_svg import Params, process


SAMPLES = [
    "pillbox_good_12.png",
    "tank_evilboat_11.png",
    "base_evil.png",
    "building_sidecorn15.png",
]

# Two parameter sets to demo the tunables.
PARAM_SETS = {
    "default": Params(
        color_tolerance=0.0,
        black_threshold=32,
        black_merge=True,
        diagonal_connect=True,
        chamfer=0.35,
        curve_tension=0.5,
        scale=10,
        layer_order="luminance_asc",
        fatten=0.25,
        fatten_mode="stroke",
    ),
    "merged": Params(
        color_tolerance=60.0,
        black_threshold=32,
        black_merge=True,
        diagonal_connect=True,
        chamfer=0.35,
        curve_tension=0.5,
        scale=10,
        layer_order="luminance_asc",
        fatten=0.25,
        fatten_mode="stroke",
    ),
    "merged_dark_on_top": Params(
        color_tolerance=60.0,
        black_threshold=32,
        black_merge=True,
        diagonal_connect=True,
        chamfer=0.35,
        curve_tension=0.5,
        scale=10,
        layer_order="luminance_desc",
        fatten=0.25,
        fatten_mode="stroke",
    ),
    "merged_geometry_fatten": Params(
        color_tolerance=60.0,
        black_threshold=32,
        black_merge=True,
        diagonal_connect=True,
        chamfer=0.35,
        curve_tension=0.5,
        scale=10,
        layer_order="luminance_asc",
        fatten=0.25,
        fatten_mode="geometry",
    ),
    "merged_no_fatten": Params(
        color_tolerance=60.0,
        black_threshold=32,
        black_merge=True,
        diagonal_connect=True,
        chamfer=0.35,
        curve_tension=0.5,
        scale=10,
        layer_order="luminance_asc",
        fatten=0.0,
    ),
}

STYLES = ["crisp", "smoothed", "organic"]
TILE_SIZE = 160


def render_svg_to_png(svg: str, size: int) -> Image.Image:
    """Render via resvg-py (preferred, bundles native binary), then cairosvg, then rsvg-convert."""
    # Try resvg-py first - works out of the box on Windows.
    try:
        import resvg_py
        png_bytes = resvg_py.svg_to_bytes(
            svg_string=svg,
            width=size,
            height=size,
        )
        # resvg-py may return a list of ints in some versions; coerce.
        if isinstance(png_bytes, list):
            png_bytes = bytes(png_bytes)
        return Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    except ImportError:
        pass
    except Exception as e:
        print(f"resvg-py failed: {e}")

    try:
        import cairosvg
        png_bytes = cairosvg.svg2png(
            bytestring=svg.encode("utf-8"),
            output_width=size,
            output_height=size,
        )
        return Image.open(io.BytesIO(png_bytes)).convert("RGBA")
    except Exception as e:
        print(f"cairosvg failed: {e}")
        try:
            result = subprocess.run(
                ["rsvg-convert", "-w", str(size), "-h", str(size)],
                input=svg.encode("utf-8"),
                capture_output=True,
                check=True,
            )
            return Image.open(io.BytesIO(result.stdout)).convert("RGBA")
        except Exception as e2:
            print(f"rsvg-convert failed: {e2}")
            raise


def make_grid(sample_name: str, params: Params, params_label: str) -> Image.Image:
    src = Image.open(sample_name).convert("RGBA")
    src_big = src.resize((TILE_SIZE, TILE_SIZE), Image.NEAREST)

    svgs = process(sample_name, params)

    cells = [("source", src_big)]
    for style in STYLES:
        rendered = render_svg_to_png(svgs[style], TILE_SIZE)
        cells.append((style, rendered))

    label_h = 24
    margin = 8
    cell_total_w = TILE_SIZE + margin
    title_h = 28

    grid_w = cell_total_w * len(cells) + margin
    grid_h = title_h + label_h + TILE_SIZE + margin * 2

    grid = Image.new("RGBA", (grid_w, grid_h), (245, 245, 245, 255))
    draw = ImageDraw.Draw(grid)

    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 14)
        font_small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12)
    except Exception:
        font = ImageFont.load_default()
        font_small = ImageFont.load_default()

    title = f"{sample_name}    [{params_label}]"
    draw.text((margin, 6), title, fill=(20, 20, 20), font=font)

    for i, (label, im) in enumerate(cells):
        x = margin + i * cell_total_w
        y = title_h
        draw.text((x, y), label, fill=(60, 60, 60), font=font_small)
        # Checkerboard background to show transparency
        bg = Image.new("RGBA", (TILE_SIZE, TILE_SIZE), (255, 255, 255, 255))
        cb = Image.new("RGBA", (TILE_SIZE, TILE_SIZE), (255, 255, 255, 255))
        d = ImageDraw.Draw(cb)
        cell = 16
        for cy in range(0, TILE_SIZE, cell):
            for cx in range(0, TILE_SIZE, cell):
                if ((cx // cell) + (cy // cell)) % 2:
                    d.rectangle([cx, cy, cx + cell, cy + cell], fill=(220, 220, 220, 255))
        bg = Image.alpha_composite(bg, cb)
        bg.alpha_composite(im)
        grid.paste(bg, (x, y + label_h))

    return grid


def main():
    out_dir = Path("out")
    out_dir.mkdir(exist_ok=True)

    for params_label, params in PARAM_SETS.items():
        all_rows = []
        for s in SAMPLES:
            grid = make_grid(s, params, params_label)
            all_rows.append(grid)

        # Stack vertically.
        total_w = max(g.width for g in all_rows)
        total_h = sum(g.height for g in all_rows)
        combined = Image.new("RGBA", (total_w, total_h), (245, 245, 245, 255))
        y = 0
        for g in all_rows:
            combined.paste(g, (0, y))
            y += g.height
        out_path = out_dir / f"comparison_{params_label}.png"
        combined.convert("RGB").save(out_path)
        print(f"wrote {out_path}")

        # Also save the individual SVGs
        for s in SAMPLES:
            svgs = process(s, params)
            for style, svg in svgs.items():
                p = out_dir / f"{Path(s).stem}__{params_label}__{style}.svg"
                p.write_text(svg)


if __name__ == "__main__":
    main()

