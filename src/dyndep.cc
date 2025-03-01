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

// 这段代码解决了 Ninja 中动态依赖的问题。
// 传统的 Ninja 构建文件是静态的，所有依赖必须提前写好。
// 但在实际项目中，有些依赖（比如 C++ 的头文件）只有在编译时才能知道。
// DyndepLoader 提供了一种机制，让 Ninja 可以动态加载这些信息，保证构建正确且高效。
#include "dyndep.h"

#include <assert.h>
#include <stdio.h>

#include "debug_flags.h"
#include "disk_interface.h"
#include "dyndep_parser.h"
#include "explanations.h"
#include "graph.h"
#include "state.h"
#include "util.h"

using namespace std;

// 对外的一级接口
bool DyndepLoader::LoadDyndeps(Node* node, std::string* err) const {
  DyndepFile ddf;
  return LoadDyndeps(node, &ddf, err);
}

// 这是真正干活的版本，参数多了一个空的 DyndepFile* ddf，用来存储加载的结果。逻辑如下
bool DyndepLoader::LoadDyndeps(Node* node, DyndepFile* ddf,
                               std::string* err) const {
  // We are loading the dyndep file now so it is no longer pending.
  // 把 node 的 dyndep_pending 状态设为 false，表示这个动态依赖文件已经被处理了
  node->set_dyndep_pending(false);

  // Load the dyndep information from the file.
  // 用 explanations_ 记录一条消息，比如“正在加载文件 xxx”，方便调试
  explanations_.Record(node, "loading dyndep file '%s'", node->path().c_str());

  // 调用 LoadDyndepFile 方法，把文件内容加载到 ddf 里。如果失败，就返回 false，并设置错误消息
  if (!LoadDyndepFile(node, ddf, err))
    return false;

  // Update each edge that specified this node as its dyndep binding.
  // 更新相关的规则（Edge）  
  // 找到所有依赖这个动态依赖文件(node)的规则（通过 node->out_edges()）。
  std::vector<Edge*> const& out_edges = node->out_edges();
  for (Edge* edge : out_edges) {
    // 对于每条规则（Edge），检查它是否真的指定了这个文件作为动态依赖（edge->dyndep_ == node）
    if (edge->dyndep_ != node)
      continue;

    // 在 ddf 里查找这条规则对应的 Dyndeps 数据。如果没找到，报错（比如“规则没在文件中提到”）。
    /// TODO: 要看一下怎么找的
    DyndepFile::iterator ddi = ddf->find(edge);
    if (ddi == ddf->end()) {
      *err = ("'" + edge->outputs_[0]->path() + "' "
              "not mentioned in its dyndep file "
              "'" + node->path() + "'");
      return false;
    }

    // 如果找到了，调用 UpdateEdge 方法，把动态依赖信息（输入、输出等）加到这条规则里
    ddi->second.used_ = true;
    Dyndeps const& dyndeps = ddi->second;
    if (!UpdateEdge(edge, &dyndeps, err)) {
      return false;
    }
  }

// 检查多余信息
// 遍历 ddf 里的所有条目，如果有些规则的动态依赖数据没被用上（used_ == false），就报错。
// used_ 是一个标记，前面代码在更新规则时会把用到的 Dyndeps 的 used_ 设为 true。  
// 如果 used_ 是 false，说明这条规则在动态依赖文件里出现了，但实际构建图里没有用到它。
// 因为这说明文件中提到了一些规则，但这些规则并没有绑定这个动态依赖文件，可能是配置错了。
// 举个例子
// 假设你的 build.ninja 是这样：
// rule compile
//   command = gcc -c $in -o $out
//   dyndep = deps.d
// build main.o: compile main.c
// 然后 deps.d 里写了两条规则的动态依赖：
// main.o 依赖 header.h。
// extra.o 依赖 extra.h。
// 但构建图里只有 main.o 的规则绑定了 deps.d，没有 extra.o 的规则。这时：
// ddf 里会有 { Edge(main.o) -> {used_=true, inputs=[header.h]}, Edge(extra.o) -> {used_=false, inputs=[extra.h]} }。
// 循环检查时，发现 extra.o 的 used_ 是 false，就报错：
// “动态依赖文件 deps.d 里提到了输出 extra.o，但 extra.o 的构建语句没绑定 deps.d。
  // Reject extra outputs in dyndep file.
  for (const auto& dyndep_output : *ddf) {
    if (!dyndep_output.second.used_) {
      Edge* const edge = dyndep_output.first;
      *err = ("dyndep file '" + node->path() + "' mentions output "
              "'" + edge->outputs_[0]->path() + "' whose build statement "
              "does not have a dyndep binding for the file");
      return false;
    }
  }

  return true;
}

bool DyndepLoader::UpdateEdge(Edge* edge, Dyndeps const* dyndeps,
                              std::string* err) const {
  // Add dyndep-discovered bindings to the edge.
  // We know the edge already has its own binding
  // scope because it has a "dyndep" binding.
  // 如果 dyndeps->restat_ 是 true，就在规则的环境变量里加一个 restat=1，告诉 Ninja 在构建后重新检查文件状态
//   解释
// restat=1 是 Ninja 的一个功能开关，意思是“在规则执行完后，重新检查（re-stat）输出文件的修改时间”。为什么要这么做？因为有些构建工具（比如编译器）可能会在生成输出文件时更新它的时间戳，但不一定真的改变了内容。Ninja 默认假设“命令执行了，输出就变了”，但加上 restat 后，它会再确认一下。
// dyndeps->restat_ 的来源：
// 这个值是从动态依赖文件里读出来的。有些工具（比如 gcc 生成的 .d 文件）会告诉 Ninja “我可能会更新文件时间，但内容没变，请检查一下”。
// 为什么要重新检查：  
// 优化构建效率：如果输出文件的时间变了，但内容没变，Ninja 可以避免不必要的后续构建。
// 动态依赖场景：动态依赖文件可能由某些工具生成，这些工具不一定每次都改变输出文件的内容。restat 确保 Ninja 不会因为时间戳变化误以为文件“脏了”。
// 举个例子
// 假设你有规则：
// rule compile
//   command = gcc -c $in -o $out
//   dyndep = deps.d
// build main.o: compile main.c
// gcc 生成了 deps.d，里面说 main.o 依赖 header.h，并且设了 restat=true。
// 你跑了一次构建，main.o 生成，时间戳是 10:00。
// 后来你改了 main.c，时间戳变成 10:01，但没改 header.h。
// Ninja 执行 gcc，但 gcc 发现 main.o 不需要重新编译，只是更新了时间戳到 10:02。
// 如果没有 restat，Ninja 看到时间变了，以为 main.o 更新了，会触发依赖它的构建（比如链接）。
// 有了 restat=1，Ninja 在执行完后会检查 main.o 的实际内容，发现没变，就不会触发后续构建。
// 总结
// 重新检查文件状态是为了让 Ninja 更聪明，避免被“假更新”（只有时间变，内容没变）误导，节省构建时间。
  if (dyndeps->restat_)
    edge->env_->AddBinding("restat", "1");

  // Add the dyndep-discovered outputs to the edge.
  // 添加额外的输出文件
// 把 dyndeps->implicit_outputs_ 里的节点加到 edge->outputs_ 里，并更新计数器 implicit_outs_
  edge->outputs_.insert(edge->outputs_.end(),
                        dyndeps->implicit_outputs_.begin(),
                        dyndeps->implicit_outputs_.end());
  edge->implicit_outs_ += dyndeps->implicit_outputs_.size();

  // 问题 3：一个输出文件只能有一个输出规则吗？
  // 答案
  // 是的，在 Ninja 的设计里，一个输出文件只能由一个规则生成。这是 Ninja 的核心规则之一。
  // 为什么
  // 避免冲突：如果多个规则都能生成同一个文件，Ninja 不知道该用哪个规则的结果，可能导致构建结果不一致。
  // 简化依赖管理：Ninja 的构建图是一个有向无环图（DAG），每个输出节点只能有一个“生产者”（规则），这样依赖关系清晰，容易计算。
  // Add this edge as incoming to each new output.
  for (Node* node : dyndeps->implicit_outputs_) {
    if (node->in_edge()) {
      // This node already has an edge producing it.
      *err = "multiple rules generate " + node->path();
      return false;
    }
    node->set_in_edge(edge);
  }

  // Add the dyndep-discovered inputs to the edge.
  edge->inputs_.insert(edge->inputs_.end() - edge->order_only_deps_,
                       dyndeps->implicit_inputs_.begin(),
                       dyndeps->implicit_inputs_.end());
  edge->implicit_deps_ += dyndeps->implicit_inputs_.size();

  // node 是什么：
  // node 是一个输入文件（比如 header.h），从动态依赖文件里读出来的。
  // AddOutEdge 做什么：
  // 每个 Node（节点）内部维护一个列表 out_edges_，记录哪些规则（Edge）依赖它。AddOutEdge(edge) 就是把当前规则加到这个列表里。
  // 为什么要记录：
  // Ninja 的构建图是双向的：
  // 规则（Edge）知道自己的输入（inputs_）和输出（outputs_）。
  // 节点（Node）也知道自己被哪些规则用作输入（out_edges_）或生成（in_edge_）。
  // 记录这个关系是为了让 Ninja 能正确追踪依赖链。比如，如果 header.h 变了，Ninja 要知道哪些规则（比如生成 main.o 的规则）需要重新运行。
  // Add this edge as outgoing from each new input.
  for (Node* node : dyndeps->implicit_inputs_)
    node->AddOutEdge(edge);

  return true;
}

bool DyndepLoader::LoadDyndepFile(Node* file, DyndepFile* ddf,
                                  std::string* err) const {
  DyndepParser parser(state_, disk_interface_, ddf);
  return parser.Load(file->path(), err);
}
