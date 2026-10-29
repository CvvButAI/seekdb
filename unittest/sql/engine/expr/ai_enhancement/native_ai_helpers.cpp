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

#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "observer/ai_service/ob_ai_service_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_isql_result_handler.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace oceanbase::common;
using namespace oceanbase::sql;
static int checks = 0;
static void require(bool ok, const char *label)
{
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL %s\n", label); std::exit(1); }
  std::printf("PASS %s\n", label);
}
static ObJsonObject *parse(ObIAllocator &alloc, const char *text)
{
  ObString input(text);
  ObJsonObject *object = nullptr;
  require(OB_SUCCESS == ObAIFuncJsonUtils::get_json_object_form_str(alloc, input, object), "real JSON parse");
  return object;
}

// Exercise real endpoint persistence and loading with an in-memory SQL boundary.
// No database is started and no SQL executor validation is implied.
class EndpointTransaction : public ObMySQLTransaction
{
public:
  std::string sql;
  int write(const char *text, const int32_t, int64_t &affected) override
  {
    sql = text;
    affected = 1;
    return OB_SUCCESS;
  }
};

struct EndpointRowData
{
  const char *provider = "Ollama";
  const char *key = "";
  const char *url = "http://127.0.0.1:8000/api/generate";
  bool key_is_null = false;
  bool key_column_missing = false;
};

class EndpointRow : public oceanbase::common::sqlclient::ObMySQLResult
{
public:
  explicit EndpointRow(EndpointRowData &data) : data_(data), rows_(0) {}
  int64_t get_column_count() const override { return 12; }
  int close() override { return OB_SUCCESS; }
  int next() override { return rows_++ == 0 ? OB_SUCCESS : OB_ITER_END; }
  int get_int(const char *name, int64_t &value) const override
  {
    value = 1;
    return std::string(name) == "endpoint_id" ? OB_SUCCESS : OB_ERR_COLUMN_NOT_FOUND;
  }
  int get_varchar(const char *name, ObString &value) const override
  {
    const std::string column(name);
    if (column == "scope") value = ObString("ALL");
    else if (column == "endpoint_name") value = ObString("native_ep");
    else if (column == "ai_model_name") value = ObString("native-test");
    else if (column == "url") value = ObString(data_.url);
    else if (column == "provider") value = ObString(data_.provider);
    else if (column == "access_key") {
      if (data_.key_column_missing) return OB_ERR_COLUMN_NOT_FOUND;
      if (data_.key_is_null) return OB_ERR_NULL_VALUE;
      value = ObString(data_.key);
    } else return OB_ERR_NULL_VALUE;  // Nullable optional endpoint columns.
    return OB_SUCCESS;
  }
#define UNSUPPORTED_GETTER(method, type) \
  int method(const int64_t, type &) const override { return OB_NOT_SUPPORTED; } \
  int method(const char *, type &) const override { return OB_NOT_SUPPORTED; }
  int get_int(const int64_t, int64_t &) const override { return OB_NOT_SUPPORTED; }
  int get_varchar(const int64_t, ObString &) const override { return OB_NOT_SUPPORTED; }
  UNSUPPORTED_GETTER(get_uint, uint64_t)
  UNSUPPORTED_GETTER(get_datetime, int64_t)
  UNSUPPORTED_GETTER(get_date, int32_t)
  UNSUPPORTED_GETTER(get_time, int64_t)
  UNSUPPORTED_GETTER(get_year, uint8_t)
  UNSUPPORTED_GETTER(get_bool, bool)
  UNSUPPORTED_GETTER(get_float, float)
  UNSUPPORTED_GETTER(get_double, double)
  UNSUPPORTED_GETTER(get_type, ObObjMeta)
#undef UNSUPPORTED_GETTER
  int get_timestamp(const int64_t, const ObTimeZoneInfo *, int64_t &) const override { return OB_NOT_SUPPORTED; }
  int get_timestamp(const char *, const ObTimeZoneInfo *, int64_t &) const override { return OB_NOT_SUPPORTED; }
  int get_obj(const int64_t, ObObj &, const ObTimeZoneInfo *, ObIAllocator *) const override { return OB_NOT_SUPPORTED; }
  int get_obj(const char *, ObObj &) const override { return OB_NOT_SUPPORTED; }
private:
  int inner_get_number(const int64_t, number::ObNumber &, IAllocator &) const override { return OB_NOT_SUPPORTED; }
  int inner_get_number(const char *, number::ObNumber &, IAllocator &) const override { return OB_NOT_SUPPORTED; }
  EndpointRowData &data_;
  int rows_;
};

class EndpointResult : public oceanbase::common::sqlclient::ObISQLResultHandler
{
public:
  explicit EndpointResult(EndpointRowData &data) : row_(data) {}
  oceanbase::common::sqlclient::ObMySQLResult *mysql_result() override { return &row_; }
private:
  EndpointRow row_;
};

class EndpointClient : public ObISQLClient
{
public:
  explicit EndpointClient(EndpointRowData &data) : data_(data) {}
  int escape(const char *, const int64_t, char *, const int64_t, int64_t &) override { return OB_NOT_SUPPORTED; }
  int read(ReadResult &result, const char *, const int32_t) override
  {
    EndpointResult *handler = nullptr;
    return result.create_handler(handler, data_);
  }
  int write(const char *, const int32_t, int64_t &) override { return OB_NOT_SUPPORTED; }
  oceanbase::common::sqlclient::ObISQLConnection *get_connection() override { return nullptr; }
private:
  EndpointRowData &data_;
};

static int load_endpoint(ObArenaAllocator &alloc, EndpointRowData &data,
                         oceanbase::share::ObAiModelEndpointInfo &endpoint)
{
  EndpointClient client(data);
  return oceanbase::share::ObAiServiceProxy::select_ai_endpoint(alloc, client, ObString("native_ep"), endpoint);
}

static void legacy_endpoint_checks(ObArenaAllocator &alloc)
{
  using namespace oceanbase::share;
  EndpointRowData row;
  row.key_is_null = true;
  ObAiModelEndpointInfo endpoint;
  const int result = load_endpoint(alloc, row, endpoint);
  std::printf("Legacy NULL Ollama endpoint read status: %d\n", result);
  require(result == OB_SUCCESS, "legacy NULL Ollama endpoint is readable");
  ObString key("stale-output");
  require(OB_SUCCESS == endpoint.get_unencrypted_access_key(alloc, key) && key.empty(),
          "legacy keyless endpoint clears decrypted output");
  require(OB_SUCCESS == endpoint.merge_delta_endpoint(alloc, *parse(alloc, R"({"access_key":""})")),
          "legacy NULL endpoint can be repaired by ALTER");
  row.provider = "openai";
  require(OB_SUCCESS != load_endpoint(alloc, row, endpoint), "NULL key remains invalid for OpenAI");
  row.provider = "Ollama";
  row.key_is_null = false;
  row.key_column_missing = true;
  require(OB_ERR_COLUMN_NOT_FOUND == load_endpoint(alloc, row, endpoint), "missing key column is not silently accepted");
}

static void endpoint_persistence_checks(ObArenaAllocator &alloc)
{
  using namespace oceanbase::share;
  ObAiModelEndpointInfo endpoint, copied;
  require(OB_SUCCESS == endpoint.parse_from_json_base(alloc, ObString("native_ep"),
      *parse(alloc, R"({"ai_model_name":"native-test","url":"http://127.0.0.1:8000/api/generate","provider":"ollama","access_key":""})")),
      "create keyless endpoint for persistence");
  endpoint.set_endpoint_id(1);
  require(OB_SUCCESS == copied.deep_copy(alloc, endpoint), "copy empty-key endpoint");
  EndpointTransaction expected, actual;
  require(OB_SUCCESS == ObAiServiceProxy::insert_ai_endpoint(expected, 2, endpoint)
      && OB_SUCCESS == ObAiServiceProxy::insert_ai_endpoint(actual, 2, copied), "real endpoint INSERT SQL");
  if (actual.sql != expected.sql) {
    std::printf("Expected INSERT: %s\nActual INSERT: %s\n", expected.sql.c_str(), actual.sql.c_str());
  }
  require(actual.sql == expected.sql, "INSERT preserves empty-string key across deep copy");
  require(OB_SUCCESS == endpoint.merge_delta_endpoint(alloc, *parse(alloc, R"({"provider":"Ollama"})"))
      && OB_SUCCESS == copied.merge_delta_endpoint(alloc, *parse(alloc, R"({"provider":"Ollama"})")),
      "provider-only partial endpoint updates");
  require(OB_SUCCESS == ObAiServiceProxy::update_ai_endpoint(expected, 3, endpoint)
      && OB_SUCCESS == ObAiServiceProxy::update_ai_endpoint(actual, 3, copied), "real endpoint UPDATE SQL");
  require(actual.sql == expected.sql, "ALTER preserves empty-string key across deep copy");
  EndpointRowData row;
  ObAiModelEndpointInfo loaded;
  require(OB_SUCCESS == load_endpoint(alloc, row, loaded), "read persisted empty-string endpoint");
  require(OB_SUCCESS == ObAiServiceProxy::update_ai_endpoint(actual, 3, loaded)
      && actual.sql == expected.sql, "read then ALTER never turns empty key into SQL NULL");
  require(OB_SUCCESS == loaded.merge_delta_endpoint(alloc, *parse(alloc, R"({"access_key":""})"))
      && OB_SUCCESS == copied.deep_copy(alloc, loaded)
      && OB_SUCCESS == ObAiServiceProxy::update_ai_endpoint(actual, 3, copied)
      && actual.sql == expected.sql, "explicit empty key survives copy and repeated ALTER");
  require(loaded.get_provider() == ObString("Ollama") && loaded.get_url() == endpoint.get_url()
      && loaded.get_ai_model_name() == endpoint.get_ai_model_name(), "partial ALTER preserves other endpoint fields");
  row.provider = "openai";
  row.key = "test-only-secret";
  require(OB_SUCCESS == load_endpoint(alloc, row, loaded)
      && OB_SUCCESS == loaded.merge_delta_endpoint(alloc, *parse(alloc, R"({"provider":"OPENAI"})")),
      "provider-only ALTER with nonempty key");
  ObString key;
  require(OB_SUCCESS == loaded.get_unencrypted_access_key(alloc, key)
      && key == ObString("test-only-secret"), "partial ALTER preserves nonempty credentials");
  require(OB_SUCCESS != loaded.merge_delta_endpoint(alloc, *parse(alloc, R"({"access_key":""})")),
      "OpenAI still rejects clearing its required key");
  legacy_endpoint_checks(alloc);
}
static std::string rendered(ObIAllocator &alloc, const char *text)
{
  ObString value;
  require(OB_SUCCESS == ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc, parse(alloc,text), value), "real prompt render");
  return std::string(value.ptr(), value.length());
}
static void http_checks(ObArenaAllocator &alloc, const char *base_url, const char *mode,
                       const char *expected_json)
{
  using oceanbase::share::ObAiModelEndpointInfo;
  using oceanbase::share::EndpointType;
  const std::string endpoint_json = std::string("{\"ai_model_name\":\"native-test\",\"url\":\"")
      + base_url + "/compatible-mode/v1/embeddings\",\"provider\":\"openai\",\"access_key\":\"test-only-key\"}";
  ObAiModelEndpointInfo endpoint;
  require(OB_SUCCESS == endpoint.parse_from_json_base(alloc, ObString("native_ep"),
      *parse(alloc,endpoint_json.c_str())), "native endpoint parse");
  ObAIFuncExprInfo info(alloc,T_FUN_SYS_AI_EMBED);
  info.name_ = ObString("native-test");
  info.model_ = ObString("native-test");
  info.type_ = EndpointType::DENSE_EMBEDDING;
  ObAIFuncModel model(alloc,info,endpoint);
  ObArray<ObString> inputs, results;
  require(OB_SUCCESS == inputs.push_back(ObString("alpha"))
      && OB_SUCCESS == inputs.push_back(ObString("beta")), "native multi-input request");
  ObJsonObject *config = nullptr;
  ObJsonInt *dimension = nullptr;
  require(OB_SUCCESS == ObAIFuncJsonUtils::get_json_object(alloc,config)
      && OB_SUCCESS == ObAIFuncJsonUtils::get_json_int(alloc,3,dimension)
      && OB_SUCCESS == config->add("dimensions",dimension), "native dimensions config");
  const int ret = model.call_dense_embedding_vector_v2(inputs,config,results);
  const bool success = std::string(mode) == "normal" || std::string(mode) == "reverse";
  require(success ? ret == OB_SUCCESS : ret != OB_SUCCESS, "real HTTP/model status");
  if (success) {
    require(results.count() == 2, "real HTTP/model result count");
    ObJsonObject *expected = parse(alloc,expected_json);
    for (int i = 0; i < results.count(); ++i) {
      std::string wrapped = std::string("{\"vector\":") + std::string(results.at(i).ptr(),results.at(i).length()) + "}";
      ObJsonNode *actual = parse(alloc,wrapped.c_str())->get_value("vector");
      ObJsonNode *wanted = expected->get_value(i == 0 ? "alpha" : "beta");
      require(actual && wanted && actual->element_count() == 3, "real HTTP vector dimension");
      for (int j = 0; j < 3; ++j) {
        ObIJsonBase *a = nullptr, *b = nullptr;
        double av = 0, bv = 0;
        require(OB_SUCCESS == actual->get_array_element(j,a)
            && OB_SUCCESS == wanted->get_array_element(j,b)
            && OB_SUCCESS == a->to_double(av) && OB_SUCCESS == b->to_double(bv)
            && av == bv, "real HTTP input-to-vector mapping");
      }
    }
    const std::string native_json = std::string("{\"ai_model_name\":\"native-test\",\"url\":\"")
        + base_url + "/api/generate\",\"provider\":\"oLlAmA\",\"access_key\":\"\"}";
    ObAiModelEndpointInfo native_endpoint;
    require(OB_SUCCESS == native_endpoint.parse_from_json_base(alloc,ObString("native_ollama"),
        *parse(alloc,native_json.c_str())), "real Ollama keyless endpoint");
    require(native_endpoint.get_provider() == ObString("oLlAmA"), "provider case preserved");
    EndpointRowData native_row;
    const std::string native_url = std::string(base_url) + "/api/generate";
    native_row.url = native_url.c_str();
    native_row.key_is_null = true;
    require(OB_SUCCESS == load_endpoint(alloc,native_row,native_endpoint), "real Ollama endpoint read with legacy NULL key");
    require(OB_SUCCESS == native_endpoint.merge_delta_endpoint(alloc,*parse(alloc,R"({"provider":"Ollama"})")),
            "real Ollama provider-only ALTER before HTTP");
    ObAIFuncExprInfo complete_info(alloc,T_FUN_SYS_AI_COMPLETE);
    complete_info.model_ = ObString("native-test");
    complete_info.type_ = EndpointType::COMPLETION;
    ObAIFuncModel native_model(alloc,complete_info,native_endpoint);
    ObString prompt("hello"), completion;
    require(OB_SUCCESS == native_model.call_completion(prompt,nullptr,completion)
        && !completion.empty(), "real Ollama completion HTTP");
  }
  std::printf("Native HTTP checks (%s): %d passed\n",mode,checks);
}
int main(int argc, char **argv)
{
  ObArenaAllocator alloc("ai_native_test");
  if (argc == 2 && std::string(argv[1]) == "--legacy-null") {
    legacy_endpoint_checks(alloc);
    return 0;
  }
  if (argc == 5) {
    http_checks(alloc,argv[1],argv[2],argv[3]);
    return 0;
  }
  endpoint_persistence_checks(alloc);
  require(rendered(alloc, R"({"template":"{0}/{0}/{1}","args":[[1,true],{"k":2}]})") == "[1, true]/[1, true]/{\"k\": 2}", "JSON structured arguments");
  require(rendered(alloc, R"({"template":"outer:{0}","args":[{"template":"inner:{0}","args":["你好"]}]})") == "outer:inner:你好", "nested Unicode prompt");
  require(rendered(alloc, R"({"template":"{literal}/{0}","args":["{1}"]})") == "{literal}/{1}", "literal braces and no rescan");
  ObString output;
  require(OB_SUCCESS != ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(alloc, parse(alloc,R"({"template":"{99999999999999999999999999}","args":["x"]})"), output), "placeholder overflow rejection");
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;
  require(OB_SUCCESS == embed.parse_output(alloc,parse(alloc,R"({"data":[{"index":1,"embedding":[2.5,3]},{"index":0,"embedding":[1,2]}]})"),result), "reverse embedding response");
  require(result != nullptr && result->element_count() == 2, "embedding result count");
  ObIJsonBase *first = nullptr;
  require(OB_SUCCESS == result->get_array_element(0,first), "ordered first vector");
  require(OB_SUCCESS == ObAIFuncJsonUtils::validate_embedding(first,2), "actual embedding validation");
  require(OB_SUCCESS != ObAIFuncJsonUtils::validate_embedding(first,3), "dimension mismatch rejection");
  const char *invalid[] = {
    R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
    R"({"data":[{"index":2,"embedding":[1]}]})",
    R"({"data":[{"index":-1,"embedding":[1]}]})",
    R"({"data":[{"index":0.5,"embedding":[1]}]})",
    R"({"data":[{"index":0,"embedding":[true]}]})",
    R"({"data":[{"index":0,"embedding":[]}]})",
    R"({"data":[]})"};
  for (const char *text:invalid) require(OB_SUCCESS != embed.parse_output(alloc,parse(alloc,text),result), "malformed response rejection");
  ObOllamaUtils::ObOllamaComplete ollama;
  ObString key, model("local-model"), prompt, content("hello");
  ObArray<ObString> headers;
  require(OB_SUCCESS == ollama.get_header(alloc,key,headers), "Ollama empty key");
  require(headers.count() == 1 && headers.at(0) == ObString("Content-Type: application/json"), "Ollama no authorization header");
  ObJsonObject *body = nullptr;
  require(OB_SUCCESS == ollama.get_body(alloc,model,prompt,content,nullptr,body), "Ollama native body");
  require(body && body->get_value("stream") && !body->get_value("stream")->get_boolean(), "Ollama stream false");
  require(OB_SUCCESS != ollama.parse_output(alloc,parse(alloc,R"({"response":""})"),result), "empty completion rejection");
  std::printf("Native real-library checks: %d passed\n",checks);
  return 0;
}
