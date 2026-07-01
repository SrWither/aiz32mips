// keyboard.h — MMIO de teclado para AIZ-32 (freestanding, sin libc)
//
// FIFO de eventos de 32 bits. Formato (debe matchear
// aiz32mips_core/src/devices/keyboard.rs):
//
//   bit31=kind (0=tecla cruda, 1=texto ya traducido por el host)
//   kind=0: bits0-7=keycode interno, bit8=pressed(1)/released(0),
//           bits9-12=modificadores (shift/ctrl/alt/gui)
//   kind=1: bits0-7=byte ASCII (para consola/shell, ya con shift/layout
//           aplicado por SDL del lado del host)

#ifndef AIZ_KEYBOARD_H
#define AIZ_KEYBOARD_H

typedef unsigned char kbd_u8;
typedef unsigned int kbd_u32;

#define KBD_MMIO_BASE 0xBF801000u

#define REG_KBD_DATA (*(volatile kbd_u32 *)(KBD_MMIO_BASE + 0x00)) // leer hace pop
#define REG_KBD_STATUS (*(volatile kbd_u32 *)(KBD_MMIO_BASE + 0x04)) // bit0 = hay datos
#define REG_KBD_CTRL (*(volatile kbd_u8 *)(KBD_MMIO_BASE + 0x08)) // bit0 = IRQ (Cause.IP2)

#define KBD_EVT_KIND_TEXT (1u << 31)
#define KBD_EVT_PRESSED (1u << 8)
#define KBD_MOD_SHIFT (1u << 9)
#define KBD_MOD_CTRL (1u << 10)
#define KBD_MOD_ALT (1u << 11)
#define KBD_MOD_GUI (1u << 12)

// Teclas sin equivalente ASCII directo (las imprimibles usan su propio
// código ASCII sin shift, p.ej. 'a' = 0x61, como keycode).
#define KBD_KC_UP 128
#define KBD_KC_DOWN 129
#define KBD_KC_LEFT 130
#define KBD_KC_RIGHT 131
#define KBD_KC_HOME 132
#define KBD_KC_END 133
#define KBD_KC_PAGEUP 134
#define KBD_KC_PAGEDOWN 135
#define KBD_KC_INSERT 136
#define KBD_KC_DELETE 137
#define KBD_KC_F1 140 // F1..F12 = 140..151

static inline int kbd_has_event(void) {
    return (REG_KBD_STATUS & 1u) != 0;
}

static inline kbd_u32 kbd_read_event(void) {
    return REG_KBD_DATA;
}

static inline void kbd_enable_irq(void) {
    REG_KBD_CTRL = 1;
}

static inline void kbd_disable_irq(void) {
    REG_KBD_CTRL = 0;
}

// Bloquea haciendo polling (sin usar la IRQ) hasta el próximo carácter de
// texto ya traducido; descarta en el camino los eventos de tecla cruda.
// Sirve para una consola simple tipo getchar().
static inline char kbd_getchar(void) {
    for (;;) {
        while (!kbd_has_event()) {
        }
        kbd_u32 ev = kbd_read_event();
        if (ev & KBD_EVT_KIND_TEXT) {
            return (char)(ev & 0xFF);
        }
    }
}

#endif // AIZ_KEYBOARD_H
