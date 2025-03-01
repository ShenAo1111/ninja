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

#ifndef NINJA_DYNDEP_LOADER_H_
#define NINJA_DYNDEP_LOADER_H_

#include <map>
#include <string>
#include <vector>

#include "explanations.h"

struct DiskInterface;
struct Edge;
struct Node;
struct State;

/// Store dynamically-discovered dependency information for one edge.
// 重点是one
struct Dyndeps {
  Dyndeps() : used_(false), restat_(false) {}
  // used_：标记这条信息有没有被用过
  bool used_;
  // restat_：一个开关，告诉 Ninja 是否需要在构建后重新检查文件的修改时间
  bool restat_;
  // implicit_inputs_：动态发现的额外输入文件（比如头文件）
  std::vector<Node*> implicit_inputs_;
  std::vector<Node*> implicit_outputs_;
};

/// Store data loaded from one dyndep file.  Map from an edge
/// to its dynamically-discovered dependency information.
/// This is a struct rather than a typedef so that we can
/// forward-declare it in other headers.
// 这是一个映射表（map），键是 Edge*（一条构建规则），值是 Dyndeps（这条规则的动态依赖信息）。它用来保存从动态依赖文件里读出来的所有数据。
/// TODO: 去看一下怎么用的
struct DyndepFile: public std::map<Edge*, Dyndeps> {};

/// DyndepLoader loads dynamically discovered dependencies, as
/// referenced via the "dyndep" attribute in build files.
// 这个类是主角，负责加载动态依赖文件并更新构建图。它有几个成员
struct DyndepLoader {
  DyndepLoader(State* state, DiskInterface* disk_interface,
               Explanations* explanations = nullptr)
      : state_(state), disk_interface_(disk_interface),
        explanations_(explanations) {}

  /// Load a dyndep file from the given node's path and update the
  /// build graph with the new information.  One overload accepts
  /// a caller-owned 'DyndepFile' object in which to store the
  /// information loaded from the dyndep file.
  bool LoadDyndeps(Node* node, std::string* err) const;
  bool LoadDyndeps(Node* node, DyndepFile* ddf, std::string* err) const;

 private:
  bool LoadDyndepFile(Node* file, DyndepFile* ddf, std::string* err) const;

  bool UpdateEdge(Edge* edge, Dyndeps const* dyndeps, std::string* err) const;

  State* state_;
  DiskInterface* disk_interface_;
  mutable OptionalExplanations explanations_;
};

#endif  // NINJA_DYNDEP_LOADER_H_
