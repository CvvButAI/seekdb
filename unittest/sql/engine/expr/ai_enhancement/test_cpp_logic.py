#!/usr/bin/env python3
"""Compile actual production helper bodies against a small API shim.

This checks protocol/rendering algorithms with ASan/UBSan. It is explicitly NOT
an engine build, ABI check, SQL integration test, or substitute for those checks.
"""
from pathlib import Path
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[5]
SOURCE = ROOT / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp'

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    # Functions selected below contain no brace literals in comments/strings,
    # except the renderer; delimit that function by its explicit namespace end.
    if signature.startswith('int render_ai_prompt('):
        return text[start:text.index('\n} // namespace', start)]
    for end in range(opening + 1, len(text)):
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
            if depth == 0:
                return text[start:end+1]
    raise ValueError(signature)

SHIM = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <climits>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
constexpr int OB_SUCCESS=0, OB_INVALID_DATA=-1, OB_INVALID_ARGUMENT=-2,
 OB_SIZE_OVERFLOW=-3, OB_ALLOCATE_MEMORY_FAILED=-4;
#define OB_ISNULL(x) ((x)==nullptr)
#define OB_NOT_NULL(x) ((x)!=nullptr)
#define OB_SUCC(x) ((x)==OB_SUCCESS)
#define OB_FAIL(x) ((ret=(x))!=OB_SUCCESS)
#define INIT_SUCC(x) int x=OB_SUCCESS
class ObString {
 std::shared_ptr<std::string> owner;
public:
 std::string_view value;
 ObString()=default;
 ObString(const char*s):ObString(std::string(s)){}
 ObString(std::string s):owner(std::make_shared<std::string>(std::move(s))),value(*owner){}
 static ObString borrow(std::string_view view){ObString s;s.value=view;return s;}
 int64_t length()const{return value.size();}
 const char*ptr()const{return value.data();}
 bool empty()const{return value.empty();}
};
class ObStringBuffer;
class ObIJsonBase;
class ObIAllocator {
public:
 std::vector<std::unique_ptr<ObIJsonBase>> nodes;
 std::vector<std::unique_ptr<std::string>> buffers;
 template<class T,class...Args>T*make(Args&&...args){
  auto p=std::make_unique<T>(std::forward<Args>(args)...);auto raw=p.get();nodes.push_back(std::move(p));return raw;
 }
};
enum class ObJsonNodeType{J_STRING,J_ARRAY,J_OBJECT,J_INT,J_UINT,J_DOUBLE,J_DECIMAL,J_BOOLEAN};
class ObIJsonBase {
public:
 virtual~ObIJsonBase()=default;
 virtual ObJsonNodeType json_type()const=0;
 virtual uint64_t element_count()const{return 0;}
 virtual int to_double(double&)const{return OB_INVALID_DATA;}
 virtual int64_t get_int()const{return 0;}
 virtual uint64_t get_uint()const{return 0;}
 virtual int get_array_element(uint64_t,ObIJsonBase*&)const{return OB_INVALID_DATA;}
 virtual std::string dump()const=0;
 virtual int print(ObStringBuffer &buffer,int)const;
};
using ObJsonNode=ObIJsonBase;
class ObJsonString:public ObJsonNode{
 ObString val;
public:
 explicit ObJsonString(ObString s):val(std::move(s)){}
 ObJsonNodeType json_type()const override{return ObJsonNodeType::J_STRING;}
 ObString get_str()const{return val;}
 std::string dump()const override{return std::string("\"")+std::string(val.value)+"\"";}
};
class Number:public ObJsonNode{
public:
 double v; ObJsonNodeType kind; uint64_t u;
 Number(double value,ObJsonNodeType type=ObJsonNodeType::J_DOUBLE):v(value),kind(type),u(0){}
 ObJsonNodeType json_type()const override{return kind;}
 int to_double(double&out)const override{out=v;return 0;}
 int64_t get_int()const override{return static_cast<int64_t>(v);}
 uint64_t get_uint()const override{return u;}
 std::string dump()const override{return kind==ObJsonNodeType::J_BOOLEAN?"true":std::to_string(static_cast<int>(v));}
};
class ObJsonArray:public ObJsonNode{
public:
 std::vector<ObJsonNode*> values;
 ObJsonNodeType json_type()const override{return ObJsonNodeType::J_ARRAY;}
 uint64_t element_count()const override{return values.size();}
 ObJsonNode*get_value(uint64_t i)const{return i<values.size()?values[i]:nullptr;}
 int append(ObJsonNode*n){values.push_back(n);return 0;}
 int get_array_element(uint64_t i,ObIJsonBase*&v)const override{v=get_value(i);return v?0:OB_INVALID_DATA;}
 std::string dump()const override{std::string s="[";for(auto*n:values){if(s.size()>1)s+=",";s+=n->dump();}return s+"]";}
};
class ObJsonObject:public ObJsonNode{
public:
 std::map<std::string,ObJsonNode*> values;
 ObJsonNodeType json_type()const override{return ObJsonNodeType::J_OBJECT;}
 uint64_t element_count()const override{return values.size();}
 ObJsonNode*get_value(const char*key)const{auto it=values.find(key);return it==values.end()?nullptr:it->second;}
 int add(const char*key,ObJsonNode*n){values[key]=n;return 0;}
 std::string dump()const override{std::string s="{";for(auto&kv:values){if(s.size()>1)s+=",";s+="\""+kv.first+"\":"+kv.second->dump();}return s+"}";}
};
template<class T>class ObArray{
 std::vector<T> values;
public:
 int push_back(T n){values.push_back(n);return 0;}
 int64_t count()const{return values.size();}
 T&at(size_t i){return values.at(i);}
};
class ObStringBuffer{
 std::unique_ptr<std::string> value=std::make_unique<std::string>();
 ObIAllocator *allocator;
public:
 explicit ObStringBuffer(ObIAllocator*a):allocator(a){}
 int64_t length()const{return value->size();}
 int append(ObString s){*value+=s.value;return 0;}
 int append(const char*p,int64_t n){value->append(p,n);return 0;}
 ObString string()const{return ObString::borrow(*value);}
 int get_result_string(ObString &result){result=ObString::borrow(*value);allocator->buffers.push_back(std::move(value));return 0;}
};
using ObJsonBuffer=ObStringBuffer;
int ObIJsonBase::print(ObStringBuffer &buffer,int)const{return buffer.append(ObString(dump()));}
class ObAIFuncJsonUtils{
public:
 static int finite_number(const ObIJsonBase*,double&);
 static int validate_embedding(ObIJsonBase*,int64_t=0);
 static int get_json_array(ObIAllocator&a,ObJsonArray*&out){out=a.make<ObJsonArray>();return 0;}
 static int print_json_to_str(ObIAllocator&,ObIJsonBase*,ObString&);
};
class ObAIFuncPromptObjectUtils{
public:
 static constexpr const char*prompt_template_key="template";
 static constexpr const char*prompt_args_key="args";
 static bool is_valid_prompt_object(ObJsonObject*);
 static int replace_all_str_args_in_template(ObIAllocator&,ObJsonObject*,ObString&);
};
class ObOpenAIUtils{
public:
 class ObOpenAIEmbed{public:int parse_output(ObIAllocator&,ObJsonObject*,ObIJsonBase*&);};
};
'''

CASES = r'''
int checks = 0;
#define CHECK(condition) do { ++checks; assert(condition); } while (0)
int main(){
 ObIAllocator a;
 auto number=[&](double n){return a.make<Number>(n,ObJsonNodeType::J_INT);};
 auto string=[&](std::string s){return a.make<ObJsonString>(ObString(s));};
 auto array=[&](){return a.make<ObJsonArray>();};
 auto object=[&](){return a.make<ObJsonObject>();};
 auto prompt=[&](std::string text,ObJsonArray*args){auto p=object();p->add("template",string(text));p->add("args",args);return p;};
 auto render=[&](ObJsonObject*p,std::string expected){ObString out;CHECK(0==ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(a,p,out));CHECK(out.value==expected);};
 auto args=array();std::string long_text(100000,'x');args->append(string(long_text));
 render(prompt("{0}/{0}/{0}",args),long_text+"/"+long_text+"/"+long_text);
 render(prompt("中文/{0}/emoji 😀",args),"中文/"+long_text+"/emoji 😀");
 render(prompt("{ordinary}/{}",args),"{ordinary}/{}");
 ObString out;
 CHECK(0!=ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(a,prompt("{1}",args),out));
 CHECK(0!=ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(a,prompt("{99999999999999999999999999999999}",args),out));
 auto inner_args=array();inner_args->append(string("value"));auto outer=array();outer->append(prompt("inner:{0}",inner_args));
 render(prompt("outer:{0}",outer),"outer:inner:value");
 auto json_args=array();auto payload=array();payload->append(number(1));payload->append(a.make<Number>(1,ObJsonNodeType::J_BOOLEAN));
 auto obj=object();obj->add("k",number(2));json_args->append(payload);json_args->append(obj);
 render(prompt("{0}/{1}",json_args),"[1,true]/{\"k\":2}");
 auto brace_args=array();brace_args->append(string("{1}"));render(prompt("{0}",brace_args),"{1}");
 auto cyclic=array();auto cycle=prompt("{0}",cyclic);cyclic->append(cycle);
 CHECK(0!=ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(a,cycle,out));
 auto embedding=array();embedding->append(number(1));embedding->append(a.make<Number>(0.25));
 CHECK(0==ObAIFuncJsonUtils::validate_embedding(embedding,2));
 CHECK(0!=ObAIFuncJsonUtils::validate_embedding(embedding,3));
 CHECK(0!=ObAIFuncJsonUtils::validate_embedding(array()));
 auto invalid=array();invalid->append(string("1"));CHECK(0!=ObAIFuncJsonUtils::validate_embedding(invalid));
 invalid->values[0]=a.make<Number>(1,ObJsonNodeType::J_BOOLEAN);CHECK(0!=ObAIFuncJsonUtils::validate_embedding(invalid));
 invalid->values[0]=a.make<Number>(INFINITY);CHECK(0!=ObAIFuncJsonUtils::validate_embedding(invalid));
 auto data=array();auto item0=object();auto item1=object();item0->add("index",number(0));item1->add("index",number(1));
 auto e0=array();auto e1=array();e0->append(number(10));e1->append(number(20));item0->add("embedding",e0);item1->add("embedding",e1);
 data->append(item1);data->append(item0);auto response=object();response->add("data",data);
 ObOpenAIUtils::ObOpenAIEmbed parser;ObIJsonBase*result=nullptr;
 CHECK(0==parser.parse_output(a,response,result));CHECK(static_cast<ObJsonArray*>(result)->get_value(0)==e0);
 item1->add("index",number(0));CHECK(0!=parser.parse_output(a,response,result));
 item1->add("index",number(-1));CHECK(0!=parser.parse_output(a,response,result));
 auto huge=a.make<Number>(0,ObJsonNodeType::J_UINT);huge->u=UINT64_MAX;item1->add("index",huge);CHECK(0!=parser.parse_output(a,response,result));
 item1->values.erase("index");CHECK(0!=parser.parse_output(a,response,result));
 item1->add("index",number(1));item1->add("embedding",string("wrong"));CHECK(0!=parser.parse_output(a,response,result));
 response->add("data",string("wrong"));CHECK(0!=parser.parse_output(a,response,result));
 std::cout<<checks<<" production helper checks passed (API shim, not engine integration)\n";
}
'''

def main():
    text = SOURCE.read_text(encoding='utf-8')
    signatures = [
        'int ObAIFuncJsonUtils::print_json_to_str(',
        'int ObAIFuncJsonUtils::finite_number(',
        'int ObAIFuncJsonUtils::validate_embedding(',
        'int ObOpenAIUtils::ObOpenAIEmbed::parse_output(',
        'bool ObAIFuncPromptObjectUtils::is_valid_prompt_object(',
        'int render_ai_prompt(',
        'int ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(']
    source = SHIM + '\n\n'.join(function(text, sig) for sig in signatures) + CASES
    with tempfile.TemporaryDirectory(prefix='seekdb-ai-logic-') as directory:
        cpp = Path(directory) / 'logic.cpp'
        executable = Path(directory) / 'logic'
        cpp.write_text(source, encoding="utf-8")
        subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(cpp), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

if __name__ == '__main__':
    main()
