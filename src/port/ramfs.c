/*
 * Tiny RAM file system backing the C library file API (fopen & friends).
 *
 * Doom uses stdio for the IWAD lookup, default.cfg and save games. This
 * module provides the POSIX system calls picolibc needs (open, read, write,
 * lseek, close, unlink, rename, mkdir) on top of a flat table of files:
 *
 *  - "ROM" files reference data that already lives in memory (the IWAD that
 *    was copied from OSPI flash into SDRAM). They are read-only.
 *  - Writable files each get a fixed size slab from an SDRAM pool.
 *
 * Paths are flattened: leading "./" and "/" are removed and the remainder is
 * used as the key, so "./.savegame/doomsav0.dsg" is simply a file name.
 * Writable files live only in RAM and are lost on reset.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ra8_port.h"

#define RAMFS_MAX_FILES     (24)
#define RAMFS_MAX_FDS       (16)
#define RAMFS_NAME_LEN      (64)
#define RAMFS_SLAB_BYTES    (256U * 1024U)
#define RAMFS_SLAB_COUNT    (RA8_RAMFS_POOL_BYTES / RAMFS_SLAB_BYTES)

/* File descriptors 0..2 are the console (stdin/stdout/stderr). */
#define RAMFS_FD_BASE       (3)

typedef struct
{
    bool            in_use;
    bool            rom;
    char            name[RAMFS_NAME_LEN];
    uint8_t       * p_data;
    size_t          size;
    size_t          capacity;
    int             slab;          /* Slab index, -1 for ROM files */
} ramfs_file_t;

typedef struct
{
    bool   in_use;
    int    file;                   /* Index into g_files */
    size_t pos;
    int    flags;
} ramfs_fd_t;

static ramfs_file_t g_files[RAMFS_MAX_FILES];
static ramfs_fd_t   g_fds[RAMFS_MAX_FDS];
static uint8_t    * g_slab_pool = NULL;
static bool         g_slab_used[RAMFS_SLAB_COUNT];

static const char * ramfs_normalise (const char * p_path)
{
    for (;;)
    {
        if (('.' == p_path[0]) && ('/' == p_path[1]))
        {
            p_path += 2;
        }
        else if ('/' == p_path[0])
        {
            p_path += 1;
        }
        else
        {
            return p_path;
        }
    }
}

static int ramfs_find (const char * p_path)
{
    const char * p_name = ramfs_normalise(p_path);

    for (int i = 0; i < RAMFS_MAX_FILES; i++)
    {
        if (g_files[i].in_use && (0 == strcmp(g_files[i].name, p_name)))
        {
            return i;
        }
    }

    return -1;
}

static int ramfs_slab_alloc (void)
{
    if (NULL == g_slab_pool)
    {
        g_slab_pool = sdram_alloc(RA8_RAMFS_POOL_BYTES);
        if (NULL == g_slab_pool)
        {
            return -1;
        }
    }

    for (int i = 0; i < (int) RAMFS_SLAB_COUNT; i++)
    {
        if (!g_slab_used[i])
        {
            g_slab_used[i] = true;

            return i;
        }
    }

    return -1;
}

static int ramfs_create (const char * p_path)
{
    const char * p_name = ramfs_normalise(p_path);

    if (strlen(p_name) >= RAMFS_NAME_LEN)
    {
        errno = ENAMETOOLONG;

        return -1;
    }

    for (int i = 0; i < RAMFS_MAX_FILES; i++)
    {
        if (!g_files[i].in_use)
        {
            int slab = ramfs_slab_alloc();
            if (slab < 0)
            {
                errno = ENOSPC;

                return -1;
            }

            ramfs_file_t * p_file = &g_files[i];
            memset(p_file, 0, sizeof(*p_file));
            p_file->in_use   = true;
            p_file->rom      = false;
            p_file->slab     = slab;
            p_file->p_data   = g_slab_pool + ((size_t) slab * RAMFS_SLAB_BYTES);
            p_file->capacity = RAMFS_SLAB_BYTES;
            strcpy(p_file->name, p_name);

            return i;
        }
    }

    errno = ENFILE;

    return -1;
}

static void ramfs_delete (int index)
{
    ramfs_file_t * p_file = &g_files[index];

    if (p_file->slab >= 0)
    {
        g_slab_used[p_file->slab] = false;
    }

    memset(p_file, 0, sizeof(*p_file));
}

static ramfs_fd_t * ramfs_fd_get (int fd)
{
    int idx = fd - RAMFS_FD_BASE;

    if ((idx < 0) || (idx >= RAMFS_MAX_FDS) || !g_fds[idx].in_use)
    {
        errno = EBADF;

        return NULL;
    }

    return &g_fds[idx];
}

void ramfs_register_rom_file (const char * p_name, const uint8_t * p_data, size_t size)
{
    for (int i = 0; i < RAMFS_MAX_FILES; i++)
    {
        if (!g_files[i].in_use)
        {
            ramfs_file_t * p_file = &g_files[i];
            memset(p_file, 0, sizeof(*p_file));
            p_file->in_use   = true;
            p_file->rom      = true;
            p_file->slab     = -1;
            p_file->p_data   = (uint8_t *) (uintptr_t) p_data;
            p_file->size     = size;
            p_file->capacity = size;
            strncpy(p_file->name, ramfs_normalise(p_name), RAMFS_NAME_LEN - 1);

            return;
        }
    }
}

const uint8_t * ramfs_get_file_data (const char * p_name, size_t * p_size)
{
    int index = ramfs_find(p_name);

    if (index < 0)
    {
        return NULL;
    }

    *p_size = g_files[index].size;

    return g_files[index].p_data;
}

/* ------------------------------------------------------------------------- */
/* POSIX system calls used by picolibc                                        */
/* ------------------------------------------------------------------------- */

int open (const char * p_path, int flags, ...)
{
    int index = ramfs_find(p_path);
    int access = flags & O_ACCMODE;
    bool writing = (O_WRONLY == access) || (O_RDWR == access);

    if (index < 0)
    {
        if (0 == (flags & O_CREAT))
        {
            errno = ENOENT;

            return -1;
        }

        index = ramfs_create(p_path);
        if (index < 0)
        {
            return -1;
        }
    }
    else
    {
        if ((flags & O_CREAT) && (flags & O_EXCL))
        {
            errno = EEXIST;

            return -1;
        }

        if (writing && g_files[index].rom)
        {
            errno = EROFS;

            return -1;
        }

        if (writing && (flags & O_TRUNC))
        {
            g_files[index].size = 0;
        }
    }

    for (int i = 0; i < RAMFS_MAX_FDS; i++)
    {
        if (!g_fds[i].in_use)
        {
            g_fds[i].in_use = true;
            g_fds[i].file   = index;
            g_fds[i].flags  = flags;
            g_fds[i].pos    = (flags & O_APPEND) ? g_files[index].size : 0U;

            return i + RAMFS_FD_BASE;
        }
    }

    errno = EMFILE;

    return -1;
}

int close (int fd)
{
    if (fd < RAMFS_FD_BASE)
    {
        return 0;
    }

    ramfs_fd_t * p_fd = ramfs_fd_get(fd);
    if (NULL == p_fd)
    {
        return -1;
    }

    p_fd->in_use = false;

    return 0;
}

ssize_t read (int fd, void * p_buf, size_t count)
{
    if (fd < RAMFS_FD_BASE)
    {
        return 0;                      /* No console input. */
    }

    ramfs_fd_t * p_fd = ramfs_fd_get(fd);
    if (NULL == p_fd)
    {
        return -1;
    }

    ramfs_file_t * p_file = &g_files[p_fd->file];

    if (p_fd->pos >= p_file->size)
    {
        return 0;
    }

    size_t avail = p_file->size - p_fd->pos;
    if (count > avail)
    {
        count = avail;
    }

    memcpy(p_buf, p_file->p_data + p_fd->pos, count);
    p_fd->pos += count;

    return (ssize_t) count;
}

ssize_t write (int fd, const void * p_buf, size_t count)
{
    if (fd < RAMFS_FD_BASE)
    {
        rtt_log_write(p_buf, count);   /* Console output. */

        return (ssize_t) count;
    }

    ramfs_fd_t * p_fd = ramfs_fd_get(fd);
    if (NULL == p_fd)
    {
        return -1;
    }

    ramfs_file_t * p_file = &g_files[p_fd->file];

    if (p_file->rom || (O_RDONLY == (p_fd->flags & O_ACCMODE)))
    {
        errno = EBADF;

        return -1;
    }

    if (p_fd->flags & O_APPEND)
    {
        p_fd->pos = p_file->size;
    }

    if (p_fd->pos >= p_file->capacity)
    {
        errno = ENOSPC;

        return -1;
    }

    size_t space = p_file->capacity - p_fd->pos;
    if (count > space)
    {
        count = space;
    }

    /* Zero fill a gap left by seeking past the end of the file. */
    if (p_fd->pos > p_file->size)
    {
        memset(p_file->p_data + p_file->size, 0, p_fd->pos - p_file->size);
    }

    memcpy(p_file->p_data + p_fd->pos, p_buf, count);
    p_fd->pos += count;

    if (p_fd->pos > p_file->size)
    {
        p_file->size = p_fd->pos;
    }

    return (ssize_t) count;
}

off_t lseek (int fd, off_t offset, int whence)
{
    ramfs_fd_t * p_fd = ramfs_fd_get(fd);
    if (NULL == p_fd)
    {
        return -1;
    }

    off_t base;
    switch (whence)
    {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = (off_t) p_fd->pos;
            break;
        case SEEK_END:
            base = (off_t) g_files[p_fd->file].size;
            break;
        default:
            errno = EINVAL;

            return -1;
    }

    off_t pos = base + offset;
    if (pos < 0)
    {
        errno = EINVAL;

        return -1;
    }

    p_fd->pos = (size_t) pos;

    return pos;
}

int unlink (const char * p_path)
{
    int index = ramfs_find(p_path);

    if ((index < 0) || g_files[index].rom)
    {
        errno = (index < 0) ? ENOENT : EROFS;

        return -1;
    }

    ramfs_delete(index);

    return 0;
}

int rename (const char * p_old, const char * p_new)
{
    int src = ramfs_find(p_old);

    if ((src < 0) || g_files[src].rom)
    {
        errno = (src < 0) ? ENOENT : EROFS;

        return -1;
    }

    const char * p_name = ramfs_normalise(p_new);
    if (strlen(p_name) >= RAMFS_NAME_LEN)
    {
        errno = ENAMETOOLONG;

        return -1;
    }

    int dst = ramfs_find(p_new);
    if ((dst >= 0) && (dst != src))
    {
        if (g_files[dst].rom)
        {
            errno = EROFS;

            return -1;
        }

        ramfs_delete(dst);
    }

    strcpy(g_files[src].name, p_name);

    return 0;
}

int mkdir (const char * p_path, mode_t mode)
{
    (void) p_path;
    (void) mode;

    return 0;                          /* Directories are implicit. */
}

int isatty (int fd)
{
    return (fd < RAMFS_FD_BASE) ? 1 : 0;
}

int fstat (int fd, struct stat * p_stat)
{
    memset(p_stat, 0, sizeof(*p_stat));

    if (fd < RAMFS_FD_BASE)
    {
        p_stat->st_mode = S_IFCHR;

        return 0;
    }

    ramfs_fd_t * p_fd = ramfs_fd_get(fd);
    if (NULL == p_fd)
    {
        return -1;
    }

    p_stat->st_mode = S_IFREG;
    p_stat->st_size = (off_t) g_files[p_fd->file].size;

    return 0;
}

int stat (const char * p_path, struct stat * p_stat)
{
    memset(p_stat, 0, sizeof(*p_stat));

    int index = ramfs_find(p_path);
    if (index < 0)
    {
        errno = ENOENT;

        return -1;
    }

    p_stat->st_mode = S_IFREG;
    p_stat->st_size = (off_t) g_files[index].size;

    return 0;
}
