/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TD_TDB_DB_H_
#define _TD_TDB_DB_H_

#include "tdb.h"
#include "tdbBtree.h"
#include "tdbHash.h"
#include "tdbHeap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  // TODO
} TDB_MPOOL;

typedef struct {
  int fd;
} TDB_FH;

struct TDB {
  pgsize_t pageSize;
  tdb_db_t type;
  char *   fname;
  char *   dbname;
  union {
    TDB_BTREE *btree;
    TDB_HASH * hash;
    TDB_HEAP * heap;
  } dbam;  // db access method

  TDB_FH *   fhp;  // The backup file handle
  TDB_MPOOL *mph;  // The memory pool handle
};

#ifdef __cplusplus
}
#endif

#endif /*_TD_TDB_DB_H_*/