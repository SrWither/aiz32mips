// fopentst.c — prueba la capa FILE* de libc/stdio.h (fopen/fread/fseek/
// ftell/fclose sobre las syscalls de fd crudo, ver kernel/fs.c::
// fs_fd_seek): lee todo el archivo, después vuelve al principio con
// fseek(SEEK_SET) y lo relee de a partes para confirmar que el cursor
// mueve de verdad.
#include "libc/stdio.h"
#include "libc/stdlib.h"

void _start(void) {
    FILE *f = fopen("/home/readme.txt", "r");
    if (!f) {
        printf("no pude abrir /home/readme.txt con fopen\n");
        exit(1);
    }

    char buf[128];
    u32 n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    printf("fread completo (%u bytes): %s", n, buf);

    if (fseek(f, 0, SEEK_SET) != 0) {
        printf("fseek fallo\n");
        exit(1);
    }
    if (ftell(f) != 0) {
        printf("ftell despues de fseek(0) deberia dar 0\n");
        exit(1);
    }

    char half[16];
    u32 n2 = fread(half, 1, 8, f);
    half[n2] = 0;
    printf("primeros 8 bytes tras rebobinar: %s (ftell=%d)\n", half, ftell(f));

    fclose(f);
    printf("fopentst ok\n");
    exit(0);
}
