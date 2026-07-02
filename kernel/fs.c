// fs.c — FAT32 sobre storage.h. El disco inicial lo arma mkfs.vfat +
// mtools del lado del host (ver kernel/Makefile, target disk.img); desde
// acá adentro ya se puede crear/sobrescribir archivos (fs_write) además
// de leerlos.
//
// Simplificaciones a propósito:
//   - VFAT/LFN: se soporta lectura Y escritura de nombres largos (ver
//     fat_find_entry/fat_write_long_entry), pero simplificado — sólo ASCII
//     (cada char de 8 bits, sin Unicode real: alcanza para lo que este
//     proyecto necesita), hasta FS_MAX_LONGNAME-1 caracteres (bien menos
//     que el máximo real de VFAT, 255), y el nombre corto autogenerado usa
//     el primer punto como separador de extensión (a diferencia de Windows
//     de verdad, que usa el último) — ningún caso real de este proyecto
//     necesita más que eso.
//   - Sin cache de directorio ni de FAT: cada resolución de path vuelve a
//     leer bloques del disco. Elección a propósito para no complicar
//     todavía; nada de este proyecto necesita esa perf por ahora.
//   - Sin "." ni "..": todo path se resuelve siempre desde la raíz.
//   - fs_write no reutiliza clusters: si el archivo ya existía, la cadena
//     vieja queda huérfana (mismo criterio que pmm_alloc_page en mm.c —
//     bump allocator, sin free). fat_alloc_cluster tampoco reutiliza
//     clusters borrados, solo avanza. Para un nombre largo NUEVO tampoco
//     se reutilizan huecos de entradas de directorio borradas (ver
//     fat_dir_alloc_run) — como nada en este proyecto borra archivos
//     todavía, esa entrada 0xE5 no llega a existir en la práctica.
//   - fs_write no hace crecer un directorio que ya está lleno (sin
//     espacio libre en ningún cluster ya asignado): falla en vez de
//     encadenarle un cluster nuevo. No hace falta todavía con /bin,
//     /home, etc. teniendo lugar de sobra. Mismo límite para una entrada
//     larga nueva: las N entradas LFN + la corta tienen que entrar TODAS
//     en el cluster donde se encontró lugar, no se reparten entre dos.
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

// FS_MAX_LONGNAME (nombre largo: hasta 63 caracteres + '\0', bien menos
// que el máximo real de VFAT, 255) vive en kernel.h — lo comparte con
// shell.c::resolve_path, ver el comentario ahí. Con 13 caracteres por
// entrada LFN, entran en 5 entradas.
#define FS_MAX_LFN_ENTRIES ((FS_MAX_LONGNAME - 1 + 12) / 13)

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

static void fs_str_copy(char *dst, const char *src, int maxlen) {
    int i = 0;
    for (; i < maxlen - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static u32 fs_str_len(const char *s) {
    u32 n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int fs_str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
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
// el nombre no entra en 8.3 (en ese caso hace falta VFAT, ver
// fat_gen_short_name/fat_write_long_entry).
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

// "HELLO   ELF" -> "HELLO.ELF" (para listar cuando no hay LFN). out tiene
// que tener 13 bytes.
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

// Checksum de un nombre corto (11 bytes crudos, ver spec VFAT): cada
// entrada LFN lo lleva, para poder verificar que de verdad precede a ESTA
// entrada corta y no a un archivo borrado/otro (acá no importa tanto —
// este proyecto no borra archivos — pero es gratis y evita falsos
// positivos si el disco lo armó otra herramienta).
static u8 fat_short_checksum(const u8 short_name11[11]) {
    u8 sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name11[i]);
    }
    return sum;
}

// Offsets (en la entrada de 32 bytes) de los 13 caracteres UTF-16LE que
// lleva cada entrada LFN — 5 + 6 + 2, con el cluster inicial (siempre 0) y
// el checksum en el medio. Ver fat_lfn_accumulate/fat_write_long_entry.
static const u8 fat_lfn_char_off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

// Suma el fragmento de nombre largo que trae esta entrada LFN al
// acumulador `buf`. Simplificado a ASCII: sólo usa el byte bajo de cada
// unidad UTF-16 (sin soporte real de Unicode, no hace falta acá). Las
// entradas LFN aparecen en la carpeta en orden DECRECIENTE de secuencia
// (la de mayor número, con el bit 0x40, primero) — por eso cada llamada
// escribe directo en su posición absoluta (seq-1)*13 en vez de ir
// concatenando: no importa en qué orden se llame, siempre da el mismo
// resultado. `checksum`/`valid` son el estado que arrastra el scan entre
// entradas — se resetean afuera cuando aparece la entrada corta que
// corresponde (o algo rompe la secuencia).
static void fat_lfn_accumulate(const u8 *e, char *buf, u8 *checksum, int *valid) {
    u8 ord = e[0];
    int last = (ord & 0x40) != 0;
    int seq = ord & 0x1F;
    if (seq < 1 || seq > FS_MAX_LFN_ENTRIES) {
        *valid = 0; // secuencia rota/fuera de rango: no confiar en este acumulador
        return;
    }
    if (last) {
        *checksum = e[13];
        *valid = 1;
        u32 upper = (u32)seq * 13;
        if (upper >= FS_MAX_LONGNAME) {
            upper = FS_MAX_LONGNAME - 1;
        }
        buf[upper] = 0; // tope provisorio: si ningún fragmento trae un \0 real, éste queda
    }
    if (!*valid) {
        return; // todavía no vimos la entrada "última" (primera físicamente) de esta secuencia
    }
    u32 base = (u32)(seq - 1) * 13;
    for (int k = 0; k < 13 && base + k < FS_MAX_LONGNAME - 1; k++) {
        char c = (char)e[fat_lfn_char_off[k]];
        buf[base + k] = c;
        if (c == 0) {
            break; // \0 real: pisa (y gana contra) el tope provisorio de arriba
        }
    }
}

// Primitiva de búsqueda compartida por fat_find_in_dir (lectura de paths)
// y fs_write (para decidir si hay que sobrescribir una entrada
// existente): recorre `dir_cluster` buscando una entrada cuyo nombre —
// corto si `name83_valid`, o reconstruido desde sus entradas LFN si no —
// coincida con `name` (el componente tal cual lo pidieron, sin convertir).
// Si `name83_valid`, sólo compara bytes cortos (comportamiento idéntico al
// de antes de VFAT, cero riesgo de regresión para nombres que ya entraban
// en 8.3); si no, la única forma de matchear es vía LFN. Devuelve 1 y
// completa out_* (posición de la entrada CORTA + su metadata) si la
// encuentra.
static int fat_find_entry(u32 dir_cluster, const char *name, const char name83[11], int name83_valid,
                           u32 *out_entry_cluster, u32 *out_entry_off, u32 *out_file_cluster, u32 *out_size,
                           int *out_is_dir) {
    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    static char lfn_buf[FS_MAX_LONGNAME];
    u8 lfn_checksum = 0;
    int lfn_valid = 0;

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
                lfn_valid = 0;
                continue;
            }
            u8 attr = e[11];
            if (attr == FAT_ATTR_LFN) {
                fat_lfn_accumulate(e, lfn_buf, &lfn_checksum, &lfn_valid);
                continue;
            }
            if (attr & FAT_ATTR_VOLUME_ID) {
                lfn_valid = 0;
                continue;
            }
            int matched;
            if (name83_valid) {
                matched = 1;
                for (int j = 0; j < 11; j++) {
                    if (e[j] != (u8)name83[j]) {
                        matched = 0;
                        break;
                    }
                }
            } else {
                matched = lfn_valid && fat_short_checksum(e) == lfn_checksum && fs_str_eq(lfn_buf, name);
            }
            lfn_valid = 0; // las LFN siempre preceden inmediatamente a SU entrada corta
            if (matched) {
                *out_entry_cluster = cluster;
                *out_entry_off = i * 32;
                u32 hi = rd16(e, 20), lo = rd16(e, 26);
                *out_file_cluster = (hi << 16) | lo;
                *out_is_dir = (attr & FAT_ATTR_DIRECTORY) != 0;
                *out_size = *out_is_dir ? 0 : rd32(e, 28);
                return 1;
            }
        }
        cluster = fat_next_cluster(cluster);
        if (cluster >= FAT_EOC_MIN) {
            return 0;
        }
    }
}

// Busca `name` (nombre tal cual, corto o largo) en el directorio que
// arranca en `dir_cluster`. Si lo encuentra, completa out_* y devuelve 1;
// si no, 0.
static int fat_find_in_dir(u32 dir_cluster, const char *name, u32 *out_cluster, u32 *out_size, int *out_is_dir) {
    char name83[11];
    int valid = (fat_name_to_83(name, name83) == 0);
    u32 entry_cluster, entry_off;
    return fat_find_entry(dir_cluster, name, name83, valid, &entry_cluster, &entry_off, out_cluster, out_size,
                           out_is_dir);
}

// Sólo mira nombres CORTOS (bytes crudos, sin decodificar LFN): la usa
// fat_gen_short_name para probar candidatos "~N" hasta encontrar uno que
// no colisione con ningún nombre corto ya existente en el directorio.
static int fat_short_name_exists(u32 dir_cluster, const char name83[11]) {
    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    u32 cluster = dir_cluster;
    while (1) {
        fs_read_cluster(cluster, buf);
        u32 nentries = fs_cluster_bytes() / 32;
        for (u32 i = 0; i < nentries; i++) {
            const u8 *e = buf + i * 32;
            if (e[0] == FAT_ENTRY_FREE_MARK) {
                return 0;
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
                if (e[j] != (u8)name83[j]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                return 1;
            }
        }
        cluster = fat_next_cluster(cluster);
        if (cluster >= FAT_EOC_MIN) {
            return 0;
        }
    }
}

// Genera un nombre corto "básico~N.ext" para un nombre largo que no entra
// en 8.3 (algoritmo simplificado del que usa VFAT de verdad: hasta 6
// caracteres de la base — sin espacios, mayúsculas — + "~" + un dígito
// 1-9, más los primeros 3 de la extensión tras el PRIMER punto). Prueba
// N=1..9 hasta encontrar uno que no colisione (fat_short_name_exists); si
// los 9 están ocupados (no debería pasar en este proyecto) se queda con
// "X~9.ext" igual, mejor que loopear infinito.
static void fat_gen_short_name(const char *longname, u32 dir_cluster, char out83[11]) {
    char base[6];
    char ext[3];
    int bi = 0, ei = 0, i = 0;
    while (longname[i] && longname[i] != '.' && bi < 6) {
        char c = longname[i++];
        if (c == ' ') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        }
        base[bi++] = c;
    }
    while (longname[i] && longname[i] != '.') {
        i++;
    }
    if (longname[i] == '.') {
        i++;
        while (longname[i] && ei < 3) {
            char c = longname[i++];
            if (c == ' ') {
                continue;
            }
            if (c >= 'a' && c <= 'z') {
                c -= 32;
            }
            ext[ei++] = c;
        }
    }
    if (bi == 0) {
        base[bi++] = '_';
    }

    for (int tail = 1; tail <= 9; tail++) {
        char cand[11];
        for (int k = 0; k < 11; k++) {
            cand[k] = ' ';
        }
        int oi = 0;
        for (int k = 0; k < bi; k++) {
            cand[oi++] = base[k];
        }
        cand[oi++] = '~';
        cand[oi++] = (char)('0' + tail);
        for (int k = 0; k < ei; k++) {
            cand[8 + k] = ext[k];
        }
        if (!fat_short_name_exists(dir_cluster, cand)) {
            for (int k = 0; k < 11; k++) {
                out83[k] = cand[k];
            }
            return;
        }
    }
    for (int k = 0; k < 11; k++) {
        out83[k] = ' ';
    }
    out83[0] = 'X';
    out83[1] = '~';
    out83[2] = '9';
}

// Busca `n_entries` posiciones libres CONSECUTIVAS en el directorio
// (n_lfn + 1 corta, para un nombre largo nuevo; o 1 sola para uno corto).
// Como este proyecto nunca borra archivos (ver el comentario de scope
// arriba del archivo), el primer marcador de fin (0x00) implica que TODO
// lo que sigue en ese mismo cluster también está libre — no hace falta
// lidiar con huecos borrados intercalados de verdad. -1 si no entran en lo
// que queda del cluster (no hacemos crecer directorios).
static int fat_dir_alloc_run(u32 dir_cluster, u32 n_entries, u32 *out_cluster, u32 *out_off) {
    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    u32 cluster = dir_cluster;
    while (1) {
        fs_read_cluster(cluster, buf);
        u32 nentries = fs_cluster_bytes() / 32;
        for (u32 i = 0; i < nentries; i++) {
            const u8 *e = buf + i * 32;
            if (e[0] == FAT_ENTRY_FREE_MARK) {
                if (i + n_entries > nentries) {
                    return -1; // no entran en lo que queda del cluster
                }
                *out_cluster = cluster;
                *out_off = i * 32;
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

// Vuelca una entrada de directorio de 32 bytes NUEVA en `dir_cluster`,
// offset `entry_off`: nombre 8.3, ATTR_ARCHIVE, cluster inicial y tamaño.
// Fecha y hora en 0 — no llevamos reloj real de pared. Para un nombre que
// no entra en 8.3, ver fat_write_long_entry (arma las LFN + esto mismo).
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

// Como fat_write_dir_entry, pero además arma las `n_lfn` entradas LFN que
// preceden a la corta con el nombre largo real (case preservado).
// `entry_off` es donde arranca la PRIMERA LFN (la de mayor secuencia,
// ord|0x40) — fat_dir_alloc_run ya garantizó que las n_lfn+1 entradas
// entran consecutivas ahí, así que esto es un solo fs_read_cluster +
// fs_write_cluster (nunca cruza un límite de cluster).
static void fat_write_long_entry(u32 dir_cluster, u32 entry_off, const char *long_name, const char name83[11],
                                  u32 file_cluster, u32 file_size) {
    u32 len = fs_str_len(long_name);
    u32 n_lfn = (len + 12) / 13;
    u8 checksum = fat_short_checksum((const u8 *)name83);

    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    fs_read_cluster(dir_cluster, buf);

    for (u32 seq = n_lfn; seq >= 1; seq--) {
        u8 *e = buf + entry_off + (n_lfn - seq) * 32;
        u8 ord = (u8)seq;
        if (seq == n_lfn) {
            ord |= 0x40; // primera físicamente = mayor secuencia = último fragmento del nombre
        }
        e[0] = ord;
        e[11] = FAT_ATTR_LFN;
        e[12] = 0;
        e[13] = checksum;
        e[26] = 0;
        e[27] = 0;
        u32 base = (seq - 1) * 13;
        for (int k = 0; k < 13; k++) {
            u32 pos = base + (u32)k;
            u16 ch;
            if (pos < len) {
                ch = (u16)(u8)long_name[pos];
            } else if (pos == len) {
                ch = 0;
            } else {
                ch = 0xFFFF;
            }
            e[fat_lfn_char_off[k]] = (u8)ch;
            e[fat_lfn_char_off[k] + 1] = (u8)(ch >> 8);
        }
    }

    u8 *short_e = buf + entry_off + n_lfn * 32;
    for (int i = 0; i < 11; i++) {
        short_e[i] = (u8)name83[i];
    }
    short_e[11] = 0x20; // ATTR_ARCHIVE
    for (int i = 12; i < 26; i++) {
        short_e[i] = 0;
    }
    short_e[20] = (u8)(file_cluster >> 16);
    short_e[21] = (u8)(file_cluster >> 24);
    short_e[26] = (u8)file_cluster;
    short_e[27] = (u8)(file_cluster >> 8);
    short_e[28] = (u8)file_size;
    short_e[29] = (u8)(file_size >> 8);
    short_e[30] = (u8)(file_size >> 16);
    short_e[31] = (u8)(file_size >> 24);

    fs_write_cluster(dir_cluster, buf);
}

// Pisa sólo cluster+tamaño de una entrada YA EXISTENTE en `entry_off`
// (nombre corto y LFN, si tenía, quedan tal cual — el nombre no cambia al
// sobrescribir el contenido de un archivo). La usa fs_write cuando el path
// ya existía; para una entrada nueva de punta a punta, ver
// fat_write_dir_entry/fat_write_long_entry (esas sí arman el nombre).
static void fat_update_entry(u32 dir_cluster, u32 entry_off, u32 file_cluster, u32 file_size) {
    static u8 buf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    fs_read_cluster(dir_cluster, buf);
    u8 *e = buf + entry_off;
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

#define FS_MAX_COMPONENT FS_MAX_LONGNAME

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

        if (!fat_find_in_dir(cluster, comp, &cluster, &size, &is_dir)) {
            return -1;
        }
    }
    *out_cluster = cluster;
    *out_size = size;
    *out_is_dir = is_dir;
    return 0;
}

// Separa `path` en directorio padre (resuelto a cluster) + nombre del
// último componente: da tanto la versión 8.3 (out_name83, válida sólo si
// *out_name83_valid) como el nombre largo tal cual (out_longname, siempre)
// — fs_write decide con cuál arma la entrada según entre o no en 8.3. -1
// si el padre no existe/no es directorio, o el nombre ni siquiera entra en
// FS_MAX_LONGNAME.
static int fs_resolve_parent(const char *path, u32 *out_dir_cluster, char out_name83[11],
                              char out_longname[FS_MAX_LONGNAME], int *out_name83_valid) {
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
    if (fs_str_len(filename) >= FS_MAX_LONGNAME) {
        return -1; // ni siquiera entra como nombre largo
    }
    fs_str_copy(out_longname, filename, FS_MAX_LONGNAME);
    *out_name83_valid = (fat_name_to_83(filename, out_name83) == 0);

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
            u32 size;
            if (!fat_find_in_dir(cluster, comp, &cluster, &size, &is_dir)) {
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
// entra ni en FS_MAX_LONGNAME, o el directorio está lleno.
int fs_write(const char *path, const u8 *data, u32 len) {
    u32 dir_cluster;
    char name83[11];
    char longname[FS_MAX_LONGNAME];
    int name83_valid;
    if (fs_resolve_parent(path, &dir_cluster, name83, longname, &name83_valid) < 0) {
        return -1;
    }

    u32 entry_cluster, entry_off;
    u32 existing_cluster, existing_size;
    int existing_is_dir;
    int existed = fat_find_entry(dir_cluster, longname, name83, name83_valid, &entry_cluster, &entry_off,
                                  &existing_cluster, &existing_size, &existing_is_dir);
    (void)existing_cluster; // "crear" trunca: la cadena vieja (si había) queda huérfana, ver comentario arriba
    (void)existing_size;
    (void)existing_is_dir;

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

    if (existed) {
        // El nombre (corto y LFN, si tenía) ya está bien en el disco: sólo
        // hace falta apuntar la entrada corta al cluster nuevo.
        fat_update_entry(entry_cluster, entry_off, first_cluster, len);
        return 0;
    }

    char gen83[11];
    const char *use83 = name83;
    u32 n_lfn = 0;
    if (!name83_valid) {
        fat_gen_short_name(longname, dir_cluster, gen83);
        use83 = gen83;
        n_lfn = (fs_str_len(longname) + 12) / 13;
    }
    u32 alloc_cluster, alloc_off;
    if (fat_dir_alloc_run(dir_cluster, n_lfn + 1, &alloc_cluster, &alloc_off) < 0) {
        return -1;
    }
    if (n_lfn > 0) {
        fat_write_long_entry(alloc_cluster, alloc_off, longname, use83, first_cluster, len);
    } else {
        fat_write_dir_entry(alloc_cluster, alloc_off, use83, first_cluster, len);
    }
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
    static char lfn_buf[FS_MAX_LONGNAME];
    u8 lfn_checksum = 0;
    int lfn_valid = 0;
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
                lfn_valid = 0; // "." y ".." tambien caen acá (mismo criterio que ls sin -a)
                continue;
            }
            u8 attr = e[11];
            if (attr == FAT_ATTR_LFN) {
                fat_lfn_accumulate(e, lfn_buf, &lfn_checksum, &lfn_valid);
                continue;
            }
            if (attr & FAT_ATTR_VOLUME_ID) {
                lfn_valid = 0;
                continue;
            }
            char name[FS_MAX_LONGNAME];
            if (lfn_valid && fat_short_checksum(e) == lfn_checksum) {
                fs_str_copy(name, lfn_buf, FS_MAX_LONGNAME);
            } else {
                fat_83_to_name(e, name);
            }
            lfn_valid = 0;
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
// (bump allocator sin free, ver el comentario de scope arriba del
// archivo), no hace falta más que eso todavía.
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
