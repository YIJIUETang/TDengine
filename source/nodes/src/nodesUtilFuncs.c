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

#include "nodes.h"
#include "nodesShowStmts.h"
#include "taoserror.h"

static SNode* makeNode(ENodeType type, size_t size) {
  SNode* p = calloc(1, size);
  if (NULL == p) {
    return NULL;
  }
  setNodeType(p, type);
  return p;
}

SNode* nodesMakeNode(ENodeType type) {
  switch (type) {
    case QUERY_NODE_COLUMN:
      return makeNode(type, sizeof(SColumnNode));
    case QUERY_NODE_VALUE:
      return makeNode(type, sizeof(SValueNode));
    case QUERY_NODE_OPERATOR:
      return makeNode(type, sizeof(SOperatorNode));
    case QUERY_NODE_LOGIC_CONDITION:
      return makeNode(type, sizeof(SLogicConditionNode));
    case QUERY_NODE_IS_NULL_CONDITION:
      return makeNode(type, sizeof(SIsNullCondNode));
    case QUERY_NODE_FUNCTION:
      return makeNode(type, sizeof(SFunctionNode));
    case QUERY_NODE_REAL_TABLE:
      return makeNode(type, sizeof(SRealTableNode));
    case QUERY_NODE_TEMP_TABLE:
      return makeNode(type, sizeof(STempTableNode));
    case QUERY_NODE_JOIN_TABLE:
      return makeNode(type, sizeof(SJoinTableNode));
    case QUERY_NODE_GROUPING_SET:
      return makeNode(type, sizeof(SGroupingSetNode));
    case QUERY_NODE_ORDER_BY_EXPR:
      return makeNode(type, sizeof(SOrderByExprNode));
    case QUERY_NODE_LIMIT:
      return makeNode(type, sizeof(SLimitNode));
    case QUERY_NODE_STATE_WINDOW:
      return makeNode(type, sizeof(SStateWindowNode));
    case QUERY_NODE_SESSION_WINDOW:
      return makeNode(type, sizeof(SSessionWindowNode));
    case QUERY_NODE_INTERVAL_WINDOW:
      return makeNode(type, sizeof(SIntervalWindowNode));
    case QUERY_NODE_SET_OPERATOR:
      return makeNode(type, sizeof(SSetOperator));
    case QUERY_NODE_SELECT_STMT:
      return makeNode(type, sizeof(SSelectStmt));
    case QUERY_NODE_SHOW_STMT:
      return makeNode(type, sizeof(SShowStmt));
    default:
      break;
  }
  return NULL;
}

void nodesDestroyNode(SNode* pNode) {

}

SNodeList* nodesMakeList() {
  SNodeList* p = calloc(1, sizeof(SNodeList));
  if (NULL == p) {
    return NULL;
  }
  return p;
}

SNodeList* nodesListAppend(SNodeList* pList, SNode* pNode) {
  if (NULL == pList || NULL == pNode) {
    return NULL;
  }
  SListCell* p = calloc(1, sizeof(SListCell));
  if (NULL == p) {
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return pList;
  }
  p->pNode = pNode;
  if (NULL == pList->pHead) {
    pList->pHead = p;
  }
  if (NULL != pList->pTail) {
    pList->pTail->pNext = p;
  }
  pList->pTail = p;
  return pList;
}

void nodesDestroyList(SNodeList* pList) {

}

bool nodesIsTimeorderQuery(const SNode* pQuery) {

}

bool nodesIsTimelineQuery(const SNode* pQuery) {

}