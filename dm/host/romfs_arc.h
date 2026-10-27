// RomFS archive (ROMFSARC): a whole RomFS tree in one index file plus a few hundred shard files.
//
// Why: a RomFS like Tears of the Kingdom's is 300,000 files. Copying that into an App's Documents
// takes hours over the Files app / Finder, and a 16 GB IPA with that many entries does not install
// (PackageExtractionFailed). Packing the tree into ~100 shard files makes both a single copy.
//
// Layout (all integers little-endian):
//   <base>.idx   header, name pool, file table, directory table
//   <base>.<NN>  shard NN: the files' bytes back to back, in file-table order
//
// Header (64 bytes):
//   0x00  char     magic[8]      "ROMFSARC"
//   0x08  uint32   version       1
//   0x0c  uint32   shard_count
//   0x10  uint64   index_size    size of this index file
//   0x18  uint64   file_count
//   0x20  uint64   dir_count
//   0x28  uint64   name_pool_size
//   0x30  uint64   reserved
//   0x38  uint64   tree_hash      FNV-1a over (path, size) of every file, sorted: a cheap identity check
//
// File table: file_count records of 32 bytes, sorted by path (the name pool offset points at the
//             NUL-terminated relative path, '/'-separated, no leading slash):
//   0x00  uint32   name_off
//   0x04  uint32   shard
//   0x08  uint64   offset_in_shard
//   0x10  uint64   size
//   0x18  uint32   mode           0777 bits (the executable bit matters for nothing in RomFS, kept for fidelity)
//   0x1c  uint32   reserved
//
// Directory table: dir_count records of 16 bytes, sorted by path ("" = the root):
//   0x00  uint32   name_off
//   0x04  uint32   first_child   index into the child list
//   0x08  uint32   child_count
//   0x0c  uint32   reserved
// Child list: uint32 file-table / directory-table indices, each tagged: bit 31 set = a directory.
//            Children of one directory are contiguous and sorted by name.
//
// Path lookup is a binary search over the sorted tables; directory listing walks one child range.
// Case sensitivity: Horizon paths are case-sensitive and the archive is built from a case-sensitive
// walk, so the host looks names up exactly (the existing dir_index/case-alias machinery is not used
// for archive mounts).
#ifndef DM_ROMFS_ARC_H
#define DM_ROMFS_ARC_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define ROMFSARC_MAGIC "ROMFSARC"
#define ROMFSARC_VERSION 1u
#define ROMFSARC_HDR 64
#define ROMFSARC_FILEREC 32
#define ROMFSARC_DIRREC 16
#define ROMFSARC_CHILD_DIR 0x80000000u

// Synthetic host path prefix for archive-backed entries (see host_path_ex in hle_fs.c). A path that
// starts with this marker is not a real file: the fs layer routes it to the archive instead.
#define ROMFSARC_SYNTH_PREFIX "@romfsarc:"
#define ROMFSARC_IS_SYNTH(p) (!strncmp((p), ROMFSARC_SYNTH_PREFIX, sizeof ROMFSARC_SYNTH_PREFIX - 1))
#define ROMFSARC_SYNTH_REL(p) ((p) + sizeof ROMFSARC_SYNTH_PREFIX - 1)

typedef struct {
    uint32_t shard_count;
    uint64_t file_count, dir_count;
    uint64_t tree_hash;
} romfsarc_hdr;

// Open the index at <base>.idx. Returns 0 on success, -1 with errno set on failure.
// Keeps the index mapped; shards are opened lazily and cached.
int romfsarc_open(const char *base, romfsarc_hdr *out);

// Whether an archive is open (the fs layer then routes lookups here instead of the host tree).
int romfsarc_active(void);

// The base path the archive was opened from (without ".idx"): the fs layer matches mount
// directories against it, so the mount name ("rom", "content", …) does not matter.
extern char g_arc_base[];

// Resolve a guest path (no leading slash, "" = root) to a file. On success fills the shard index,
// offset and size; returns 1. Returns 0 when the path is not a file (missing or a directory).
int romfsarc_stat_file(const char *rel, uint32_t *shard, uint64_t *off, uint64_t *size);

// Whether the path names a directory ("" = root): 1 yes, 0 no.
int romfsarc_is_dir(const char *rel);

// Read from an open archive file. Same semantics as pread.
ssize_t romfsarc_pread(uint32_t shard, uint64_t off, void *buf, size_t n);

// Directory listing: write the entries of directory <rel> ("" = root) into the caller's array.
// Each entry gets its name and type; files also get their size. Returns the number written, or -1.
typedef struct { char name[0x301]; int is_dir; int64_t size; } romfsarc_ent;
int romfsarc_list(const char *rel, romfsarc_ent *out, int max);

// Number of entries in a directory (unfiltered), or -1.
int romfsarc_count(const char *rel);

void romfsarc_close(void);

#endif
