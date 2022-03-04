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

#include "parserInt.h"

#include "catalog.h"
#include "cmdnodes.h"
#include "functionMgt.h"
#include "parserUtil.h"
#include "ttime.h"

static bool afterGroupBy(ESqlClause clause) {
  return clause > SQL_CLAUSE_GROUP_BY;
}

static bool beforeHaving(ESqlClause clause) {
  return clause < SQL_CLAUSE_HAVING;
}

typedef struct STranslateContext {
  SParseContext* pParseCxt;
  int32_t errCode;
  SMsgBuf msgBuf;
  SArray* pNsLevel; // element is SArray*, the element of this subarray is STableNode*
  int32_t currLevel;
  ESqlClause currClause;
  SSelectStmt* pCurrStmt;
  SCmdMsgInfo* pCmdMsg;
} STranslateContext;

static int32_t translateSubquery(STranslateContext* pCxt, SNode* pNode);

static char* getSyntaxErrFormat(int32_t errCode) {
  switch (errCode) {
    case TSDB_CODE_PAR_INVALID_COLUMN:
      return "Invalid column name : %s";
    case TSDB_CODE_PAR_TABLE_NOT_EXIST:
      return "Table does not exist : %s";
    case TSDB_CODE_PAR_AMBIGUOUS_COLUMN:
      return "Column ambiguously defined : %s";
    case TSDB_CODE_PAR_WRONG_VALUE_TYPE:
      return "Invalid value type : %s";
    case TSDB_CODE_PAR_INVALID_FUNTION:
      return "Invalid function name : %s";
    case TSDB_CODE_PAR_FUNTION_PARA_NUM:
      return "Invalid number of arguments : %s";
    case TSDB_CODE_PAR_FUNTION_PARA_TYPE:
      return "Inconsistent datatypes : %s";
    case TSDB_CODE_PAR_ILLEGAL_USE_AGG_FUNCTION:
      return "There mustn't be aggregation";
    case TSDB_CODE_PAR_WRONG_NUMBER_OF_SELECT:
      return "ORDER BY item must be the number of a SELECT-list expression";
    case TSDB_CODE_PAR_GROUPBY_LACK_EXPRESSION:
      return "Not a GROUP BY expression";
    case TSDB_CODE_PAR_NOT_SELECTED_EXPRESSION:
      return "Not SELECTed expression";
    case TSDB_CODE_PAR_NOT_SINGLE_GROUP:
      return "Not a single-group group function";
    case TSDB_CODE_OUT_OF_MEMORY:
      return "Out of memory";
    default:
      return "Unknown error";
  }
}

static int32_t generateSyntaxErrMsg(STranslateContext* pCxt, int32_t errCode, ...) {
  va_list vArgList;
  va_start(vArgList, errCode);
  vsnprintf(pCxt->msgBuf.buf, pCxt->msgBuf.len, getSyntaxErrFormat(errCode), vArgList);
  va_end(vArgList);
  pCxt->errCode = errCode;
  return errCode;
}

static int32_t addNamespace(STranslateContext* pCxt, void* pTable) {
  size_t currTotalLevel = taosArrayGetSize(pCxt->pNsLevel);
  if (currTotalLevel > pCxt->currLevel) {
    SArray* pTables = taosArrayGetP(pCxt->pNsLevel, pCxt->currLevel);
    taosArrayPush(pTables, &pTable);
  } else {
    do {
      SArray* pTables = taosArrayInit(TARRAY_MIN_SIZE, POINTER_BYTES);
      if (pCxt->currLevel == currTotalLevel) {
        taosArrayPush(pTables, &pTable);
      }
      taosArrayPush(pCxt->pNsLevel, &pTables);
      ++currTotalLevel;
    } while (currTotalLevel <= pCxt->currLevel);
  }
  return TSDB_CODE_SUCCESS;
}

static SName* toName(int32_t acctId, const SRealTableNode* pRealTable, SName* pName) {
  pName->type = TSDB_TABLE_NAME_T;
  pName->acctId = acctId;
  strcpy(pName->dbname, pRealTable->table.dbName);
  strcpy(pName->tname, pRealTable->table.tableName);
  return pName;
}

static bool belongTable(const char* currentDb, const SColumnNode* pCol, const STableNode* pTable) {
  int cmp = 0;
  if ('\0' != pCol->dbName[0]) {
    cmp = strcmp(pCol->dbName, pTable->dbName);
  } else {
    cmp = (QUERY_NODE_REAL_TABLE == nodeType(pTable) ? strcmp(currentDb, pTable->dbName) : 0);
  }
  if (0 == cmp) {
    cmp = strcmp(pCol->tableAlias, pTable->tableAlias);
  }
  return (0 == cmp);
}

static SNodeList* getProjectList(SNode* pNode) {
  if (QUERY_NODE_SELECT_STMT == nodeType(pNode)) {
    return ((SSelectStmt*)pNode)->pProjectionList;
  }
  return NULL;
}

static void setColumnInfoBySchema(const SRealTableNode* pTable, const SSchema* pColSchema, bool isTag, SColumnNode* pCol) {
  strcpy(pCol->dbName, pTable->table.dbName);
  strcpy(pCol->tableAlias, pTable->table.tableAlias);
  strcpy(pCol->tableName, pTable->table.tableName);
  strcpy(pCol->colName, pColSchema->name);
  if ('\0' == pCol->node.aliasName[0]) {
    strcpy(pCol->node.aliasName, pColSchema->name);
  }
  pCol->tableId = pTable->pMeta->uid;
  pCol->colId = pColSchema->colId;
  pCol->colType = isTag ? COLUMN_TYPE_TAG : COLUMN_TYPE_COLUMN;
  pCol->node.resType.type = pColSchema->type;
  pCol->node.resType.bytes = pColSchema->bytes;
}

static void setColumnInfoByExpr(const STableNode* pTable, SExprNode* pExpr, SColumnNode* pCol) {
  pCol->pProjectRef = (SNode*)pExpr;
  nodesListAppend(pExpr->pAssociationList, (SNode*)pCol);
  if (NULL != pTable) {
    strcpy(pCol->tableAlias, pTable->tableAlias);
  }
  strcpy(pCol->colName, pExpr->aliasName);
  pCol->node.resType = pExpr->resType;
}

static int32_t createColumnNodeByTable(STranslateContext* pCxt, const STableNode* pTable, SNodeList* pList) {
  if (QUERY_NODE_REAL_TABLE == nodeType(pTable)) {
    const STableMeta* pMeta = ((SRealTableNode*)pTable)->pMeta;
    int32_t nums = pMeta->tableInfo.numOfTags + pMeta->tableInfo.numOfColumns;
    for (int32_t i = 0; i < nums; ++i) {
      SColumnNode* pCol = (SColumnNode*)nodesMakeNode(QUERY_NODE_COLUMN);
      if (NULL == pCol) {
        return generateSyntaxErrMsg(pCxt, TSDB_CODE_OUT_OF_MEMORY);
      }
      setColumnInfoBySchema((SRealTableNode*)pTable, pMeta->schema + i, (i < pMeta->tableInfo.numOfTags), pCol);
      nodesListAppend(pList, (SNode*)pCol);
    }
  } else {
    SNodeList* pProjectList = getProjectList(((STempTableNode*)pTable)->pSubquery);
    SNode* pNode;
    FOREACH(pNode, pProjectList) {
      SColumnNode* pCol = (SColumnNode*)nodesMakeNode(QUERY_NODE_COLUMN);
      if (NULL == pCol) {
        return generateSyntaxErrMsg(pCxt, TSDB_CODE_OUT_OF_MEMORY);
      }
      setColumnInfoByExpr(pTable, (SExprNode*)pNode, pCol);
      nodesListAppend(pList, (SNode*)pCol);
    }
  }
  return TSDB_CODE_SUCCESS;
}

static bool findAndSetColumn(SColumnNode* pCol, const STableNode* pTable) {
  bool found = false;
  if (QUERY_NODE_REAL_TABLE == nodeType(pTable)) {
    const STableMeta* pMeta = ((SRealTableNode*)pTable)->pMeta;
    int32_t nums = pMeta->tableInfo.numOfTags + pMeta->tableInfo.numOfColumns;
    for (int32_t i = 0; i < nums; ++i) {
      if (0 == strcmp(pCol->colName, pMeta->schema[i].name)) {
        setColumnInfoBySchema((SRealTableNode*)pTable, pMeta->schema + i, (i < pMeta->tableInfo.numOfTags), pCol);
        found = true;
        break;
      }
    }
  } else {
    SNodeList* pProjectList = getProjectList(((STempTableNode*)pTable)->pSubquery);
    SNode* pNode;
    FOREACH(pNode, pProjectList) {
      SExprNode* pExpr = (SExprNode*)pNode;
      if (0 == strcmp(pCol->colName, pExpr->aliasName)) {
        setColumnInfoByExpr(pTable, pExpr, pCol);
        found = true;
        break;
      }
    }
  }
  return found;
}

static EDealRes translateColumnWithPrefix(STranslateContext* pCxt, SColumnNode* pCol) {
  SArray* pTables = taosArrayGetP(pCxt->pNsLevel, pCxt->currLevel);
  size_t nums = taosArrayGetSize(pTables);
  bool foundTable = false;
  for (size_t i = 0; i < nums; ++i) {
    STableNode* pTable = taosArrayGetP(pTables, i);
    if (belongTable(pCxt->pParseCxt->db, pCol, pTable)) {
      foundTable = true;
      if (findAndSetColumn(pCol, pTable)) {
        break;
      }
      generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_INVALID_COLUMN, pCol->colName);
      return DEAL_RES_ERROR;
    }
  }
  if (!foundTable) {
    generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_TABLE_NOT_EXIST, pCol->tableAlias);
    return DEAL_RES_ERROR;
  }
  return DEAL_RES_CONTINUE;
}

static EDealRes translateColumnWithoutPrefix(STranslateContext* pCxt, SColumnNode* pCol) {
  SArray* pTables = taosArrayGetP(pCxt->pNsLevel, pCxt->currLevel);
  size_t nums = taosArrayGetSize(pTables);
  bool found = false;
  for (size_t i = 0; i < nums; ++i) {
    STableNode* pTable = taosArrayGetP(pTables, i);
    if (findAndSetColumn(pCol, pTable)) {
      if (found) {
        generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_AMBIGUOUS_COLUMN, pCol->colName);
        return DEAL_RES_ERROR;
      }
      found = true;
    }
  }
  if (!found) {
    generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_INVALID_COLUMN, pCol->colName);
    return DEAL_RES_ERROR;
  }
  return DEAL_RES_CONTINUE;
}

static bool translateColumnUseAlias(STranslateContext* pCxt, SColumnNode* pCol) {
  SNodeList* pProjectionList = pCxt->pCurrStmt->pProjectionList;
  SNode* pNode;
  FOREACH(pNode, pProjectionList) {
    SExprNode* pExpr = (SExprNode*)pNode;
    if (0 == strcmp(pCol->colName, pExpr->aliasName)) {
        setColumnInfoByExpr(NULL, pExpr, pCol);
        return true;
    }
  }
  return false;
}

static EDealRes translateColumn(STranslateContext* pCxt, SColumnNode* pCol) {
  // count(*)/first(*)/last(*)
  if (0 == strcmp(pCol->colName, "*")) {
    return DEAL_RES_CONTINUE;
  }
  if ('\0' != pCol->tableAlias[0]) {
    return translateColumnWithPrefix(pCxt, pCol);
  }
  bool found = false;
  if (SQL_CLAUSE_ORDER_BY == pCxt->currClause) {
    found = translateColumnUseAlias(pCxt, pCol);
  }
  return found ? DEAL_RES_CONTINUE : translateColumnWithoutPrefix(pCxt, pCol);
}

static int32_t trimStringCopy(const char* src, int32_t len, bool format, char* dst) {
  char* dstVal = dst;
  if (format) {
    varDataSetLen(dst, len);
    dstVal = varDataVal(dst);
  }
  // delete escape character: \\, \', \"
  char delim = src[0];
  int32_t cnt = 0;
  int32_t j = 0;
  for (uint32_t k = 1; k < len - 1; ++k) {
    if (src[k] == '\\' || (src[k] == delim && src[k + 1] == delim)) {
      dstVal[j] = src[k + 1];
      cnt++;
      j++;
      k++;
      continue;
    }
    dstVal[j] = src[k];
    j++;
  }
  dstVal[j] = '\0';
  return j;
}

static EDealRes translateValue(STranslateContext* pCxt, SValueNode* pVal) {
  if (pVal->isDuration) {
    char unit = 0;
    if (parseAbsoluteDuration(pVal->literal, strlen(pVal->literal), &pVal->datum.i, &unit, pVal->node.resType.precision) != TSDB_CODE_SUCCESS) {
      generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_WRONG_VALUE_TYPE, pVal->literal);
      return DEAL_RES_ERROR;
    }
  } else {
    switch (pVal->node.resType.type) {
      case TSDB_DATA_TYPE_NULL:
        break;
      case TSDB_DATA_TYPE_BOOL:
        pVal->datum.b = (0 == strcasecmp(pVal->literal, "true"));
        break;
      case TSDB_DATA_TYPE_TINYINT:
      case TSDB_DATA_TYPE_SMALLINT:
      case TSDB_DATA_TYPE_INT:
      case TSDB_DATA_TYPE_BIGINT: {
        char* endPtr = NULL;
        pVal->datum.i = strtoll(pVal->literal, &endPtr, 10);
        break;
      }
      case TSDB_DATA_TYPE_UTINYINT:
      case TSDB_DATA_TYPE_USMALLINT:
      case TSDB_DATA_TYPE_UINT:
      case TSDB_DATA_TYPE_UBIGINT: {
        char* endPtr = NULL;
        pVal->datum.u = strtoull(pVal->literal, &endPtr, 10);
        break;
      }
      case TSDB_DATA_TYPE_FLOAT:
      case TSDB_DATA_TYPE_DOUBLE: {
        char* endPtr = NULL;
        pVal->datum.d = strtold(pVal->literal, &endPtr);
        break;
      }
      case TSDB_DATA_TYPE_BINARY:
      case TSDB_DATA_TYPE_NCHAR:
      case TSDB_DATA_TYPE_VARCHAR:
      case TSDB_DATA_TYPE_VARBINARY: {
        int32_t n = strlen(pVal->literal);
        pVal->datum.p = calloc(1, n + VARSTR_HEADER_SIZE);
        if (NULL == pVal->datum.p) {
          generateSyntaxErrMsg(pCxt, TSDB_CODE_OUT_OF_MEMORY);
          return DEAL_RES_ERROR;
        }
        trimStringCopy(pVal->literal, n, true, pVal->datum.p);
        break;
      }
      case TSDB_DATA_TYPE_TIMESTAMP: {
        int32_t n = strlen(pVal->literal);
        char* tmp = calloc(1, n);
        if (NULL == tmp) {
          generateSyntaxErrMsg(pCxt, TSDB_CODE_OUT_OF_MEMORY);
          return DEAL_RES_ERROR;
        }
        int32_t len = trimStringCopy(pVal->literal, n, false, tmp);
        if (taosParseTime(tmp, &pVal->datum.i, len, pVal->node.resType.precision, tsDaylight) != TSDB_CODE_SUCCESS) {
          tfree(tmp);
          generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_WRONG_VALUE_TYPE, pVal->literal);
          return DEAL_RES_ERROR;
        }
        tfree(tmp);
        break;
      }
      case TSDB_DATA_TYPE_JSON:
      case TSDB_DATA_TYPE_DECIMAL:
      case TSDB_DATA_TYPE_BLOB:
        // todo
      default:
        break;
    }
  }
  return DEAL_RES_CONTINUE;
}

static EDealRes translateOperator(STranslateContext* pCxt, SOperatorNode* pOp) {
  SDataType ldt = ((SExprNode*)(pOp->pLeft))->resType;
  SDataType rdt = ((SExprNode*)(pOp->pRight))->resType;
  if (nodesIsArithmeticOp(pOp)) {
    if (TSDB_DATA_TYPE_JSON == ldt.type || TSDB_DATA_TYPE_BLOB == ldt.type ||
        TSDB_DATA_TYPE_JSON == rdt.type || TSDB_DATA_TYPE_BLOB == rdt.type) {
      generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_WRONG_VALUE_TYPE, ((SExprNode*)(pOp->pRight))->aliasName);
      return DEAL_RES_ERROR;
    }
    pOp->node.resType.type = TSDB_DATA_TYPE_DOUBLE;
    pOp->node.resType.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes;
  } else if (nodesIsComparisonOp(pOp)) {
    if (TSDB_DATA_TYPE_JSON == ldt.type || TSDB_DATA_TYPE_BLOB == ldt.type ||
        TSDB_DATA_TYPE_JSON == rdt.type || TSDB_DATA_TYPE_BLOB == rdt.type) {
      generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_WRONG_VALUE_TYPE, ((SExprNode*)(pOp->pRight))->aliasName);
      return DEAL_RES_ERROR;
    }
    pOp->node.resType.type = TSDB_DATA_TYPE_BOOL;
    pOp->node.resType.bytes = tDataTypes[TSDB_DATA_TYPE_BOOL].bytes;
  } else {
    // todo json operator
  }
  return DEAL_RES_CONTINUE;
}

static EDealRes translateFunction(STranslateContext* pCxt, SFunctionNode* pFunc) {
  if (TSDB_CODE_SUCCESS != fmGetFuncInfo(pFunc->functionName, &pFunc->funcId, &pFunc->funcType)) {
    generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_INVALID_FUNTION, pFunc->functionName);
    return DEAL_RES_ERROR;
  }
  int32_t code = fmGetFuncResultType(pFunc);
  if (TSDB_CODE_SUCCESS != code) {
    generateSyntaxErrMsg(pCxt, code, pFunc->functionName);
    return DEAL_RES_ERROR;
  }
  if (fmIsAggFunc(pFunc->funcId) && beforeHaving(pCxt->currClause)) {
    generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_ILLEGAL_USE_AGG_FUNCTION);
    return DEAL_RES_ERROR;
  }
  return DEAL_RES_CONTINUE;
}

static EDealRes translateExprSubquery(STranslateContext* pCxt, SNode* pNode) {
  return (TSDB_CODE_SUCCESS == translateSubquery(pCxt, pNode) ? DEAL_RES_CONTINUE : DEAL_RES_ERROR);
}

static EDealRes translateLogicCond(STranslateContext* pCxt, SLogicConditionNode* pCond) {
  pCond->node.resType.type = TSDB_DATA_TYPE_BOOL;
  pCond->node.resType.bytes = tDataTypes[TSDB_DATA_TYPE_BOOL].bytes;
  return DEAL_RES_CONTINUE;
}

static EDealRes doTranslateExpr(SNode* pNode, void* pContext) {
  STranslateContext* pCxt = (STranslateContext*)pContext;
  switch (nodeType(pNode)) {
    case QUERY_NODE_COLUMN:
      return translateColumn(pCxt, (SColumnNode*)pNode);
    case QUERY_NODE_VALUE:
      return translateValue(pCxt, (SValueNode*)pNode);
    case QUERY_NODE_OPERATOR:
      return translateOperator(pCxt, (SOperatorNode*)pNode);
    case QUERY_NODE_FUNCTION:
      return translateFunction(pCxt, (SFunctionNode*)pNode);
    case QUERY_NODE_LOGIC_CONDITION:
      return translateLogicCond(pCxt, (SLogicConditionNode*)pNode);
    case QUERY_NODE_TEMP_TABLE:
      return translateExprSubquery(pCxt, ((STempTableNode*)pNode)->pSubquery);
    default:
      break;
  }
  return DEAL_RES_CONTINUE;
}

static int32_t translateExpr(STranslateContext* pCxt, SNode* pNode) {
  nodesWalkNodePostOrder(pNode, doTranslateExpr, pCxt);
  return pCxt->errCode;
}

static int32_t translateExprList(STranslateContext* pCxt, SNodeList* pList) {
  nodesWalkListPostOrder(pList, doTranslateExpr, pCxt);
  return pCxt->errCode;
}

static bool isAliasColumn(SColumnNode* pCol) {
  return ('\0' == pCol->tableAlias[0]);
}

static bool isDistinctOrderBy(STranslateContext* pCxt) {
  return (SQL_CLAUSE_ORDER_BY == pCxt->currClause && pCxt->pCurrStmt->isDistinct);
}

static SNodeList* getGroupByList(STranslateContext* pCxt) {
  if (isDistinctOrderBy(pCxt)) {
    return pCxt->pCurrStmt->pProjectionList;
  }
  return pCxt->pCurrStmt->pGroupByList;
}

static SNode* getGroupByNode(SNode* pNode) {
  if (QUERY_NODE_GROUPING_SET == nodeType(pNode)) {
    return nodesListGetNode(((SGroupingSetNode*)pNode)->pParameterList, 0);
  }
  return pNode;
}

static int32_t getGroupByErrorCode(STranslateContext* pCxt) {
  if (isDistinctOrderBy(pCxt)) {
    return TSDB_CODE_PAR_NOT_SELECTED_EXPRESSION;
  }
  return TSDB_CODE_PAR_GROUPBY_LACK_EXPRESSION;
}

static EDealRes doCheckExprForGroupBy(SNode* pNode, void* pContext) {
  STranslateContext* pCxt = (STranslateContext*)pContext;
  if (!nodesIsExprNode(pNode) || (QUERY_NODE_COLUMN == nodeType(pNode) && isAliasColumn((SColumnNode*)pNode))) {
    return DEAL_RES_CONTINUE;
  }
  if (QUERY_NODE_FUNCTION == nodeType(pNode) && fmIsAggFunc(((SFunctionNode*)pNode)->funcId) && !isDistinctOrderBy(pCxt)) {
    return DEAL_RES_IGNORE_CHILD;
  }
  SNode* pGroupNode;
  FOREACH(pGroupNode, getGroupByList(pCxt)) {
    if (nodesEqualNode(getGroupByNode(pGroupNode), pNode)) {
      return DEAL_RES_IGNORE_CHILD;
    }
  }
  if (QUERY_NODE_COLUMN == nodeType(pNode) ||
      (QUERY_NODE_FUNCTION == nodeType(pNode) && fmIsAggFunc(((SFunctionNode*)pNode)->funcId) && isDistinctOrderBy(pCxt))) {
    generateSyntaxErrMsg(pCxt, getGroupByErrorCode(pCxt));
    return DEAL_RES_ERROR;
  }
  return DEAL_RES_CONTINUE;
}

static int32_t checkExprForGroupBy(STranslateContext* pCxt, SNode* pNode) {
  nodesWalkNode(pNode, doCheckExprForGroupBy, pCxt);
  return pCxt->errCode;
}

static int32_t checkExprListForGroupBy(STranslateContext* pCxt, SNodeList* pList) {
  if (NULL == getGroupByList(pCxt)) {
    return TSDB_CODE_SUCCESS;
  }
  nodesWalkList(pList, doCheckExprForGroupBy, pCxt);
  return pCxt->errCode;
}

typedef struct CheckAggColCoexistCxt {
  STranslateContext* pTranslateCxt;
  bool existAggFunc;
  bool existCol;
} CheckAggColCoexistCxt;

static EDealRes doCheckAggColCoexist(SNode* pNode, void* pContext) {
  CheckAggColCoexistCxt* pCxt = (CheckAggColCoexistCxt*)pContext;
  if (QUERY_NODE_FUNCTION == nodeType(pNode) && fmIsAggFunc(((SFunctionNode*)pNode)->funcId)) {
    pCxt->existAggFunc = true;
    return DEAL_RES_IGNORE_CHILD;
  }
  if (QUERY_NODE_COLUMN == nodeType(pNode)) {
    pCxt->existCol = true;
  }
  return DEAL_RES_CONTINUE;
}

static int32_t checkAggColCoexist(STranslateContext* pCxt, SSelectStmt* pSelect) {
  if (NULL != pSelect->pGroupByList) {
    return TSDB_CODE_SUCCESS;
  }
  CheckAggColCoexistCxt cxt = { .pTranslateCxt = pCxt, .existAggFunc = false, .existCol = false };
  nodesWalkList(pSelect->pProjectionList, doCheckAggColCoexist, &cxt);
  if (!pSelect->isDistinct) {
    nodesWalkList(pSelect->pOrderByList, doCheckAggColCoexist, &cxt);
  }
  if (cxt.existAggFunc && cxt.existCol) {
    return generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_NOT_SINGLE_GROUP);
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t setTableVgroupList(STranslateContext *pCxt, SName* name, SVgroupsInfo **pVgList) {
  SArray* vgroupList = NULL;
  int32_t code = catalogGetTableDistVgInfo(pCxt->pParseCxt->pCatalog, pCxt->pParseCxt->pTransporter, &(pCxt->pParseCxt->mgmtEpSet), name, &vgroupList);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }
  
  size_t vgroupNum = taosArrayGetSize(vgroupList);

  SVgroupsInfo *vgList = calloc(1, sizeof(SVgroupsInfo) + sizeof(SVgroupInfo) * vgroupNum);
  vgList->numOfVgroups = vgroupNum;
  
  for (int32_t i = 0; i < vgroupNum; ++i) {
    SVgroupInfo *vg = taosArrayGet(vgroupList, i);
    vgList->vgroups[i] = *vg;
  }

  *pVgList = vgList;
  taosArrayDestroy(vgroupList);

  return TSDB_CODE_SUCCESS;
}

static int32_t translateTable(STranslateContext* pCxt, SNode* pTable) {
  int32_t code = TSDB_CODE_SUCCESS;
  switch (nodeType(pTable)) {
    case QUERY_NODE_REAL_TABLE: {
      SRealTableNode* pRealTable = (SRealTableNode*)pTable;
      SName name;
      code = catalogGetTableMeta(pCxt->pParseCxt->pCatalog, pCxt->pParseCxt->pTransporter, &(pCxt->pParseCxt->mgmtEpSet),
          toName(pCxt->pParseCxt->acctId, pRealTable, &name), &(pRealTable->pMeta));
      if (TSDB_CODE_SUCCESS != code) {
        return generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_TABLE_NOT_EXIST, pRealTable->table.tableName);
      }
      code = setTableVgroupList(pCxt, &name, &(pRealTable->pVgroupList));
      if (TSDB_CODE_SUCCESS != code) {
        return code;
      }
      code = addNamespace(pCxt, pRealTable);
      break;
    }
    case QUERY_NODE_TEMP_TABLE: {
      STempTableNode* pTempTable = (STempTableNode*)pTable;
      code = translateSubquery(pCxt, pTempTable->pSubquery);
      if (TSDB_CODE_SUCCESS == code) {
        code = addNamespace(pCxt, pTempTable);
      }
      break;
    }
    case QUERY_NODE_JOIN_TABLE: {
      SJoinTableNode* pJoinTable = (SJoinTableNode*)pTable;
      code = translateTable(pCxt, pJoinTable->pLeft);
      if (TSDB_CODE_SUCCESS == code) {
        code = translateTable(pCxt, pJoinTable->pRight);
      }
      if (TSDB_CODE_SUCCESS == code) {
        code = translateExpr(pCxt, pJoinTable->pOnCond);
      }
      break;
    }
    default:
      break;
  }
  return code;
}

static int32_t translateStar(STranslateContext* pCxt, SSelectStmt* pSelect, bool* pIsSelectStar) {
  if (NULL == pSelect->pProjectionList) { // select * ...
    SArray* pTables = taosArrayGetP(pCxt->pNsLevel, pCxt->currLevel);
    size_t nums = taosArrayGetSize(pTables);
    pSelect->pProjectionList = nodesMakeList();
    if (NULL == pSelect->pProjectionList) {
      return generateSyntaxErrMsg(pCxt, TSDB_CODE_OUT_OF_MEMORY);
    }
    for (size_t i = 0; i < nums; ++i) {
      STableNode* pTable = taosArrayGetP(pTables, i);
      int32_t code = createColumnNodeByTable(pCxt, pTable, pSelect->pProjectionList);
      if (TSDB_CODE_SUCCESS != code) {
        return code;
      }
    }
    *pIsSelectStar = true;
  } else {

  }
  return TSDB_CODE_SUCCESS;
}

static int32_t getPositionValue(const SValueNode* pVal) {
  switch (pVal->node.resType.type) {
    case TSDB_DATA_TYPE_NULL:
    case TSDB_DATA_TYPE_BINARY:
    case TSDB_DATA_TYPE_TIMESTAMP:
    case TSDB_DATA_TYPE_NCHAR:
    case TSDB_DATA_TYPE_VARCHAR:
    case TSDB_DATA_TYPE_VARBINARY:
    case TSDB_DATA_TYPE_JSON:
      return -1;
    case TSDB_DATA_TYPE_BOOL:
      return (pVal->datum.b ? 1 : 0);
    case TSDB_DATA_TYPE_TINYINT:
    case TSDB_DATA_TYPE_SMALLINT:
    case TSDB_DATA_TYPE_INT:
    case TSDB_DATA_TYPE_BIGINT:
      return pVal->datum.i;
    case TSDB_DATA_TYPE_FLOAT:
    case TSDB_DATA_TYPE_DOUBLE:
      return pVal->datum.d;
    case TSDB_DATA_TYPE_UTINYINT:
    case TSDB_DATA_TYPE_USMALLINT:
    case TSDB_DATA_TYPE_UINT:
    case TSDB_DATA_TYPE_UBIGINT:
      return pVal->datum.u; 
    default:
      break;
  }
  return -1;
}

static int32_t translateOrderByPosition(STranslateContext* pCxt, SNodeList* pProjectionList, SNodeList* pOrderByList, bool* pOther) {
  *pOther = false;
  SNode* pNode;
  FOREACH(pNode, pOrderByList) {
    SNode* pExpr = ((SOrderByExprNode*)pNode)->pExpr;
    if (QUERY_NODE_VALUE == nodeType(pExpr)) {
      SValueNode* pVal = (SValueNode*)pExpr;
      if (!translateValue(pCxt, pVal)) {
        return pCxt->errCode;
      }
      int32_t pos = getPositionValue(pVal);
      if (pos < 0) {
        ERASE_NODE(pOrderByList);
        continue;
      } else if (0 == pos || pos > LIST_LENGTH(pProjectionList)) {
        return generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_WRONG_NUMBER_OF_SELECT);
      } else {
        SColumnNode* pCol = (SColumnNode*)nodesMakeNode(QUERY_NODE_COLUMN);
        if (NULL == pCol) {
          return generateSyntaxErrMsg(pCxt, TSDB_CODE_OUT_OF_MEMORY);
        }
        setColumnInfoByExpr(NULL, (SExprNode*)nodesListGetNode(pProjectionList, pos - 1), pCol);
        ((SOrderByExprNode*)pNode)->pExpr = (SNode*)pCol;
        nodesDestroyNode(pExpr);
      }
    } else {
      *pOther = true;
    }
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t translateOrderBy(STranslateContext* pCxt, SSelectStmt* pSelect) {
  bool other;
  int32_t code = translateOrderByPosition(pCxt, pSelect->pProjectionList, pSelect->pOrderByList, &other);
  if (TSDB_CODE_SUCCESS != code) {
    return code;
  }
  if (!other) {
    return TSDB_CODE_SUCCESS;
  }
  pCxt->currClause = SQL_CLAUSE_ORDER_BY;
  code = translateExprList(pCxt, pSelect->pOrderByList);
  if (TSDB_CODE_SUCCESS == code) {
    code = checkExprListForGroupBy(pCxt, pSelect->pOrderByList);
  }
  return code;
}

static int32_t translateSelectList(STranslateContext* pCxt, SSelectStmt* pSelect) {
  bool isSelectStar = false;
  int32_t code = translateStar(pCxt, pSelect, &isSelectStar);
  if (TSDB_CODE_SUCCESS == code && !isSelectStar) {
    pCxt->currClause = SQL_CLAUSE_SELECT;
    code = translateExprList(pCxt, pSelect->pProjectionList);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = checkExprListForGroupBy(pCxt, pSelect->pProjectionList);
  }
  return code;
}

static int32_t translateHaving(STranslateContext* pCxt, SSelectStmt* pSelect) {
  if (NULL == pSelect->pGroupByList && NULL != pSelect->pHaving) {
    return generateSyntaxErrMsg(pCxt, TSDB_CODE_PAR_GROUPBY_LACK_EXPRESSION);
  }
  pCxt->currClause = SQL_CLAUSE_HAVING;
  int32_t code = translateExpr(pCxt, pSelect->pHaving);
  if (TSDB_CODE_SUCCESS == code) {
    code = checkExprForGroupBy(pCxt, pSelect->pHaving);
  }
  return code;
}

static int32_t translateGroupBy(STranslateContext* pCxt, SNodeList* pGroupByList) {
  pCxt->currClause = SQL_CLAUSE_GROUP_BY;
  return translateExprList(pCxt, pGroupByList);
}

static int32_t translateWindow(STranslateContext* pCxt, SNode* pWindow) {
  pCxt->currClause = SQL_CLAUSE_WINDOW;
  return translateExpr(pCxt, pWindow);
}

static int32_t translatePartitionBy(STranslateContext* pCxt, SNodeList* pPartitionByList) {
  pCxt->currClause = SQL_CLAUSE_PARTITION_BY;
  return translateExprList(pCxt, pPartitionByList);
}

static int32_t translateWhere(STranslateContext* pCxt, SNode* pWhere) {
  pCxt->currClause = SQL_CLAUSE_WHERE;
  return translateExpr(pCxt, pWhere);
}

static int32_t translateFrom(STranslateContext* pCxt, SNode* pTable) {
  pCxt->currClause = SQL_CLAUSE_FROM;
  return translateTable(pCxt, pTable);
}

static int32_t translateSelect(STranslateContext* pCxt, SSelectStmt* pSelect) {
  pCxt->pCurrStmt = pSelect;
  int32_t code = translateFrom(pCxt, pSelect->pFromTable);
  if (TSDB_CODE_SUCCESS == code) {
    code = translateWhere(pCxt, pSelect->pWhere);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = translatePartitionBy(pCxt, pSelect->pPartitionByList);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = translateWindow(pCxt, pSelect->pWindow);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = translateGroupBy(pCxt, pSelect->pGroupByList);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = translateHaving(pCxt, pSelect);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = translateSelectList(pCxt, pSelect);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = translateOrderBy(pCxt, pSelect);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = checkAggColCoexist(pCxt, pSelect);
  }
  return code;
}

static void buildCreateDbReq(STranslateContext* pCxt, SCreateDatabaseStmt* pStmt, SCreateDbReq* pReq) {
  SName name = {0};
  tNameSetDbName(&name, pCxt->pParseCxt->acctId, pStmt->dbName, strlen(pStmt->dbName));
  tNameGetFullDbName(&name, pReq->db);
  pReq->numOfVgroups = pStmt->options.numOfVgroups;
  pReq->cacheBlockSize = pStmt->options.cacheBlockSize;
  pReq->totalBlocks = pStmt->options.numOfBlocks;
  pReq->daysPerFile = pStmt->options.daysPerFile;
  pReq->daysToKeep0 = pStmt->options.keep;
  pReq->daysToKeep1 = -1;
  pReq->daysToKeep2 = -1;
  pReq->minRows = pStmt->options.minRowsPerBlock;
  pReq->maxRows = pStmt->options.maxRowsPerBlock;
  pReq->commitTime = -1;
  pReq->fsyncPeriod = pStmt->options.fsyncPeriod;
  pReq->walLevel = pStmt->options.walLevel;
  pReq->precision = pStmt->options.precision;
  pReq->compression = pStmt->options.compressionLevel;
  pReq->replications = pStmt->options.replica;
  pReq->quorum = pStmt->options.quorum;
  pReq->update = -1;
  pReq->cacheLastRow = pStmt->options.cachelast;
  pReq->ignoreExist = pStmt->ignoreExists;
  pReq->streamMode = pStmt->options.streamMode;
  return;
}

static int32_t translateCreateDatabase(STranslateContext* pCxt, SCreateDatabaseStmt* pStmt) {
  SCreateDbReq createReq = {0};
  buildCreateDbReq(pCxt, pStmt, &createReq);

  pCxt->pCmdMsg = malloc(sizeof(SCmdMsgInfo));
  if (NULL== pCxt->pCmdMsg) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pCxt->pCmdMsg->epSet = pCxt->pParseCxt->mgmtEpSet;
  pCxt->pCmdMsg->msgType = TDMT_MND_CREATE_DB;
  pCxt->pCmdMsg->msgLen = tSerializeSCreateDbReq(NULL, 0, &createReq);
  pCxt->pCmdMsg->pMsg = malloc(pCxt->pCmdMsg->msgLen);
  if (NULL== pCxt->pCmdMsg->pMsg) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  tSerializeSCreateDbReq(pCxt->pCmdMsg->pMsg, pCxt->pCmdMsg->msgLen, &createReq);

  return TSDB_CODE_SUCCESS;
}

static int32_t translateUseDatabase(STranslateContext* pCxt, SUseDatabaseStmt* pStmt) {
  SName name = {0};
  tNameSetDbName(&name, pCxt->pParseCxt->acctId, pStmt->dbName, strlen(pStmt->dbName));

  SUseDbReq usedbReq = {0};
  tNameExtractFullName(&name, usedbReq.db);

  pCxt->pCmdMsg = malloc(sizeof(SCmdMsgInfo));
  if (NULL== pCxt->pCmdMsg) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pCxt->pCmdMsg->epSet = pCxt->pParseCxt->mgmtEpSet;
  pCxt->pCmdMsg->msgType = TDMT_MND_USE_DB;
  pCxt->pCmdMsg->msgLen = tSerializeSUseDbReq(NULL, 0, &usedbReq);
  pCxt->pCmdMsg->pMsg = malloc(pCxt->pCmdMsg->msgLen);
  if (NULL== pCxt->pCmdMsg->pMsg) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  tSerializeSUseDbReq(pCxt->pCmdMsg->pMsg, pCxt->pCmdMsg->msgLen, &usedbReq);

  return TSDB_CODE_SUCCESS;
}

static int32_t translateShowDatabases(STranslateContext* pCxt) {
  SShowReq showReq = { .type = TSDB_MGMT_TABLE_DB };

  pCxt->pCmdMsg = malloc(sizeof(SCmdMsgInfo));
  if (NULL== pCxt->pCmdMsg) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pCxt->pCmdMsg->epSet = pCxt->pParseCxt->mgmtEpSet;
  pCxt->pCmdMsg->msgType = TDMT_MND_SHOW;
  pCxt->pCmdMsg->msgLen = tSerializeSShowReq(NULL, 0, &showReq);
  pCxt->pCmdMsg->pMsg = malloc(pCxt->pCmdMsg->msgLen);
  if (NULL== pCxt->pCmdMsg->pMsg) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  tSerializeSShowReq(pCxt->pCmdMsg->pMsg, pCxt->pCmdMsg->msgLen, &showReq);

  return TSDB_CODE_SUCCESS;
}

static int32_t translateShowTables(STranslateContext* pCxt) {
  SName name = {0};
  SVShowTablesReq* pShowReq = calloc(1, sizeof(SVShowTablesReq));
  tNameSetDbName(&name, pCxt->pParseCxt->acctId, pCxt->pParseCxt->db, strlen(pCxt->pParseCxt->db));
  char dbFname[TSDB_DB_FNAME_LEN] = {0};
  tNameGetFullDbName(&name, dbFname);

  SArray* array = NULL;
  int32_t code = catalogGetDBVgInfo(pCxt->pParseCxt->pCatalog, pCxt->pParseCxt->pTransporter, &pCxt->pParseCxt->mgmtEpSet, dbFname, false, &array);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }
  SVgroupInfo* info = taosArrayGet(array, 0);
  pShowReq->head.vgId = htonl(info->vgId);

  pCxt->pCmdMsg = malloc(sizeof(SCmdMsgInfo));
  if (NULL== pCxt->pCmdMsg) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }
  pCxt->pCmdMsg->epSet = info->epset;
  pCxt->pCmdMsg->msgType = TDMT_VND_SHOW_TABLES;
  pCxt->pCmdMsg->msgLen = sizeof(SVShowTablesReq);
  pCxt->pCmdMsg->pMsg = pShowReq;
  pCxt->pCmdMsg->pExtension = array;

  return TSDB_CODE_SUCCESS;
}

static int32_t translateQuery(STranslateContext* pCxt, SNode* pNode) {
  int32_t code = TSDB_CODE_SUCCESS;
  switch (nodeType(pNode)) {
    case QUERY_NODE_SELECT_STMT:
      code = translateSelect(pCxt, (SSelectStmt*)pNode);
      break;
    case QUERY_NODE_CREATE_DATABASE_STMT:
      code = translateCreateDatabase(pCxt, (SCreateDatabaseStmt*)pNode);
      break;
    case QUERY_NODE_USE_DATABASE_STMT:
      code = translateUseDatabase(pCxt, (SUseDatabaseStmt*)pNode);
      break;
    case QUERY_NODE_SHOW_DATABASES_STMT:
      code = translateShowDatabases(pCxt);
      break;
    case QUERY_NODE_SHOW_TABLES_STMT:
      code = translateShowTables(pCxt);
      break;
    default:
      break;
  }
  return code;
}

static int32_t translateSubquery(STranslateContext* pCxt, SNode* pNode) {
  ++(pCxt->currLevel);
  ESqlClause currClause = pCxt->currClause;
  SSelectStmt* pCurrStmt = pCxt->pCurrStmt;
  int32_t code = translateQuery(pCxt, pNode);
  --(pCxt->currLevel);
  pCxt->currClause = currClause;
  pCxt->pCurrStmt = pCurrStmt;
  return code;
}

static int32_t setReslutSchema(STranslateContext* pCxt, SQuery* pQuery) {
  if (QUERY_NODE_SELECT_STMT == nodeType(pQuery->pRoot)) {
    SSelectStmt* pSelect = (SSelectStmt*)pQuery->pRoot;
    pQuery->numOfResCols = LIST_LENGTH(pSelect->pProjectionList);
    pQuery->pResSchema = calloc(pQuery->numOfResCols, sizeof(SSchema));
    if (NULL == pQuery->pResSchema) {
      return generateSyntaxErrMsg(pCxt, TSDB_CODE_OUT_OF_MEMORY);
    }
    SNode* pNode;
    int32_t index = 0;
    FOREACH(pNode, pSelect->pProjectionList) {
      SExprNode* pExpr = (SExprNode*)pNode;
      pQuery->pResSchema[index].type = pExpr->resType.type;
      pQuery->pResSchema[index].bytes = pExpr->resType.bytes;
      strcpy(pQuery->pResSchema[index].name, pExpr->aliasName);
    }
  }
  return TSDB_CODE_SUCCESS;
}

static void destroyTranslateContext(STranslateContext* pCxt) {
  taosArrayDestroy(pCxt->pNsLevel);
  if (NULL != pCxt->pCmdMsg) {
    tfree(pCxt->pCmdMsg->pMsg);
    tfree(pCxt->pCmdMsg);
  }
}

typedef struct SVgroupTablesBatch {
  SVCreateTbBatchReq req;
  SVgroupInfo        info;
} SVgroupTablesBatch;

static void toSchema(const SColumnDefNode* pCol, int32_t colId, SSchema* pSchema) {
  pSchema->colId = colId;
  pSchema->type = pCol->dataType.type;
  pSchema->bytes = pCol->dataType.bytes;
  strcpy(pSchema->name, pCol->colName);
}

static int32_t doBuildSingleTableBatchReq(SName* pTableName, SNodeList* pColumns, SVgroupInfo* pVgroupInfo, SVgroupTablesBatch* pBatch) {
  SVCreateTbReq req = {0};
  req.type = TD_NORMAL_TABLE;
  req.name = strdup(tNameGetTableName(pTableName));

  req.ntbCfg.nCols = LIST_LENGTH(pColumns);
  int32_t num = req.ntbCfg.nCols;

  req.ntbCfg.pSchema = calloc(num, sizeof(SSchema));
  SNode* pCol;
  int32_t index = 0;
  FOREACH(pCol, pColumns) {
    toSchema((SColumnDefNode*)pCol, index + 1, req.ntbCfg.pSchema + index);
    ++index;
  }

  pBatch->info = *pVgroupInfo;
  pBatch->req.pArray = taosArrayInit(1, sizeof(struct SVCreateTbReq));
  if (pBatch->req.pArray == NULL) {
    return TSDB_CODE_QRY_OUT_OF_MEMORY;
  }

  taosArrayPush(pBatch->req.pArray, &req);
  return TSDB_CODE_SUCCESS;
}

static int32_t serializeVgroupTablesBatchImpl(SVgroupTablesBatch* pTbBatch, SArray* pBufArray) {
  int tlen = sizeof(SMsgHead) + tSerializeSVCreateTbBatchReq(NULL, &(pTbBatch->req));
  void* buf = malloc(tlen);
  if (buf == NULL) {
    // TODO: handle error
  }

  ((SMsgHead*)buf)->vgId = htonl(pTbBatch->info.vgId);
  ((SMsgHead*)buf)->contLen = htonl(tlen);

  void* pBuf = POINTER_SHIFT(buf, sizeof(SMsgHead));
  tSerializeSVCreateTbBatchReq(&pBuf, &(pTbBatch->req));

  SVgDataBlocks* pVgData = calloc(1, sizeof(SVgDataBlocks));
  pVgData->vg    = pTbBatch->info;
  pVgData->pData = buf;
  pVgData->size  = tlen;
  pVgData->numOfTables = (int32_t) taosArrayGetSize(pTbBatch->req.pArray);

  taosArrayPush(pBufArray, &pVgData);
}

static void destroyCreateTbReqBatch(SVgroupTablesBatch* pTbBatch) {
  size_t size = taosArrayGetSize(pTbBatch->req.pArray);
  for(int32_t i = 0; i < size; ++i) {
    SVCreateTbReq* pTableReq = taosArrayGet(pTbBatch->req.pArray, i);
    tfree(pTableReq->name);

    if (pTableReq->type == TSDB_NORMAL_TABLE) {
      tfree(pTableReq->ntbCfg.pSchema);
    } else if (pTableReq->type == TSDB_CHILD_TABLE) {
      tfree(pTableReq->ctbCfg.pTag);
    } else {
      assert(0);
    }
  }

  taosArrayDestroy(pTbBatch->req.pArray);
}

static int32_t rewriteQuery(STranslateContext* pCxt, SQuery* pQuery) {
  if (QUERY_NODE_CREATE_TABLE_STMT == nodeType(pQuery->pRoot)) {
    SCreateTableStmt* pStmt = (SCreateTableStmt*)pQuery->pRoot;

    SName tableName = { .type = TSDB_TABLE_NAME_T, .acctId = pCxt->pParseCxt->acctId };
    if ('\0' == pStmt->dbName[0]) {
      strcpy(tableName.dbname, pCxt->pParseCxt->db);
    } else {
      strcpy(tableName.dbname, pStmt->dbName);
    }
    strcpy(tableName.tname, pStmt->tableName);
    SVgroupInfo info = {0};
    catalogGetTableHashVgroup(pCxt->pParseCxt->pCatalog, pCxt->pParseCxt->pTransporter, &pCxt->pParseCxt->mgmtEpSet, &tableName, &info);

    SVgroupTablesBatch tbatch = {0};
    int32_t code = doBuildSingleTableBatchReq(&tableName, pStmt->pCols, &info, &tbatch);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }

    SArray* pBufArray = taosArrayInit(1, POINTER_BYTES);
    if (pBufArray == NULL) {
      return TSDB_CODE_QRY_OUT_OF_MEMORY;
    }

    serializeVgroupTablesBatchImpl(&tbatch, pBufArray);
    destroyCreateTbReqBatch(&tbatch);

    SVnodeModifOpStmt* pNewStmt = nodesMakeNode(QUERY_NODE_VNODE_MODIF_STMT);
    pNewStmt->sqlNodeType = nodeType(pQuery->pRoot);
    pNewStmt->pDataBlocks = pBufArray;
    pQuery->sqlNodeType = nodeType(pQuery->pRoot);
    nodesDestroyNode(pQuery->pRoot);
    pQuery->pRoot = (SNode*)pNewStmt;
  }
  return TSDB_CODE_SUCCESS;
}

int32_t doTranslate(SParseContext* pParseCxt, SQuery* pQuery) {
  STranslateContext cxt = {
    .pParseCxt = pParseCxt,
    .errCode = TSDB_CODE_SUCCESS,
    .msgBuf = { .buf = pParseCxt->pMsg, .len = pParseCxt->msgLen },
    .pNsLevel = taosArrayInit(TARRAY_MIN_SIZE, POINTER_BYTES),
    .currLevel = 0,
    .currClause = 0
  };
  int32_t code = fmFuncMgtInit();
  if (TSDB_CODE_SUCCESS == code) {
    code = rewriteQuery(&cxt, pQuery);
  }
  if (TSDB_CODE_SUCCESS == code) {
    code = translateQuery(&cxt, pQuery->pRoot);
  }
  if (TSDB_CODE_SUCCESS == code) {
    if (pQuery->directRpc) {
      pQuery->pCmdMsg = cxt.pCmdMsg;
      cxt.pCmdMsg = NULL;
    }
    if (pQuery->haveResultSet) {
      code = setReslutSchema(&cxt, pQuery);
    }
  }
  destroyTranslateContext(&cxt);
  return code;
}