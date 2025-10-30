# extract_font_rom.py
from PIL import Image

IMG_PATH = "font8x8.png"
OUT_PATH = "font_rom.bin"

CELL_W, CELL_H = 10, 10   # tamaño de cada celda con margen
GLYPH_W, GLYPH_H = 8, 8   # lo que queremos extraer
MARGIN_X, MARGIN_Y = 1, 0 # margen a recortar
COLS, ROWS = 16, 16

img = Image.open(IMG_PATH).convert("L")
pixels = img.load()
data = bytearray()

for ch in range(256):
    cx = (ch % COLS) * CELL_W + MARGIN_X
    cy = (ch // COLS) * CELL_H + MARGIN_Y

    for y in range(GLYPH_H):
        byte = 0
        for x in range(GLYPH_W):
            val = pixels[cx + x, cy + y]
            bit = 1 if val > 128 else 0  # negro = 1
            byte = (byte << 1) | bit
        data.append(byte)

with open(OUT_PATH, "wb") as f:
    f.write(data)

print(f"Generado {OUT_PATH} ({len(data)} bytes)")
