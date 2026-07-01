// shell.c — línea de comandos simple: junta lo que se tipea y lo despacha
// al apretar Enter. Corre en contexto de keyboard_irq (IRQ), no hay
// syscalls acá: se escribe directo a la consola.
#include "kernel.h"

#define CMD_MAX 120
#define PATH_MAX 96

static char cmd_buf[CMD_MAX];
static int cmd_len = 0;

static char cwd[PATH_MAX] = "/";

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, int maxlen) {
    int i = 0;
    for (; i < maxlen - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

// Corta `s` en el primer espacio y devuelve lo que sigue ("" si no había
// espacio). La usan los comandos con argumentos (ls/cat/cd/run) para
// separar el nombre del resto de la línea.
static char *split_first_space(char *s) {
    while (*s && *s != ' ') {
        s++;
    }
    if (*s == ' ') {
        *s = 0;
        return s + 1;
    }
    return s;
}

// Arma un path absoluto a partir de `in`: si ya es absoluto lo normaliza
// tal cual, si no lo prefija con cwd. La normalización resuelve "." y ".."
// por componentes (sin tocar el disco: fs.c no sabe nada de cwd, siempre
// recibe paths ya resueltos desde acá).
static void resolve_path(char *out, u32 outmax, const char *in) {
    char merged[PATH_MAX + CMD_MAX];
    u32 mi = 0;
    if (in[0] != '/') {
        for (u32 i = 0; cwd[i] && mi < sizeof(merged) - 1; i++) {
            merged[mi++] = cwd[i];
        }
        if (mi == 0 || merged[mi - 1] != '/') {
            if (mi < sizeof(merged) - 1) {
                merged[mi++] = '/';
            }
        }
    }
    for (u32 i = 0; in[i] && mi < sizeof(merged) - 1; i++) {
        merged[mi++] = in[i];
    }
    merged[mi] = 0;

    char comps[8][16];
    int ncomps = 0;
    u32 i = 0;
    while (merged[i]) {
        while (merged[i] == '/') {
            i++;
        }
        if (!merged[i]) {
            break;
        }
        char comp[16];
        int ci = 0;
        while (merged[i] && merged[i] != '/' && ci < 15) {
            comp[ci++] = merged[i++];
        }
        comp[ci] = 0;
        if (str_eq(comp, ".")) {
            // no-op
        } else if (str_eq(comp, "..")) {
            if (ncomps > 0) {
                ncomps--;
            }
        } else if (ncomps < 8) {
            str_copy(comps[ncomps], comp, 16);
            ncomps++;
        }
    }

    u32 oi = 0;
    out[oi++] = '/';
    for (int c = 0; c < ncomps; c++) {
        if (c > 0 && oi < outmax - 1) {
            out[oi++] = '/';
        }
        for (int k = 0; comps[c][k] && oi < outmax - 1; k++) {
            out[oi++] = comps[c][k];
        }
    }
    out[oi] = 0;
}

static void cmd_ping(void) {
    console_puts("pong\n");
}

// run <path>: path relativo (a cwd) o absoluto a un .elf MIPS. No bloquea:
// sched_spawn solo registra el proceso, el scheduler lo hace correr desde
// el próximo tick del timer (ver sched.c).
static void cmd_run(char *rest) {
    if (rest[0] == 0) {
        console_puts("uso: run <path>\n");
        return;
    }
    char path[PATH_MAX];
    resolve_path(path, sizeof(path), rest);
    if (sched_spawn(path, 0) < 0) {
        console_puts("no se pudo ejecutar: ");
        console_puts(path);
        console_putc('\n');
    }
}

static void cmd_pingpong(void) {
    // Semáforos en (0,0): arrancan "vacíos", el primer sys_sem_wait de
    // cada rol bloquea hasta que el otro haga su sys_sem_signal (ver
    // user/pingpong.c y kernel/sem.c).
    sem_init(0, 0);
    sem_init(1, 0);
    if (sched_spawn("/bin/pingpong.elf", 0) < 0 || sched_spawn("/bin/pingpong.elf", 1) < 0) {
        console_puts("no se pudo ejecutar /bin/pingpong.elf\n");
    }
}

// cat <path>: imprime el contenido de un archivo (hasta 512 bytes). FAT32
// es de solo lectura por ahora (ver fs.c), así que no hay "write" todavía.
static void cmd_cat(char *rest) {
    if (rest[0] == 0) {
        console_puts("uso: cat <path>\n");
        return;
    }
    char path[PATH_MAX];
    resolve_path(path, sizeof(path), rest);
    static u8 buf[512];
    int n = fs_read(path, buf, sizeof(buf));
    if (n < 0) {
        console_puts("no existe: ");
        console_puts(path);
        console_putc('\n');
        return;
    }
    for (int i = 0; i < n; i++) {
        console_putc((char)buf[i]);
    }
    console_putc('\n');
}

static void cmd_pwd(void) {
    console_puts(cwd);
    console_putc('\n');
}

// cd [path]: sin argumento vuelve a la raíz (todavía no hay $HOME).
static void cmd_cd(char *rest) {
    char path[PATH_MAX];
    resolve_path(path, sizeof(path), rest[0] ? rest : "/");
    int r = fs_is_dir(path);
    if (r < 0) {
        console_puts("no existe: ");
        console_puts(path);
        console_putc('\n');
        return;
    }
    if (r == 0) {
        console_puts("no es un directorio: ");
        console_puts(path);
        console_putc('\n');
        return;
    }
    str_copy(cwd, path, sizeof(cwd));
}

static void shell_dispatch(void) {
    cmd_buf[cmd_len] = 0;
    char *rest = split_first_space(cmd_buf);
    if (cmd_buf[0] == 0) {
        // linea vacia: no hace nada
    } else if (str_eq(cmd_buf, "ping")) {
        cmd_ping();
    } else if (str_eq(cmd_buf, "run")) {
        cmd_run(rest);
    } else if (str_eq(cmd_buf, "pingpong")) {
        cmd_pingpong();
    } else if (str_eq(cmd_buf, "ls")) {
        char path[PATH_MAX];
        resolve_path(path, sizeof(path), rest);
        fs_list(path);
    } else if (str_eq(cmd_buf, "cat")) {
        cmd_cat(rest);
    } else if (str_eq(cmd_buf, "pwd")) {
        cmd_pwd();
    } else if (str_eq(cmd_buf, "cd")) {
        cmd_cd(rest);
    } else if (str_eq(cmd_buf, "clear")) {
        console_clear();
    } else {
        console_puts("comando desconocido: ");
        console_puts(cmd_buf);
        console_putc('\n');
    }
    cmd_len = 0;
}

void shell_init(void) {
    cmd_len = 0;
}

void shell_input(char c) {
    if (c == 3) { // Ctrl+C (ETX, ver trap.c::keyboard_irq): corta lo que esté corriendo
        sched_kill_all_user();
        gpu_force_release(); // por si el que se cortó era dueño de la pantalla (ver trap.c)
        console_puts("^C\n");
        cmd_len = 0; // lo que hubiera tipeado a medias, se descarta
        console_puts("> ");
        return;
    }
    if (c == '\n' || c == '\r') {
        console_putc('\n');
        shell_dispatch();
        console_puts("> ");
        return;
    }
    if (c == 8 || c == 127) { // backspace/delete
        if (cmd_len > 0) {
            cmd_len--;
            console_putc(8); // console.c borra la ultima celda y retrocede
        }
        return;
    }
    if (c < 32 || c > 126) {
        return; // resto de codigos de control: se ignoran
    }
    if (cmd_len < CMD_MAX - 1) {
        cmd_buf[cmd_len++] = c;
        console_putc(c);
    }
}
