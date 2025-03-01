// Copyright 2011 Google Inc. All Rights Reserved.
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

#include "state.h"

#include <assert.h>
#include <stdio.h>

#include "edit_distance.h"
#include "graph.h"
#include "util.h"

using namespace std;

// 加命令的权重到 current_use_。
void Pool::EdgeScheduled(const Edge& edge) {
  if (depth_ != 0)
    current_use_ += edge.weight();
}

// 减去命令的权重
void Pool::EdgeFinished(const Edge& edge) {
  if (depth_ != 0)
    current_use_ -= edge.weight();
}

void Pool::DelayEdge(Edge* edge) {
  assert(depth_ != 0);
  delayed_.insert(edge);
}

// 完整例子
// 假设：  
// depth_ = 5（容量 5）。  
// current_use_ = 3（用了 3）。  
// delayed_ = [edge1(weight=1), edge2(weight=2), edge3(weight=3)]。
// 执行：  
// it = edge1，3 + 1 = 4 < 5，放 edge1，current_use_ = 4，it 移到 edge2。  
// it = edge2，4 + 2 = 6 > 5，超了，break。  
// erase(begin(), it)，删掉 edge1，剩下 [edge2, edge3]。
// 结果：  
// ready_queue 里有 edge1。  
// current_use_ = 4。  
// delayed_ = [edge2, edge3]。
// 逻辑理解
// 核心：每次挑一个命令，看加进去会不会超容量，能就放出去，不能就停。  
// 比喻：你有 5 个工位，已经用了 3 个：  
// 来了个小活（占 1 个），4 < 5，能干，放出去。  
// 又来了个中活（占 2 个），6 > 5，超了，等等再说。  
// 把干了的活从等待名单划掉。
void Pool::RetrieveReadyEdges(EdgePriorityQueue* ready_queue) {
  DelayedEdges::iterator it = delayed_.begin();
  while (it != delayed_.end()) {
    Edge* edge = *it;
    if (current_use_ + edge->weight() > depth_)
      break;
    ready_queue->push(edge);
    EdgeScheduled(*edge);
    ++it;
  }
  delayed_.erase(delayed_.begin(), it);
}

void Pool::Dump() const {
  printf("%s (%d/%d) ->\n", name_.c_str(), current_use_, depth_);
  for (DelayedEdges::const_iterator it = delayed_.begin();
       it != delayed_.end(); ++it)
  {
    printf("\t");
    (*it)->Dump();
  }
}

Pool State::kDefaultPool("", 0);
Pool State::kConsolePool("console", 1);

// 加一个假规则（phony）到 bindings_，用来标记不用真干活的目标。  
// 加两个默认池子：kDefaultPool（无限容量）、kConsolePool（容量 1）
State::State() {
  bindings_.AddRule(Rule::Phony());
  AddPool(&kDefaultPool);
  AddPool(&kConsolePool);
}

void State::AddPool(Pool* pool) {
  assert(LookupPool(pool->name()) == NULL);
  pools_[pool->name()] = pool;
}

Pool* State::LookupPool(const string& pool_name) {
  map<string, Pool*>::iterator i = pools_.find(pool_name);
  if (i == pools_.end())
    return NULL;
  return i->second;
}

// 创建 Edge（比如 build main.o: compile main.c）。  
// 设置规则（rule_）、默认池子（kDefaultPool）、变量环境（bindings_）。  
// 给个编号（id_），加到 edges_。
Edge* State::AddEdge(const Rule* rule) {
  Edge* edge = new Edge();
  edge->rule_ = rule;
  edge->pool_ = &State::kDefaultPool;
  edge->env_ = &bindings_;
  edge->id_ = edges_.size();
  edges_.push_back(edge);
  return edge;
}

// 先查 paths_ 有没有这个路径。  
// 有就返回，没就新建一个 Node，加到 paths_。  
// slash_bits 是路径斜杠的标记（区分正反斜杠）。
Node* State::GetNode(StringPiece path, uint64_t slash_bits) {
  Node* node = LookupNode(path);
  if (node)
    return node;
  node = new Node(path.AsString(), slash_bits);
  paths_[node->path()] = node; // 在这个地方加的paths
  return node;
}

Node* State::LookupNode(StringPiece path) const {
  Paths::const_iterator i = paths_.find(path);
  if (i != paths_.end())
    return i->second;
  return NULL;
}

Node* State::SpellcheckNode(const string& path) {
  const bool kAllowReplacements = true;
  const int kMaxValidEditDistance = 3;

  int min_distance = kMaxValidEditDistance + 1;
  Node* result = NULL;
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i) {
    int distance = EditDistance(
        i->first, path, kAllowReplacements, kMaxValidEditDistance);
    if (distance < min_distance && i->second) {
      min_distance = distance;
      result = i->second;
    }
  }
  return result;
}

// 找或创建 node（比如 main.c）。  
// 标记不是动态依赖生成的。  
// 加到 edge->inputs_，告诉 node 它被这条规则用。
void State::AddIn(Edge* edge, StringPiece path, uint64_t slash_bits) {
  Node* node = GetNode(path, slash_bits);
  node->set_generated_by_dep_loader(false);
  edge->inputs_.push_back(node);
  node->AddOutEdge(edge);
}

// 找或创建 node（比如 main.o）。  
// 检查有没有别的规则生成它，有就报错（Ninja 不允许一个文件多个生成者）。  
// 加到 edge->outputs_，告诉 node 谁生成它。
bool State::AddOut(Edge* edge, StringPiece path, uint64_t slash_bits,
                   std::string* err) {
  Node* node = GetNode(path, slash_bits);
  if (Edge* other = node->in_edge()) {
    if (other == edge) {
      *err = path.AsString() + " is defined as an output multiple times";
    } else {
      *err = "multiple rules generate " + path.AsString();
    }
    return false;
  }
  edge->outputs_.push_back(node);
  node->set_in_edge(edge);
  node->set_generated_by_dep_loader(false);
  return true;
}

void State::AddValidation(Edge* edge, StringPiece path, uint64_t slash_bits) {
  Node* node = GetNode(path, slash_bits);
  edge->validations_.push_back(node);
  node->AddValidationOutEdge(edge);
  node->set_generated_by_dep_loader(false);
}

bool State::AddDefault(StringPiece path, string* err) {
  Node* node = LookupNode(path);
  if (!node) {
    *err = "unknown target '" + path.AsString() + "'";
    return false;
  }
  defaults_.push_back(node);
  return true;
}

// 做什么：找“根节点”（没被别的规则依赖的文件）。  
// 细节：  
// 遍历所有规则的输出，找没有后续依赖的（out_edges().empty()）。  
// 如果有规则但没根节点，报错。
// 例子：main.o 没人依赖，它就是根节点。
vector<Node*> State::RootNodes(string* err) const {
  vector<Node*> root_nodes;
  // Search for nodes with no output.
  for (vector<Edge*>::const_iterator e = edges_.begin();
       e != edges_.end(); ++e) {
    for (vector<Node*>::const_iterator out = (*e)->outputs_.begin();
         out != (*e)->outputs_.end(); ++out) {
      if ((*out)->out_edges().empty())
        root_nodes.push_back(*out);
    }
  }

  if (!edges_.empty() && root_nodes.empty())
    *err = "could not determine root nodes of build graph";

  return root_nodes;
}

vector<Node*> State::DefaultNodes(string* err) const {
  return defaults_.empty() ? RootNodes(err) : defaults_;
}

void State::Reset() {
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i)
    i->second->ResetState();
  for (vector<Edge*>::iterator e = edges_.begin(); e != edges_.end(); ++e) {
    (*e)->outputs_ready_ = false;
    (*e)->deps_loaded_ = false;
    (*e)->mark_ = Edge::VisitNone;
  }
}

void State::Dump() {
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i) {
    Node* node = i->second;
    printf("%s %s [id:%d]\n",
           node->path().c_str(),
           node->status_known() ? (node->dirty() ? "dirty" : "clean")
                                : "unknown",
           node->id());
  }
  if (!pools_.empty()) {
    printf("resource_pools:\n");
    for (map<string, Pool*>::const_iterator it = pools_.begin();
         it != pools_.end(); ++it)
    {
      if (!it->second->name().empty()) {
        it->second->Dump();
      }
    }
  }
}
