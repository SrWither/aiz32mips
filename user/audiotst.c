// audiotst.c — prueba el device de audio (ver kernel/audio.h +
// aiz32mips_core/src/devices/audio.rs): genera un tono cuadrado simple
// (sin libm: alcanza con una onda cuadrada por módulo entero) y lo
// somete en varias tandas chicas, después confirma vía sys_audio_status()
// que quedó encolado de verdad para reproducción. Tandas chicas a
// propósito: todo proceso de este proyecto entra en una única página de
// 4KB (texto+datos+bss, ver kernel/sched.c::sched_spawn) — un buffer de
// varios miles de samples no entraría.
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/audio.h"

#define CHUNK_SAMPLES 256u
#define N_CHUNKS 8u // 8*256 = 2048 samples sometidos en total, ~93ms
#define PERIOD 100u // ~220Hz aprox (22050/100)
#define AMPLITUDE 8000

static short buf[CHUNK_SAMPLES];

void _start(void) {
    unsigned int before = audio_pending();
    unsigned int total = 0;
    for (unsigned int c = 0; c < N_CHUNKS; c++) {
        for (unsigned int i = 0; i < CHUNK_SAMPLES; i++) {
            unsigned int t = c * CHUNK_SAMPLES + i;
            buf[i] = ((t / (PERIOD / 2)) % 2) ? AMPLITUDE : -AMPLITUDE;
        }
        audio_submit(buf, CHUNK_SAMPLES);
        total += CHUNK_SAMPLES;
    }
    unsigned int after = audio_pending();

    printf("audio: pendientes antes=%u despues=%u (sometidos %u)\n", before, after, total);
    if (after >= before + total) {
        printf("audiotst ok\n");
    } else {
        printf("audiotst fallo: la cola no crecio lo esperado\n");
    }
    exit(0);
}
