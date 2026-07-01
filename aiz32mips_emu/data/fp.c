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

// ===================== Prototipos =====================
static inline void gpu_param_u8(int i, unsigned char v);
static inline void gpu_cmd_u8(int i, unsigned char v);
static inline void gpu_param_u16(unsigned short v);
static inline void gpu_param_u32(unsigned int v);
static inline void gpu_cmd(unsigned short c);
void gpu_puts(unsigned short x, unsigned short y, const char* msg, unsigned int fg, unsigned int bg);

// helpers de texto / números
void uitoa(unsigned int val, char* buf);
void itoa(int val, char* buf);
void ftoa_fixed(float f, int decimals, char* out);  // imprime con 'decimals' decimales

// aritmética float (sin libm)
float sin_taylor(float x);      // sin(x) por Taylor (x en radianes), con normalización
float sqrt_newton(float x);     // sqrt(x) por Newton

// =================== Programa principal ===================
void _start() {
    // Config GPU + fuente
    REG_WIDTH  = 320;
    REG_HEIGHT = 200;
    REG_PITCH  = 320;
    REG_BPP    = 32;
    REG_FBADDR = 0;
    REG_FONTADDR = 0x00200000;  // donde la cargaste en VRAM
    REG_FONTW = 8;
    REG_FONTH = 8;

    // Limpiar pantalla (negro)
    gpu_param_u32(0xFF000000);
    gpu_cmd(0x0001); // CLEAR

    // ====== Demo FPU ======
    // 1) sin(1.0)
    float x = 1.0f;
    float s = sin_taylor(x);

    // 2) sqrt(2.0)
    float r = sqrt_newton(2.0f);

    // 3) 1/3 (periodico) y una combinacion con senos
    float one_third = 1.0f / 3.0f;
    float combo = 0.5f * sin_taylor(0.75f) + 0.5f * sin_taylor(2.25f);

    // Buffers de impresión
    char line1[64], line2[64], line3[64], line4[64];
    char a[24], b[24], c[24], d[24];

    ftoa_fixed(s, 5, a);       // sin(1.0)
    ftoa_fixed(r, 5, b);       // sqrt(2.0)
    ftoa_fixed(one_third, 6, c); // 0.333333
    ftoa_fixed(combo, 5, d);   // combinación de senos

    // Construir textos
    // "sin(1.0) = a"
    line1[0]=0;  // inicia vacío
    {
        const char* p = "sin(1.0) = ";
        int k=0; while(p[k]){ line1[k]=p[k]; k++; }
        int j=0; while(a[j]){ line1[k++]=a[j++]; }
        line1[k]=0;
    }
    // "sqrt(2.0) = b"
    line2[0]=0;
    {
        const char* p = "sqrt(2.0) = ";
        int k=0; while(p[k]){ line2[k]=p[k]; k++; }
        int j=0; while(b[j]){ line2[k++]=b[j++]; }
        line2[k]=0;
    }
    // "1/3 = c"
    line3[0]=0;
    {
        const char* p = "1/3 = ";
        int k=0; while(p[k]){ line3[k]=p[k]; k++; }
        int j=0; while(c[j]){ line3[k++]=c[j++]; }
        line3[k]=0;
    }
    // "mix = d"
    line4[0]=0;
    {
        const char* p = "mix = 0.5*sin(0.75)+0.5*sin(2.25) = ";
        int k=0; while(p[k]){ line4[k]=p[k]; k++; }
        int j=0; while(d[j]){ line4[k++]=d[j++]; }
        line4[k]=0;
    }

    // Posiciones en pantalla (8x8)
    unsigned int x0 = 24;
    unsigned int y0 = 64;
    unsigned int fg = 0xFFFFFFFF, bg = 0x00000000;

    gpu_puts(x0, y0 + 0*10, line1, fg, bg);
    gpu_puts(x0, y0 + 1*10, line2, fg, bg);
    gpu_puts(x0, y0 + 2*10, line3, fg, bg);
    gpu_puts(x0, y0 + 3*10, line4, fg, bg);

    while (1) {}
}

// ================== Implementaciones ==================
static inline void gpu_param_u8(int i, unsigned char v) { *(volatile unsigned char*)(GPU_MMIO_BASE + 0x12 + i) = v; }
static inline void gpu_cmd_u8(int i, unsigned char v)   { *(volatile unsigned char*)(GPU_MMIO_BASE + 0x10 + i) = v; }
static inline void gpu_param_u16(unsigned short v)      { gpu_param_u8(0, v & 0xFF); gpu_param_u8(1, v >> 8); }
static inline void gpu_param_u32(unsigned int v)        { gpu_param_u16(v & 0xFFFF); gpu_param_u16(v >> 16); }
static inline void gpu_cmd(unsigned short c)            { gpu_cmd_u8(0, c & 0xFF);   gpu_cmd_u8(1, c >> 8);  }

void gpu_puts(unsigned short x, unsigned short y, const char* msg, unsigned int fg, unsigned int bg) {
    unsigned int len = 0;
    while (msg[len]) len++;

    gpu_param_u16(x);
    gpu_param_u16(y);
    gpu_param_u16(len);
    gpu_param_u32(fg);
    gpu_param_u32(bg);

    for (unsigned int i = 0; i < len; i++)
        gpu_param_u16((unsigned short)msg[i]);

    gpu_cmd(0x0004); // PUTS
}

// -------- int / float a string --------
void uitoa(unsigned int val, char* buf) {
    char tmp[16];
    int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0; while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

void itoa(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    if (val < 0) { *buf++ = '-'; val = -val; }
    uitoa((unsigned int)val, buf);
}

// f (ej. -3.14159) → string con 'decimals' decimales
void ftoa_fixed(float f, int decimals, char* out) {
    // manejar signo
    if (f < 0.0f) { *out++ = '-'; f = -f; }

    // parte entera
    int ip = (int)f;
    float frac = f - (float)ip;

    // imprimir entero
    char tmp[16];
    uitoa((unsigned int)ip, tmp);
    int k=0; while(tmp[k]) { *out++ = tmp[k++]; }

    if (decimals <= 0) { *out=0; return; }

    *out++ = '.';

    // fracción: multiplicar por 10^decimals y redondear
    unsigned int mult = 1;
    for (int i=0;i<decimals;i++) mult *= 10;

    // +0.5 ulp para redondeo
    unsigned int frac_i = (unsigned int)(frac * (float)mult + 0.5f);

    // rellenar con ceros a la izquierda si hace falta
    unsigned int p = mult / 10;
    while (p > 0 && frac_i < p) { *out++ = '0'; p /= 10; }

    // escribir dígitos
    uitoa(frac_i, tmp);
    k=0; while(tmp[k]) { *out++ = tmp[k++]; }
    *out = 0;
}

// -------- sin(x) por Taylor con normalización a [-PI, PI] --------
float sin_taylor(float x) {
    const float PI = 3.14159265358979323846f;
    const float TWO_PI = 6.2831853071795864769f;

    // normalizar a [-PI, PI]
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;

    // sin(x) ≈ x - x^3/3! + x^5/5! - x^7/7!  (suficiente para demo visual)
    float x2 = x * x;
    float x3 = x * x2;
    float x5 = x3 * x2;
    float x7 = x5 * x2;

    return x
         - (x3 * (1.0f/6.0f))
         + (x5 * (1.0f/120.0f))
         - (x7 * (1.0f/5040.0f));
}

// -------- sqrt(x) por Newton-Raphson --------
float sqrt_newton(float x) {
    if (x <= 0.0f) return 0.0f;
    float y = x;               // guess inicial
    for (int i=0; i<10; i++) { // 10 iteraciones: suficiente para texto
        y = 0.5f * (y + x / y);
    }
    return y;
}
