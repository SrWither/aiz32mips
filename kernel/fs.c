// fs.c — FAT32 de solo lectura sobre storage.h. El disco lo arma mkfs.vfat
// + mtools del lado del host (ver kernel/Makefile, target disk.img): el
// kernel todavía no sabe escribir FAT32 (allocar clusters, actualizar la
// FAT, crear entradas de directorio es harina de otro costal — write-path
// queda para más adelante).
//
// Simplificaciones a propósito para esta primera pasada:
//   - Solo nombres cortos 8.3 (sin VFAT/LFN: se saltean esas entradas).
//   - Sin cache de directorio ni de FAT: cada resolución de path vuelve a
//     leer bloques del disco. Total elección para no complicar todavía;
//     nada de este proyecto necesita esa perf por ahora.
//   - Sin "." ni "..": todo path se resuelve siempre desde la raíz.
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

int fs_read(const char *path, u8 *buf, u32 maxlen) {
    u32 cluster, size;
    int is_dir;
    if (fs_resolve(path, &cluster, &size, &is_dir) < 0 || is_dir) {
        return -1;
    }

    u32 total = (size < maxlen) ? size : maxlen;
    u32 cbytes = fs_cluster_bytes();
    static u8 cbuf[FS_CLUSTER_BUF_BYTES]; // static: ver comentario en fs_ensure_mounted
    u32 off = 0;
    while (off < total && cluster < FAT_EOC_MIN) {
        fs_read_cluster(cluster, cbuf);
        u32 chunk = total - off;
        if (chunk > cbytes) {
            chunk = cbytes;
        }
        for (u32 i = 0; i < chunk; i++) {
            buf[off + i] = cbuf[i];
        }
        off += chunk;
        cluster = fat_next_cluster(cluster);
    }
    return (int)total;
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
