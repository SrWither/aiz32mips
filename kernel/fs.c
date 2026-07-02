// fs.c — FAT32 sobre storage.h. El disco inicial lo arma mkfs.vfat +
// mtools del lado del host (ver kernel/Makefile, target disk.img); desde
// acá adentro ya se puede crear/sobrescribir archivos (fs_write) además
// de leerlos.
//
// Simplificaciones a propósito:
//   - Solo nombres cortos 8.3 (sin VFAT/LFN: se saltean esas entradas al
//     listar/buscar, y fat_name_to_83 rechaza nombres que no entren).
//   - Sin cache de directorio ni de FAT: cada resolución de path vuelve a
//     leer bloques del disco. Elección a propósito para no complicar
//     todavía; nada de este proyecto necesita esa perf por ahora.
//   - Sin "." ni "..": todo path se resuelve siempre desde la raíz.
//   - fs_write no reutiliza clusters: si el archivo ya existía, la cadena
//     vieja queda huérfana (mismo criterio que pmm_alloc_page en mm.c —
//     bump allocator, sin free). fat_alloc_cluster tampoco reutiliza
//     clusters borrados, solo avanza.
//   - fs_write no hace crecer un directorio que ya está lleno (sin
//     espacio libre en ningún cluster ya asignado): falla en vez de
//     encadenarle un cluster nuevo. No hace falta todavía con /bin,
//     /home, etc. teniendo lugar de sobra.
#include "storage.h"
#include "kernel.h"

// Cluster de hasta 4KB (8 sectores de 512): sobra para los ~512 bytes/
// cluster que usa disk.img hoy, con margen si el disco se agranda a futuro.
#define FS_MAX_SECTORS_PER_CLUSTER 8
#define FS_CLUSTER_BUF_BYTES (FS_MAX_SECTORS_PER_CLUSTER * STORAGE_BLOCK_SIZE)

#define FAT_ATTR_VOLUME_ID 0x08u
#define FAT_ATTR_DIRECTORY 0x10u
#define FAT_ATTR_LFN 0x0Fu // combinación RO|HIDDEN|SYSTEM|VOLUME_ID: entrada de nombre largo

#define FAT_ENTRY_FREE_MARK 0x00u
#define FAT_ENTRY_DELETED_MARK 0xE5u

#define FAT_EOC_MIN 0x0FFFFFF8u

typedef struct {
    u32 fat_begin_lba;
    u32 cluster_begin_lba;
    u32 sectors_per_cluster;
    u32 root_cluster;
    u32 fat_size_sectors; // tamaño de UNA copia de la FAT, para ubicar la 2da (fat_set_next_cluster)
    u32 num_fats;
} Fat32Info;

static Fat32Info fi;
static int mounted = 0;  // ya se intentó montar
static int mount_ok = 0; // y salió bien (boot sector con firma válida)

static u16 rd16(const u8 *b, u32 off) {
    return (u16)(b[off] | (b[off + 1] << 8));
}

static u32 rd32(const u8 *b, u32 off) {
    return (u32)b[off] | ((u32)b[off + 1] << 8) | ((u32)b[off + 2] << 16) | ((u32)b[off + 3] << 24);
}

static void fs_ensure_mounted(void) {
    if (mounted) {
        return;
    }
    mounted = 1;

    // static: fs.c no es reentrante (kernel sin threads, un solo caller a
    // la vez) y el stack del kernel es chico (16KB, ver linker.ld) y
    // compartido con las TrapFrame de las excepciones — buffers de este
    // tamaño ahí adentro corrompían .bss (mounted/mount_ok quedaban a
    // centímetros del límite del stack).
    static u8 boot[STORAGE_BLOCK_SIZE];
    storage_read_block(0, boot);
    if (boot[510] != 0x55 || boot[511] != 0xAA) {
        return; // disco sin formatear (o no es FAT32): mount_ok se queda en 0
    }

    u32 reserved = rd16(boot, 14);
    u32 nfats = boot[16];
    u32 fatsz32 = rd32(boot, 36);

    fi.sectors_per_cluster = boot[13];
    fi.fat_begin_lba = reserved;
    fi.cluster_begin_lba = reserved + nfats * fatsz32;
    fi.root_cluster = rd32(boot, 44);
    fi.fat_size_sectors = fatsz32;
    fi.num_fats = nfats;
    mount_ok = 1;
}

static u32 fs_cluster_bytes(void) {
    return fi.sectors_per_cluster * STORAGE_BLOCK_SIZE;
}

static u32 fs_cluster_to_lba(u32 cluster) {
    return fi.cluster_begin_lba + (cluster - 2) * fi.sectors_per_cluster;
}

// Lee un cluster entero (fi.sectors_per_cluster bloques consecutivos) a dst.
// dst tiene que tener FS_CLUSTER_BUF_BYTES de espacio.
static void fs_read_cluster(u32 cluster, u8 *dst) {
    u32 lba = fs_cluster_to_lba(cluster);
    for (u32 s = 0; s < fi.sectors_per_cluster; s++) {
        storage_read_block(lba + s, dst + s * STORAGE_BLOCK_SIZE);
    }
}

// Simétrica a fs_read_cluster, para el write-path.
static void fs_write_cluster(u32 cluster, const u8 *src) {
    u32 lba = fs_cluster_to_lba(cluster);
    for (u32 s = 0; s < fi.sectors_per_cluster; s++) {
        storage_write_block(lba + s, src + s * STORAGE_BLOCK_SIZE);
    }
}

// Entrada de la FAT en sí (no del directorio): 4 bytes por cluster, con los
// 4 bits altos reservados (se enmascaran acá para que el resto del código
// no tenga que acordarse).
static u32 fat_next_cluster(u32 cluster) {
    u32 fat_offset = cluster * 4;
    u32 fat_sector = fi.fat_begin_lba + fat_offset / STORAGE_BLOCK_SIZE;
    u32 ent_offset = fat_offset % STORAGE_BLOCK_SIZE;
    static u8 blk[STORAGE_BLOCK_SIZE]; // static: ver comentario en fs_ensure_mounted
    storage_read_block(fat_sector, blk);
    return rd32(blk, ent_offset) & 0x0FFFFFFFu;
}

// Escribe una entrada de la FAT (en las dos copias, si hay dos — mkfs.vfat
// arma 2 por default). Preserva los 4 bits altos de lo que ya hubiera en
// el sector (reservados por spec, por las dudas de que algún día se usen).
static void fat_set_next_cluster(u32 cluster, u32 value) {
    u32 fat_offset = cluster * 4;
    u32 sector_off = fat_offset / STORAGE_BLOCK_SIZE;
    u32 ent_offset = fat_offset % STORAGE_BLOCK_SIZE;
    value &= 0x0FFFFFFFu;

    static u8 blk[STORAGE_BLOCK_SIZE]; // static: ver comentario en fs_ensure_mounted
    for (u32 copy = 0; copy < fi.num_fats; copy++) {
        u32 sector = fi.fat_begin_lba + copy * fi.fat_size_sectors + sector_off;
        storage_read_block(sector, blk);
        u32 preserved = rd32(blk, ent_offset) & 0xF0000000u;
        u32 merged = preserved | value;
        blk[ent_offset + 0] = (u8)merged;
        blk[ent_offset + 1] = (u8)(merged >> 8);
        blk[ent_offset + 2] = (u8)(merged >> 16);
        blk[ent_offset + 3] = (u8)(merged >> 24);
        storage_write_block(sector, blk);
    }
}

// Bump allocator sobre los clusters (mismo criterio que pmm_alloc_page en
// mm.c): avanza buscando el próximo cluster con entrada de FAT en 0, nunca
// reutiliza uno liberado. Alcanza para lo que este proyecto necesita hoy —
// nada borra archivos todavía, así que "liberado" ni siquiera pasa.
static u32 next_free_cluster_hint = 0;

static u32 fat_alloc_cluster(void) {
    if (next_free_cluster_hint < 2) {
        next_free_cluster_hint = 2;
    }
    while (fat_next_cluster(next_free_cluster_hint) != 0) {
        next_free_cluster_hint++;
    }
    return next_free_cluster_hint++;
}

// "hello.elf" -> "HELLO   ELF" (8+3, espacio-rellenado, sin punto). -1 si
// el nombre no entra en 8.3 (sin soporte VFAT todavía).
static int fat_name_to_83(const char *in, char out[11]) {
    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }
    int i = 0, oi = 0;
    while (in[i] && in[i] != '.' && oi < 8) {
        char c = in[i++];
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        }
        out[oi++] = c;
    }
    if (in[i] == '.') {
        i++;
        int ei = 8;
        while (in[i] && ei < 11) {
            char c = in[i++];
            if (c >= 'a' && c <= 'z') {
                c -= 32;
            }
            out[ei++] = c;
        }
    }
    return (in[i] == 0) ? 0 : -1;
}

// "HELLO   ELF" -> "HELLO.ELF" (para listar). out tiene que tener 13 bytes.
static void fat_83_to_name(const u8 entry[11], char *out) {
    int oi = 0;
    for (int i = 0; i < 8 && entry[i] != ' '; i++) {
        out[oi++] = (char)entry[i];
    }
    if (entry[8] != ' ') {
        out[oi++] = '.';
        for (int i = 8; i < 11 && entry[i] != ' '; i++) {
            out[oi++] = (char)entry[i];
        }
    }
    out[oi] = 0;
}

// Busca `name83` en el directorio que arranca en `dir_cluster`. Si lo
// encuentra, completa out_* y devuelve 1; si no, 0.
static int fat_find_in_dir(u32 dir_cluster, const char name83[11], u32 *out_cluster, u32 *out_size,
                            int *out_is_dir) {
    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    u32 cluster = dir_cluster;
    while (1) {
        fs_read_cluster(cluster, buf);
        u32 nentries = fs_cluster_bytes() / 32;
        for (u32 i = 0; i < nentries; i++) {
            const u8 *e = buf + i * 32;
            if (e[0] == FAT_ENTRY_FREE_MARK) {
                return 0; // fin del directorio
            }
            if (e[0] == FAT_ENTRY_DELETED_MARK) {
                continue;
            }
            u8 attr = e[11];
            if (attr == FAT_ATTR_LFN || (attr & FAT_ATTR_VOLUME_ID)) {
                continue;
            }
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (e[j] != name83[j]) {
                    match = 0;
                    break;
                }
            }
            if (!match) {
                continue;
            }
            u32 hi = rd16(e, 20), lo = rd16(e, 26);
            *out_cluster = (hi << 16) | lo;
            *out_is_dir = (attr & FAT_ATTR_DIRECTORY) != 0;
            *out_size = *out_is_dir ? 0 : rd32(e, 28);
            return 1;
        }
        cluster = fat_next_cluster(cluster);
        if (cluster >= FAT_EOC_MIN) {
            return 0;
        }
    }
}

// Busca `name83` en el directorio `dir_cluster`: si ya existe, devuelve su
// posición (para sobrescribirla — "crear" trunca, como open() con
// O_TRUNC). Si no, devuelve la primera posición libre (una entrada
// borrada, o el marcador de fin de directorio). -1 si el directorio está
// lleno y no hay ninguna posición libre en los clusters que ya tiene
// asignados (no hacemos crecer directorios todavía).
static int fat_dir_find_or_alloc_entry(u32 dir_cluster, const char name83[11], u32 *out_cluster,
                                        u32 *out_entry_off, int *out_existed) {
    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    u32 cluster = dir_cluster;
    u32 free_cluster = 0;
    u32 free_off = 0xFFFFFFFFu;
    while (1) {
        fs_read_cluster(cluster, buf);
        u32 nentries = fs_cluster_bytes() / 32;
        for (u32 i = 0; i < nentries; i++) {
            const u8 *e = buf + i * 32;
            if (e[0] == FAT_ENTRY_FREE_MARK) {
                if (free_off == 0xFFFFFFFFu) {
                    free_cluster = cluster;
                    free_off = i * 32;
                }
                *out_cluster = free_cluster;
                *out_entry_off = free_off;
                *out_existed = 0;
                return 0;
            }
            if (e[0] == FAT_ENTRY_DELETED_MARK) {
                if (free_off == 0xFFFFFFFFu) {
                    free_cluster = cluster;
                    free_off = i * 32;
                }
                continue;
            }
            u8 attr = e[11];
            if (attr == FAT_ATTR_LFN || (attr & FAT_ATTR_VOLUME_ID)) {
                continue;
            }
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (e[j] != name83[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                *out_cluster = cluster;
                *out_entry_off = i * 32;
                *out_existed = 1;
                return 0;
            }
        }
        u32 next = fat_next_cluster(cluster);
        if (next >= FAT_EOC_MIN) {
            return -1; // directorio lleno, sin soporte para hacerlo crecer todavía
        }
        cluster = next;
    }
}

// Vuelca una entrada de directorio de 32 bytes en `dir_cluster`, offset
// `entry_off`: nombre 8.3, ATTR_ARCHIVE, cluster inicial y tamaño. Fecha y
// hora en 0 — no llevamos reloj real de pared.
static void fat_write_dir_entry(u32 dir_cluster, u32 entry_off, const char name83[11], u32 file_cluster,
                                 u32 file_size) {
    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    fs_read_cluster(dir_cluster, buf);
    u8 *e = buf + entry_off;
    for (int i = 0; i < 11; i++) {
        e[i] = (u8)name83[i];
    }
    e[11] = 0x20; // ATTR_ARCHIVE
    for (int i = 12; i < 26; i++) {
        e[i] = 0; // reservado + fecha/hora de creación/acceso/escritura
    }
    e[20] = (u8)(file_cluster >> 16);
    e[21] = (u8)(file_cluster >> 24);
    e[26] = (u8)file_cluster;
    e[27] = (u8)(file_cluster >> 8);
    e[28] = (u8)file_size;
    e[29] = (u8)(file_size >> 8);
    e[30] = (u8)(file_size >> 16);
    e[31] = (u8)(file_size >> 24);
    fs_write_cluster(dir_cluster, buf);
}

#define FS_MAX_COMPONENT 13 // 8 + '.' + 3 + '\0'

// Resuelve un path absoluto (o relativo: por ahora es lo mismo, todo
// arranca en la raíz — no hay cwd ni ".."/"." todavía) a cluster/tamaño.
static int fs_resolve(const char *path, u32 *out_cluster, u32 *out_size, int *out_is_dir) {
    fs_ensure_mounted();
    if (!mount_ok) {
        return -1;
    }

    u32 cluster = fi.root_cluster;
    u32 size = 0;
    int is_dir = 1;

    const char *p = path;
    while (*p == '/') {
        p++;
    }
    while (*p) {
        if (!is_dir) {
            return -1; // quiso bajar por un path que en el medio no es directorio
        }
        char comp[FS_MAX_COMPONENT];
        int ci = 0;
        while (*p && *p != '/' && ci < FS_MAX_COMPONENT - 1) {
            comp[ci++] = *p++;
        }
        comp[ci] = 0;
        while (*p == '/') {
            p++;
        }

        char name83[11];
        if (fat_name_to_83(comp, name83) < 0) {
            return -1;
        }
        if (!fat_find_in_dir(cluster, name83, &cluster, &size, &is_dir)) {
            return -1;
        }
    }
    *out_cluster = cluster;
    *out_size = size;
    *out_is_dir = is_dir;
    return 0;
}

// Separa `path` en directorio padre (resuelto a cluster) + nombre del
// último componente (convertido a 8.3): lo que fs_write necesita para
// crear o sobrescribir un archivo sin que ese archivo tenga que existir
// todavía (a diferencia de fs_resolve, que sí lo exige). -1 si el padre
// no existe/no es directorio, o el nombre no entra en 8.3.
static int fs_resolve_parent(const char *path, u32 *out_dir_cluster, char out_name83[11]) {
    fs_ensure_mounted();
    if (!mount_ok) {
        return -1;
    }

    const char *last_slash = 0;
    for (const char *q = path; *q; q++) {
        if (*q == '/') {
            last_slash = q;
        }
    }
    const char *filename = last_slash ? last_slash + 1 : path;
    if (filename[0] == 0) {
        return -1; // termina en '/': no hay nombre de archivo
    }
    if (fat_name_to_83(filename, out_name83) < 0) {
        return -1;
    }

    u32 cluster = fi.root_cluster;
    int is_dir = 1;
    if (last_slash) {
        const char *p = path;
        while (*p == '/') {
            p++;
        }
        while (p < last_slash) {
            if (!is_dir) {
                return -1;
            }
            char comp[FS_MAX_COMPONENT];
            int ci = 0;
            while (p < last_slash && *p != '/' && ci < FS_MAX_COMPONENT - 1) {
                comp[ci++] = *p++;
            }
            comp[ci] = 0;
            while (*p == '/') {
                p++;
            }
            char comp83[11];
            u32 size;
            if (fat_name_to_83(comp, comp83) < 0) {
                return -1;
            }
            if (!fat_find_in_dir(cluster, comp83, &cluster, &size, &is_dir)) {
                return -1;
            }
        }
    }
    if (!is_dir) {
        return -1;
    }
    *out_dir_cluster = cluster;
    return 0;
}

// 1 si `path` existe y es directorio, 0 si existe pero es archivo, -1 si
// no existe. Lo usa shell.c (cmd_cd) para validar antes de cambiar cwd.
int fs_is_dir(const char *path) {
    u32 cluster, size;
    int is_dir;
    if (fs_resolve(path, &cluster, &size, &is_dir) < 0) {
        return -1;
    }
    return is_dir ? 1 : 0;
}

// Lee hasta maxlen bytes empezando en `offset` dentro del archivo (la usa
// fs_fd_read para sostener el cursor de un fd abierto entre llamadas).
// Devuelve bytes leídos (0 si offset ya pasó el final), -1 si el path no
// existe o es directorio.
int fs_read_at(const char *path, u32 offset, u8 *buf, u32 maxlen) {
    u32 cluster, size;
    int is_dir;
    if (fs_resolve(path, &cluster, &size, &is_dir) < 0 || is_dir) {
        return -1;
    }
    if (offset >= size) {
        return 0;
    }
    u32 total = size - offset;
    if (total > maxlen) {
        total = maxlen;
    }

    u32 cbytes = fs_cluster_bytes();
    u32 skip = offset / cbytes;
    u32 within = offset % cbytes;
    for (u32 i = 0; i < skip && cluster < FAT_EOC_MIN; i++) {
        cluster = fat_next_cluster(cluster);
    }

    static u8 cbuf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    u32 done = 0;
    while (done < total && cluster < FAT_EOC_MIN) {
        fs_read_cluster(cluster, cbuf);
        u32 start = (done == 0) ? within : 0;
        u32 chunk = cbytes - start;
        if (chunk > total - done) {
            chunk = total - done;
        }
        for (u32 i = 0; i < chunk; i++) {
            buf[done + i] = cbuf[start + i];
        }
        done += chunk;
        cluster = fat_next_cluster(cluster);
    }
    return (int)done;
}

int fs_read(const char *path, u8 *buf, u32 maxlen) {
    return fs_read_at(path, 0, buf, maxlen);
}

// Crea (o sobrescribe, truncando) `path` con el contenido de `data`
// (len bytes). 0 ok, -1 si el directorio padre no existe, el nombre no
// entra en 8.3, o el directorio está lleno (ver fat_dir_find_or_alloc_entry).
int fs_write(const char *path, const u8 *data, u32 len) {
    u32 dir_cluster;
    char name83[11];
    if (fs_resolve_parent(path, &dir_cluster, name83) < 0) {
        return -1;
    }

    u32 entry_cluster, entry_off;
    int existed;
    if (fat_dir_find_or_alloc_entry(dir_cluster, name83, &entry_cluster, &entry_off, &existed) < 0) {
        return -1;
    }

    u32 cbytes = fs_cluster_bytes();
    u32 nclusters = (len + cbytes - 1) / cbytes;
    if (nclusters == 0) {
        nclusters = 1; // hasta un archivo vacío ocupa un cluster
    }

    static u8 cbuf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    u32 first_cluster = fat_alloc_cluster();
    u32 cluster = first_cluster;
    for (u32 c = 0; c < nclusters; c++) {
        u32 off = c * cbytes;
        u32 chunk = (off < len) ? len - off : 0;
        if (chunk > cbytes) {
            chunk = cbytes;
        }
        for (u32 i = 0; i < cbytes; i++) {
            cbuf[i] = (i < chunk) ? data[off + i] : 0;
        }
        fs_write_cluster(cluster, cbuf);

        if (c + 1 < nclusters) {
            u32 next = fat_alloc_cluster();
            fat_set_next_cluster(cluster, next);
            cluster = next;
        } else {
            fat_set_next_cluster(cluster, FAT_EOC_MIN);
        }
    }

    fat_write_dir_entry(entry_cluster, entry_off, name83, first_cluster, len);
    (void)existed; // "crear" trunca: la cadena vieja (si había) queda huérfana, ver comentario arriba del archivo
    return 0;
}

void fs_list(const char *path) {
    u32 dir_cluster, dir_size;
    int is_dir;
    if (fs_resolve(path, &dir_cluster, &dir_size, &is_dir) < 0) {
        console_puts("no existe: ");
        console_puts(path);
        console_putc('\n');
        return;
    }
    if (!is_dir) {
        console_puts("no es un directorio: ");
        console_puts(path);
        console_putc('\n');
        return;
    }

    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    u32 cluster = dir_cluster;
    int any = 0;
    while (1) {
        fs_read_cluster(cluster, buf);
        u32 nentries = fs_cluster_bytes() / 32;
        for (u32 i = 0; i < nentries; i++) {
            const u8 *e = buf + i * 32;
            if (e[0] == FAT_ENTRY_FREE_MARK) {
                goto done;
            }
            if (e[0] == FAT_ENTRY_DELETED_MARK || e[0] == '.') {
                continue; // "." y ".." tambien caen acá (mismo criterio que ls sin -a)
            }
            u8 attr = e[11];
            if (attr == FAT_ATTR_LFN || (attr & FAT_ATTR_VOLUME_ID)) {
                continue;
            }
            char name[13];
            fat_83_to_name(e, name);
            console_puts(name);
            console_puts("  ");
            if (attr & FAT_ATTR_DIRECTORY) {
                console_puts("<DIR>");
            } else {
                console_put_uint(rd32(e, 28));
            }
            console_putc('\n');
            any = 1;
        }
        cluster = fat_next_cluster(cluster);
        if (cluster >= FAT_EOC_MIN) {
            break;
        }
    }
done:
    if (!any) {
        console_puts("(vacio)\n");
    }
}

// ───────────────────────── descriptores de archivo ─────────────────────
// Tabla global chica: la usan las syscalls SYS_OPEN/READ/WRITE/CLOSE (ver
// trap.c) para darle acceso a archivos a procesos de usuario — hasta acá
// fs_read/fs_write solo los llamaba la shell, en contexto kernel. Sin
// streaming real de escritura: lo que se sys_write() se acumula en un
// buffer del propio fd y recién se vuelca al disco (fs_write, un solo
// fs_write_cluster por cluster) al hacer sys_close(). Alcanza para
// archivos chicos (hasta FS_WRITE_BUF_SIZE); no hay demand-paging del
// buffer de escritura, así que no crece más que eso.
#define MAX_OPEN_FILES 8
#define FD_BASE 3 // 0,1,2 quedan reservados para un futuro stdin/stdout/stderr
#define FS_PATH_MAX 96
#define FS_WRITE_BUF_SIZE 4096

typedef struct {
    char path[FS_PATH_MAX];
    u32 offset; // cursor de lectura
    u32 size;   // tamaño del archivo (solo lectura)
    int owner_pid;
    int used;
    int writing;
    u32 write_len;
    u8 write_buf[FS_WRITE_BUF_SIZE];
} OpenFile;

static OpenFile open_files[MAX_OPEN_FILES];

static void fs_str_copy(char *dst, const char *src, int maxlen) {
    int i = 0;
    for (; i < maxlen - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

// write_mode: 0 = lectura (el archivo tiene que existir ya), 1 = escritura
// (crea/trunca recién en close, ver fs_fd_close). Devuelve un fd (>=
// FD_BASE) o -1 si no existe (lectura) o no hay slots libres.
int fs_open(const char *path, int write_mode, int owner_pid) {
    int slot = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!open_files[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    OpenFile *f = &open_files[slot];
    if (write_mode) {
        f->writing = 1;
        f->write_len = 0;
        f->size = 0;
        f->offset = 0;
    } else {
        u32 cluster, size;
        int is_dir;
        if (fs_resolve(path, &cluster, &size, &is_dir) < 0 || is_dir) {
            return -1;
        }
        f->writing = 0;
        f->size = size;
        f->offset = 0;
    }
    fs_str_copy(f->path, path, FS_PATH_MAX);
    f->owner_pid = owner_pid;
    f->used = 1;
    return slot + FD_BASE;
}

int fs_fd_read(int fd, u8 *buf, u32 maxlen) {
    int idx = fd - FD_BASE;
    if (idx < 0 || idx >= MAX_OPEN_FILES || !open_files[idx].used || open_files[idx].writing) {
        return -1;
    }
    OpenFile *f = &open_files[idx];
    int n = fs_read_at(f->path, f->offset, buf, maxlen);
    if (n > 0) {
        f->offset += (u32)n;
    }
    return n;
}

// Escritura "parcial" al estilo write(2) real: si no entra todo en lo que
// queda del buffer, acepta lo que entra y devuelve cuánto fue (no -1).
int fs_fd_write(int fd, const u8 *buf, u32 len) {
    int idx = fd - FD_BASE;
    if (idx < 0 || idx >= MAX_OPEN_FILES || !open_files[idx].used || !open_files[idx].writing) {
        return -1;
    }
    OpenFile *f = &open_files[idx];
    u32 space = FS_WRITE_BUF_SIZE - f->write_len;
    u32 n = (len < space) ? len : space;
    for (u32 i = 0; i < n; i++) {
        f->write_buf[f->write_len + i] = buf[i];
    }
    f->write_len += n;
    return (int)n;
}

// whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END. Sólo fds de lectura: los de
// escritura son un buffer de acumulación sin "posición" real (ver
// fs_fd_write) — igual que el resto de la escritura en este proyecto
// (bump allocator sin free, ver el comentario de scope en sched.c), no
// hace falta más que eso todavía.
int fs_fd_seek(int fd, i32 offset, int whence) {
    int idx = fd - FD_BASE;
    if (idx < 0 || idx >= MAX_OPEN_FILES || !open_files[idx].used || open_files[idx].writing) {
        return -1;
    }
    OpenFile *f = &open_files[idx];
    u32 base;
    switch (whence) {
        case 0:
            base = 0;
            break;
        case 1:
            base = f->offset;
            break;
        case 2:
            base = f->size;
            break;
        default:
            return -1;
    }
    i32 new_off = (i32)base + offset;
    if (new_off < 0 || (u32)new_off > f->size) {
        return -1;
    }
    f->offset = (u32)new_off;
    return (int)f->offset;
}

int fs_fd_close(int fd) {
    int idx = fd - FD_BASE;
    if (idx < 0 || idx >= MAX_OPEN_FILES || !open_files[idx].used) {
        return -1;
    }
    OpenFile *f = &open_files[idx];
    int r = 0;
    if (f->writing) {
        r = fs_write(f->path, f->write_buf, f->write_len);
    }
    f->used = 0;
    return r;
}

// La llaman sched_exit_current/sched_kill_pid: sin esto, un proceso
// que muere con un archivo abierto para escribir pierde lo que tenía en
// el buffer (nunca llega al fs_write de close), y el slot de la tabla
// queda ocupado para siempre.
void fs_close_all_owned_by(int pid) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[i].used && open_files[i].owner_pid == pid) {
            fs_fd_close(i + FD_BASE);
        }
    }
}
