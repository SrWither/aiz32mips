// filetest.c — prueba de punta a punta las syscalls de archivo para
// userland (SYS_OPEN/READ/WRITE/CLOSE, ver kernel/fs.c y libc/unistd.h):
// lee un archivo que ya está en el disco y escribe uno nuevo.
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/string.h"
#include "libc/unistd.h"

void _start(void) {
    int fd = open("/home/readme.txt", O_RDONLY);
    if (fd < 0) {
        printf("no pude abrir /home/readme.txt\n");
    } else {
        char buf[128];
        int n = read(fd, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = 0;
        printf("leido de /home/readme.txt: %s", buf);
        close(fd);
    }

    int wfd = open("/home/fromusr.txt", O_WRONLY);
    if (wfd < 0) {
        printf("no pude abrir /home/fromusr.txt para escribir\n");
    } else {
        const char *msg = "escrito desde userland via syscalls\n";
        write(wfd, msg, strlen(msg));
        close(wfd);
        printf("escribi /home/fromusr.txt\n");
    }

    exit(0);
}
