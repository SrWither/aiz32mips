#define GPU_MMIO_BASE  0x1F802000

#define REG_WIDTH      (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x00))
#define REG_HEIGHT     (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x02))
#define REG_PITCH      (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x04))
#define REG_BPP        (*(volatile unsigned char *)(GPU_MMIO_BASE + 0x06))
#define REG_FBADDR     (*(volatile unsigned int  *)(GPU_MMIO_BASE + 0x08))
#define REG_FONTADDR   (*(volatile unsigned int  *)(GPU_MMIO_BASE + 0x20))
#define REG_FONTW      (*(volatile unsigned char *)(GPU_MMIO_BASE + 0x24))
#define REG_FONTH      (*(volatile unsigned char *)(GPU_MMIO_BASE + 0x25))
#define REG_CMD16      (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x10))
#define REG_PARAM16    (*(volatile unsigned short*)(GPU_MMIO_BASE + 0x12))

#define VRAM_BASE      0x10000000

static inline void gpu_param_u8(int i, unsigned char v) { *(volatile unsigned char*)(GPU_MMIO_BASE + 0x12 + i) = v; }
static inline void gpu_cmd_u8(int i, unsigned char v) { *(volatile unsigned char*)(GPU_MMIO_BASE + 0x10 + i) = v; }
static inline void gpu_param_u16(unsigned short v) { gpu_param_u8(0,v&0xFF); gpu_param_u8(1,v>>8); }
static inline void gpu_param_u32(unsigned int v) { gpu_param_u16(v&0xFFFF); gpu_param_u16(v>>16); }
static inline void gpu_cmd(unsigned short c) { gpu_cmd_u8(0,c&0xFF); gpu_cmd_u8(1,c>>8); }

void _start() {
    REG_WIDTH  = 320;
    REG_HEIGHT = 200;
    REG_PITCH  = 320;
    REG_BPP    = 32;
    REG_FBADDR = 0;

    REG_FONTADDR = 0x00200000;
    REG_FONTW = 8;
    REG_FONTH = 8;

    // Limpiar pantalla con negro
    gpu_param_u32(0xFF000000);
    gpu_cmd(0x0001); // CLEAR

    const char* msg = "Hello World";
    unsigned int len = 11;

    // Calcular posición centrada
    unsigned int text_w = len * 8;
    unsigned int text_h = 8;
    unsigned int x = (320 - text_w) / 2;
    unsigned int y = (200 - text_h) / 2;

    // Enviar parámetros a GPU
    gpu_param_u16(x);
    gpu_param_u16(y);
    gpu_param_u16(len);
    gpu_param_u32(0xFFFFFFFF); // texto blanco
    gpu_param_u32(0x00000000); // fondo negro

    // añadir cada caracter
    for (unsigned int i = 0; i < len; i++) {
        gpu_param_u16(msg[i]);
    }

    gpu_cmd(0x0004); // PUTS

    while (1) {} 
}
