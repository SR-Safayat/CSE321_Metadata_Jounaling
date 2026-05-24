#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IMG_NAME "vsfs.img"

#define FS_MAGIC      0x56534653u // "VSFS"
#define JOURNAL_MAGIC 0x4A524E4Cu // "JRNL"

#define BLOCK_SIZE 4096
#define JOURNAL_BLOCKS 16
#define JOURNAL_SIZE_BYTES (JOURNAL_BLOCKS * BLOCK_SIZE)

#define REC_DATA   1
#define REC_COMMIT 2

#define NAME_LEN 28

// ================================================================
// DATA STRUCTURES (Per PDF Specification)
// ================================================================
// Inode and Data Bitmaps are just byte arrays with 4096 elements.
// These are not defined as structs; they are treated as uint8_t arrays.

#pragma pack(push, 1)

// Superblock (Block 0) - 128 bytes
struct superblock {
  uint32_t magic;          // FS magic number
  uint32_t block_size;     // 4096
  uint32_t total_blocks;   // total blocks in FS
  uint32_t inode_count;    // total inodes
  uint32_t journal_block;  // block index of journal
  uint32_t inode_bitmap;   // inode bitmap block
  uint32_t data_bitmap;    // data bitmap block
  uint32_t inode_start;    // first inode block
  uint32_t data_start;     // first data block
  uint8_t  _pad[128 - 9*4];  // padding to 128 bytes
};

// Inode - 128 bytes each (32 inodes per 4096-byte block)
struct inode {
  uint16_t type;           // 0=free, 1-file, 2-dir
  uint16_t links;          // link count
  uint32_t size;           // file size in bytes
  uint32_t direct[8];      // 8 direct block pointers
  uint32_t ctime;          // creation time
  uint32_t mtime;          // modification time
  uint8_t  _pad[128 - (2+2+4 + 8*4 + 4+4)];  // padding to 128 bytes
};

// Directory Entry - 32 bytes each (128 entries per 4096-byte block)
struct dirent {
  uint32_t inode;          // inode number (0 = unused)
  char     name[NAME_LEN]; // null-terminated if shorter
};

// Journal Header (at offset 0 of journal blocks)
struct journal_header {
  uint32_t magic;          // store JOURNAL MAGIC
  uint32_t nbytes_used;    // total bytes currently used
};

// Journal Record Header (precedes every record in journal)
struct rec_header {
  uint16_t type;           // REC_DATA or REC_COMMIT
  uint16_t size;           // total size of this record in bytes
};

// DATA Record - Logs a filesystem block image to be written to home location
struct data_record {
  struct rec_header hdr;   // type = REC_DATA
  uint32_t block_no;       // home block number on disk
  uint8_t  data[4096];     // full block image
};

// COMMIT Record - Seals a transaction (makes it valid/replay-able)
struct commit_record {
  struct rec_header hdr;   // type = REC_COMMIT
};

#pragma pack(pop)

// ================================================================
// UTILITY FUNCTIONS
// ================================================================

static void die(const char *msg) {
  perror(msg);
  exit(1);
}

// ================================================================
// BLOCK I/O FUNCTIONS
// ================================================================

static off_t blk_off(uint32_t block_no) {
  return (off_t)block_no * (off_t)BLOCK_SIZE;
}

static void pread_full(int fd, void *buf, size_t n, off_t off) {
  uint8_t *p = (uint8_t*)buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = pread(fd, p + got, n - got, off + (off_t)got);
    if (r < 0) die("pread");
    if (r == 0) {
      fprintf(stderr, "Unexpected EOF while reading\n");
      exit(1);
    }
    got += (size_t)r;
  }
}

static void pwrite_full(int fd, const void *buf, size_t n, off_t off) {
  const uint8_t *p = (const uint8_t*)buf;
  size_t put = 0;
  while (put < n) {
    ssize_t w = pwrite(fd, p + put, n - put, off + (off_t)put);
    if (w < 0) die("pwrite");
    put += (size_t)w;
  }
}

static void read_block(int fd, uint32_t block_no, void *buf4096) {
  pread_full(fd, buf4096, BLOCK_SIZE, blk_off(block_no));
}

static void write_block(int fd, uint32_t block_no, const void *buf4096) {
  pwrite_full(fd, buf4096, BLOCK_SIZE, blk_off(block_no));
}

// ================================================================
// SUPERBLOCK FUNCTIONS
// ================================================================

static void read_super(int fd, struct superblock *sb) {
  uint8_t blk[BLOCK_SIZE];
  read_block(fd, 0, blk);
  memcpy(sb, blk, sizeof(*sb));
  if (sb->magic != FS_MAGIC) {
    fprintf(stderr, "Bad FS magic (expected VSFS)\n");
    exit(1);
  }
  if (sb->block_size != BLOCK_SIZE) {
    fprintf(stderr, "Unexpected block_size=%u\n", sb->block_size);
    exit(1);
  }
  if (sb->journal_block == 0 || sb->total_blocks < 10) {
    fprintf(stderr, "Superblock fields look invalid\n");
    exit(1);
  }
}

// ================================================================
// BITMAP FUNCTIONS
// ================================================================

static int test_bit(const uint8_t *bitmap, uint32_t idx) {
  return (bitmap[idx / 8] >> (idx % 8)) & 1;
}

static void set_bit(uint8_t *bitmap, uint32_t idx) {
  bitmap[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static int find_free_inode(const uint8_t *ibm, uint32_t inode_count) {
  // inode 0 is root; create should find a free one (typically start from 1)
  for (uint32_t i = 1; i < inode_count; i++) {
    if (!test_bit(ibm, i)) return (int)i;
  }
  return -1;
}

// ================================================================
// DIRECTORY FUNCTIONS
// ================================================================

static int find_free_dirent_slot(const uint8_t *dirblk) {
  const struct dirent *ents = (const struct dirent*)dirblk;
  int n = BLOCK_SIZE / (int)sizeof(struct dirent);
  for (int i = 2; i < n; i++) {
    if (ents[i].inode == 0) return i;
  }
  return -1;
}

// ================================================================
// JOURNAL RECORD SIZE CALCULATIONS
// ================================================================

static uint16_t data_rec_size(void) {
  // header (4 bytes) + block_no (4 bytes) + data (4096 bytes)
  return (uint16_t)(sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE);
}

static uint16_t commit_rec_size(void) {
  // header (4 bytes) only
  return (uint16_t)(sizeof(struct rec_header));
}

// ================================================================
// JOURNAL I/O FUNCTIONS
// ================================================================

static off_t journal_start_off(const struct superblock *sb) {
  return blk_off(sb->journal_block);
}

static void journal_read_header(int fd, const struct superblock *sb, struct journal_header *jh) {
  pread_full(fd, jh, sizeof(*jh), journal_start_off(sb));
}

static void journal_write_header(int fd, const struct superblock *sb, const struct journal_header *jh) {
  pwrite_full(fd, jh, sizeof(*jh), journal_start_off(sb));
}

static void journal_init_if_needed(int fd, const struct superblock *sb, struct journal_header *jh) {
  journal_read_header(fd, sb, jh);
  if (jh->magic != JOURNAL_MAGIC) {
    // Initialize journal header in-place
    jh->magic = JOURNAL_MAGIC;
    jh->nbytes_used = (uint32_t)sizeof(struct journal_header);
    journal_write_header(fd, sb, jh);
  } else {
    // Validate nbytes_used sanity
    if (jh->nbytes_used < sizeof(struct journal_header) ||
        jh->nbytes_used > JOURNAL_SIZE_BYTES) {
      fprintf(stderr, "Journal header corrupted (nbytes_used=%u)\n", jh->nbytes_used);
      exit(1);
    }
  }
}

static void journal_require_exists(int fd, const struct superblock *sb, struct journal_header *jh) {
  journal_read_header(fd, sb, jh);
  if (jh->magic != JOURNAL_MAGIC) {
    fprintf(stderr, "Journal does not exist (magic mismatch). Run create first.\n");
    exit(1);
  }
  if (jh->nbytes_used < sizeof(struct journal_header) ||
      jh->nbytes_used > JOURNAL_SIZE_BYTES) {
    fprintf(stderr, "Journal header corrupted (nbytes_used=%u)\n", jh->nbytes_used);
    exit(1);
  }
}

static void journal_append_data(int fd, const struct superblock *sb, uint32_t *cursor,
                                uint32_t home_block_no, const uint8_t block_img[BLOCK_SIZE]) {
  struct rec_header rh;
  rh.type = REC_DATA;
  rh.size = data_rec_size();

  off_t base = journal_start_off(sb);

  // record header
  pwrite_full(fd, &rh, sizeof(rh), base + (off_t)(*cursor));
  *cursor += (uint32_t)sizeof(rh);

  // home block number
  pwrite_full(fd, &home_block_no, sizeof(home_block_no), base + (off_t)(*cursor));
  *cursor += (uint32_t)sizeof(home_block_no);

  // full 4096B image
  pwrite_full(fd, block_img, BLOCK_SIZE, base + (off_t)(*cursor));
  *cursor += (uint32_t)BLOCK_SIZE;
}

static void journal_append_commit(int fd, const struct superblock *sb, uint32_t *cursor) {
  struct rec_header rh;
  rh.type = REC_COMMIT;
  rh.size = commit_rec_size();
  off_t base = journal_start_off(sb);
  pwrite_full(fd, &rh, sizeof(rh), base + (off_t)(*cursor));
  *cursor += (uint32_t)sizeof(rh);
}

static void journal_clear(int fd, const struct superblock *sb) {
  uint8_t zeros[BLOCK_SIZE];
  memset(zeros, 0, sizeof(zeros));
  for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++) {
    write_block(fd, sb->journal_block + i, zeros);
  }
  struct journal_header jh;
  jh.magic = JOURNAL_MAGIC;
  jh.nbytes_used = (uint32_t)sizeof(struct journal_header);
  journal_write_header(fd, sb, &jh);
}

// ================================================================
// CREATE COMMAND: ./journal create <name>
// ================================================================
// Appends metadata changes to journal without modifying home locations.
// Creates a file entry by logging:
//   - Updated inode bitmap
//   - Updated inode table block(s)
//   - Updated root directory block
//   - A COMMIT record to seal the transaction

static void cmd_create(const char *name) {
  if (!name || name[0] == '\0') {
    fprintf(stderr, "create: name required\n");
    exit(1);
  }
  if (strlen(name) >= NAME_LEN) {
    fprintf(stderr, "create: name too long (max %d)\n", NAME_LEN - 1);
    exit(1);
  }

  int fd = open(IMG_NAME, O_RDWR);
  if (fd < 0) die("open vsfs.img");

  struct superblock sb;
  read_super(fd, &sb);

  // Load current metadata blocks (home locations) to compute new versions in memory
  uint8_t ibm_blk[BLOCK_SIZE];
  uint8_t itable_blk0[BLOCK_SIZE];
  uint8_t itable_blk1[BLOCK_SIZE];
  uint8_t root_dir_blk[BLOCK_SIZE];

  read_block(fd, sb.inode_bitmap, ibm_blk);
  read_block(fd, sb.inode_start + 0, itable_blk0);
  read_block(fd, sb.inode_start + 1, itable_blk1);

  // Root directory block from inode 0's first direct block
  uint32_t root_dir_block = ((struct inode *)itable_blk0)[0].direct[0];
  read_block(fd, root_dir_block, root_dir_blk);

  // Find free inode and free directory slot
  int inum = find_free_inode(ibm_blk, sb.inode_count);
  if (inum < 0) {
    fprintf(stderr, "create: no free inodes\n");
    close(fd);
    exit(1);
  }
  int slot = find_free_dirent_slot(root_dir_blk);
  if (slot < 0) {
    fprintf(stderr, "create: root directory full\n");
    close(fd);
    exit(1);
  }

  // Build new versions in memory (do NOT write to home blocks)
  uint8_t new_ibm_blk[BLOCK_SIZE];
  uint8_t new_root_dir_blk[BLOCK_SIZE];
  uint8_t new_itable_blk0[BLOCK_SIZE];
  uint8_t new_itable_blk1[BLOCK_SIZE];

  memcpy(new_ibm_blk, ibm_blk, BLOCK_SIZE);
  memcpy(new_root_dir_blk, root_dir_blk, BLOCK_SIZE);
  memcpy(new_itable_blk0, itable_blk0, BLOCK_SIZE);
  memcpy(new_itable_blk1, itable_blk1, BLOCK_SIZE);

  // Update inode bitmap
  set_bit(new_ibm_blk, (uint32_t)inum);

  // Update inode table: pick which inode-table block contains inum
  // inode size = 128 bytes => 4096/128 = 32 inodes per block
  const uint32_t inodes_per_block = BLOCK_SIZE / sizeof(struct inode);
  uint32_t which = (uint32_t)inum / inodes_per_block;     // 0 or 1 (since inode table is 2 blocks)
  uint32_t idx   = (uint32_t)inum % inodes_per_block;

  struct inode newi;
  memset(&newi, 0, sizeof(newi));
  newi.type  = 1;          // file
  newi.links = 1;
  newi.size  = 0;
  uint32_t now = (uint32_t)time(NULL);
  newi.ctime = now;
  newi.mtime = now;

  if (which == 0) {
    struct inode *arr = (struct inode*)new_itable_blk0;
    arr[idx] = newi;
  } else if (which == 1) {
    struct inode *arr = (struct inode*)new_itable_blk1;
    arr[idx] = newi;
  } else {
    fprintf(stderr, "create: inode table block out of range (inum=%d)\n", inum);
    close(fd);
    exit(1);
  }

  // Update root dir entry
  struct dirent *ents = (struct dirent*)new_root_dir_blk;
  ents[slot].inode = (uint32_t)inum;
  memset(ents[slot].name, 0, NAME_LEN);
  strncpy(ents[slot].name, name, NAME_LEN - 1);

  // Update root inode mtime and size
  struct inode *root_itable = (struct inode*)new_itable_blk0;
  root_itable[0].mtime = (uint32_t)time(NULL);
  root_itable[0].size = (uint32_t)((slot + 1) * sizeof(struct dirent));

  // Prepare journal
  struct journal_header jh;
  journal_init_if_needed(fd, &sb, &jh);

  // Compute transaction size: records vary based on which inode block changed
  // 1. Inode bitmap (always)
  // 2. Inode table block 0 (always, for root inode update)
  // 3. Inode table block 1 (if new inode is in block 1)
  // 4. Root directory block
  // 5. Commit record
  uint32_t num_data_records = 3;  // bitmap, itable_blk0, root_dir
  if (which == 1) num_data_records++;  // Add itable_blk1 if needed
  
  uint32_t txn_bytes =
      (uint32_t)(data_rec_size()) * num_data_records + (uint32_t)(commit_rec_size());

  if (jh.nbytes_used + txn_bytes > JOURNAL_SIZE_BYTES) {
    fprintf(stderr,
            "Journal full / insufficient space for new transaction.\n"
            "Please run: ./journal install\n");
    close(fd);
    exit(1);
  }

  // Append records at current cursor, then update header at the end
  uint32_t cursor = jh.nbytes_used;

  // Log inode bitmap block (metadata)
  journal_append_data(fd, &sb, &cursor, sb.inode_bitmap, new_ibm_blk);

  // Log the one inode-table block that changed (always block 0 for root inode update)
  journal_append_data(fd, &sb, &cursor, sb.inode_start + 0, new_itable_blk0);

  // Log the other inode-table block if the new inode was added there
  if (which == 1) {
    journal_append_data(fd, &sb, &cursor, sb.inode_start + 1, new_itable_blk1);
  }

  // Log root directory data block (metadata)
  journal_append_data(fd, &sb, &cursor, root_dir_block, new_root_dir_blk);

  // Commit record seals this transaction
  journal_append_commit(fd, &sb, &cursor);

  // Finally update journal header with new nbytes_used
  jh.magic = JOURNAL_MAGIC;
  jh.nbytes_used = cursor;
  journal_write_header(fd, &sb, &jh);

  close(fd);
  printf("create: logged metadata transaction for '%s' (inum=%d, root_dir_block=%u). Run install to apply.\n", name, inum, root_dir_block);
}

// ================================================================
// INSTALL COMMAND: ./journal install
// ================================================================
// Replays committed journal transactions to their home locations,
// then clears (checkpoints) the journal.

struct pending_data {
  uint32_t block_no;
  uint8_t  img[BLOCK_SIZE];
};

static void cmd_install(void) {
  int fd = open(IMG_NAME, O_RDWR);
  if (fd < 0) die("open vsfs.img");

  struct superblock sb;
  read_super(fd, &sb);

  struct journal_header jh;
  journal_require_exists(fd, &sb, &jh);

  if (jh.nbytes_used == sizeof(struct journal_header)) {
    // Empty journal: still "exists"; clear anyway (no-op semantics)
    printf("install: journal is empty, nothing to replay.\n");
    close(fd);
    return;
  }

  // We'll parse sequentially from after the journal_header up to nbytes_used.
  off_t base = journal_start_off(&sb);
  uint32_t off = (uint32_t)sizeof(struct journal_header);

  // Pending DATA records for current transaction
  struct pending_data *pending = NULL;
  size_t pending_cnt = 0;
  size_t pending_cap = 0;

  while (off + sizeof(struct rec_header) <= jh.nbytes_used) {
    struct rec_header rh;
    pread_full(fd, &rh, sizeof(rh), base + (off_t)off);

    if (rh.size < sizeof(struct rec_header)) {
      // Corrupt or unreadable record => stop parsing
      break;
    }
    if (off + rh.size > jh.nbytes_used) {
      // Partial record at end => stop (uncommitted tail ignored)
      break;
    }

    if (rh.type == REC_DATA) {
      // Must match exact expected size
      if (rh.size != data_rec_size()) {
        // Treat as corruption; stop safely
        break;
      }

      uint32_t block_no;
      pread_full(fd, &block_no, sizeof(block_no), base + (off_t)off + (off_t)sizeof(rh));

      // Read full image
      uint8_t img[BLOCK_SIZE];
      pread_full(fd, img, BLOCK_SIZE,
                 base + (off_t)off + (off_t)sizeof(rh) + (off_t)sizeof(block_no));

      // Append to pending list
      if (pending_cnt == pending_cap) {
        size_t new_cap = pending_cap ? pending_cap * 2 : 8;
        struct pending_data *tmp = realloc(pending, new_cap * sizeof(*pending));
        if (!tmp) die("realloc");
        pending = tmp;
        pending_cap = new_cap;
      }
      pending[pending_cnt].block_no = block_no;
      memcpy(pending[pending_cnt].img, img, BLOCK_SIZE);
      pending_cnt++;

    } else if (rh.type == REC_COMMIT) {
      // Must match exact expected size
      if (rh.size != commit_rec_size()) {
        break;
      }

      // COMMIT fully readable => transaction is valid; replay all pending DATA
      for (size_t i = 0; i < pending_cnt; i++) {
        // Replay: write logged block image to its home block number
        write_block(fd, pending[i].block_no, pending[i].img);
      }
      // Clear pending for next transaction
      pending_cnt = 0;

    } else {
      // Unknown record type => stop parsing safely
      break;
    }

    off += rh.size;
  }

  // Any pending data without a COMMIT is ignored (transaction not valid)
  free(pending);

  // After replaying committed txns, clear/checkpoint journal
  journal_clear(fd, &sb);

  close(fd);
  printf("install: replay complete; journal cleared.\n");
}

// ================================================================
// MAIN ENTRY POINT
// ================================================================

static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage:\n"
          "  %s create <name>\n"
          "  %s install\n",
          argv0, argv0);
  exit(1);
}

int main(int argc, char **argv) {
  if (argc < 2) usage(argv[0]);

  if (strcmp(argv[1], "create") == 0) {
    if (argc != 3) usage(argv[0]);
    cmd_create(argv[2]);
  } else if (strcmp(argv[1], "install") == 0) {
    if (argc != 2) usage(argv[0]);
    cmd_install();
  } else {
    usage(argv[0]);
  }

  return 0;
}
