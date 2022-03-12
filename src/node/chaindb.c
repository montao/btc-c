/*!
 * chaindb.c - chaindb for mako
 * Copyright (c) 2021, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/mako
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lcdb.h>

#include <io/core.h>
#include <mako/block.h>
#include <mako/coins.h>
#include <mako/consensus.h>
#include <mako/crypto/hash.h>
#include <mako/entry.h>
#include <mako/list.h>
#include <mako/map.h>
#include <mako/network.h>
#include <mako/tx.h>
#include <mako/util.h>
#include <mako/vector.h>
#include <node/chaindb.h>

#include "../bio.h"
#include "../impl.h"
#include "../internal.h"

/*
 * Constants
 */

#define WRITE_FLAGS (BTC_O_WRONLY | BTC_O_CREAT | BTC_O_APPEND)
#define READ_FLAGS (BTC_O_RDONLY | BTC_O_RANDOM)
#define MAX_FILE_SIZE (128 << 20)
#define BLOCK_FILE 0
#define UNDO_FILE 1

/*
 * Database Keys
 */

static uint8_t meta_key_[1] = {'R'};
static uint8_t blockfile_key_[1] = {'B'};
static uint8_t undofile_key_[1] = {'U'};

static const ldb_slice_t meta_key = {meta_key_, 1, 0};
static const ldb_slice_t blockfile_key = {blockfile_key_, 1, 0};
static const ldb_slice_t undofile_key = {undofile_key_, 1, 0};

#define ENTRY_PREFIX 'e'
#define ENTRY_KEYLEN 33

static uint8_t entry_min_[ENTRY_KEYLEN] = {
  ENTRY_PREFIX,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t entry_max_[ENTRY_KEYLEN] = {
  ENTRY_PREFIX,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static const ldb_slice_t entry_min = {entry_min_, ENTRY_KEYLEN, 0};
static const ldb_slice_t entry_max = {entry_max_, ENTRY_KEYLEN, 0};

static size_t
entry_key(uint8_t *key, const uint8_t *hash) {
  key[0] = ENTRY_PREFIX;
  memcpy(key + 1, hash, 32);
  return ENTRY_KEYLEN;
}

#define TIP_PREFIX 'p'
#define TIP_KEYLEN 33

static uint8_t tip_min_[TIP_KEYLEN] = {
  TIP_PREFIX,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint8_t tip_max_[TIP_KEYLEN] = {
  TIP_PREFIX,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

BTC_UNUSED static const ldb_slice_t tip_min = {tip_min_, TIP_KEYLEN, 0};
BTC_UNUSED static const ldb_slice_t tip_max = {tip_max_, TIP_KEYLEN, 0};

static size_t
tip_key(uint8_t *key, const uint8_t *hash) {
  key[0] = TIP_PREFIX;
  memcpy(key + 1, hash, 32);
  return TIP_KEYLEN;
}

#define FILE_PREFIX 'f'
#define FILE_KEYLEN 6

static uint8_t file_min_[FILE_KEYLEN] =
  {FILE_PREFIX, 0x00, 0x00, 0x00, 0x00, 0x00};

static uint8_t file_max_[FILE_KEYLEN] =
  {FILE_PREFIX, 0xff, 0xff, 0xff, 0xff, 0xff};

static const ldb_slice_t file_min = {file_min_, FILE_KEYLEN, 0};
static const ldb_slice_t file_max = {file_max_, FILE_KEYLEN, 0};

static size_t
file_key(uint8_t *key, uint8_t type, uint32_t id) {
  key[0] = FILE_PREFIX;
  key[1] = type;
  btc_write32be(key + 2, id);
  return FILE_KEYLEN;
}

#define COIN_PREFIX 'c'
#define COIN_KEYLEN 37

static uint8_t coin_min_[COIN_KEYLEN] = {
  COIN_PREFIX,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

static uint8_t coin_max_[COIN_KEYLEN] = {
  COIN_PREFIX,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff
};

BTC_UNUSED static const ldb_slice_t coin_min = {coin_min_, COIN_KEYLEN, 0};
BTC_UNUSED static const ldb_slice_t coin_max = {coin_max_, COIN_KEYLEN, 0};

static size_t
coin_key(uint8_t *key, const uint8_t *hash, uint32_t index) {
  key[0] = COIN_PREFIX;
  memcpy(key + 1, hash, 32);
  btc_write32be(key + 33, index);
  return COIN_KEYLEN;
}

/*
 * Chain File
 */

#define BTC_CHAINFILE_SIZE 37

typedef struct btc_chainfile_s {
  int fd;
  uint8_t type;
  int32_t id;
  int32_t pos;
  int32_t items;
  int64_t min_time;
  int64_t max_time;
  int32_t min_height;
  int32_t max_height;
  struct btc_chainfile_s *prev;
  struct btc_chainfile_s *next;
} btc_chainfile_t;

DEFINE_SERIALIZABLE_OBJECT(btc_chainfile, SCOPE_STATIC)

static void
btc_chainfile_init(btc_chainfile_t *z) {
  z->fd = -1;
  z->type = BLOCK_FILE;
  z->id = 0;
  z->pos = 0;
  z->items = 0;
  z->min_time = -1;
  z->max_time = -1;
  z->min_height = -1;
  z->max_height = -1;
  z->prev = NULL;
  z->next = NULL;
}

static void
btc_chainfile_clear(btc_chainfile_t *z) {
  btc_chainfile_init(z);
}

static void
btc_chainfile_copy(btc_chainfile_t *z, const btc_chainfile_t *x) {
  z->fd = -1;
  z->type = x->type;
  z->id = x->id;
  z->pos = x->pos;
  z->items = x->items;
  z->min_time = x->min_time;
  z->max_time = x->max_time;
  z->min_height = x->min_height;
  z->max_height = x->max_height;
  z->prev = NULL;
  z->next = NULL;
}

static size_t
btc_chainfile_size(const btc_chainfile_t *x) {
  (void)x;
  return BTC_CHAINFILE_SIZE;
}

static uint8_t *
btc_chainfile_write(uint8_t *zp, const btc_chainfile_t *x) {
  zp = btc_uint8_write(zp, x->type);
  zp = btc_int32_write(zp, x->id);
  zp = btc_int32_write(zp, x->pos);
  zp = btc_int32_write(zp, x->items);
  zp = btc_int64_write(zp, x->min_time);
  zp = btc_int64_write(zp, x->max_time);
  zp = btc_int32_write(zp, x->min_height);
  zp = btc_int32_write(zp, x->max_height);
  return zp;
}

static int
btc_chainfile_read(btc_chainfile_t *z, const uint8_t **xp, size_t *xn) {
  if (!btc_uint8_read(&z->type, xp, xn))
    return 0;

  if (!btc_int32_read(&z->id, xp, xn))
    return 0;

  if (!btc_int32_read(&z->pos, xp, xn))
    return 0;

  if (!btc_int32_read(&z->items, xp, xn))
    return 0;

  if (!btc_int64_read(&z->min_time, xp, xn))
    return 0;

  if (!btc_int64_read(&z->max_time, xp, xn))
    return 0;

  if (!btc_int32_read(&z->min_height, xp, xn))
    return 0;

  if (!btc_int32_read(&z->max_height, xp, xn))
    return 0;

  return 1;
}

static void
btc_chainfile_update(btc_chainfile_t *z, const btc_entry_t *entry) {
  z->items += 1;

  if (z->min_time == -1 || entry->header.time < z->min_time)
    z->min_time = entry->header.time;

  if (z->max_time == -1 || entry->header.time > z->max_time)
    z->max_time = entry->header.time;

  if (z->min_height == -1 || entry->height < z->min_height)
    z->min_height = entry->height;

  if (z->max_height == -1 || entry->height > z->max_height)
    z->max_height = entry->height;
}

/*
 * Database Helpers
 */

static int
ldb_iter_le(ldb_iter_t *iter, const ldb_slice_t *key) {
  ldb_slice_t k;

  if (!ldb_iter_valid(iter))
    return 0;

  k = ldb_iter_key(iter);

  return ldb_compare(&k, key) <= 0;
}

/*
 * Chain Database
 */

struct btc_chaindb_s {
  const btc_network_t *network;
  char prefix[BTC_PATH_MAX - 26];
  unsigned int flags;
  ldb_t *lsm;
  ldb_lru_t *block_cache;
  btc_hashmap_t *hashes;
  btc_vector_t heights;
  btc_entry_t *head;
  btc_entry_t *tail;
  struct btc_chainfiles_s {
    btc_chainfile_t *head;
    btc_chainfile_t *tail;
    size_t length;
  } files;
  btc_chainfile_t block;
  btc_chainfile_t undo;
  uint8_t *slab;
};

static void
btc_chaindb_path(btc_chaindb_t *db, char *path, int type, int id) {
  const char *tag = (type == BLOCK_FILE ? "blk" : "rev");

#if defined(_WIN32)
  sprintf(path, "%s\\blocks\\%s%.5d.dat", db->prefix, tag, id);
#else
  sprintf(path, "%s/blocks/%s%.5d.dat", db->prefix, tag, id);
#endif
}

static void
btc_chaindb_init(btc_chaindb_t *db, const btc_network_t *network) {
  memset(db, 0, sizeof(*db));

  db->network = network;
  db->prefix[0] = '/';
  db->hashes = btc_hashmap_create();
  db->flags = BTC_CHAIN_DEFAULT_FLAGS;
  db->block_cache = ldb_lru_create(64 << 20);

  btc_vector_init(&db->heights);

  db->slab = (uint8_t *)btc_malloc(24 + BTC_MAX_RAW_BLOCK_SIZE);
}

static void
btc_chaindb_clear(btc_chaindb_t *db) {
  ldb_lru_destroy(db->block_cache);

  btc_hashmap_destroy(db->hashes);
  btc_vector_clear(&db->heights);
  btc_free(db->slab);

  memset(db, 0, sizeof(*db));
}

btc_chaindb_t *
btc_chaindb_create(const btc_network_t *network) {
  btc_chaindb_t *db = (btc_chaindb_t *)btc_malloc(sizeof(btc_chaindb_t));
  btc_chaindb_init(db, network);
  return db;
}

void
btc_chaindb_destroy(btc_chaindb_t *db) {
  btc_chaindb_clear(db);
  btc_free(db);
}

static int
btc_chaindb_load_prefix(btc_chaindb_t *db, const char *prefix) {
  char path[BTC_PATH_MAX];

  if (!btc_path_resolve(db->prefix, sizeof(db->prefix), prefix, NULL))
    return 0;

  if (!btc_fs_mkdirp(db->prefix, 0755))
    return 0;

  if (!btc_path_join(path, sizeof(path), db->prefix, "blocks", NULL))
    return 0;

  if (!btc_fs_exists(path) && !btc_fs_mkdir(path, 0755))
    return 0;

  return 1;
}

static int
btc_chaindb_load_database(btc_chaindb_t *db) {
  ldb_dbopt_t options = *ldb_dbopt_default;
  char path[BTC_PATH_MAX];
  int rc;

  if (!btc_path_join(path, sizeof(path), db->prefix, "chain", NULL)) {
    fprintf(stderr, "ldb_open: path too long\n");
    return 0;
  }

  options.create_if_missing = 1;
  options.block_cache = db->block_cache;
  options.write_buffer_size = 32 << 20;
#ifdef _WIN32
  options.max_open_files = 1000;
#else
  options.max_open_files = sizeof(void *) < 8 ? 64 : 1000;
#endif
  options.compression = LDB_NO_COMPRESSION;
  options.filter_policy = NULL; /* ldb_bloom_default */
  options.use_mmap = 0;

  rc = ldb_open(path, &options, &db->lsm);

  if (rc != LDB_OK) {
    fprintf(stderr, "ldb_open: %s\n", ldb_strerror(rc));
    return 0;
  }

  return 1;
}

static void
btc_chaindb_unload_database(btc_chaindb_t *db) {
  ldb_close(db->lsm);
  db->lsm = NULL;
}

static int
btc_chaindb_load_files(btc_chaindb_t *db) {
  char path[BTC_PATH_MAX];
  btc_chainfile_t *file;
  ldb_slice_t val;
  ldb_iter_t *it;
  int rc;

  /* Read best block file. */
  rc = ldb_get(db->lsm, &blockfile_key, &val, 0);

  if (rc == LDB_OK) {
    CHECK(btc_chainfile_import(&db->block, val.data, val.size));
    CHECK(db->block.type == BLOCK_FILE);

    ldb_free(val.data);
  } else {
    CHECK(rc == LDB_NOTFOUND);

    btc_chainfile_init(&db->block);

    db->block.type = BLOCK_FILE;
  }

  /* Read best undo file. */
  rc = ldb_get(db->lsm, &undofile_key, &val, 0);

  if (rc == LDB_OK) {
    CHECK(btc_chainfile_import(&db->undo, val.data, val.size));
    CHECK(db->undo.type == UNDO_FILE);

    ldb_free(val.data);
  } else {
    CHECK(rc == LDB_NOTFOUND);

    btc_chainfile_init(&db->undo);

    db->undo.type = UNDO_FILE;
  }

  /* Read file index and build vector. */
  it = ldb_iterator(db->lsm, 0);

  ldb_iter_seek(it, &file_min);

  while (ldb_iter_le(it, &file_max)) {
    file = btc_chainfile_create();
    val = ldb_iter_value(it);

    CHECK(btc_chainfile_import(file, val.data, val.size));

    btc_list_push(&db->files, file, btc_chainfile_t);

    ldb_iter_next(it);
  }

  CHECK(ldb_iter_status(it) == LDB_OK);

  ldb_iter_destroy(it);

  /* Open block file for writing. */
  btc_chaindb_path(db, path, BLOCK_FILE, db->block.id);

  db->block.fd = btc_fs_open(path, WRITE_FLAGS, 0644);

  CHECK(db->block.fd != -1);

  /* Open undo file for writing. */
  btc_chaindb_path(db, path, UNDO_FILE, db->undo.id);

  db->undo.fd = btc_fs_open(path, WRITE_FLAGS, 0644);

  CHECK(db->undo.fd != -1);

  return 1;
}

static void
btc_chaindb_unload_files(btc_chaindb_t *db) {
  btc_chainfile_t *file, *next;

  btc_fs_fsync(db->block.fd);
  btc_fs_fsync(db->undo.fd);

  btc_fs_close(db->block.fd);
  btc_fs_close(db->undo.fd);

  for (file = db->files.head; file != NULL; file = next) {
    next = file->next;
    btc_chainfile_destroy(file);
  }

  btc_list_reset(&db->files);
}

static int
btc_chaindb_init_index(btc_chaindb_t *db) {
  btc_view_t *view = btc_view_create();
  btc_entry_t *entry = btc_entry_create();
  btc_block_t block;

  btc_block_init(&block);
  btc_block_import(&block, db->network->genesis.data,
                           db->network->genesis.length);

  btc_entry_set_block(entry, &block, NULL);

  CHECK(btc_chaindb_save(db, entry, &block, view));

  btc_block_clear(&block);
  btc_view_destroy(view);

  return 1;
}

static int
btc_chaindb_load_index(btc_chaindb_t *db) {
  btc_entry_t *entry, *tip;
  btc_entry_t *gen = NULL;
  btc_hashmapiter_t iter;
  uint8_t tip_hash[32];
  ldb_slice_t val;
  ldb_iter_t *it;
  int rc;

  /* Read tip hash. */
  {
    rc = ldb_get(db->lsm, &meta_key, &val, 0);

    if (rc == LDB_NOTFOUND)
      return btc_chaindb_init_index(db);

    CHECK(rc == LDB_OK);
    CHECK(val.size == 32);

    memcpy(tip_hash, val.data, 32);

    ldb_free(val.data);
  }

  /* Read block index and create hash->entry map. */
  it = ldb_iterator(db->lsm, 0);

  ldb_iter_seek(it, &entry_min);

  while (ldb_iter_le(it, &entry_max)) {
    entry = btc_entry_create();
    val = ldb_iter_value(it);

    CHECK(btc_entry_import(entry, val.data, val.size));
    CHECK(btc_hashmap_put(db->hashes, entry->hash, entry));

    ldb_iter_next(it);
  }

  CHECK(ldb_iter_status(it) == LDB_OK);

  ldb_iter_destroy(it);

  /* Create `prev` links and retrieve genesis block. */
  btc_hashmap_iterate(&iter, db->hashes);

  while (btc_hashmap_next(&iter)) {
    entry = iter.val;

    if (entry->height == 0) {
      gen = entry;
      continue;
    }

    entry->prev = btc_hashmap_get(db->hashes, entry->header.prev_block);

    CHECK(entry->prev != NULL);
  }

  CHECK(gen != NULL);

  /* Retrieve tip. */
  tip = btc_hashmap_get(db->hashes, tip_hash);

  CHECK(tip != NULL);

  /* Create height->entry vector. */
  btc_vector_grow(&db->heights, (btc_hashmap_size(db->hashes) * 3) / 2);
  btc_vector_resize(&db->heights, tip->height + 1);

  /* Populate height vector and create `next` links. */
  entry = tip;

  do {
    CHECK((size_t)entry->height < db->heights.length);

    db->heights.items[entry->height] = entry;

    if (entry->prev != NULL)
      entry->prev->next = entry;

    entry = entry->prev;
  } while (entry != NULL);

  db->head = gen;
  db->tail = tip;

  return 1;
}

static void
btc_chaindb_unload_index(btc_chaindb_t *db) {
  btc_hashmapiter_t iter;

  btc_hashmap_iterate(&iter, db->hashes);

  while (btc_hashmap_next(&iter))
    btc_entry_destroy(iter.val);

  btc_hashmap_reset(db->hashes);
  btc_vector_clear(&db->heights);

  db->head = NULL;
  db->tail = NULL;
}

int
btc_chaindb_open(btc_chaindb_t *db,
                 const char *prefix,
                 unsigned int flags) {
  db->flags = flags;

  if (!btc_chaindb_load_prefix(db, prefix))
    return 0;

  if (!btc_chaindb_load_database(db))
    return 0;

  if (!btc_chaindb_load_files(db))
    return 0;

  if (!btc_chaindb_load_index(db))
    return 0;

  return 1;
}

void
btc_chaindb_close(btc_chaindb_t *db) {
  btc_chaindb_unload_index(db);
  btc_chaindb_unload_files(db);
  btc_chaindb_unload_database(db);
}

static btc_coin_t *
read_coin(const btc_outpoint_t *prevout, void *arg1, void *arg2) {
  btc_chaindb_t *db = (btc_chaindb_t *)arg1;
  uint8_t kbuf[COIN_KEYLEN];
  ldb_slice_t key, val;
  btc_coin_t *coin;
  int rc;

  (void)arg2;

  key.data = kbuf;
  key.size = coin_key(kbuf, prevout->hash, prevout->index);

  rc = ldb_get(db->lsm, &key, &val, 0);

  if (rc == LDB_NOTFOUND)
    return NULL;

  if (rc != LDB_OK) {
    fprintf(stderr, "ldb_get: %s\n", ldb_strerror(rc));
    return NULL;
  }

  coin = btc_coin_create();

  CHECK(btc_coin_import(coin, val.data, val.size));

  ldb_free(val.data);

  return coin;
}

int
btc_chaindb_spend(btc_chaindb_t *db,
                  btc_view_t *view,
                  const btc_tx_t *tx) {
  return btc_view_spend(view, tx, read_coin, db, NULL);
}

int
btc_chaindb_fill(btc_chaindb_t *db,
                 btc_view_t *view,
                 const btc_tx_t *tx) {
  return btc_view_fill(view, tx, read_coin, db, NULL);
}

static void
btc_chaindb_save_view(btc_chaindb_t *db,
                      ldb_batch_t *batch,
                      const btc_view_t *view) {
  uint8_t kbuf[COIN_KEYLEN];
  uint8_t *vbuf = db->slab;
  const btc_coin_t *coin;
  ldb_slice_t key, val;
  btc_viewiter_t iter;

  key.data = kbuf;
  key.size = sizeof(kbuf);

  val.data = vbuf;
  val.size = 0;

  btc_view_iterate(&iter, view);

  while (btc_view_next(&coin, &iter)) {
    coin_key(kbuf, iter.hash, iter.index);

    if (coin->spent) {
      ldb_batch_del(batch, &key);
    } else {
      val.size = btc_coin_export(vbuf, coin);

      ldb_batch_put(batch, &key, &val);
    }
  }
}

static int
btc_chaindb_read(btc_chaindb_t *db,
                 uint8_t **raw,
                 size_t *len,
                 int type,
                 int id,
                 int pos) {
  char path[BTC_PATH_MAX];
  uint8_t *data = NULL;
  uint8_t hdr[24];
  size_t size;
  int ret = 0;
  int fd;

  btc_chaindb_path(db, path, type, id);

  fd = btc_fs_open(path, READ_FLAGS, 0);

  if (fd == -1)
    return 0;

  if (btc_fs_seek(fd, pos, BTC_SEEK_SET) != pos)
    goto fail;

  if (!btc_fs_read(fd, hdr, 24))
    goto fail;

  size = btc_read32le(hdr + 16);

  if (size > (64 << 20))
    goto fail;

  size += 24;
  data = (uint8_t *)malloc(size);

  if (data == NULL)
    goto fail;

  memcpy(data, hdr, 24);

  if (!btc_fs_read(fd, data + 24, size - 24))
    goto fail;

  *raw = data;
  *len = size;

  data = NULL;
  ret = 1;
fail:
  if (data != NULL)
    free(data);

  btc_fs_close(fd);

  return ret;
}

static btc_block_t *
btc_chaindb_read_block(btc_chaindb_t *db, const btc_entry_t *entry) {
  btc_block_t *block;
  uint8_t *buf;
  size_t len;

  if (entry->block_pos == -1)
    return NULL;

  if (!btc_chaindb_read(db, &buf, &len, BLOCK_FILE, entry->block_file,
                                                    entry->block_pos)) {
    return NULL;
  }

  block = btc_block_decode(buf + 24, len - 24);

  free(buf);

  return block;
}

static btc_undo_t *
btc_chaindb_read_undo(btc_chaindb_t *db, const btc_entry_t *entry) {
  btc_undo_t *undo;
  uint8_t *buf;
  size_t len;

  if (entry->undo_pos == -1)
    return btc_undo_create();

  if (!btc_chaindb_read(db, &buf, &len, UNDO_FILE, entry->undo_file,
                                                   entry->undo_pos)) {
    return NULL;
  }

  undo = btc_undo_decode(buf + 24, len - 24);

  free(buf);

  return undo;
}

static int
should_sync(const btc_entry_t *entry) {
  if (entry->header.time >= btc_now() - 24 * 60 * 60)
    return 1;

  if ((entry->height % 20000) == 0)
    return 1;

  return 0;
}

static int
btc_chaindb_alloc(btc_chaindb_t *db,
                  ldb_batch_t *batch,
                  btc_chainfile_t *file,
                  size_t len) {
  uint8_t vbuf[BTC_CHAINFILE_SIZE];
  uint8_t kbuf[FILE_KEYLEN];
  char path[BTC_PATH_MAX];
  ldb_slice_t key, val;
  int fd;

  if (file->pos + len <= MAX_FILE_SIZE)
    return 1;

  key.data = kbuf;
  key.size = file_key(kbuf, file->type, file->id);

  val.data = vbuf;
  val.size = btc_chainfile_export(vbuf, file);

  ldb_batch_put(batch, &key, &val);

  btc_chaindb_path(db, path, file->type, file->id + 1);

  fd = btc_fs_open(path, WRITE_FLAGS, 0644);

  if (fd == -1)
    return 0;

  btc_fs_fsync(file->fd);
  btc_fs_close(file->fd);

  btc_list_push(&db->files, btc_chainfile_clone(file),
                            btc_chainfile_t);

  file->fd = fd;
  file->id++;
  file->pos = 0;
  file->items = 0;
  file->min_time = -1;
  file->max_time = -1;
  file->min_height = -1;
  file->max_height = -1;

  return 1;
}

static int
btc_chaindb_write_block(btc_chaindb_t *db,
                        ldb_batch_t *batch,
                        btc_entry_t *entry,
                        const btc_block_t *block) {
  uint8_t vbuf[BTC_CHAINFILE_SIZE];
  uint8_t hash[32];
  ldb_slice_t val;
  size_t len;

  len = btc_block_export(db->slab + 24, block);

  btc_hash256(hash, db->slab + 24, len);

  /* Store in network format. */
  btc_uint32_write(db->slab +  0, db->network->magic);
  btc_uint32_write(db->slab +  4, 0x636f6c62);
  btc_uint32_write(db->slab +  8, 0x0000006b);
  btc_uint32_write(db->slab + 12, 0x00000000);
  btc_uint32_write(db->slab + 16, len);

  btc_raw_write(db->slab + 20, hash, 4);

  len += 24;

  if (!btc_chaindb_alloc(db, batch, &db->block, len))
    return 0;

  if (!btc_fs_write(db->block.fd, db->slab, len))
    return 0;

  if (should_sync(entry))
    btc_fs_fsync(db->block.fd);

  entry->block_file = db->block.id;
  entry->block_pos = db->block.pos;

  db->block.pos += len;

  btc_chainfile_update(&db->block, entry);

  val.data = vbuf;
  val.size = btc_chainfile_export(vbuf, &db->block);

  ldb_batch_put(batch, &blockfile_key, &val);

  return 1;
}

static int
btc_chaindb_write_undo(btc_chaindb_t *db,
                       ldb_batch_t *batch,
                       btc_entry_t *entry,
                       const btc_undo_t *undo) {
  size_t len = btc_undo_size(undo);
  uint8_t vbuf[BTC_CHAINFILE_SIZE];
  uint8_t *buf = db->slab;
  uint8_t hash[32];
  ldb_slice_t val;
  int ret = 0;

  if (len > BTC_MAX_RAW_BLOCK_SIZE)
    buf = (uint8_t *)btc_malloc(24 + len);

  len = btc_undo_export(buf + 24, undo);

  btc_hash256(hash, buf + 24, len);

  btc_uint32_write(buf +  0, db->network->magic);
  btc_uint32_write(buf +  4, 0x00000000);
  btc_uint32_write(buf +  8, 0x00000000);
  btc_uint32_write(buf + 12, 0x00000000);
  btc_uint32_write(buf + 16, len);

  btc_raw_write(buf + 20, hash, 4);

  len += 24;

  if (!btc_chaindb_alloc(db, batch, &db->undo, len))
    goto fail;

  if (!btc_fs_write(db->undo.fd, buf, len))
    goto fail;

  if (should_sync(entry))
    btc_fs_fsync(db->undo.fd);

  entry->undo_file = db->undo.id;
  entry->undo_pos = db->undo.pos;

  db->undo.pos += len;

  btc_chainfile_update(&db->undo, entry);

  val.data = vbuf;
  val.size = btc_chainfile_export(vbuf, &db->undo);

  ldb_batch_put(batch, &undofile_key, &val);

  ret = 1;
fail:
  if (buf != db->slab)
    btc_free(buf);

  return ret;
}

static int
btc_chaindb_prune_files(btc_chaindb_t *db,
                        ldb_batch_t *batch,
                        const btc_entry_t *entry) {
  btc_chainfile_t *file, *next;
  uint8_t kbuf[FILE_KEYLEN];
  char path[BTC_PATH_MAX];
  ldb_slice_t key;
  int32_t target;

  if (!(db->flags & BTC_CHAIN_PRUNE))
    return 1;

  if (entry->height < db->network->block.keep_blocks)
    return 1;

  target = entry->height - db->network->block.keep_blocks;

  if (target <= db->network->block.prune_after_height)
    return 1;

  key.data = kbuf;
  key.size = sizeof(kbuf);

  for (file = db->files.head; file != NULL; file = next) {
    next = file->next;

    if (file->max_height >= target)
      continue;

    file_key(kbuf, file->type, file->id);

    ldb_batch_del(batch, &key);

    btc_chaindb_path(db, path, file->type, file->id);

    btc_fs_unlink(path);

    btc_list_remove(&db->files, file, btc_chainfile_t);

    btc_chainfile_destroy(file);
  }

  return 1;
}

static int
btc_chaindb_connect_block(btc_chaindb_t *db,
                          ldb_batch_t *batch,
                          btc_entry_t *entry,
                          const btc_block_t *block,
                          const btc_view_t *view) {
  const btc_undo_t *undo;

  (void)block;

  /* Genesis block's coinbase is unspendable. */
  if (entry->height == 0)
    return 1;

  /* Commit new coin state. */
  btc_chaindb_save_view(db, batch, view);

  /* Write undo coins (if there are any). */
  undo = btc_view_undo(view);

  if (undo->length != 0 && entry->undo_pos == -1) {
    if (!btc_chaindb_write_undo(db, batch, entry, undo))
      return 0;
  }

  /* Prune height-288 if pruning is enabled. */
  return btc_chaindb_prune_files(db, batch, entry);
}

static btc_view_t *
btc_chaindb_disconnect_block(btc_chaindb_t *db,
                             ldb_batch_t *batch,
                             const btc_entry_t *entry,
                             const btc_block_t *block) {
  btc_undo_t *undo = btc_chaindb_read_undo(db, entry);
  const btc_input_t *input;
  const btc_tx_t *tx;
  btc_coin_t *coin;
  btc_view_t *view;
  size_t i, j;

  if (undo == NULL)
    return NULL;

  view = btc_view_create();

  /* Disconnect all transactions. */
  for (i = block->txs.length - 1; i != (size_t)-1; i--) {
    tx = block->txs.items[i];

    if (i > 0) {
      for (j = tx->inputs.length - 1; j != (size_t)-1; j--) {
        input = tx->inputs.items[j];
        coin = btc_undo_pop(undo);

        btc_view_put(view, &input->prevout, coin);
      }
    }

    /* Remove any created coins. */
    btc_view_add(view, tx, entry->height, 1);
  }

  /* Undo coins should be empty. */
  CHECK(undo->length == 0);

  btc_undo_destroy(undo);

  /* Commit new coin state. */
  btc_chaindb_save_view(db, batch, view);

  return view;
}

static int
btc_chaindb_save_block(btc_chaindb_t *db,
                       ldb_batch_t *batch,
                       btc_entry_t *entry,
                       const btc_block_t *block,
                       const btc_view_t *view) {
  /* Write actual block data. */
  if (entry->block_pos == -1) {
    if (!btc_chaindb_write_block(db, batch, entry, block))
      return 0;
  }

  if (view == NULL)
    return 1;

  return btc_chaindb_connect_block(db, batch, entry, block, view);
}

int
btc_chaindb_save(btc_chaindb_t *db,
                 btc_entry_t *entry,
                 const btc_block_t *block,
                 const btc_view_t *view) {
  uint8_t vbuf[BTC_ENTRY_SIZE];
  uint8_t kbuf[ENTRY_KEYLEN];
  ldb_slice_t key, val;
  ldb_batch_t batch;
  int ret = 0;

  /* Sanity checks. */
  CHECK(entry->prev != NULL || entry->height == 0);
  CHECK(entry->next == NULL);

  /* Begin transaction. */
  ldb_batch_init(&batch);

  /* Connect block and save data. */
  if (!btc_chaindb_save_block(db, &batch, entry, block, view))
    goto fail;

  /* Write entry data. */
  key.data = kbuf;
  key.size = entry_key(kbuf, entry->hash);

  val.data = vbuf;
  val.size = btc_entry_export(vbuf, entry);

  ldb_batch_put(&batch, &key, &val);

  /* Clear old tip. */
  if (entry->height != 0) {
    key.data = kbuf;
    key.size = tip_key(kbuf, entry->header.prev_block);

    ldb_batch_del(&batch, &key);
  }

  /* Write new tip. */
  key.data = kbuf;
  key.size = tip_key(kbuf, entry->hash);
  val.size = 1;

  ldb_batch_put(&batch, &key, &val);

  /* Write state (main chain only). */
  if (view != NULL) {
    /* Commit new chain state. */
    val.data = entry->hash;
    val.size = 32;

    ldb_batch_put(&batch, &meta_key, &val);
  }

  /* Commit transaction. */
  if (ldb_write(db->lsm, &batch, 0) != LDB_OK)
    goto fail;

  /* Update hashes. */
  CHECK(btc_hashmap_put(db->hashes, entry->hash, entry));

  /* Main-chain-only stuff. */
  if (view != NULL) {
    /* Set next pointer. */
    if (entry->prev != NULL)
      entry->prev->next = entry;

    /* Update heights. */
    CHECK(db->heights.length == (size_t)entry->height);
    btc_vector_push(&db->heights, entry);

    /* Update tip. */
    if (entry->height == 0)
      db->head = entry;

    db->tail = entry;
  }

  ret = 1;
fail:
  ldb_batch_clear(&batch);
  return ret;
}

int
btc_chaindb_reconnect(btc_chaindb_t *db,
                      btc_entry_t *entry,
                      const btc_block_t *block,
                      const btc_view_t *view) {
  uint8_t vbuf[BTC_ENTRY_SIZE];
  uint8_t kbuf[ENTRY_KEYLEN];
  ldb_slice_t key, val;
  ldb_batch_t batch;
  int ret = 0;

  /* Begin transaction. */
  ldb_batch_init(&batch);

  /* Connect inputs. */
  if (!btc_chaindb_connect_block(db, &batch, entry, block, view))
    goto fail;

  /* Re-write entry data (we may have updated the undo pos). */
  key.data = kbuf;
  key.size = entry_key(kbuf, entry->hash);

  val.data = vbuf;
  val.size = btc_entry_export(vbuf, entry);

  ldb_batch_put(&batch, &key, &val);

  /* Commit new chain state. */
  val.data = entry->hash;
  val.size = 32;

  ldb_batch_put(&batch, &meta_key, &val);

  /* Commit transaction. */
  if (ldb_write(db->lsm, &batch, 0) != LDB_OK)
    goto fail;

  /* Set next pointer. */
  CHECK(entry->prev != NULL);
  CHECK(entry->next == NULL);
  entry->prev->next = entry;

  /* Update heights. */
  CHECK(db->heights.length == (size_t)entry->height);
  btc_vector_push(&db->heights, entry);

  /* Update tip. */
  db->tail = entry;

  ret = 1;
fail:
  ldb_batch_clear(&batch);
  return ret;
}

btc_view_t *
btc_chaindb_disconnect(btc_chaindb_t *db,
                       btc_entry_t *entry,
                       const btc_block_t *block) {
  ldb_batch_t batch;
  btc_view_t *view;
  ldb_slice_t val;

  /* Begin transaction. */
  ldb_batch_init(&batch);

  /* Disconnect inputs. */
  view = btc_chaindb_disconnect_block(db, &batch, entry, block);

  if (view == NULL)
    goto fail;

  /* Revert chain state to previous tip. */
  val.data = entry->header.prev_block;
  val.size = 32;

  ldb_batch_put(&batch, &meta_key, &val);

  /* Commit transaction. */
  if (ldb_write(db->lsm, &batch, 0) != LDB_OK)
    goto fail;

  /* Set next pointer. */
  CHECK(entry->prev != NULL);
  CHECK(entry->next == NULL);
  entry->prev->next = NULL;

  /* Update heights. */
  CHECK((btc_entry_t *)btc_vector_pop(&db->heights) == entry);

  /* Revert tip. */
  db->tail = entry->prev;

  ldb_batch_clear(&batch);

  return view;
fail:
  if (view != NULL)
    btc_view_destroy(view);

  ldb_batch_clear(&batch);

  return NULL;
}

const btc_entry_t *
btc_chaindb_head(btc_chaindb_t *db) {
  return db->head;
}

const btc_entry_t *
btc_chaindb_tail(btc_chaindb_t *db) {
  return db->tail;
}

int32_t
btc_chaindb_height(btc_chaindb_t *db) {
  return db->tail->height;
}

const btc_entry_t *
btc_chaindb_by_hash(btc_chaindb_t *db, const uint8_t *hash) {
  return btc_hashmap_get(db->hashes, hash);
}

const btc_entry_t *
btc_chaindb_by_height(btc_chaindb_t *db, int32_t height) {
  if ((size_t)height >= db->heights.length)
    return NULL;

  return (btc_entry_t *)db->heights.items[height];
}

int
btc_chaindb_is_main(btc_chaindb_t *db, const btc_entry_t *entry) {
  if ((size_t)entry->height >= db->heights.length)
    return 0;

  return (btc_entry_t *)db->heights.items[entry->height] == entry;
}

int
btc_chaindb_has_coins(btc_chaindb_t *db, const btc_tx_t *tx) {
  uint8_t kbuf[COIN_KEYLEN];
  ldb_slice_t key;
  size_t i;
  int rc;

  key.data = kbuf;
  key.size = sizeof(kbuf);

  for (i = 0; i < tx->outputs.length; i++) {
    coin_key(kbuf, tx->hash, i);

    rc = ldb_has(db->lsm, &key, 0);

    if (rc == LDB_OK)
      return 1;

    CHECK(rc == LDB_NOTFOUND);
  }

  return 0;
}

btc_block_t *
btc_chaindb_get_block(btc_chaindb_t *db, const btc_entry_t *entry) {
  return btc_chaindb_read_block(db, entry);
}

int
btc_chaindb_get_raw_block(btc_chaindb_t *db,
                          uint8_t **data,
                          size_t *length,
                          const btc_entry_t *entry) {
  if (entry->block_pos == -1)
    return 0;

  return btc_chaindb_read(db, data, length, BLOCK_FILE, entry->block_file,
                                                        entry->block_pos);

}
