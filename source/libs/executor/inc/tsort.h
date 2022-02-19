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

#ifndef TDENGINE_TSORT_H
#define TDENGINE_TSORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "os.h"

enum {
  SORT_MULTIWAY_MERGE = 0x1,
  SORT_SINGLESOURCE = 0x2,
};

typedef struct SMultiMergeSource {
  int32_t      type;
  int32_t      rowIndex;
  SSDataBlock *pBlock;
} SMultiMergeSource;

typedef struct SExternalMemSource {
  SMultiMergeSource src;
  SArray*           pageIdList;
  int32_t           pageIndex;
} SExternalMemSource;

typedef struct SOperatorSource {
  SMultiMergeSource src;
  void* param;
} SOperatorSource;

typedef struct SSortHandle SSortHandle;
typedef struct STupleHandle STupleHandle;

typedef SSDataBlock* (*_sort_fetch_block_fn_t)(void* param);
typedef int32_t (*_sort_merge_compar_fn_t)(const void* p1, const void* p2, void* param);

/**
 *
 * @param type
 * @return
 */
SSortHandle* createSortHandle(SArray* pOrderInfo, bool nullFirst, int32_t type, int32_t pageSize, int32_t numOfPages, SSchema* pSchema, int32_t numOfCols, const char* idstr);

/**
 *
 * @param pSortHandle
 */
void destroySortHandle(SSortHandle* pSortHandle);

/**
 *
 * @param pHandle
 * @return
 */
int32_t sortOpen(SSortHandle* pHandle);

/**
 *
 * @param pHandle
 * @return
 */
int32_t sortClose(SSortHandle* pHandle);

/**
 *
 * @return
 */
int32_t setFetchRawDataFp(SSortHandle* pHandle, _sort_fetch_block_fn_t fp);

/**
 *
 * @param pHandle
 * @param fp
 * @return
 */
int32_t setComparFn(SSortHandle* pHandle, _sort_merge_compar_fn_t fp);

/**
 *
 * @param pHandle
 * @param pSource
 * @return success or failed
 */
int32_t sortAddSource(SSortHandle* pSortHandle, void* pSource);

/**
 *
 * @param pHandle
 * @return
 */
STupleHandle* sortNextTuple(SSortHandle* pHandle);

/**
 *
 * @param pHandle
 * @param colIndex
 * @return
 */
bool sortIsValueNull(STupleHandle* pVHandle, int32_t colIndex);

/**
 *
 * @param pHandle
 * @param colIndex
 * @return
 */
void* sortGetValue(STupleHandle* pVHandle, int32_t colIndex);

#ifdef __cplusplus
}
#endif

#endif  // TDENGINE_TSORT_H