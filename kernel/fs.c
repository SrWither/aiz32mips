// fs.c — filesystem plano mínimo sobre storage.h: bloque 0 = superblock +
// directorio, bloques 1+ asignados linealmente (bump, sin free todavía —
// mismo criterio que pmm_alloc_page en mm.c). Sin subdirectorios.
#include "storage.h"
#include "kernel.h"

#define FS_MAGIC 0x46533332u // "FS32"
#define FS_MAX_FILES 16
#define FS_NAME_LEN 16

typedef struct {
    char name[FS_NAME_LEN]; // name[0]==0 -> entrada libre
    u32 start_block;
    u32 length;
} FsEntry;

// Ocupa exactamente un bloque: así se lee/escribe directo con
// storage_read_block/storage_write_block, sin buffer intermedio.
typedef struct {
    u32 magic;
    u32 next_free_block;
    FsEntry entries[FS_MAX_FILES];
    u8 reserved[STORAGE_BLOCK_SIZE - 8 - FS_MAX_FILES * sizeof(FsEntry)];
} FsSuperblock;

static FsSuperblock sb;
static int mounted = 0;

static int fs_str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void fs_str_copy(char *dst, const char *src, int maxlen) {
    int i = 0;
    for (; i < maxlen - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static void fs_format(void) {
    sb.magic = FS_MAGIC;
    sb.next_free_block = 1; // el bloque 0 es el superblock
    for (int i = 0; i < FS_MAX_FILES; i++) {
        sb.entries[i].name[0] = 0;
    }
    storage_write_block(0, (u8 *)&sb);
}

// Montaje perezoso: recién toca el device la primera vez que se usa un
// comando de FS, así ningún test que no lo use se ve afectado.
static void fs_ensure_mounted(void) {
    if (mounted) {
        return;
    }
    storage_read_block(0, (u8 *)&sb);
    if (sb.magic != FS_MAGIC) {
        fs_format();
    }
    mounted = 1;
}

static FsEntry *fs_find(const char *name) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (sb.entries[i].name[0] && fs_str_eq(sb.entries[i].name, name)) {
            return &sb.entries[i];
        }
    }
    return 0;
}

static u32 fs_ceil_div(u32 a, u32 b) {
    return (a + b - 1) / b;
}

int fs_create(const char *name, const u8 *data, u32 len) {
    fs_ensure_mounted();

    FsEntry *e = fs_find(name);
    if (!e) {
        for (int i = 0; i < FS_MAX_FILES; i++) {
            if (sb.entries[i].name[0] == 0) {
                e = &sb.entries[i];
                break;
            }
        }
    }
    if (!e) {
        return -1; // directorio lleno
    }

    // Si el nombre ya existía, los bloques viejos quedan huérfanos (sin
    // free todavía, igual que pmm_alloc_page): esta asignación es siempre
    // nueva, nunca reusa espacio.
    u32 nblocks = fs_ceil_div(len, STORAGE_BLOCK_SIZE);
    u32 start = sb.next_free_block;
    u8 block[STORAGE_BLOCK_SIZE];
    for (u32 b = 0; b < nblocks; b++) {
        u32 off = b * STORAGE_BLOCK_SIZE;
        u32 chunk = len - off;
        if (chunk > STORAGE_BLOCK_SIZE) {
            chunk = STORAGE_BLOCK_SIZE;
        }
        for (u32 i = 0; i < STORAGE_BLOCK_SIZE; i++) {
            block[i] = (i < chunk) ? data[off + i] : 0;
        }
        storage_write_block(start + b, block);
    }
    sb.next_free_block += nblocks;

    fs_str_copy(e->name, name, FS_NAME_LEN);
    e->start_block = start;
    e->length = len;
    storage_write_block(0, (u8 *)&sb); // persiste el directorio
    return 0;
}

int fs_read(const char *name, u8 *buf, u32 maxlen) {
    fs_ensure_mounted();
    FsEntry *e = fs_find(name);
    if (!e) {
        return -1;
    }
    u32 total = (e->length < maxlen) ? e->length : maxlen;
    u32 nblocks = fs_ceil_div(total, STORAGE_BLOCK_SIZE);
    u8 block[STORAGE_BLOCK_SIZE];
    for (u32 b = 0; b < nblocks; b++) {
        storage_read_block(e->start_block + b, block);
        u32 off = b * STORAGE_BLOCK_SIZE;
        u32 chunk = total - off;
        if (chunk > STORAGE_BLOCK_SIZE) {
            chunk = STORAGE_BLOCK_SIZE;
        }
        for (u32 i = 0; i < chunk; i++) {
            buf[off + i] = block[i];
        }
    }
    return (int)total;
}

void fs_list(void) {
    fs_ensure_mounted();
    int any = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (sb.entries[i].name[0]) {
            console_puts(sb.entries[i].name);
            console_puts("  ");
            console_put_uint(sb.entries[i].length);
            console_putc('\n');
            any = 1;
        }
    }
    if (!any) {
        console_puts("(disco vacio)\n");
    }
}
