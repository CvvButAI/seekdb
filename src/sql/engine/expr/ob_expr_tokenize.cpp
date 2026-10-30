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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
namespace
{

// TOKENIZE is a pure function of (text, parser, parser properties): building the
// ObFTParseHelper (parse the parser name, parse/normalize the property JSON, resolve the
// parser descriptor) does exactly the same work on every call that shares a parser config.
// Keep one fully initialized helper plus its arena per thread and recycle the arena between
// calls, so the hot path only pays for segmentation and result packing.
class ObFTTokenizeCache
{
public:
  static constexpr int64_t MAX_PARSER_NAME_LEN = 96;
  static constexpr int64_t MAX_PROPERTIES_LEN = 1024;

  static ObFTTokenizeCache &instance()
  {
    static thread_local ObFTTokenizeCache cache;
    return cache;
  }

  // Return a ready-to-use helper (whose arena has been recycled) for the given parser
  // config. OB_SIZE_OVERFLOW tells the caller to fall back to a one-shot helper, which
  // keeps behavior for exotic (very long) parser properties unchanged.
  int acquire(const common::ObString &parser_name,
              const common::ObString &properties,
              storage::ObFTParseHelper *&helper,
              common::ObIAllocator *&allocator)
  {
    int ret = OB_SUCCESS;
    helper = nullptr;
    allocator = nullptr;
    if (OB_UNLIKELY(parser_name.empty() || parser_name.length() > MAX_PARSER_NAME_LEN
                    || properties.length() > MAX_PROPERTIES_LEN)) {
      ret = OB_SIZE_OVERFLOW;
    } else if (is_same_parser_config(parser_name, properties)) {
      arena_.reuse();
      helper = &helper_;
      allocator = &arena_;
    } else {
      helper_.reset();
      arena_.reuse();
      if (OB_FAIL(helper_.init(&arena_, parser_name, properties))) {
        is_valid_ = false;
      } else {
        MEMCPY(parser_name_buf_, parser_name.ptr(), parser_name.length());
        parser_name_len_ = parser_name.length();
        props_len_ = properties.length();
        if (properties.length() > 0) {
          MEMCPY(props_buf_, properties.ptr(), properties.length());
        }
        is_valid_ = true;
        helper = &helper_;
        allocator = &arena_;
      }
    }
    return ret;
  }

private:
  ObFTTokenizeCache()
    : arena_(lib::ObMemAttr("FTTokenizeCache")),
      helper_(),
      parser_name_len_(0),
      props_len_(0),
      is_valid_(false)
  {
    parser_name_buf_[0] = '\0';
    props_buf_[0] = '\0';
  }
  ~ObFTTokenizeCache() = default;

  bool is_same_parser_config(const common::ObString &parser_name,
                             const common::ObString &properties) const
  {
    return is_valid_
           && parser_name_len_ == parser_name.length()
           && props_len_ == properties.length()
           && 0 == MEMCMP(parser_name_buf_, parser_name.ptr(), parser_name.length())
           && (0 == properties.length()
               || 0 == MEMCMP(props_buf_, properties.ptr(), properties.length()));
  }

private:
  common::ObArenaAllocator arena_;
  storage::ObFTParseHelper helper_;
  char parser_name_buf_[MAX_PARSER_NAME_LEN + 1];
  char props_buf_[MAX_PROPERTIES_LEN + 1];
  int64_t parser_name_len_;
  int64_t props_len_;
  bool is_valid_;

  DISALLOW_COPY_AND_ASSIGN(ObFTTokenizeCache);
};

} // anonymous namespace

namespace
{

// Normalizing parser properties (rebuild_props_for_ddl + to_format_json) is only a function
// of the parser name when the caller passes no parser properties, which is the common case
// (`tokenize(text, 'ik')`). Cache the normalized json string per thread so repeated calls
// skip building and serializing a json object just to hand it to ObFTParseHelper::init.
class ObFTCanonicalPropsCache
{
public:
  static constexpr int64_t MAX_PARSER_NAME_LEN = 96;
  static constexpr int64_t MAX_PROPERTIES_LEN = 1024;

  static ObFTCanonicalPropsCache &instance()
  {
    static thread_local ObFTCanonicalPropsCache cache;
    return cache;
  }

  bool find(const common::ObString &parser_name, common::ObString &properties) const
  {
    bool hit = is_valid_
               && parser_name.length() == parser_name_len_
               && parser_name.length() > 0
               && 0 == MEMCMP(parser_name_buf_, parser_name.ptr(), parser_name.length());
    if (hit) {
      properties = common::ObString(static_cast<int32_t>(props_len_), props_buf_);
    }
    return hit;
  }

  void store(const common::ObString &parser_name, const common::ObString &properties)
  {
    if (parser_name.empty()
        || parser_name.length() > MAX_PARSER_NAME_LEN
        || properties.length() > MAX_PROPERTIES_LEN) {
      is_valid_ = false;
    } else {
      MEMCPY(parser_name_buf_, parser_name.ptr(), parser_name.length());
      parser_name_len_ = parser_name.length();
      if (properties.length() > 0) {
        MEMCPY(props_buf_, properties.ptr(), properties.length());
      }
      props_len_ = properties.length();
      is_valid_ = true;
    }
  }

private:
  ObFTCanonicalPropsCache() : parser_name_len_(0), props_len_(0), is_valid_(false)
  {
    parser_name_buf_[0] = '\0';
    props_buf_[0] = '\0';
  }
  ~ObFTCanonicalPropsCache() = default;

private:
  char parser_name_buf_[MAX_PARSER_NAME_LEN + 1];
  char props_buf_[MAX_PROPERTIES_LEN + 1];
  int64_t parser_name_len_;
  int64_t props_len_;
  bool is_valid_;

  DISALLOW_COPY_AND_ASSIGN(ObFTCanonicalPropsCache);
};

} // anonymous namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper local_helper;
  storage::ObFTParseHelper *tokenize_helper = nullptr;
  common::ObIAllocator *helper_allocator = nullptr;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObFTTokenizeCache::instance().acquire(param.parser_name_,
                                                           param.properties_,
                                                           tokenize_helper,
                                                           helper_allocator))) {
    if (OB_SIZE_OVERFLOW != ret) {
    } else {
      // parser config too long to cache, keep the original one-shot behavior
      ret = OB_SUCCESS;
      if (OB_FAIL(local_helper.init(&allocator, param.parser_name_, param.properties_))) {
      } else {
        tokenize_helper = &local_helper;
        helper_allocator = &allocator;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(tokenize_helper) || OB_ISNULL(helper_allocator)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper->segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper->make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper->make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  if (OB_UNLIKELY(storage::OB_FT_PARSER_NAME_LENGTH > TokenizeParam::PARSER_NAME_BUF_LEN)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(param.parser_name_buf_,
                                             TokenizeParam::PARSER_NAME_BUF_LEN))) {
  } else {
    param.parser_name_ = ObString::make_string(param.parser_name_buf_);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;
  ObFTCanonicalPropsCache &props_cache = ObFTCanonicalPropsCache::instance();
  ObString cached_properties;

  if (properties.empty() && props_cache.find(parser_name_, cached_properties)) {
    // the normalized form only depends on the parser name here, reuse it instead of
    // rebuilding a json object and serializing it on every call
    properties_ = cached_properties;
  } else if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  } else if (properties.empty()) {
    props_cache.store(parser_name_, properties_);
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
