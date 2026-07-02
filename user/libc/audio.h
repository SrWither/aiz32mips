// audio.h — API mínima de audio para userland, sobre sys_audio_submit/
// sys_audio_status (ver kernel/abi.h y kernel/audio.h): un canal PCM mono
// s16 a AUDIO_SAMPLE_RATE fijo, sin mezcla ni volumen — el kernel encola
// lo que se le somete tal cual.
#ifndef AIZ_LIBC_AUDIO_H
#define AIZ_LIBC_AUDIO_H

#include "../../kernel/abi.h"

#define AUDIO_SAMPLE_RATE 22050

static inline void audio_submit(const short *samples, unsigned int n) {
    sys_audio_submit(samples, n);
}

// Samples todavía sin reproducir en la cola del device.
static inline unsigned int audio_pending(void) {
    return sys_audio_status();
}

#endif // AIZ_LIBC_AUDIO_H
