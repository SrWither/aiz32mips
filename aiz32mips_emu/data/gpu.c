// gpu_test.c
#define GPU_MMIO_BASE  0x1F802000

#define REG_WIDTH      (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x00))
#define REG_HEIGHT     (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x02))
#define REG_PITCH      (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x04))
#define REG_BPP        (*(volatile unsigned char *)(GPU_MMIO_BASE + 0x06))
#define REG_FBADDR     (*(volatile unsigned int  *)(GPU_MMIO_BASE + 0x08))
#define REG_STATUS     (*(volatile unsigned int  *)(GPU_MMIO_BASE + 0x0C))

#define VRAM_BASE      0x10000000
#define FRAMEBUFFER    ((volatile unsigned int*)(VRAM_BASE))

void _start() {
    // Configuración inicial de la GPU
    REG_WIDTH  = 320;
    REG_HEIGHT = 200;
    REG_PITCH  = 320;
    REG_BPP    = 32;
    REG_FBADDR = 0;

    // Pintar gradiente horizontal (de azul a rojo)
    int width  = REG_WIDTH;
    int height = REG_HEIGHT;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned char r = (x * 255) / width;
            unsigned char g = (y * 255) / height;
            unsigned char b = 128;
            unsigned int color = (r << 16) | (g << 8) | b;
            FRAMEBUFFER[y * width + x] = color;
        }
    }

    // Loop infinito para mantener la pantalla
    while (1) {}
}
