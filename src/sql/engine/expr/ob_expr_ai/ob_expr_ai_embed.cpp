/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (!ob_is_string_type(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    ret = ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator, *arg_content,
        expr.args_[CONTENT_IDX]->datum_meta_, expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content);
    if (OB_SUCC(ret) && (model_id.empty() || content.empty())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->is_null() ? 0 : arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
    const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *output = expr.locate_batch_datums(ctx);
  ObBitVector &flags = expr.get_evaluated_flags(ctx);
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
    ret = expr.args_[i]->eval_batch(ctx, skip, batch_size);
  }
  ObEvalCtx::TempAllocGuard alloc_guard(ctx);
  MultimodeAlloctor allocator(alloc_guard.get_allocator());
  ObEvalCtx::BatchInfoScopeGuard batch_guard(ctx);
  batch_guard.set_batch_size(batch_size);
  ObArray<int64_t> pending;
  ObArray<ObString> models;
  ObArray<ObString> texts;
  ObArray<int64_t> dimensions;
  ObArray<uint8_t> consumed;
  for (int64_t row = 0; OB_SUCC(ret) && row < batch_size; ++row) {
    if (skip.at(row) || flags.at(row)) {
      continue;
    }
    batch_guard.set_batch_idx(row);
    ObDatum *model = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx).at(row);
    ObDatum *text = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx).at(row);
    ObString value;
    int64_t dimension = 0;
    if (model->is_null()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (text->is_null()) {
      output[row].set_null();
      flags.set(row);
    } else if (model->get_string().empty()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      if (expr.arg_cnt_ == 3) {
        ObDatum *dim = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx).at(row);
        if (dim->is_null() || (dimension = dim->get_int()) <= 0) {
          ret = OB_INVALID_ARGUMENT;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, *text,
          expr.args_[CONTENT_IDX]->datum_meta_, expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), value))) {
      } else if (value.empty()) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(pending.push_back(row))) {
      } else if (OB_FAIL(models.push_back(model->get_string()))) {
      } else if (OB_FAIL(texts.push_back(value))) {
      } else if (OB_FAIL(dimensions.push_back(dimension))) {
      } else if (OB_FAIL(consumed.push_back(0))) {
      }
    }
  }
  query::ObIAiEndpointResolver *resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  for (int64_t seed = 0; OB_SUCC(ret) && seed < pending.count(); ++seed) {
    if (consumed.at(seed)) {
      continue;
    }
    ObArray<ObString> inputs;
    ObArray<int64_t> rows;
    // Bound request size and keep different models/dimensions in separate groups.
    for (int64_t i = seed; OB_SUCC(ret) && i < pending.count() && rows.count() < 128; ++i) {
      if (!consumed.at(i) && models.at(i) == models.at(seed) && dimensions.at(i) == dimensions.at(seed)) {
        if (OB_FAIL(inputs.push_back(texts.at(i)))) {
        } else if (OB_FAIL(rows.push_back(pending.at(i)))) {
        } else {
          consumed.at(i) = 1;
        }
      }
    }
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    ObJsonObject *config = nullptr;
    ObJsonInt *dimension = nullptr;
    ObArray<ObString> results;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, models.at(seed), info))) {
    } else if (OB_FAIL(resolver->resolve_by_model_name(models.at(seed), allocator, endpoint))) {
    } else {
      if (dimensions.at(seed) > 0) {
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dimensions.at(seed), dimension))) {
        } else if (OB_FAIL(config->add("dimensions", dimension))) {
        }
      }
      if (OB_SUCC(ret)) {
        ObAIFuncModel model(allocator, *info, endpoint);
        ret = model.call_dense_embedding_vector_v2(inputs, config, results);
      }
    }
    if (OB_SUCC(ret) && results.count() != rows.count()) {
      ret = OB_INVALID_DATA;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
      const int64_t row = rows.at(i);
      batch_guard.set_batch_idx(row);
      ret = ObAIFuncUtils::set_string_result(expr, ctx, output[row], results.at(i));
    }
    // Publish only after all results in this request have been copied successfully.
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
      flags.set(rows.at(i));
    }
  }
  if (OB_FAIL(ret)) {
    for (int64_t row = 0; row < batch_size; ++row) {
      if (!skip.at(row)) {
        flags.unset(row);
        output[row].set_null();
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
