// ping.c — versión userland de lo que antes era el builtin "ping" de la
// shell (kernel/shell.c): ahora es un binario en /bin como cualquier
// otro, se resuelve por $PATH igual que el resto.
#include "libc/stdio.h"
#include "libc/stdlib.h"

void _start(void) {
    printf("pong\n");
    exit(0);
}
