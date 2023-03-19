// Copyright 2010-2018 Alibaba Inc. All Rights Reserved.
// Author:
//   

#define USING_LOG_PREFIX SQL_OPT
#include "sql/optimizer/ob_log_link_scan.h"
#include "sql/resolver/expr/ob_expr_info_flag.h"

using namespace oceanbase::sql;
using namespace oceanbase::common;
using namespace oceanbase::sql::log_op_def;

namespace oceanbase
{
namespace sql
{

ObLogLinkScan::ObLogLinkScan(ObLogPlan &plan)
  : ObLogLink(plan)
{}

int ObLogLinkScan::allocate_expr_post(ObAllocExprContext &ctx)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < output_exprs_.count(); ++i) {
    ObRawExpr *expr = output_exprs_.at(i);
    if (OB_ISNULL(expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null expr", K(ret));
    } else if (OB_FAIL(mark_expr_produced(expr,
                                          branch_id_,
                                          id_,
                                          ctx))) {
      LOG_WARN("failed to mark expr as produced", K(branch_id_), K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObLogicalOperator::allocate_expr_post(ctx))) {
      LOG_WARN("failed to allocate_expr_post", K(ret));
    } else { /*do nothing*/ }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
