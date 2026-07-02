// shell.c — línea de comandos simple: junta lo que se tipea y lo despacha
// al apretar Enter. Corre en contexto de keyboard_irq (IRQ), no hay
// syscalls acá: se escribe directo a la consola.
#include "kernel.h"

#define CMD_MAX 120
#define PATH_MAX 96

static char cmd_buf[CMD_MAX];
static int cmd_len = 0;

static char cwd[PATH_MAX] = "/";

// ─────────────────────────── historial de comandos ─────────────────────
// Array chico y plano (no ring buffer): con HIST_MAX=32 entradas de hasta
// CMD_MAX bytes cada una, "correr todo un lugar" al llenarse (hist_add)
// es un memmove de unos pocos KB — insignificante al ritmo en que se
// escriben comandos, y mucho más simple de razonar que aritmética modular
// de ring buffer. Se persiste entero en /home/.history: fs_write no tiene
// modo "append" (crea/trunca, ver fs.c), así que cada comando nuevo
// reescribe el archivo completo (hist_save) — de nuevo, insignificante a
// este tamaño.
#define HIST_MAX 32
#define HIST_FILE "/home/.history"
static char hist[HIST_MAX][CMD_MAX];
static int hist_count = 0;

// Estado de navegación (flechas arriba/abajo): -1 = no se está navegando
// (cmd_buf es lo que el usuario está tipeando de verdad). 0..hist_count-1
// = índice del comando de hist[] que se está mostrando ahora mismo
// (0=más vieja, hist_count-1=más nueva). hist_draft guarda lo que había
// tipeado antes de arrancar a navegar, para poder volver con Down.
static int hist_nav = -1;
static char hist_draft[CMD_MAX];

// ────────────────── comando en foreground (wait) ────────────────────────
// 0 = la shell acepta entrada normalmente. Si no, es el pid que se
// spawneó en foreground y todavía no terminó — la shell deja de aceptar
// comandos nuevos (ver shell_input) hasta que sched.c avise que terminó
// (shell_on_process_exit, llamada desde sched_notify_exit en sched.c cada
// vez que CUALQUIER proceso muere). Ver el comentario de shell_spawn_fg
// para por qué esto NO usa el bloqueo de scheduler (sched_block_current).
static int fg_wait_pid = 0;

int shell_is_waiting_fg(void) {
    return fg_wait_pid != 0;
}

void shell_on_process_exit(int pid) {
    if (pid != 0 && pid == fg_wait_pid) {
        fg_wait_pid = 0;
        console_puts("> ");
        console_flush();
    }
}

// Ctrl+C mató (por acción default) al proceso que la shell tenía en
// foreground: shell_on_process_exit ya se encargó (o se va a encargar, si
// tenía un handler instalado y sigue vivo, más adelante) de mostrar el
// próximo prompt — acá sólo falta el eco visual "^C". Si NO había nada en
// foreground, este Ctrl+C es el de siempre: cancela lo que hubiera
// tipeado a medias.
void shell_ctrl_c(int was_fg_waiting) {
    console_puts("^C\n");
    if (!was_fg_waiting) {
        cmd_len = 0;
        hist_nav = -1;
        console_puts("> ");
    }
}

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

    // FS_MAX_LONGNAME (kernel.h): tiene que coincidir con lo que fs.c
    // soporta de verdad (VFAT/LFN) — un buffer más chico acá truncaría un
    // nombre largo ANTES de que le llegue a fs.c, así se manifestó la
    // primera vez (ver el comentario en kernel.h).
    char comps[8][FS_MAX_LONGNAME];
    int ncomps = 0;
    u32 i = 0;
    while (merged[i]) {
        while (merged[i] == '/') {
            i++;
        }
        if (!merged[i]) {
            break;
        }
        char comp[FS_MAX_LONGNAME];
        int ci = 0;
        while (merged[i] && merged[i] != '/' && ci < FS_MAX_LONGNAME - 1) {
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
            str_copy(comps[ncomps], comp, FS_MAX_LONGNAME);
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

// Entero decimal simple (sin signo) para usar como arg0 al spawnear — así
// "pingpong 1" corre con rol 1 en vez del 0 por defecto (ver user/pingpong.c:
// ya no hay builtin que lo lance con los dos roles a la vez, son dos
// comandos sueltos). Sin dígitos válidos: 0.
static u32 parse_arg0(const char *s) {
    u32 v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u32)(*s - '0');
        s++;
    }
    return v;
}

// Spawnea `path` y, si !background, deja a la shell "esperando" a que
// termine antes de mostrar el próximo prompt (ver fg_wait_pid/
// shell_on_process_exit más abajo) — a propósito NO usa sched_wait_for
// (el bloqueo de scheduler que sí usa SYS_WAIT): sched_wait_for asume que
// sched_current_pid() es quien llama, algo que sólo vale para una syscall
// real (siempre la dispara el proceso que la ejecuta). Un IRQ de teclado
// puede llegar mientras CUALQUIER otro proceso es el "current" (el
// teclado le llega a la shell sin importar quién tenga la CPU en ese
// momento, ver keyboard_irq en trap.c) — bloquear ahí con sched_wait_for
// corrompería el estado de scheduling de quien sea que estuviera
// corriendo. Por eso la shell usa esta bandera simple en cambio: no toca
// el scheduler para nada, sólo dejar de aceptar entrada nueva hasta que
// sched.c avise que `pid` terminó.
// Devuelve 1 si quedó esperando (no hay que imprimir "> " todavía), 0 si
// ya terminó del todo (error de spawn, o corre en segundo plano).
static int shell_spawn_fg(const char *path, u32 arg0, int background) {
    int pid = sched_spawn(path, arg0);
    if (pid < 0) {
        console_puts("no se pudo ejecutar: ");
        console_puts(path);
        console_putc('\n');
        return 0;
    }
    if (!background) {
        fg_wait_pid = pid;
        return 1;
    }
    return 0;
}

// run <cmd> [arg0] [&]: mismo resolutor que un comando suelto (path con
// '/' o búsqueda en $PATH). `background` ya viene decidido por
// shell_dispatch (el '&' se despoja de la línea completa antes de
// separar comando/args, no sólo acá).
static int cmd_run(char *rest, int background) {
    if (rest[0] == 0) {
        console_puts("uso: run <comando> [arg0] [&]\n");
        return 0;
    }
    char *arg = split_first_space(rest);
    char path[PATH_MAX];
    if (resolve_exec(path, sizeof(path), rest) < 0) {
        console_puts("no encontrado: ");
        console_puts(rest);
        console_putc('\n');
        return 0;
    }
    return shell_spawn_fg(path, parse_arg0(arg), background);
}

// cat <path>: imprime el contenido de un archivo (hasta 512 bytes).
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

// write <path> <texto>: crea (o pisa, truncando) un archivo con el resto
// de la línea como contenido — ahora persiste de verdad (ver fs.c: FAT32
// ya soporta escritura).
static void cmd_write(char *rest) {
    char *content = split_first_space(rest);
    if (rest[0] == 0) {
        console_puts("uso: write <path> <texto>\n");
        return;
    }
    char path[PATH_MAX];
    resolve_path(path, sizeof(path), rest);
    u32 len = 0;
    while (content[len]) {
        len++;
    }
    if (fs_write(path, (const u8 *)content, len) < 0) {
        console_puts("no se pudo escribir: ");
        console_puts(path);
        console_putc('\n');
    }
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

// Reescribe /home/.history entero a partir de hist[] (sin modo "append" en
// fs_write, ver el comentario de HIST_MAX más arriba).
static void hist_save(void) {
    static char buf[HIST_MAX * CMD_MAX]; // static: no CMD_MAX*32 en el stack del IRQ
    u32 len = 0;
    for (int i = 0; i < hist_count; i++) {
        for (const char *p = hist[i]; *p && len < sizeof(buf) - 1; p++) {
            buf[len++] = *p;
        }
        if (len < sizeof(buf) - 1) {
            buf[len++] = '\n';
        }
    }
    fs_write(HIST_FILE, (const u8 *)buf, len);
}

// Agrega `line` al historial: no guarda líneas vacías ni una igual a la
// última que ya estaba (mismo criterio que HISTCONTROL=ignoredups de
// bash — repetir Enter en la misma línea no debería inflar la historia).
// Al llenarse HIST_MAX, tira la más vieja corriendo todo un lugar.
static void hist_add(const char *line) {
    if (!line[0]) {
        return;
    }
    if (hist_count > 0 && str_eq(hist[hist_count - 1], line)) {
        return;
    }
    if (hist_count == HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++) {
            str_copy(hist[i - 1], hist[i], CMD_MAX);
        }
        hist_count--;
    }
    str_copy(hist[hist_count], line, CMD_MAX);
    hist_count++;
}

// La llama shell_init: carga /home/.history línea por línea (si existe —
// fs_read devuelve <0 si no, hist_load no hace nada, arranca vacío) para
// que el historial sobreviva a un reinicio.
static void hist_load(void) {
    static u8 buf[HIST_MAX * CMD_MAX];
    int n = fs_read(HIST_FILE, buf, sizeof(buf));
    if (n <= 0) {
        return;
    }
    int start = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            int len = i - start;
            if (len > 0 && len < CMD_MAX) {
                char line[CMD_MAX];
                for (int k = 0; k < len; k++) {
                    line[k] = (char)buf[start + k];
                }
                line[len] = 0;
                hist_add(line);
            }
            start = i + 1;
        }
    }
}

// Borra visualmente lo que hay tipeado ahora mismo (backspace por cada
// carácter, mismo mecanismo que ya usa el backspace normal) y lo
// reemplaza por `new_content` — la usan shell_history_up/down para
// mostrar el comando de la historia sin tocar cmd_len a mano en cada
// lugar.
static void shell_replace_line(const char *new_content) {
    while (cmd_len > 0) {
        console_putc(8);
        cmd_len--;
    }
    for (int i = 0; new_content[i] && cmd_len < CMD_MAX - 1; i++) {
        cmd_buf[cmd_len++] = new_content[i];
        console_putc(new_content[i]);
    }
}

void shell_history_up(void) {
    if (fg_wait_pid != 0 || hist_count == 0) {
        return; // hay un comando en foreground: no tiene sentido navegar
    }
    if (hist_nav == -1) {
        // primera flecha arriba de esta línea: guarda el borrador para
        // poder volver a él con Down, y arranca desde el más nuevo.
        cmd_buf[cmd_len] = 0;
        str_copy(hist_draft, cmd_buf, CMD_MAX);
        hist_nav = hist_count - 1;
    } else if (hist_nav > 0) {
        hist_nav--;
    } else {
        return; // ya está en el más viejo
    }
    shell_replace_line(hist[hist_nav]);
}

void shell_history_down(void) {
    if (fg_wait_pid != 0 || hist_nav == -1) {
        return; // idem shell_history_up
    }
    hist_nav++;
    if (hist_nav >= hist_count) {
        hist_nav = -1;
        shell_replace_line(hist_draft);
    } else {
        shell_replace_line(hist[hist_nav]);
    }
}

// Devuelve 1 si ya dejó a la shell esperando un proceso en foreground (no
// hay que imprimir "> " de nuevo, ver shell_input) — 0 en cualquier otro
// caso (builtin, línea vacía, error, o spawn en segundo plano).
static int shell_dispatch(void) {
    cmd_buf[cmd_len] = 0;
    hist_add(cmd_buf);
    hist_save();
    hist_nav = -1; // el próximo Up arranca de nuevo desde el más nuevo

    // "cmd &" al final == correr en segundo plano (no bloquea la shell,
    // el comportamiento de siempre). Sin soporte de comillas/escapes: un
    // '&' en cualquier otro lugar de la línea (p.ej. un argumento) no se
    // interpreta especial, sólo el último carácter no-espacio de TODA la
    // línea, antes de separar comando/args — por eso este chequeo va acá
    // arriba, no adentro de cmd_run/resolve_exec.
    int background = 0;
    int end = cmd_len;
    while (end > 0 && cmd_buf[end - 1] == ' ') {
        end--;
    }
    if (end > 0 && cmd_buf[end - 1] == '&') {
        background = 1;
        end--;
        while (end > 0 && cmd_buf[end - 1] == ' ') {
            end--;
        }
        cmd_buf[end] = 0;
    }

    char *rest = split_first_space(cmd_buf);
    int blocked = 0;
    if (cmd_buf[0] == 0) {
        // linea vacia: no hace nada
    } else if (str_eq(cmd_buf, "run")) {
        blocked = cmd_run(rest, background);
    } else if (str_eq(cmd_buf, "ls")) {
        char path[PATH_MAX];
        resolve_path(path, sizeof(path), rest);
        fs_list(path);
    } else if (str_eq(cmd_buf, "cat")) {
        cmd_cat(rest);
    } else if (str_eq(cmd_buf, "write")) {
        cmd_write(rest);
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
            blocked = shell_spawn_fg(path, parse_arg0(rest), background);
        } else {
            console_puts("comando desconocido: ");
            console_puts(cmd_buf);
            console_putc('\n');
        }
    }
    cmd_len = 0;
    return blocked;
}

void shell_init(void) {
    cmd_len = 0;
    fg_wait_pid = 0;
    env_set("PATH", "/bin");
    hist_load();
}

void shell_input(char c) {
    if (fg_wait_pid != 0) {
        // Hay un comando en foreground: no se acepta entrada nueva hasta
        // que termine (shell_on_process_exit se encarga de retomar el
        // prompt). Ctrl+C YA NO pasa por acá (ver shell_ctrl_c/trap.c) —
        // sched_signal_fg ya le llega directo al proceso en foreground
        // sin pasar por shell_input.
        return;
    }
    if (c == 12) { // Ctrl+L (FF): limpia pantalla y redibuja el prompt con
                    // lo que hubiera tipeado (a diferencia de Ctrl+C, NO
                    // se descarta la línea a medias).
        cmd_buf[cmd_len] = 0;
        console_clear();
        console_puts("> ");
        console_puts(cmd_buf);
        return;
    }
    if (c == '\n' || c == '\r') {
        console_putc('\n');
        if (!shell_dispatch()) {
            console_puts("> ");
        }
        return;
    }
    if (c == 8 || c == 127) { // backspace/delete
        if (cmd_len > 0) {
            cmd_len--;
            console_putc(8); // console.c borra la ultima celda y retrocede
        }
        hist_nav = -1; // editar la línea la desengancha de la historia
        return;
    }
    if (c < 32 || c > 126) {
        return; // resto de codigos de control: se ignoran
    }
    if (cmd_len < CMD_MAX - 1) {
        cmd_buf[cmd_len++] = c;
        console_putc(c);
    }
    hist_nav = -1; // idem backspace: tipear algo nuevo desengancha de la historia
}
