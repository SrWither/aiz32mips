# generate_fontsheet.py
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "IBM_VGA8.ttf"
OUT_PATH  = "font8x8.png"

# Tamaño de celda con margen
CELL_W, CELL_H = 10, 10   # área de dibujo
GLYPH_W, GLYPH_H = 8, 8   # área útil real (queremos 8x8 finales)
COLS, ROWS = 16, 16
IMG_W, IMG_H = COLS * CELL_W, ROWS * CELL_H

img = Image.new("L", (IMG_W, IMG_H), 0)
draw = ImageDraw.Draw(img)
font = ImageFont.truetype(FONT_PATH, GLYPH_H)

for code in range(256):
    x = (code % COLS) * CELL_W + 1  # margen izq
    y = (code // COLS) * CELL_H     # margen sup leve
    draw.text((x, y), chr(code), fill=255, font=font)

img.save(OUT_PATH)
print(f"Saved {OUT_PATH} ({IMG_W}x{IMG_H})")
