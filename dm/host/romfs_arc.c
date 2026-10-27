// RomFS archive reader: see romfs_arc.h for the on-disk format.
#include "romfs_arc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

typedef struct __attribute__((packed)) {
    uint32_t name_off, shard;
    uint64_t off, size;
    uint32_t mode, res;
} filerec;

typedef struct __attribute__((packed)) {
    uint32_t name_off, first_child, child_count, res;
} dirrec;

static struct {
    int open;
    void *idx;            // mmap of the index file
    size_t idx_size;
    romfsarc_hdr hdr;
    const char *pool;
    const filerec *files;
    const dirrec *dirs;
    const uint32_t *children;
    char base[1024];
    int shard_fd[256];
    char shard_path[256][1024];
    pthread_mutex_t lock;
} A = { .shard_fd = { [0 ... 255] = -1 }, .lock = PTHREAD_MUTEX_INITIALIZER };

static uint64_t rd64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }

char g_arc_base[1024];   // the archive's base path, matched against mount directories by hle_fs.c

static const char *fname(const filerec *f) { return A.pool + rd32((const uint8_t *)f); }
static const char *dname(const dirrec *d) { return A.pool + rd32((const uint8_t *)d); }


/* Validate all index regions and references before publishing mapped pointers. */

static int arc_valid_path(const char *s, int allow_root) {
    if (!s) return 0;
    if (!*s) return allow_root;
    if (*s == '/' || strchr(s, '\\') || strstr(s, "//")) return 0;

    const char *part = s;
    for (const char *q = s; ; ++q) {
        if (*q == '/' || *q == '\0') {
            size_t n = (size_t)(q - part);
            if (n == 0 ||
                (n == 1 && part[0] == '.') ||
                (n == 2 && part[0] == '.' && part[1] == '.'))
                return 0;
            if (*q == '\0') break;
            part = q + 1;
        }
    }
    return 1;
}

static const char *arc_leaf(const char *s) {
    const char *p = strrchr(s, '/');
    return p ? p + 1 : s;
}

static int arc_direct_child(const char *parent, const char *child) {
    const char *rest;

    if (!*parent) {
        rest = child;
    } else {
        size_t n = strlen(parent);
        if (strncmp(parent, child, n) != 0 || child[n] != '/')
            return 0;
        rest = child + n + 1;
    }

    return *rest != '\0' && strchr(rest, '/') == NULL;
}
static int validate_index(const uint8_t *b, size_t sz) {
    if (!b || sz < ROMFSARC_HDR) { errno = EINVAL; return -1; }

    uint32_t nshard = rd32(b + 12);
    uint64_t nf = rd64(b + 24), nd = rd64(b + 32);
    uint64_t pool_sz = rd64(b + 40);

    if (nshard == 0 || nshard > 256 || nd == 0) {
        errno = EINVAL; return -1;
    }

    /* Use remaining-length division checks to avoid multiplication overflow. */
    size_t rem = sz - ROMFSARC_HDR;
    if (pool_sz > rem || pool_sz > SIZE_MAX) {
        errno = EINVAL; return -1;
    }
    size_t pool_n = (size_t)pool_sz;
    rem -= pool_n;
    if (nf > rem / ROMFSARC_FILEREC) {
        errno = EINVAL; return -1;
    }
    size_t file_bytes = (size_t)nf * ROMFSARC_FILEREC;
    rem -= file_bytes;
    if (nd > rem / ROMFSARC_DIRREC) {
        errno = EINVAL; return -1;
    }
    size_t dir_bytes = (size_t)nd * ROMFSARC_DIRREC;
    rem -= dir_bytes;
    if ((rem & 3u) != 0) {
        errno = EINVAL; return -1;
    }

    const char *pool = (const char *)(b + ROMFSARC_HDR);
    const filerec *files = (const filerec *)(pool + pool_n);
    const dirrec *dirs = (const dirrec *)((const uint8_t *)files + file_bytes);
    const uint32_t *kids = (const uint32_t *)((const uint8_t *)dirs + dir_bytes);
    size_t nkids = rem / sizeof(uint32_t);

    /* Every referenced name must begin inside the pool and terminate there. */
    for (uint64_t i = 0; i < nf; i++) {
        uint32_t no = rd32((const uint8_t *)&files[i]);
        uint32_t shard = rd32((const uint8_t *)&files[i] + 4);
        uint64_t off = rd64((const uint8_t *)&files[i] + 8);
        uint64_t len = rd64((const uint8_t *)&files[i] + 16);
        if ((uint64_t)no >= pool_sz || shard >= nshard ||
            off > UINT64_MAX - len ||
            !memchr(pool + no, 0, pool_n - no)) {
            errno = EINVAL; return -1;
        }
        const char *name = pool + no;
        if (!arc_valid_path(name, 0)) {
            errno = EINVAL; return -1;
        }
        if (i && strcmp(pool + rd32((const uint8_t *)&files[i - 1]), name) >= 0) {
            errno = EINVAL; return -1;
        }
    }

    for (uint64_t i = 0; i < nd; i++) {
        uint32_t no = rd32((const uint8_t *)&dirs[i]);
        uint32_t first = rd32((const uint8_t *)&dirs[i] + 4);
        uint32_t count = rd32((const uint8_t *)&dirs[i] + 8);
        if ((uint64_t)no >= pool_sz ||
            !memchr(pool + no, 0, pool_n - no) ||
            (uint64_t)first > nkids ||
            (uint64_t)count > nkids - first) {
            errno = EINVAL; return -1;
        }
        const char *name = pool + no;
        if (i == 0) {
            if (*name) { errno = EINVAL; return -1; }
        } else {
            if (!arc_valid_path(name, 0)) {
                errno = EINVAL; return -1;
            }
            if (strcmp(pool + rd32((const uint8_t *)&dirs[i - 1]), name) >= 0) {
                errno = EINVAL; return -1;
            }
        }
    }

    /* The root is first; directory child ranges must partition the child array. */
    uint64_t expected_first = 0;
    for (uint64_t i = 0; i < nd; i++) {
        uint32_t first = rd32((const uint8_t *)&dirs[i] + 4);
        uint32_t count = rd32((const uint8_t *)&dirs[i] + 8);
        if (first != expected_first) { errno = EINVAL; return -1; }
        expected_first += count;
        if (expected_first > nkids) { errno = EINVAL; return -1; }
    }
    if (expected_first != nkids) { errno = EINVAL; return -1; }

    /* Every child tag must reference an existing table entry. */
    for (size_t i = 0; i < nkids; i++) {
        uint32_t tag = rd32((const uint8_t *)&kids[i]);
        if (tag & ROMFSARC_CHILD_DIR) {
            if ((uint64_t)(tag & ~ROMFSARC_CHILD_DIR) >= nd) {
                errno = EINVAL; return -1;
            }
        } else if ((uint64_t)tag >= nf) {
            errno = EINVAL; return -1;
        }
    }

    /* Validate direct parent-child relationships, ordering, and complete coverage. */
    if (nf > SIZE_MAX || nd > SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    uint8_t *seen_files = (uint8_t *)calloc((size_t)(nf ? nf : 1), 1);
    uint8_t *seen_dirs  = (uint8_t *)calloc((size_t)nd, 1);
    if (!seen_files || !seen_dirs) {
        free(seen_files);
        free(seen_dirs);
        errno = ENOMEM;
        return -1;
    }

    int tree_error = 0;

    for (uint64_t di = 0; di < nd && !tree_error; di++) {
        const char *parent = pool + rd32((const uint8_t *)&dirs[di]);
        uint32_t first = rd32((const uint8_t *)&dirs[di] + 4);
        uint32_t count = rd32((const uint8_t *)&dirs[di] + 8);
        const char *previous_leaf = NULL;

        for (uint32_t j = 0; j < count; j++) {
            uint32_t tag = rd32((const uint8_t *)&kids[first + j]);
            const char *child_path;

            if (tag & ROMFSARC_CHILD_DIR) {
                uint32_t ci = tag & ~ROMFSARC_CHILD_DIR;
                if (ci == 0 || (uint64_t)ci >= nd || seen_dirs[ci]) {
                    tree_error = 1;
                    break;
                }
                seen_dirs[ci] = 1;
                child_path = pool + rd32((const uint8_t *)&dirs[ci]);
            } else {
                if ((uint64_t)tag >= nf || seen_files[tag]) {
                    tree_error = 1;
                    break;
                }
                seen_files[tag] = 1;
                child_path = pool + rd32((const uint8_t *)&files[tag]);
            }

            if (!arc_direct_child(parent, child_path)) {
                tree_error = 1;
                break;
            }

            const char *leaf = arc_leaf(child_path);
            if (previous_leaf && strcmp(previous_leaf, leaf) >= 0) {
                tree_error = 1;
                break;
            }
            previous_leaf = leaf;
        }
    }

    /* Every file and every non-root directory must appear exactly once. */
    if (!tree_error) {
        for (uint64_t i = 0; i < nf; i++) {
            if (!seen_files[i]) {
                tree_error = 1;
                break;
            }
        }
    }
    if (!tree_error) {
        for (uint64_t i = 1; i < nd; i++) {
            if (!seen_dirs[i]) {
                tree_error = 1;
                break;
            }
        }
    }

    free(seen_files);
    free(seen_dirs);

    if (tree_error) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

// Binary search the file table by path. Returns the record or NULL.
static const filerec *find_file(const char *rel) {
    uint64_t lo = 0, hi = A.hdr.file_count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = strcmp(fname(&A.files[mid]), rel);
        if (c == 0) return &A.files[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

// Binary search the directory table by path. Returns the record or NULL.
static const dirrec *find_dir(const char *rel) {
    uint64_t lo = 0, hi = A.hdr.dir_count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        int c = strcmp(dname(&A.dirs[mid]), rel);
        if (c == 0) return &A.dirs[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

// Open and validate one index file. Returns the mapping (and its size) or MAP_FAILED.
static void *map_index(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return MAP_FAILED;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); errno = EIO; return MAP_FAILED; }
    if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX ||
        (uintmax_t)st.st_size < ROMFSARC_HDR) {
        close(fd); errno = EINVAL; return MAP_FAILED;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return MAP_FAILED;
    const uint8_t *b = m;
    if (memcmp(b, ROMFSARC_MAGIC, 8) || rd32(b + 8) != ROMFSARC_VERSION) {
        munmap(m, (size_t)st.st_size); errno = EINVAL; return MAP_FAILED;
    }
    *size = (size_t)st.st_size;
    return m;
}


/* Check shard files and ensure every indexed file extent fits its shard. */
static int validate_shards(const char *base, const uint8_t *b,
                           uint32_t nshard, uint64_t nf) {
    uint64_t *lengths = calloc(nshard, sizeof(*lengths));
    if (!lengths) return -1;

    int result = -1;
    for (uint32_t i = 0; i < nshard; i++) {
        char path[1200];
        if (snprintf(path, sizeof(path), "%s.%02u", base, i) >= (int)sizeof(path)) {
            errno = ENAMETOOLONG;
            goto done;
        }

        int fd = open(path, O_RDONLY);
        if (fd < 0) goto done;

        struct stat st;
        int stat_result = fstat(fd, &st);
        int saved_errno = errno;
        close(fd);
        if (stat_result != 0) {
            errno = saved_errno;
            goto done;
        }
        if (st.st_size < 0) {
            errno = EINVAL;
            goto done;
        }
        lengths[i] = (uint64_t)st.st_size;
    }

    uint64_t pool_sz = rd64(b + 40);
    const uint8_t *files = b + ROMFSARC_HDR + (size_t)pool_sz;
    for (uint64_t i = 0; i < nf; i++) {
        const uint8_t *r = files + (size_t)i * ROMFSARC_FILEREC;
        uint32_t shard = rd32(r + 4);
        uint64_t off = rd64(r + 8);
        uint64_t size = rd64(r + 16);

        if (shard >= nshard || off > lengths[shard] ||
            size > lengths[shard] - off) {
            errno = EINVAL;
            goto done;
        }
    }

    result = 0;

done:
    free(lengths);
    return result;
}

int romfsarc_open(const char *base, romfsarc_hdr *out) {
    // The base is where the shards live, without the ".idx". Callers pass either the base itself
    // (DM_ROMFS=/games/totk/romfs with romfs.idx beside it) or a directory that holds the archive
    // (DM_ROMFS=Documents/romfs with Documents/romfs/romfs.idx inside, which is what the iOS app
    // sets). Try both, and a base that already carries the .idx suffix.
    char cand[4][1200];
    char stripped[1200];
    strlcpy(stripped, base, sizeof stripped);
    size_t sl = strlen(stripped);
    if (sl > 4 && !strcmp(stripped + sl - 4, ".idx")) stripped[sl - 4] = 0;
    snprintf(cand[0], sizeof cand[0], "%s.idx", stripped);          // <base>.idx
    snprintf(cand[1], sizeof cand[1], "%s/romfs.idx", stripped);    // <base>/romfs.idx
    snprintf(cand[2], sizeof cand[2], "%s/romfs/romfs.idx", stripped);  // one more level (a bundle-style layout)
    cand[3][0] = 0;
    void *m = MAP_FAILED; size_t sz = 0; char used[1200] = "";
    for (int i = 0; i < 3; i++) {
        m = map_index(cand[i], &sz);
        if (m != MAP_FAILED) { strlcpy(used, cand[i], sizeof used); break; }
    }
    if (m == MAP_FAILED) return -1;
    // The resolved base is the index path minus ".idx": shard paths are built from it
    size_t ul = strlen(used); if (ul > 4) used[ul - 4] = 0;
    const uint8_t *b = m;
    romfsarc_hdr h;
    h.shard_count = rd32(b + 12);
    h.file_count = rd64(b + 24);
    h.dir_count = rd64(b + 32);
    h.tree_hash = rd64(b + 56);
    uint64_t pool_sz = rd64(b + 40);

    if (strlen(used) >= sizeof A.base) {
        munmap(m, sz);
        errno = ENAMETOOLONG;
        return -1;
    }

    if (validate_index(b, sz) != 0) {
        int saved_errno = errno ? errno : EINVAL;
        munmap(m, sz);
        errno = saved_errno;
        return -1;
    }

    if (validate_shards(used, b, h.shard_count, h.file_count) != 0) {
        int saved_errno = errno ? errno : EINVAL;
        munmap(m, sz);
        errno = saved_errno;
        return -1;
    }

    pthread_mutex_lock(&A.lock);
    /* Dispose of a previously opened archive only after the new index validates. */
    for (int i = 0; i < 256; i++) {
        if (A.shard_fd[i] >= 0) close(A.shard_fd[i]);
        A.shard_fd[i] = -1;
        A.shard_path[i][0] = 0;
    }
    if (A.idx) munmap(A.idx, A.idx_size);
    A.open = 1; A.idx = m; A.idx_size = sz; A.hdr = h;
    A.pool = (const char *)(b + ROMFSARC_HDR);
    A.files = (const filerec *)(b + ROMFSARC_HDR + pool_sz);
    A.dirs = (const dirrec *)((const uint8_t *)A.files + h.file_count * ROMFSARC_FILEREC);
    A.children = (const uint32_t *)((const uint8_t *)A.dirs + h.dir_count * ROMFSARC_DIRREC);
    for (int i = 0; i < 256; i++) { A.shard_fd[i] = -1; A.shard_path[i][0] = 0; }
    // Shards are named after the index file's own base, so <dir>/romfs.idx pairs with <dir>/romfs.00
    strlcpy(A.base, used, sizeof A.base);
    strlcpy(g_arc_base, used, sizeof g_arc_base);
    pthread_mutex_unlock(&A.lock);
    if (out) *out = h;
    return 0;
}

int romfsarc_active(void) { return A.open; }

int romfsarc_stat_file(const char *rel, uint32_t *shard, uint64_t *off, uint64_t *size) {
    if (!A.open) return 0;
    const filerec *f = find_file(rel);
    if (!f) return 0;
    const uint8_t *r = (const uint8_t *)f;
    if (shard) *shard = rd32(r + 4);
    if (off) *off = rd64(r + 8);
    if (size) *size = rd64(r + 16);
    return 1;
}

int romfsarc_is_dir(const char *rel) {
    if (!A.open) return 0;
    return find_dir(rel) != NULL;
}

static int shard_fd(uint32_t i) {
    if (i >= A.hdr.shard_count) { errno = EINVAL; return -1; }
    if (A.shard_fd[i] >= 0) return A.shard_fd[i];
    pthread_mutex_lock(&A.lock);
    if (A.shard_fd[i] < 0) {
        char p[1024];
        if (snprintf(p, sizeof p, "%s.%02u", A.base, i) < (int)sizeof p) {
            int fd = open(p, O_RDONLY);
            if (fd >= 0) { A.shard_fd[i] = fd; strlcpy(A.shard_path[i], p, sizeof A.shard_path[i]); }
        }
    }
    int fd = A.shard_fd[i];
    pthread_mutex_unlock(&A.lock);
    return fd;
}

ssize_t romfsarc_pread(uint32_t shard, uint64_t off, void *buf, size_t n) {
    if (n == 0) return 0;
    if (!buf || off > (uint64_t)INT64_MAX || n > (size_t)SSIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }

    int fd = shard_fd(shard);
    if (fd < 0) return -1;

    size_t done = 0;
    while (done < n) {
        /* Keep both uint64_t addition and signed off_t conversion in range. */
        if ((uint64_t)done > (uint64_t)INT64_MAX - off) {
            errno = EOVERFLOW;
            return -1;
        }

        uint64_t current = off + (uint64_t)done;
        ssize_t r = pread(fd, (uint8_t *)buf + done,
                          n - done, (off_t)current);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        done += (size_t)r;
    }
    return (ssize_t)done;
}

int romfsarc_list(const char *rel, romfsarc_ent *out, int max) {
    if (!A.open) return -1;
    const dirrec *d = find_dir(rel);
    if (!d) return -1;
    int n = 0;
    const uint8_t *dr = (const uint8_t *)d;
    uint32_t first_child = rd32(dr + 4);
    uint32_t child_count = rd32(dr + 8);
    for (uint32_t i = 0; i < child_count && n < max; i++) {
        uint32_t tag = rd32((const uint8_t *)A.children + (size_t)(first_child + i) * sizeof(uint32_t));
        romfsarc_ent *e = &out[n];
        if (tag & ROMFSARC_CHILD_DIR) {
            const dirrec *c = &A.dirs[tag & ~ROMFSARC_CHILD_DIR];
            // The directory table stores full paths; a child list entry carries only the leaf name
            const char *full = dname(c), *leaf = strrchr(full, '/');
            strlcpy(e->name, leaf ? leaf + 1 : full, sizeof e->name);
            e->is_dir = 1; e->size = 0;
        } else {
            const filerec *c = &A.files[tag];
            // The file table stores full paths; a child list entry carries only the leaf name
            const char *full = fname(c), *leaf = strrchr(full, '/');
            strlcpy(e->name, leaf ? leaf + 1 : full, sizeof e->name);
            uint64_t file_size = rd64((const uint8_t *)c + 16);
            if (file_size > INT64_MAX) { errno = EOVERFLOW; return -1; }
            e->is_dir = 0; e->size = (int64_t)file_size;
        }
        n++;
    }
    return n;
}

int romfsarc_count(const char *rel) {
    if (!A.open) return -1;
    const dirrec *d = find_dir(rel);
    if (!d) return -1;
    uint32_t count = rd32((const uint8_t *)d + 8);
    return count > INT_MAX ? INT_MAX : (int)count;
}

void romfsarc_close(void) {
    pthread_mutex_lock(&A.lock);
    for (int i = 0; i < 256; i++) if (A.shard_fd[i] >= 0) { close(A.shard_fd[i]); A.shard_fd[i] = -1; }
    if (A.idx) { munmap(A.idx, A.idx_size); A.idx = NULL; }
    A.open = 0;
    pthread_mutex_unlock(&A.lock);
}
