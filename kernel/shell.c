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

// Concatena `src` al final de `dst` (que ya tiene que ser una C-string),
// sin pasarse de `maxlen` bytes en total. La usa resolve_exec para armar
// "<dir>/<cmd>" y "<dir>/<cmd>.elf" al buscar en $PATH.
static void str_append(char *dst, const char *src, int maxlen) {
    int i = 0;
    while (dst[i]) {
        i++;
    }
    int j = 0;
    for (; src[j] && i < maxlen - 1; j++, i++) {
        dst[i] = src[j];
    }
    dst[i] = 0;
}

// Corta `s` en el primer espacio y devuelve lo que sigue ("" si no había
// espacio). La usan los comandos con argumentos (ls/cat/cd/run/export/echo)
// para separar el nombre del resto de la línea.
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

// ───────────────────────── variables de entorno ────────────────────────
// Tabla fija chiquita: alcanza para $PATH y algún que otro export manual.
// Viven solo acá (la shell): no se propagan todavía al proceso que se
// spawnea (eso requeriría armarle argv/envp de verdad en el stack inicial,
// ver kernel/sched.c — el día que haga falta un programa que las lea).
#define ENV_MAX 8
#define ENV_NAME_MAX 16
#define ENV_VALUE_MAX 48

typedef struct {
    char name[ENV_NAME_MAX];
    char value[ENV_VALUE_MAX];
} EnvVar;

static EnvVar env[ENV_MAX];
static int env_count = 0;

static const char *env_get(const char *name) {
    for (int i = 0; i < env_count; i++) {
        if (str_eq(env[i].name, name)) {
            return env[i].value;
        }
    }
    return 0;
}

static void env_set(const char *name, const char *value) {
    for (int i = 0; i < env_count; i++) {
        if (str_eq(env[i].name, name)) {
            str_copy(env[i].value, value, ENV_VALUE_MAX);
            return;
        }
    }
    if (env_count < ENV_MAX) {
        str_copy(env[env_count].name, name, ENV_NAME_MAX);
        str_copy(env[env_count].value, value, ENV_VALUE_MAX);
        env_count++;
    }
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

// Resuelve un comando a un ejecutable: si tiene '/' es un path (relativo a
// cwd o absoluto, vía resolve_path); si no, se busca en cada directorio de
// $PATH (por defecto "/bin"), probando primero el nombre tal cual y
// después con ".elf" agregado (los binarios en disco son .elf de punta a
// punta, ver kernel/Makefile). 0 y `out` con el path si lo encontró, -1 si
// no existe en ningún lado.
static int resolve_exec(char *out, u32 outmax, const char *cmd) {
    int has_slash = 0;
    for (const char *p = cmd; *p; p++) {
        if (*p == '/') {
            has_slash = 1;
            break;
        }
    }
    if (has_slash) {
        resolve_path(out, outmax, cmd);
        return (fs_is_dir(out) == 0) ? 0 : -1;
    }

    const char *path_var = env_get("PATH");
    if (!path_var) {
        path_var = "/bin";
    }

    char dir[PATH_MAX];
    int di = 0;
    for (const char *p = path_var;; p++) {
        if (*p == ':' || *p == 0) {
            dir[di] = 0;
            if (di > 0) {
                char candidate[PATH_MAX];
                str_copy(candidate, dir, sizeof(candidate));
                str_append(candidate, "/", sizeof(candidate));
                str_append(candidate, cmd, sizeof(candidate));
                if (fs_is_dir(candidate) == 0) {
                    str_copy(out, candidate, (int)outmax);
                    return 0;
                }
                str_append(candidate, ".elf", sizeof(candidate));
                if (fs_is_dir(candidate) == 0) {
                    str_copy(out, candidate, (int)outmax);
                    return 0;
                }
            }
            if (*p == 0) {
                break;
            }
            di = 0;
        } else if (di < PATH_MAX - 1) {
            dir[di++] = *p;
        }
    }
    return -1;
}

static void cmd_ping(void) {
    console_puts("pong\n");
}

// run <cmd>: mismo resolutor que un comando suelto (path con '/' o
// búsqueda en $PATH) — no bloquea: sched_spawn solo registra el proceso,
// el scheduler lo hace correr desde el próximo tick del timer.
static void cmd_run(char *rest) {
    if (rest[0] == 0) {
        console_puts("uso: run <comando>\n");
        return;
    }
    char path[PATH_MAX];
    if (resolve_exec(path, sizeof(path), rest) < 0) {
        console_puts("no encontrado: ");
        console_puts(rest);
        console_putc('\n');
        return;
    }
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

// export NAME=valor
static void cmd_export(char *rest) {
    char *eq = rest;
    while (*eq && *eq != '=') {
        eq++;
    }
    if (rest[0] == 0 || *eq != '=') {
        console_puts("uso: export NOMBRE=valor\n");
        return;
    }
    *eq = 0;
    env_set(rest, eq + 1);
}

// echo <texto>: expande $NOMBRE con el valor de la variable (o nada si no
// está seteada). Sin comillas ni escapes, lo justo para probar $PATH.
static void cmd_echo(const char *rest) {
    const char *p = rest;
    while (*p) {
        if (*p == '$' && (p[1] == '_' || (p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z'))) {
            p++;
            char name[ENV_NAME_MAX];
            int ni = 0;
            while (*p &&
                   (*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) &&
                   ni < ENV_NAME_MAX - 1) {
                name[ni++] = *p++;
            }
            name[ni] = 0;
            const char *v = env_get(name);
            if (v) {
                console_puts(v);
            }
        } else {
            console_putc(*p++);
        }
    }
    console_putc('\n');
}

static void cmd_env(void) {
    for (int i = 0; i < env_count; i++) {
        console_puts(env[i].name);
        console_putc('=');
        console_puts(env[i].value);
        console_putc('\n');
    }
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
    } else if (str_eq(cmd_buf, "export")) {
        cmd_export(rest);
    } else if (str_eq(cmd_buf, "echo")) {
        cmd_echo(rest);
    } else if (str_eq(cmd_buf, "env")) {
        cmd_env();
    } else {
        // No es un builtin: se busca como ejecutable (path con '/' o
        // nombre suelto por $PATH) antes de darse por vencido — así
        // "hello" corre igual que antes "run hello".
        char path[PATH_MAX];
        if (resolve_exec(path, sizeof(path), cmd_buf) == 0) {
            if (sched_spawn(path, 0) < 0) {
                console_puts("no se pudo ejecutar: ");
                console_puts(path);
                console_putc('\n');
            }
        } else {
            console_puts("comando desconocido: ");
            console_puts(cmd_buf);
            console_putc('\n');
        }
    }
    cmd_len = 0;
}

void shell_init(void) {
    cmd_len = 0;
    env_set("PATH", "/bin");
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
