// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dyndep_parser.h"

#include <vector>

#include "dyndep.h"
#include "graph.h"
#include "state.h"
#include "util.h"
#include "version.h"

using namespace std;

DyndepParser::DyndepParser(State* state, FileReader* file_reader,
                           DyndepFile* dyndep_file)
    : Parser(state, file_reader)
    , dyndep_file_(dyndep_file) {
}

bool DyndepParser::Parse(const string& filename, const string& input,
                         string* err) {
  lexer_.Start(filename, input);

  // Require a supported ninja_dyndep_version value immediately so
  // we can exit before encountering any syntactic surprises.
  bool haveDyndepVersion = false;

  for (;;) {
    Lexer::Token token = lexer_.ReadToken();
    switch (token) {
    case Lexer::BUILD: {
      if (!haveDyndepVersion)
        return lexer_.Error("expected 'ninja_dyndep_version = ...'", err);
      if (!ParseEdge(err))
        return false;
      break;
    }
    case Lexer::IDENT: {
      lexer_.UnreadToken();
      if (haveDyndepVersion)
        return lexer_.Error(string("unexpected ") + Lexer::TokenName(token),
                            err);
      if (!ParseDyndepVersion(err))
        return false;
      haveDyndepVersion = true;
      break;
    }
    case Lexer::ERROR:
      return lexer_.Error(lexer_.DescribeLastError(), err);
    case Lexer::TEOF:
      if (!haveDyndepVersion)
        return lexer_.Error("expected 'ninja_dyndep_version = ...'", err);
      return true;
    case Lexer::NEWLINE:
      break;
    default:
      return lexer_.Error(string("unexpected ") + Lexer::TokenName(token),
                          err);
    }
  }
  return false;  // not reached
}

bool DyndepParser::ParseDyndepVersion(string* err) {
  string name;
  EvalString let_value;
  if (!ParseLet(&name, &let_value, err))
    return false;
  if (name != "ninja_dyndep_version") {
    return lexer_.Error("expected 'ninja_dyndep_version = ...'", err);
  }
  string version = let_value.Evaluate(&env_);
  int major, minor;
  ParseVersion(version, &major, &minor);
  if (major != 1 || minor != 0) {
    return lexer_.Error(
      string("unsupported 'ninja_dyndep_version = ") + version + "'", err);
  }
  return true;
}
// ParseLet 方法：解析变量赋值
// cpp
// bool DyndepParser::ParseLet(string* key, EvalString* value, string* err)
// 这个方法解析形如 key = value 的语句，比如 restat = 1。
// 读变量名
// lexer_.ReadIdent(key)：读取标识符（比如 restat），存到 key。
// 检查等号
// ExpectToken(Lexer::EQUALS, err)：期待一个 =，没有就报错。
// 读值
// lexer_.ReadVarValue(value, err)：读取等号后面的值（可能包含变量，比如 $var），存到 value。
// 返回结果
// 成功返回 true，失败返回 false。

bool DyndepParser::ParseLet(string* key, EvalString* value, string* err) {
  if (!lexer_.ReadIdent(key))
    return lexer_.Error("expected variable name", err);
  return (ExpectToken(Lexer::EQUALS, err) && lexer_.ReadVarValue(value, err));
}

// 这是解析动态依赖文件里**最重要**的部分，处理形如 build 输出: dyndep | 隐式输出 || 隐式输入 的语句
// 假设输入是：
// build main.o: dyndep | extra.o || header.h
//   restat = 1



// 对于上面的例子，dyndep_file_ 会存：
// 键：生成 main.o 的规则（Edge*）。
// 值：Dyndeps：
// implicit_outputs_ = [extra.o]
// implicit_inputs_ = [header.h]
// restat_ = true
bool DyndepParser::ParseEdge(string* err) {
  // Parse one explicit output.  We expect it to already have an edge.
  // We will record its dynamically-discovered dependency information.
  Dyndeps* dyndeps = NULL;
  {
    EvalString out0;
    if (!lexer_.ReadPath(&out0, err))
      return false;
    if (out0.empty())
      return lexer_.Error("expected path", err);

    string path = out0.Evaluate(&env_);
    if (path.empty())
      return lexer_.Error("empty path", err);
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node* node = state_->LookupNode(path);
    if (!node || !node->in_edge())
      return lexer_.Error("no build statement exists for '" + path + "'", err);
    Edge* edge = node->in_edge();
    std::pair<DyndepFile::iterator, bool> res =
      dyndep_file_->insert(DyndepFile::value_type(edge, Dyndeps()));
    if (!res.second)
      return lexer_.Error("multiple statements for '" + path + "'", err);
    dyndeps = &res.first->second;
  }

  // Disallow explicit outputs.
  // 只允许一个显式输出：动态依赖文件是对已有规则的补充，不是定义新规则
  {
    EvalString out;
    if (!lexer_.ReadPath(&out, err))
      return false;
    if (!out.empty())
      return lexer_.Error("explicit outputs not supported", err);
  }

  // Parse implicit outputs, if any.
  vector<EvalString> outs;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    for (;;) {
      EvalString out;
      if (!lexer_.ReadPath(&out, err))
        return err;
      if (out.empty())
        break;
      outs.push_back(out);
    }
  }

  if (!ExpectToken(Lexer::COLON, err))
    return false;

  string rule_name;
  if (!lexer_.ReadIdent(&rule_name) || rule_name != "dyndep")
    return lexer_.Error("expected build command name 'dyndep'", err);

  // Disallow explicit inputs.
  {
    EvalString in;
    if (!lexer_.ReadPath(&in, err))
      return false;
    if (!in.empty())
      return lexer_.Error("explicit inputs not supported", err);
  }

  // Parse implicit inputs, if any.
  vector<EvalString> ins;
  if (lexer_.PeekToken(Lexer::PIPE)) {
    for (;;) {
      EvalString in;
      if (!lexer_.ReadPath(&in, err))
        return err;
      if (in.empty())
        break;
      ins.push_back(in);
    }
  }

  // Disallow order-only inputs.
  if (lexer_.PeekToken(Lexer::PIPE2))
    return lexer_.Error("order-only inputs not supported", err);

  if (!ExpectToken(Lexer::NEWLINE, err))
    return false;

  if (lexer_.PeekToken(Lexer::INDENT)) {
    string key;
    EvalString val;
    if (!ParseLet(&key, &val, err))
      return false;
    if (key != "restat")
      return lexer_.Error("binding is not 'restat'", err);
    string value = val.Evaluate(&env_);
    dyndeps->restat_ = !value.empty();
  }

  dyndeps->implicit_inputs_.reserve(ins.size());
  for (const EvalString& in : ins) {
    string path = in.Evaluate(&env_);
    if (path.empty())
      return lexer_.Error("empty path", err);
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node* n = state_->GetNode(path, slash_bits);
    dyndeps->implicit_inputs_.push_back(n);
  }

  dyndeps->implicit_outputs_.reserve(outs.size());
  for (const EvalString& out : outs) {
    string path = out.Evaluate(&env_);
    if (path.empty())
      return lexer_.Error("empty path", err);
    uint64_t slash_bits;
    CanonicalizePath(&path, &slash_bits);
    Node* n = state_->GetNode(path, slash_bits);
    dyndeps->implicit_outputs_.push_back(n);
  }

  return true;
}
