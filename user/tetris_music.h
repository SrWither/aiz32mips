// tetris_music.h — la melodía de Tetris (Korobeiniki), en onda cuadrada,
// sobre el device de audio nuevo (ver kernel/audio.h + libc/audio.h).
//
// Antes esto era un fragmento de ~20 notas cortado a la mitad de la frase
// (ni siquiera llegaba a la cadencia final) y con un buffer partido en
// varias llamadas por frame + music_tick() forzada a noinline: todo eso
// era para entrar en la única página de 4KB que tenía el proceso entero
// (texto+datos+bss, ver kernel/sched.c) — ya no aplica desde que esa
// región es demand-paged (ver kernel/mm.c, [[aiz32mips-multipage-text-plan]]
// en la memoria del proyecto). Ahora es la frase completa (arranca en E5 y
// llega hasta la cadencia real en A4-A4-silencio, incluyendo el puente
// D5-F5-A5-G5-F5 que antes se recortaba) y una sola función simple.
#ifndef AIZ_TETRIS_MUSIC_H
#define AIZ_TETRIS_MUSIC_H

#ifndef AIZ_GPU_TYPES_DEFINED
#define AIZ_GPU_TYPES_DEFINED
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
#endif

#include "libc/audio.h"

// Período (en samples, a AUDIO_SAMPLE_RATE=22050Hz) de un ciclo completo
// de onda cuadrada para cada nota: period = round(SAMPLE_RATE / freq_Hz).
// 0 = silencio.
#define NOTE_REST 0
#define NOTE_A4 50 // 440.00 Hz
#define NOTE_B4 45 // 493.88 Hz
#define NOTE_C5 42 // 523.25 Hz
#define NOTE_D5 38 // 587.33 Hz
#define NOTE_E5 33 // 659.25 Hz
#define NOTE_F5 32 // 698.46 Hz
#define NOTE_G5 28 // 783.99 Hz
#define NOTE_A5 25 // 880.00 Hz

typedef struct {
    u16 period;
    u8 units; // duración en unidades de MUSIC_UNIT_SAMPLES (1=corchea, 2=negra, 3=negra con puntillo)
} music_note_t;

// Frase completa (antes cortada a la mitad): arranca igual que antes pero
// ahora sigue por el puente ascendente (D5 F5 A5 G5 F5) y la segunda mitad
// de la frase hasta la cadencia real en A4-A4-silencio, antes de loopear
// sola de nuevo al principio (ver TETRIS_MELODY_LEN).
static const music_note_t tetris_melody[] = {
    {NOTE_E5, 2}, {NOTE_B4, 1}, {NOTE_C5, 1}, {NOTE_D5, 2}, {NOTE_C5, 1}, {NOTE_B4, 1},
    {NOTE_A4, 2}, {NOTE_A4, 1}, {NOTE_C5, 1}, {NOTE_E5, 2}, {NOTE_D5, 1}, {NOTE_C5, 1},
    {NOTE_B4, 3}, {NOTE_C5, 1}, {NOTE_D5, 2}, {NOTE_E5, 2},
    {NOTE_C5, 2}, {NOTE_A4, 2}, {NOTE_A4, 1}, {NOTE_A4, 2},
    {NOTE_D5, 3}, {NOTE_F5, 1}, {NOTE_A5, 2}, {NOTE_G5, 1}, {NOTE_F5, 1},
    {NOTE_E5, 3}, {NOTE_C5, 1}, {NOTE_E5, 2}, {NOTE_D5, 1}, {NOTE_C5, 1},
    {NOTE_B4, 2}, {NOTE_B4, 1}, {NOTE_C5, 1}, {NOTE_D5, 2}, {NOTE_E5, 2},
    {NOTE_C5, 2}, {NOTE_A4, 2}, {NOTE_A4, 2}, {NOTE_REST, 2},
};
#define TETRIS_MELODY_LEN (sizeof(tetris_melody) / sizeof(tetris_melody[0]))

// 4410 samples = 200ms por corchea a 22050Hz (negra = 400ms, ~150 BPM):
// antes eran 2200 (100ms/corchea, ~300 BPM), sonaba disparada. El tempo
// no tiene relación con MUSIC_CHUNK_SAMPLES de abajo — ese es sólo cuánto
// se somete por llamada, no a qué velocidad suena cada nota.
#define MUSIC_UNIT_SAMPLES 4410u
#define MUSIC_AMPLITUDE 6000

// A 60fps hacen falta ~367 samples/frame para no atrasarse contra
// AUDIO_SAMPLE_RATE (22050/60) — 384 da margen. Ya no hace falta partirlo
// en varias sub-llamadas ni marcar nada noinline: sin el límite de una
// página por proceso, un solo buffer de este tamaño no es problema.
#define MUSIC_CHUNK_SAMPLES 384u

static u32 music_note_idx = 0;
static u32 music_sample_in_note = 0;
static short music_chunk[MUSIC_CHUNK_SAMPLES];

// Llamar una vez por frame: genera MUSIC_CHUNK_SAMPLES samples de onda
// cuadrada avanzando por tetris_melody (loopea sola al llegar al final) y
// los somete de una.
static inline void music_tick_frame(void) {
    for (u32 i = 0; i < MUSIC_CHUNK_SAMPLES; i++) {
        music_note_t n = tetris_melody[music_note_idx];
        u32 note_len = (u32)n.units * MUSIC_UNIT_SAMPLES;
        if (n.period == NOTE_REST) {
            music_chunk[i] = 0;
        } else {
            u32 phase = music_sample_in_note % n.period;
            music_chunk[i] = (phase < n.period / 2) ? MUSIC_AMPLITUDE : -MUSIC_AMPLITUDE;
        }
        music_sample_in_note++;
        if (music_sample_in_note >= note_len) {
            music_sample_in_note = 0;
            music_note_idx++;
            if (music_note_idx >= TETRIS_MELODY_LEN) {
                music_note_idx = 0;
            }
        }
    }
    audio_submit(music_chunk, MUSIC_CHUNK_SAMPLES);
}

#endif // AIZ_TETRIS_MUSIC_H
