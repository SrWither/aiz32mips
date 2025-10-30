#define GPU_MMIO_BASE  0x1F802000

#define REG_WIDTH      (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x00))
#define REG_HEIGHT     (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x02))
#define REG_PITCH      (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x04))
#define REG_BPP        (*(volatile unsigned char *)(GPU_MMIO_BASE + 0x06))
#define REG_FBADDR     (*(volatile unsigned int  *)(GPU_MMIO_BASE + 0x08))
#define REG_CMD16      (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x10))
#define REG_PARAM16    (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x12))

#define VRAM_BASE      0x10000000

static inline void gpu_param_u16(unsigned short v);
static inline void gpu_param_u32(unsigned int v);
static inline void gpu_cmd(unsigned short c);

void _start() {
    // Configuración inicial
    REG_WIDTH  = 320;
    REG_HEIGHT = 200;
    REG_PITCH  = 320;
    REG_BPP    = 32;
    REG_FBADDR = 0;

    // Colores (ARGB8888)
    unsigned int leftColor  = 0xFF0000FF; // Azul
    unsigned int rightColor = 0xFFFF0000; // Rojo

    // Enviar parámetros al FIFO
    gpu_param_u32(leftColor);
    gpu_param_u32(rightColor);

    // Ejecutar comando GRAD_X
    gpu_cmd(0x0002);

    // Mantener pantalla
    while (1) {}
}



static inline void gpu_param_u16(unsigned short v) {
    REG_PARAM16 = v;
}

static inline void gpu_param_u32(unsigned int v) {
    gpu_param_u16((unsigned short)(v & 0xFFFF));
    gpu_param_u16((unsigned short)(v >> 16));
}

static inline void gpu_cmd(unsigned short c) {
    REG_CMD16 = c;
}
