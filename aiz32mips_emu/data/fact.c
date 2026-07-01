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

// === Prototipos ===
static inline void gpu_param_u8(int i, unsigned char v);
static inline void gpu_cmd_u8(int i, unsigned char v);
static inline void gpu_param_u16(unsigned short v);
static inline void gpu_param_u32(unsigned int v);
static inline void gpu_cmd(unsigned short c);
unsigned int factorial(unsigned int n);
void uitoa(unsigned int val, char* buf);
void gpu_puts(unsigned short x, unsigned short y, const char* msg, unsigned int fg, unsigned int bg);

// === Programa principal ===
void _start() {
    // Configuración inicial de la GPU
    REG_WIDTH  = 320;
    REG_HEIGHT = 200;
    REG_PITCH  = 320;
    REG_BPP    = 32;
    REG_FBADDR = 0;
    REG_FONTADDR = 0x00200000;
    REG_FONTW = 8;
    REG_FONTH = 8;

    // Fondo negro
    gpu_param_u32(0xFF000000);
    gpu_cmd(0x0001); // CLEAR

    // Calcular factorial
    unsigned int n = 5;
    unsigned int res = factorial(n);

    // Convertir a texto "5! = 120"
    char num[16];
    char msg[32];
    uitoa(res, num);

    msg[0] = '0' + n;
    msg[1] = '!';
    msg[2] = ' ';
    msg[3] = '=';
    msg[4] = ' ';
    int i = 0;
    while (num[i]) { msg[5 + i] = num[i]; i++; }
    msg[5 + i] = 0;

    // Centrado en pantalla
    unsigned int len = 5 + i;
    unsigned int text_w = len * 8;
    unsigned int text_h = 8;
    unsigned int x = (320 - text_w) / 2;
    unsigned int y = (200 - text_h) / 2;

    // Mostrar en pantalla
    gpu_puts(x, y, msg, 0xFFFFFFFF, 0x00000000); // blanco sobre negro

    while (1) {}
}

// === Implementaciones ===
static inline void gpu_param_u8(int i, unsigned char v) { *(volatile unsigned char*)(GPU_MMIO_BASE + 0x12 + i) = v; }
static inline void gpu_cmd_u8(int i, unsigned char v)   { *(volatile unsigned char*)(GPU_MMIO_BASE + 0x10 + i) = v; }
static inline void gpu_param_u16(unsigned short v)      { gpu_param_u8(0, v & 0xFF); gpu_param_u8(1, v >> 8); }
static inline void gpu_param_u32(unsigned int v)        { gpu_param_u16(v & 0xFFFF); gpu_param_u16(v >> 16); }
static inline void gpu_cmd(unsigned short c)            { gpu_cmd_u8(0, c & 0xFF); gpu_cmd_u8(1, c >> 8); }

unsigned int factorial(unsigned int n) {
    unsigned int r = 1;
    for (unsigned int i = 2; i <= n; i++) r *= i;
    return r;
}

void uitoa(unsigned int val, char* buf) {
    char tmp[16];
    int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

void gpu_puts(unsigned short x, unsigned short y, const char* msg, unsigned int fg, unsigned int bg) {
    unsigned int len = 0;
    while (msg[len]) len++;

    gpu_param_u16(x);
    gpu_param_u16(y);
    gpu_param_u16(len);
    gpu_param_u32(fg);
    gpu_param_u32(bg);

    for (unsigned int i = 0; i < len; i++)
        gpu_param_u16(msg[i]);

    gpu_cmd(0x0004); // PUTS
}
