// libctest.c — prueba las funciones nuevas de la libc (ctype.h, más
// atoi/strtol/calloc/realloc en stdlib.h, strchr/strrchr/strstr/strdup en
// string.h, snprintf/sprintf/fprintf/fgets en stdio.h, assert.h) antes de
// confiar en ellas para portar programas de verdad.
#include "libc/assert.h"
#include "libc/ctype.h"
#include "libc/stdio.h"
#include "libc/stdlib.h"
#include "libc/string.h"

void _start(void) {
    u32 ok = 1;

    // ctype.h
    if (!isdigit('5') || isdigit('x')) {
        printf("ctype: isdigit mal\n");
        ok = 0;
    }
    if (!isalpha('Q') || isalpha('9')) {
        printf("ctype: isalpha mal\n");
        ok = 0;
    }
    if (!isspace(' ') || isspace('a')) {
        printf("ctype: isspace mal\n");
        ok = 0;
    }
    if (toupper('a') != 'A' || tolower('Z') != 'z') {
        printf("ctype: toupper/tolower mal\n");
        ok = 0;
    }

    // stdlib.h: atoi/strtol
    if (atoi("  -42abc") != -42) {
        printf("atoi mal\n");
        ok = 0;
    }
    char *end;
    long v = strtol("0x2A", &end, 0);
    if (v != 42 || *end != 0) {
        printf("strtol hex mal\n");
        ok = 0;
    }

    // stdlib.h: calloc/realloc
    u32 *arr = (u32 *)calloc(16, sizeof(u32));
    if (!arr || arr[0] != 0 || arr[15] != 0) {
        printf("calloc mal\n");
        ok = 0;
    }
    arr[0] = 0xCAFEu;
    arr = (u32 *)realloc(arr, 64 * sizeof(u32));
    if (!arr || arr[0] != 0xCAFEu) {
        printf("realloc mal\n");
        ok = 0;
    }
    free(arr);

    // string.h: strchr/strrchr/strstr/strdup
    const char *s = "hola mundo hola";
    if (strchr(s, 'm') != s + 5) {
        printf("strchr mal\n");
        ok = 0;
    }
    if (strrchr(s, 'h') != s + 11) {
        printf("strrchr mal\n");
        ok = 0;
    }
    if (strstr(s, "mundo") != s + 5) {
        printf("strstr mal\n");
        ok = 0;
    }
    char *dup = strdup(s);
    if (!dup || strcmp(dup, s) != 0) {
        printf("strdup mal\n");
        ok = 0;
    }
    free(dup);

    // stdio.h: snprintf/sprintf
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s=%d,0x%X", "x", -7, 255u);
    if (n != 9 || strcmp(buf, "x=-7,0xFF") != 0) {
        printf("snprintf mal: n=%d buf=%s\n", n, buf);
        ok = 0;
    }
    char small[4];
    snprintf(small, sizeof(small), "12345");
    if (strcmp(small, "123") != 0) {
        printf("snprintf truncado mal: %s\n", small);
        ok = 0;
    }

    // stdio.h: fprintf/fgets sobre un archivo real (FAT32)
    FILE *wf = fopen("/home/libctest.txt", "w");
    if (!wf) {
        printf("fopen w fallo\n");
        ok = 0;
    } else {
        fprintf(wf, "linea %d\notra\n", 1);
        fclose(wf);
        FILE *rf = fopen("/home/libctest.txt", "r");
        char line[32];
        if (!rf || !fgets(line, sizeof(line), rf) || strcmp(line, "linea 1\n") != 0) {
            printf("fgets linea 1 mal\n");
            ok = 0;
        } else if (!fgets(line, sizeof(line), rf) || strcmp(line, "otra\n") != 0) {
            printf("fgets linea 2 mal\n");
            ok = 0;
        }
        if (rf) {
            fclose(rf);
        }
    }

    assert(1 == 1); // si esto no compila/corre, algo anda mal con assert.h

    if (ok) {
        printf("libctest: todo ok\n");
    }
    exit(0);
}
