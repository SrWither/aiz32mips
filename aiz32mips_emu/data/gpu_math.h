// gpu_math.h — matemática 3D en punto fijo Q16.16 (no flotantes: el COP1
// todavía no tiene aritmética real implementada) + gpu_vertex_t. Separado
// de gpu.h a propósito: esto no toca VRAM ni MMIO, así que lo puede usar
// tanto código kernel (vía gpu.h, que lo incluye) como userland (que no
// tiene acceso directo a esas direcciones — ver user/gpu_user.h).
#ifndef AIZ_GPU_MATH_H
#define AIZ_GPU_MATH_H

#ifndef AIZ_GPU_TYPES_DEFINED
#define AIZ_GPU_TYPES_DEFINED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
#endif

// helper para armar colores ARGB8888 (alpha en el byte alto)
#define ARGB(a, r, g, b) ((u32)(((a) << 24) | ((r) << 16) | ((g) << 8) | (b)))
#define RGB(r, g, b) ARGB(0xFF, r, g, b)

typedef struct {
    i32 x, y;
    u32 z;     // 0 = más cerca, 0xFFFFFFFF = más lejos
    u32 color; // ARGB8888
    u32 u, v;  // coords de textura en 0..65535 (0..1 en 16.16)
} gpu_vertex_t;

typedef i32 fx_t; // Q16.16
#define FX_ONE (1 << 16)
#define FX_SHIFT 16

static inline fx_t fx_from_int(i32 i) { return i << FX_SHIFT; }
static inline i32 fx_to_int(fx_t f) { return f >> FX_SHIFT; }
static inline fx_t fx_mul(fx_t a, fx_t b) { return (fx_t)(((long long)a * (long long)b) >> FX_SHIFT); }

// División 64/32 manual (shift-subtract): con -nostdlib no tenemos
// compiler-rt, así que el operador `/` de 64 bits (__divdi3/__udivdi3)
// no está disponible. Multiplicación y shifts de 64 bits sí los inlinea
// el compilador directo, división no.
static inline unsigned long long fx_udiv64(unsigned long long num, unsigned long long den) {
    if (den == 0) return 0xFFFFFFFFFFFFFFFFULL;
    unsigned long long quotient = 0;
    unsigned long long rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((num >> i) & 1ULL);
        if (rem >= den) {
            rem -= den;
            quotient |= (1ULL << i);
        }
    }
    return quotient;
}

static inline fx_t fx_div(fx_t a, fx_t b) {
    int neg = (a < 0) != (b < 0);
    unsigned long long ua = (unsigned long long)(a < 0 ? -a : a) << FX_SHIFT;
    unsigned long long ub = (unsigned long long)(b < 0 ? -b : b);
    fx_t r = (fx_t)fx_udiv64(ua, ub);
    return neg ? -r : r;
}

// seno/coseno por tabla (8.8 grados -> Q16.16), evita Taylor con floats.
// 256 entradas cubriendo 0..2*pi.
static const short fx_sin_table[256] = {
    0, 25, 50, 75, 100, 125, 150, 175, 200, 224, 249, 273, 297, 321, 345, 369,
    392, 415, 438, 460, 483, 505, 526, 548, 569, 590, 610, 630, 650, 669, 688, 706,
    724, 742, 759, 775, 792, 807, 822, 837, 851, 865, 878, 891, 903, 915, 926, 936,
    946, 955, 964, 972, 980, 987, 993, 999, 1004, 1009, 1013, 1016, 1019, 1021, 1023, 1023,
    1024, 1023, 1023, 1021, 1019, 1016, 1013, 1009, 1004, 999, 993, 987, 980, 972, 964, 955,
    946, 936, 926, 915, 903, 891, 878, 865, 851, 837, 822, 807, 792, 775, 759, 742,
    724, 706, 688, 669, 650, 630, 610, 590, 569, 548, 526, 505, 483, 460, 438, 415,
    392, 369, 345, 321, 297, 273, 249, 224, 200, 175, 150, 125, 100, 75, 50, 25,
    0, -25, -50, -75, -100, -125, -150, -175, -200, -224, -249, -273, -297, -321, -345, -369,
    -392, -415, -438, -460, -483, -505, -526, -548, -569, -590, -610, -630, -650, -669, -688, -706,
    -724, -742, -759, -775, -792, -807, -822, -837, -851, -865, -878, -891, -903, -915, -926, -936,
    -946, -955, -964, -972, -980, -987, -993, -999, -1004, -1009, -1013, -1016, -1019, -1021, -1023, -1023,
    -1024, -1023, -1023, -1021, -1019, -1016, -1013, -1009, -1004, -999, -993, -987, -980, -972, -964, -955,
    -946, -936, -926, -915, -903, -891, -878, -865, -851, -837, -822, -807, -792, -775, -759, -742,
    -724, -706, -688, -669, -650, -630, -610, -590, -569, -548, -526, -505, -483, -460, -438, -415,
    -392, -369, -345, -321, -297, -273, -249, -224, -200, -175, -150, -125, -100, -75, -50, -25,
};

// angle: 0..255 representa 0..2*pi (un "byte de ángulo", como en muchos
// motores retro). Devuelve Q16.16 con escala 1.0 = 1024 de la tabla.
static inline fx_t fx_sin(u8 angle) { return (fx_t)fx_sin_table[angle] << (FX_SHIFT - 10); }
static inline fx_t fx_cos(u8 angle) { return (fx_t)fx_sin_table[(u8)(angle + 64)] << (FX_SHIFT - 10); }

typedef struct {
    fx_t x, y, z;
} vec3_t;

typedef struct {
    fx_t m[16]; // row-major
} mat4_t;

static inline mat4_t mat4_identity(void) {
    mat4_t r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = FX_ONE;
    return r;
}

static inline mat4_t mat4_mul(mat4_t a, mat4_t b) {
    mat4_t r = {{0}};
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            long long acc = 0;
            for (int k = 0; k < 4; k++) {
                acc += (long long)a.m[row * 4 + k] * (long long)b.m[k * 4 + col];
            }
            r.m[row * 4 + col] = (fx_t)(acc >> FX_SHIFT);
        }
    }
    return r;
}

static inline mat4_t mat4_translate(fx_t x, fx_t y, fx_t z) {
    mat4_t r = mat4_identity();
    r.m[3] = x;
    r.m[7] = y;
    r.m[11] = z;
    return r;
}

static inline mat4_t mat4_rotate_y(u8 angle) {
    mat4_t r = mat4_identity();
    fx_t s = fx_sin(angle), c = fx_cos(angle);
    r.m[0] = c;
    r.m[2] = s;
    r.m[8] = -s;
    r.m[10] = c;
    return r;
}

static inline mat4_t mat4_rotate_x(u8 angle) {
    mat4_t r = mat4_identity();
    fx_t s = fx_sin(angle), c = fx_cos(angle);
    r.m[5] = c;
    r.m[6] = -s;
    r.m[9] = s;
    r.m[10] = c;
    return r;
}

static inline mat4_t mat4_rotate_z(u8 angle) {
    mat4_t r = mat4_identity();
    fx_t s = fx_sin(angle), c = fx_cos(angle);
    r.m[0] = c;
    r.m[1] = -s;
    r.m[4] = s;
    r.m[5] = c;
    return r;
}

// Transforma `v` por `m` y deja el resultado en espacio "vista" (sin
// dividir por w todavía, eso lo hace gpu_project_vertex).
static inline vec3_t mat4_transform(mat4_t m, vec3_t v) {
    vec3_t r;
    r.x = fx_mul(m.m[0], v.x) + fx_mul(m.m[1], v.y) + fx_mul(m.m[2], v.z) + m.m[3];
    r.y = fx_mul(m.m[4], v.x) + fx_mul(m.m[5], v.y) + fx_mul(m.m[6], v.z) + m.m[7];
    r.z = fx_mul(m.m[8], v.x) + fx_mul(m.m[9], v.y) + fx_mul(m.m[10], v.z) + m.m[11];
    return r;
}

// Proyección perspectiva simple (sin matriz 4x4 homogénea: alcanza con
// dividir x,y por z para algo "primitivo" estilo PS1) + viewport a
// píxeles de pantalla. `focal` típicamente ~= ancho de pantalla.
static inline gpu_vertex_t gpu_project_vertex(vec3_t cam_space, fx_t focal, u32 color, u16 screen_w, u16 screen_h) {
    gpu_vertex_t out;
    fx_t z = cam_space.z;
    if (z < FX_ONE / 16) z = FX_ONE / 16; // evita división por ~0 / detrás de cámara

    fx_t px = fx_mul(fx_div(cam_space.x, z), focal);
    fx_t py = fx_mul(fx_div(cam_space.y, z), focal);

    out.x = fx_to_int(px) + screen_w / 2;
    out.y = screen_h / 2 - fx_to_int(py);
    // mapeo de Z a 0..65535 para el Z-buffer (u16): asumimos cam_space.z
    // ya viene en un rango razonable (ej. 1..64 unidades de mundo).
    i32 zi = fx_to_int(z) * 1000;
    out.z = (zi < 0) ? 0 : (zi > 65535 ? 65535 : (u32)zi);
    out.color = color;
    out.u = 0;
    out.v = 0;
    return out;
}

#endif // AIZ_GPU_MATH_H
