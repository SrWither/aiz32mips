// tetris_music.h — la melodía de Tetris (Korobeiniki, frase inicial —
// "E B C D C B A A C E D C B B C D E C A A", el gancho más reconocible del
// tema) en onda cuadrada, sobre el device de audio nuevo (ver
// kernel/audio.h + libc/audio.h). Sin tabla de PCM precalculada completa:
// no entraría en la única página de 4KB del proceso (mismo motivo que el
// comentario de GPU_CMD_MAX_WORDS en gpu_user.h) — se genera de a un
// chunk chico por llamada (music_tick), avanzando nota a nota con estado
// propio, pensado para llamarse una vez por frame desde el loop principal
// de quien la use.
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

typedef struct {
    u16 period;
    u8 units; // duración en unidades de MUSIC_UNIT_SAMPLES (1=corchea, 2=negra)
} music_note_t;

static const music_note_t tetris_melody[] = {
    {NOTE_E5, 2}, {NOTE_B4, 1}, {NOTE_C5, 1}, {NOTE_D5, 2}, {NOTE_C5, 1},  {NOTE_B4, 1}, {NOTE_A4, 2}, {NOTE_A4, 2},
    {NOTE_C5, 1}, {NOTE_E5, 2}, {NOTE_D5, 1}, {NOTE_C5, 1}, {NOTE_B4, 2},  {NOTE_B4, 1}, {NOTE_C5, 1}, {NOTE_D5, 2},
    {NOTE_E5, 2}, {NOTE_C5, 2}, {NOTE_A4, 2}, {NOTE_A4, 2}, {NOTE_REST, 2},
};
#define TETRIS_MELODY_LEN (sizeof(tetris_melody) / sizeof(tetris_melody[0]))

#define MUSIC_UNIT_SAMPLES 2200u // ~100ms por corchea a 22050Hz
#define MUSIC_AMPLITUDE 6000
// Chunk chico a propósito: el proceso entero (texto+datos+bss) tiene que
// entrar en una única página de 4KB (ver kernel/sched.c::sched_spawn), y
// un buffer de más de un par de cientos de samples ya se come el margen
// que le quedaba a cube3d.c. ~6ms/llamada a 22050Hz — quien la use tiene
// que llamar music_tick() varias veces por frame (ver MUSIC_TICKS_PER_FRAME
// más abajo) para no quedarse sin cola a 60fps.
#define MUSIC_CHUNK_SAMPLES 96u
// A 60fps hacen falta ~367 samples/frame para no atrasarse contra
// AUDIO_SAMPLE_RATE (22050/60); 4 llamadas de 96 = 384, con margen.
#define MUSIC_TICKS_PER_FRAME 4

static u32 music_note_idx = 0;
static u32 music_sample_in_note = 0;
static short music_chunk[MUSIC_CHUNK_SAMPLES];

// Genera MUSIC_CHUNK_SAMPLES samples de onda cuadrada avanzando por
// tetris_melody (loopea sola al llegar al final) y los somete.
// noinline a propósito: la llama 3 veces por frame (ver music_tick_frame)
// y sin esto clang -O2 la inlineaba 3 veces, casi duplicando el tamaño
// del binario — justo lo que cube3d.c no se puede permitir (una sola
// página de 4KB para todo el proceso, ver kernel/sched.c::sched_spawn).
static void __attribute__((noinline)) music_tick(void) {
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

// Llamar una vez por frame: genera MUSIC_TICKS_PER_FRAME chunks de una
// (suficiente a 60fps para que la cola de reproducción no se atrase).
static inline void music_tick_frame(void) {
    for (u32 i = 0; i < MUSIC_TICKS_PER_FRAME; i++) {
        music_tick();
    }
}

#endif // AIZ_TETRIS_MUSIC_H
