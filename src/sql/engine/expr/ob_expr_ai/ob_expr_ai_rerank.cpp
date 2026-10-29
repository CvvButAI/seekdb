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
#include "ob_expr_ai_rerank.h"
#include <algorithm>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *arg_model = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_key = nullptr;
  if (expr.arg_cnt_ == 4 ? OB_FAIL(expr.eval_param_value(ctx, arg_model, arg_query, arg_documents, arg_key))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model, arg_query, arg_documents))) {
  } else if (arg_model->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObEvalCtx::TempAllocGuard guard(ctx);
    MultimodeAlloctor allocator(guard.get_allocator());
    ObIJsonBase *base = nullptr;
    bool is_null = false;
    ObJsonArray *documents = nullptr;
    ObJsonArray *texts = nullptr;
    ObJsonArray *output = nullptr;
    const bool with_key = OB_NOT_NULL(arg_key) && !arg_key->is_null();
    ObString key = with_key ? arg_key->get_string() : ObString();
    ObString model_name = arg_model->get_string();
    ObString query;
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    query::ObIAiEndpointResolver *resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, *arg_query,
        expr.args_[QUERY_IDX]->datum_meta_, expr.args_[QUERY_IDX]->obj_meta_.has_lob_header(), query))) {
    } else if (model_name.empty() || query.empty() || (with_key && key.empty())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator, DOCUMENTS_IDX, base, is_null))) {
    } else if (is_null || OB_ISNULL(base) || base->json_type() != ObJsonNodeType::J_ARRAY
               || base->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      documents = static_cast<ObJsonArray *>(base);
      if (with_key) {
        ret = get_doc_array_from_documents_array_with_key(allocator, documents, key, texts);
      } else {
        texts = documents;
        for (int64_t i = 0; OB_SUCC(ret) && i < texts->element_count(); ++i) {
          if (OB_ISNULL(texts->get_value(i)) || texts->get_value(i)->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
    } else if (OB_ISNULL(resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint))) {
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, output))) {
    }
    struct RankedDocument {
      int64_t index;
      double score;
      ObJsonNode *score_node;
      ObJsonObject *response_node;
    };
    ObArray<RankedDocument> ranked;
    if (OB_SUCC(ret)) {
      ObAIFuncModel model(allocator, *info, endpoint);
      for (int64_t start = 0; OB_SUCC(ret) && start < texts->element_count(); start += 20) {
        const int64_t end = std::min<int64_t>(start + 20, texts->element_count());
        ObJsonArray *batch = nullptr;
        ObJsonArray *response = nullptr;
        ObArray<uint8_t> seen;
        if (OB_FAIL(construct_batch_document_array(allocator, texts, start, end, batch))) {
        } else if (OB_FAIL(model.call_rerank(query, batch, response))) {
        } else if (OB_ISNULL(response) || response->element_count() != end - start) {
          ret = OB_INVALID_DATA;
        }
        for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
          ret = seen.push_back(0);
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < response->element_count(); ++i) {
          ObJsonNode *item = response->get_value(i);
          if (OB_ISNULL(item) || item->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_INVALID_DATA;
          } else {
            ObJsonObject *object = static_cast<ObJsonObject *>(item);
            ObJsonNode *index_node = object->get_value(INDEX_KEY);
            ObJsonNode *score_node = object->get_value(SCORE_KEY);
            int64_t index = -1;
            double score = 0;
            if (OB_ISNULL(index_node)) {
              ret = OB_INVALID_DATA;
            } else if (index_node->json_type() == ObJsonNodeType::J_INT) {
              index = index_node->get_int();
            } else if (index_node->json_type() == ObJsonNodeType::J_UINT
                       && index_node->get_uint() < static_cast<uint64_t>(end - start)) {
              index = index_node->get_uint();
            } else {
              ret = OB_INVALID_DATA;
            }
            if (OB_FAIL(ret)) {
            } else if (index < 0 || index >= end - start || seen.at(index)) {
              ret = OB_INVALID_DATA;
            } else if (OB_FAIL(ObAIFuncJsonUtils::finite_number(score_node, score))) {
            } else {
              seen.at(index) = 1;
              RankedDocument document{start + index, score, score_node, object};
              ret = ranked.push_back(document);
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      std::sort(ranked.begin(), ranked.end(), [](const RankedDocument &a, const RankedDocument &b) {
        return a.score > b.score || (a.score == b.score && a.index < b.index);
      });
      for (int64_t i = 0; OB_SUCC(ret) && i < ranked.count(); ++i) {
        const RankedDocument &document = ranked.at(i);
        ObJsonObject *item = nullptr;
        if (with_key) {
          item = static_cast<ObJsonObject *>(documents->get_value(document.index)->clone(&allocator, true));
          if (OB_ISNULL(item)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            ret = item->add(MODEL_SCORE_KEY, document.score_node);
          }
        } else {
          ObJsonInt *index = nullptr;
          item = static_cast<ObJsonObject *>(document.response_node->clone(&allocator, true));
          if (OB_ISNULL(item)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, document.index, index))) {
          } else if (OB_FAIL(item->add(INDEX_KEY, index))) {
          }
        }
        if (OB_SUCC(ret)) {
          ret = output->append(item);
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObString raw;
      if (OB_FAIL(output->get_raw_binary(raw, &allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   bool got_data = false;
  //   ObObj const_data;
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
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
