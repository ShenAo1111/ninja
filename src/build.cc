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

#include "build.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <climits>
#include <functional>
#include <unordered_set>

#if defined(__SVR4) && defined(__sun)
#include <sys/termios.h>
#endif

#include "build_log.h"
#include "clparser.h"
#include "debug_flags.h"
#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "exit_status.h"
#include "explanations.h"
#include "graph.h"
#include "metrics.h"
#include "state.h"
#include "status.h"
#include "util.h"

using namespace std;

namespace {

/// A CommandRunner that doesn't actually run the commands.
struct DryRunCommandRunner : public CommandRunner {
  // Overridden from CommandRunner:
  size_t CanRunMore() const override;
  bool StartCommand(Edge* edge) override;
  bool WaitForCommand(Result* result) override;

 private:
  queue<Edge*> finished_;
};

size_t DryRunCommandRunner::CanRunMore() const {
  return SIZE_MAX;
}

bool DryRunCommandRunner::StartCommand(Edge* edge) {
  finished_.push(edge);
  return true;
}

bool DryRunCommandRunner::WaitForCommand(Result* result) {
   if (finished_.empty())
     return false;

   result->status = ExitSuccess;
   result->edge = finished_.front();
   finished_.pop();
   return true;
}

}  // namespace

// 简单说，Plan 就像工地上的任务表，告诉包工头（Builder）啥时候干啥活。
Plan::Plan(Builder* builder)
  : builder_(builder)
  , command_edges_(0)
  , wanted_edges_(0)
{}

void Plan::Reset() {
  command_edges_ = 0;
  wanted_edges_ = 0;
  ready_.clear();
  want_.clear();
}

bool Plan::AddTarget(const Node* target, string* err) {
  targets_.push_back(target);
  // 这个地方没用动态依赖
  return AddSubTarget(target, NULL, err, NULL);
}

// 这个函数是递归安排目标和它的依赖。它检查目标的生成规则（Edge），
// 决定要不要跑，然后递归处理所有输入（依赖），确保整个依赖链都加到计划里
// 检查规则 → 没规则处理缺文件 → 已好不管 → 标记要跑 → 递归输入
bool Plan::AddSubTarget(const Node* node, const Node* dependent, string* err,
                        set<Edge*>* dyndep_walk) {
// 做什么：找目标的生成规则。  
// in_edge()：返回生成 node 的 Edge（比如 build main.o: compile main.c）。  
// 例子：node = main.o，edge = compile main.c -> main.o。
  Edge* edge = node->in_edge();
  if (!edge) {
     // Leaf node, this can be either a regular input from the manifest
     // (e.g. a source file), or an implicit input from a depfile or dyndep
     // file. In the first case, a dirty flag means the file is missing,
     // and the build should stop. In the second, do not do anything here
     // since there is no producing edge to add to the plan.
     // 很合理
// 做什么：如果没规则，检查是不是问题。  
// node->dirty()：目标“脏了”（缺了或变了）。  
// !node->generated_by_dep_loader()：不是动态依赖加的（普通输入）。  
// 逻辑：如果是普通输入且脏了，说明缺文件，没法建，报错。  
// 例子：node = main.c，没规则且缺了，报错 "main.c missing and no known rule to make it"。

     if (node->dirty() && !node->generated_by_dep_loader()) {
       string referenced;
       if (dependent)
         referenced = ", needed by '" + dependent->path() + "',";
       *err = "'" + node->path() + "'" + referenced +
              " missing and no known rule to make it";
     }
     return false;
  }

  // 做什么：如果输出已经好了，不用干。  
  // outputs_ready()：规则的输出都最新，不需要跑。  
  // 例子：main.o 已经是最新的，返回 false（啥也不干）。
  if (edge->outputs_ready())
    return false;  // Don't need to do anything.

  // If an entry in want_ does not already exist for edge, create an entry which
  // maps to kWantNothing, indicating that we do not want to build this entry itself.
  // edge 加到 want_，默认 kWantNothing。
  pair<map<Edge*, Want>::iterator, bool> want_ins =
    want_.insert(make_pair(edge, kWantNothing));
  Want& want = want_ins.first->second;

  // dyndep_walk：动态依赖处理的标记。  
  // kWantToFinish：已经安排跑了。  
  // 例子：edge 已计划，不重复处理。
  if (dyndep_walk && want == kWantToFinish)
    return false;  // Don't need to do anything with already-scheduled edge.

  // If we do need to build edge and we haven't already marked it as wanted,
  // mark it now.
// 做什么：如果目标脏了且没计划，标记要跑。  
// 逻辑：目标需要建（dirty），还没想跑（kWantNothing），改成 kWantToStart。  
// EdgeWanted(edge)：增加计数，通知状态。  
// 例子：main.o 脏了，标记 edge 要跑。
  if (node->dirty() && want == kWantNothing) {
    want = kWantToStart;
    EdgeWanted(edge);
  }

  if (dyndep_walk)
    dyndep_walk->insert(edge);

    // 做什么：如果规则已处理，跳过后面。  
    // 逻辑：want_ins.second = false 表示 edge 已存在，不用再查输入。  
    // 例子：edge 之前处理过，返回 true
  if (!want_ins.second)
    return true;  // We've already processed the inputs.

  for (vector<Node*>::iterator i = edge->inputs_.begin();
       i != edge->inputs_.end(); ++i) {
    if (!AddSubTarget(*i, node, err, dyndep_walk) && !err->empty())
      return false;
  }

  return true;
}

// 他记下总任务数（wanted_edges_）。  
// 如果不是假活（phony），再记实际命令数（command_edges_），告诉公告板
void Plan::EdgeWanted(const Edge* edge) {
  ++wanted_edges_;
  if (!edge->is_phony()) {
    ++command_edges_;
    if (builder_)
      builder_->status_->EdgeAddedToPlan(edge);
  }
}

Edge* Plan::FindWork() {
  if (ready_.empty())
    return NULL;

  Edge* work = ready_.top();
  ready_.pop();
  return work;
}

void Plan::ScheduleWork(map<Edge*, Want>::iterator want_e) {
// 做什么：如果已安排，啥也不干。  
// kWantToFinish：已经计划跑了。  
// 例子：edge 已安排，跳过。
  if (want_e->second == kWantToFinish) {
    // This edge has already been scheduled.  We can get here again if an edge
    // and one of its dependencies share an order-only input, or if a node
    // duplicates an out edge (see https://github.com/ninja-build/ninja/pull/519).
    // Avoid scheduling the work again.
    return;
  }
  // assert(want_e->second == kWantToStart);  
  // 做什么：确认状态是想跑。  
  // 例子：确保 edge 是新的。
  // want_e->second = kWantToFinish;  
  // 做什么：标记已安排。  
  // 例子：edge 从 kWantToStart 变成 kWantToFinish。
  assert(want_e->second == kWantToStart);
  want_e->second = kWantToFinish;

  // Pool* pool = edge->pool();  
  // 做什么：找规则的资源池。  
  // 例子：pool = kDefaultPool。
  // if (pool->ShouldDelayEdge()) { ... }  
  // 做什么：如果池子满了，先等。  
  // ShouldDelayEdge()：容量不是无限（depth_ != 0）。  
  // DelayEdge：加到等待队列。  
  // RetrieveReadyEdges：放能跑的活到 ready_。  
  // 例子：depth_ = 1，已用 1，edge 等着。
  // else { pool->EdgeScheduled(*edge); ready_.push(edge); }  
  // 做什么：池子够，直接跑。  
  // EdgeScheduled：增加使用量。  
  // ready_：准备队列。  
  // 例子：depth_ = 0，edge 加到 ready_。
  // 合理
  Edge* edge = want_e->first;
  Pool* pool = edge->pool();
  if (pool->ShouldDelayEdge()) {
    pool->DelayEdge(edge);
    pool->RetrieveReadyEdges(&ready_);
  } else {
    pool->EdgeScheduled(*edge);
    ready_.push(edge);
  }
}

bool Plan::EdgeFinished(Edge* edge, EdgeResult result, string* err) {
  map<Edge*, Want>::iterator e = want_.find(edge);
  assert(e != want_.end());
  bool directly_wanted = e->second != kWantNothing;

  // See if this job frees up any delayed jobs.
  if (directly_wanted)
    edge->pool()->EdgeFinished(*edge);
// 做什么：放能跑的等待活。  
// 例子：等待的 edge2 加到 ready_。
  edge->pool()->RetrieveReadyEdges(&ready_);

  // The rest of this function only applies to successful commands.
  // TODO: 为什么命令出错还返回true？因为那就是failed说明，说明调用这个就是为了完成上面的内容而已，下面的是成功的才会
  // 进行操作的内容
  if (result != kEdgeSucceeded)
    return true;

  if (directly_wanted)
    --wanted_edges_;
    // 做什么：删状态，标记输出完成。  
    // 例子：edge 从 want_ 删掉，outputs_ready_ = true。
  want_.erase(e);
  edge->outputs_ready_ = true;

  // Check off any nodes we were waiting for with this edge.
  // 做什么：处理每个输出。  
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    if (!NodeFinished(*o, err))
      return false;
  }
  return true;
}

bool Plan::NodeFinished(Node* node, string* err) {
  // If this node provides dyndep info, load it now.
// 做什么：如果有动态依赖，加载。  
// dyndep_pending()：节点有未处理的动态依赖文件。  
// LoadDyndeps：加载依赖，更新计划。  
// 例子：main.o 有 deps.d，加载它


// 依赖文件生成：动态依赖（比如 deps.d）通常是编译器在跑规则时生成的，只有规则跑完（EdgeFinished 调用 NodeFinished），文件才准备好。
// 状态更新：加载 dyndep 会改变构建图（新输入输出），需要在 node 完成生成后更新，确保图一致。
// 避免提前加载：如果在规则跑之前加载，文件可能还没生成，加载会失败或不完整
  if (node->dyndep_pending()) {
    assert(builder_ && "dyndep requires Plan to have a Builder");
    // Load the now-clean dyndep file.  This will also update the
    // build plan and schedule any new work that is ready.
    return builder_->LoadDyndeps(node, err);
  }

  // See if we we want any edges from this node.
  for (vector<Edge*>::const_iterator oe = node->out_edges().begin();
       oe != node->out_edges().end(); ++oe) {
    map<Edge*, Want>::iterator want_e = want_.find(*oe);
// 做什么：规则不在计划里，跳过。  
// 例子：link 没标记要跑。
    if (want_e == want_.end())
      continue;

// 做什么：检查规则能不能跑。  
// 例子：link 的输入都好了，安排它。
    // See if the edge is now ready.
    if (!EdgeMaybeReady(want_e, err))
      return false;
  }
  return true;
}

bool Plan::EdgeMaybeReady(map<Edge*, Want>::iterator want_e, string* err) {
  Edge* edge = want_e->first;
// 做什么：检查输入都好了没。  
// AllInputsReady()：所有输入（inputs_）都最新。  
// 例子：main.o 的 main.c 好了。
  if (edge->AllInputsReady()) {
    if (want_e->second != kWantNothing) {
      ScheduleWork(want_e);
    } else {
      // We do not need to build this edge, but we might need to build one of
      // its dependents.
      if (!EdgeFinished(edge, kEdgeSucceeded, err))
        return false;
    }
  }
  return true;
}

bool Plan::CleanNode(DependencyScan* scan, Node* node, string* err) {
  node->set_dirty(false);

  for (vector<Edge*>::const_iterator oe = node->out_edges().begin();
       oe != node->out_edges().end(); ++oe) {
    // Don't process edges that we don't actually want.
    map<Edge*, Want>::iterator want_e = want_.find(*oe);
    if (want_e == want_.end() || want_e->second == kWantNothing)
      continue;

    // Don't attempt to clean an edge if it failed to load deps.
    if ((*oe)->deps_missing_)
      continue;

    // If all non-order-only inputs for this edge are now clean,
    // we might have changed the dirty state of the outputs.
    vector<Node*>::iterator
        begin = (*oe)->inputs_.begin(),
        end = (*oe)->inputs_.end() - (*oe)->order_only_deps_;
#if __cplusplus < 201703L
#define MEM_FN mem_fun
#else
#define MEM_FN mem_fn  // mem_fun was removed in C++17.
#endif
    if (find_if(begin, end, MEM_FN(&Node::dirty)) == end) {
      // Recompute most_recent_input.
      Node* most_recent_input = NULL;
      for (vector<Node*>::iterator i = begin; i != end; ++i) {
        if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime())
          most_recent_input = *i;
      }

      // Now, this edge is dirty if any of the outputs are dirty.
      // If the edge isn't dirty, clean the outputs and mark the edge as not
      // wanted.
      bool outputs_dirty = false;
      if (!scan->RecomputeOutputsDirty(*oe, most_recent_input,
                                       &outputs_dirty, err)) {
        return false;
      }
      if (!outputs_dirty) {
        for (vector<Node*>::iterator o = (*oe)->outputs_.begin();
             o != (*oe)->outputs_.end(); ++o) {
          if (!CleanNode(scan, *o, err))
            return false;
        }

        want_e->second = kWantNothing;
        --wanted_edges_;
        if (!(*oe)->is_phony()) {
          --command_edges_;
          if (builder_)
            builder_->status_->EdgeRemovedFromPlan(*oe);
        }
      }
    }
  }
  return true;
}

// 这个函数在加载动态依赖（dyndep）文件后，更新任务清单。它重新检查依赖关系，把新发现的活儿加进来，确保计划跟得上
bool Plan::DyndepsLoaded(DependencyScan* scan, const Node* node,
                         const DyndepFile& ddf, string* err) {
  // Recompute the dirty state of all our direct and indirect dependents now
  // that our dyndep information has been loaded.
  if (!RefreshDyndepDependents(scan, node, err))
    return false;

  // We loaded dyndep information for those out_edges of the dyndep node that
  // specify the node in a dyndep binding, but they may not be in the plan.
  // Starting with those already in the plan, walk newly-reachable portion
  // of the graph through the dyndep-discovered dependencies.

  // Find edges in the the build plan for which we have new dyndep info.
  // 做什么：从动态依赖文件（ddf）里挑出已经在计划里的规则。
  std::vector<DyndepFile::const_iterator> dyndep_roots;
  for (DyndepFile::const_iterator oe = ddf.begin(); oe != ddf.end(); ++oe) {
    Edge* edge = oe->first;

    // If the edge outputs are ready we do not need to consider it here.
    if (edge->outputs_ready())
      continue;

    map<Edge*, Want>::iterator want_e = want_.find(edge);

    // If the edge has not been encountered before then nothing already in the
    // plan depends on it so we do not need to consider the edge yet either.
    if (want_e == want_.end())
      continue;

    // This edge is already in the plan so queue it for the walk.
    dyndep_roots.push_back(oe);
  }

  // Walk dyndep-discovered portion of the graph to add it to the build plan.
  std::set<Edge*> dyndep_walk;
  for (std::vector<DyndepFile::const_iterator>::iterator
       oei = dyndep_roots.begin(); oei != dyndep_roots.end(); ++oei) {
    DyndepFile::const_iterator oe = *oei;
    for (vector<Node*>::const_iterator i = oe->second.implicit_inputs_.begin();
         i != oe->second.implicit_inputs_.end(); ++i) {
      if (!AddSubTarget(*i, oe->first->outputs_[0], err, &dyndep_walk) &&
          !err->empty())
        return false;
    }
  }

  // Add out edges from this node that are in the plan (just as
  // Plan::NodeFinished would have without taking the dyndep code path).
  for (vector<Edge*>::const_iterator oe = node->out_edges().begin();
       oe != node->out_edges().end(); ++oe) {
    map<Edge*, Want>::iterator want_e = want_.find(*oe);
    if (want_e == want_.end())
      continue;
    dyndep_walk.insert(want_e->first);
  }

  // See if any encountered edges are now ready.
  for (set<Edge*>::iterator wi = dyndep_walk.begin();
       wi != dyndep_walk.end(); ++wi) {
    map<Edge*, Want>::iterator want_e = want_.find(*wi);
    if (want_e == want_.end())
      continue;
    if (!EdgeMaybeReady(want_e, err))
      return false;
  }

  return true;
}

// 这个函数在加载动态依赖后，更新所有依赖 node 的目标的状态，标记需要跑的规则
bool Plan::RefreshDyndepDependents(DependencyScan* scan, const Node* node,
                                   string* err) {
  // Collect the transitive closure of dependents and mark their edges
  // as not yet visited by RecomputeDirty.
  set<Node*> dependents;
  UnmarkDependents(node, &dependents);

  // Update the dirty state of all dependents and check if their edges
  // have become wanted.
  for (set<Node*>::iterator i = dependents.begin();
       i != dependents.end(); ++i) {
    Node* n = *i;

    // Check if this dependent node is now dirty.  Also checks for new cycles.
    std::vector<Node*> validation_nodes;
    if (!scan->RecomputeDirty(n, &validation_nodes, err))
      return false;

    // Add any validation nodes found during RecomputeDirty as new top level
    // targets.
    for (std::vector<Node*>::iterator v = validation_nodes.begin();
         v != validation_nodes.end(); ++v) {
      if (Edge* in_edge = (*v)->in_edge()) {
        if (!in_edge->outputs_ready() &&
            !AddTarget(*v, err)) {
          return false;
        }
      }
    }
    if (!n->dirty())
      continue;

    // This edge was encountered before.  However, we may not have wanted to
    // build it if the outputs were not known to be dirty.  With dyndep
    // information an output is now known to be dirty, so we want the edge.
    Edge* edge = n->in_edge();
    assert(edge && !edge->outputs_ready());
    map<Edge*, Want>::iterator want_e = want_.find(edge);
    assert(want_e != want_.end());
    //  如果当前边的需求是“不构建”：
    // 将其状态改为 kWantToStart，表示现在需要构建。
    if (want_e->second == kWantNothing) {
      want_e->second = kWantToStart;
      EdgeWanted(edge);
    }
  }
  return true;
}

void Plan::UnmarkDependents(const Node* node, set<Node*>* dependents) {
  for (vector<Edge*>::const_iterator oe = node->out_edges().begin();
       oe != node->out_edges().end(); ++oe) {
    Edge* edge = *oe;

    map<Edge*, Want>::iterator want_e = want_.find(edge);
    if (want_e == want_.end())
      continue;

    if (edge->mark_ != Edge::VisitNone) {
      edge->mark_ = Edge::VisitNone;
      for (vector<Node*>::iterator o = edge->outputs_.begin();
           o != edge->outputs_.end(); ++o) {
        if (dependents->insert(*o).second)
          UnmarkDependents(*o, dependents);
      }
    }
  }
}
#include <random> // 需要包含这个头文件
namespace {

// Heuristic for edge priority weighting.
// Phony edges are free (0 cost), all other edges are weighted equally.
// 最小的抽象粒度就是在这里改了，根据edge自带的信息
int64_t EdgeWeightHeuristic(Edge *edge) {
  return edge->is_phony() ? 0 : edge->prev_elapsed_time_millis;
}

}  // namespace

void Plan::ComputeCriticalPath() {
  METRIC_RECORD("ComputeCriticalPath");

  // Convenience class to perform a topological sort of all edges
  // reachable from a set of unique targets. Usage is:
  //
  // 1) Create instance.
  //
  // 2) Call VisitTarget() as many times as necessary.
  //    Note that duplicate targets are properly ignored.
  //
  // 3) Call result() to get a sorted list of edges,
  //    where each edge appears _after_ its parents,
  //    i.e. the edges producing its inputs, in the list.
  //
  // 例子：main.c -> main.o -> main.exe，排序成 [compile main.c, link main.o]。
// 做什么：从后往前更新权重。  
// edge_weight：当前规则权重。  
// candidate_weight：当前权重 + 输入规则的初始权重。  
// 逻辑：如果新权重更大，更新输入规则。  
  // 例子：link = 1，compile = 1，link + compile = 2，compile 更新为 2
  struct TopoSort {
    void VisitTarget(const Node* target) {
      Edge* producer = target->in_edge();
      if (producer)
        Visit(producer);
    }

    const std::vector<Edge*>& result() const { return sorted_edges_; }

   private:
    // Implementation note:
    //
    // This is the regular depth-first-search algorithm described
    // at https://en.wikipedia.org/wiki/Topological_sorting, except
    // that:
    //
    // - Edges are appended to the end of the list, for performance
    //   reasons. Hence the order used in result().
    //
    // - Since the graph cannot have any cycles, temporary marks
    //   are not necessary, and a simple set is used to record
    //   which edges have already been visited.
    //
    void Visit(Edge* edge) {
      auto insertion = visited_set_.emplace(edge);
      if (!insertion.second)
        return;

      for (const Node* input : edge->inputs_) {
        Edge* producer = input->in_edge();
        if (producer)
          Visit(producer);
      }
      sorted_edges_.push_back(edge);
    }

    std::unordered_set<Edge*> visited_set_;
    std::vector<Edge*> sorted_edges_;
  };

  TopoSort topo_sort;
  for (const Node* target : targets_) { // 这里就是1个
    topo_sort.VisitTarget(target);
  }

  const auto& sorted_edges = topo_sort.result();

  // First, reset all weights to 1.
  for (Edge* edge : sorted_edges)
    edge->set_critical_path_weight(EdgeWeightHeuristic(edge));

  // Second propagate / increment weights from
  // children to parents. Scan the list
  // in reverse order to do so.
  for (auto reverse_it = sorted_edges.rbegin();
       reverse_it != sorted_edges.rend(); ++reverse_it) {
    Edge* edge = *reverse_it;
    int64_t edge_weight = edge->critical_path_weight();

    for (const Node* input : edge->inputs_) {
      Edge* producer = input->in_edge();
      if (!producer)
        continue;

      int64_t producer_weight = producer->critical_path_weight();
      int64_t candidate_weight = edge_weight + EdgeWeightHeuristic(producer);
      if (candidate_weight > producer_weight)
        producer->set_critical_path_weight(candidate_weight);
    }
  }
}

// 你告诉它要建啥（AddTarget）。  
// 它用 scan_ 检查依赖，列出哪些文件需要弄（加到 plan_ 里）
void Plan::ScheduleInitialEdges() {
  // Add ready edges to queue.
  assert(ready_.empty());
  std::set<Pool*> pools;

  for (std::map<Edge*, Plan::Want>::iterator it = want_.begin(),
           end = want_.end(); it != end; ++it) {
    Edge* edge = it->first;
    Plan::Want want = it->second;
    if (want == kWantToStart && edge->AllInputsReady()) {
      Pool* pool = edge->pool();
      if (pool->ShouldDelayEdge()) {
        pool->DelayEdge(edge);
        pools.insert(pool);
      } else {
        ScheduleWork(it);
      }
    }
  }

  // Call RetrieveReadyEdges only once at the end so higher priority
  // edges are retrieved first, not the ones that happen to be first
  // in the want_ map.
  for (std::set<Pool*>::iterator it=pools.begin(),
           end = pools.end(); it != end; ++it) {
    (*it)->RetrieveReadyEdges(&ready_);
  }
}

void Plan::PrepareQueue() {
  ComputeCriticalPath();
  ScheduleInitialEdges();
}

void Plan::Dump() const {
  printf("pending: %d\n", (int)want_.size());
  for (map<Edge*, Want>::const_iterator e = want_.begin(); e != want_.end(); ++e) {
    if (e->second != kWantNothing)
      printf("want ");
    e->first->Dump();
  }
  printf("ready: %d\n", (int)ready_.size());
}

Builder::Builder(State* state, const BuildConfig& config, BuildLog* build_log,
                 DepsLog* deps_log, DiskInterface* disk_interface,
                 Status* status, int64_t start_time_millis)
    : state_(state), config_(config), plan_(this), status_(status),
      start_time_millis_(start_time_millis), disk_interface_(disk_interface),
      explanations_(g_explaining ? new Explanations() : nullptr),
      scan_(state, build_log, deps_log, disk_interface,
            &config_.depfile_parser_options, explanations_.get()) {
  lock_file_path_ = ".ninja_lock";
  string build_dir = state_->bindings_.LookupVariable("builddir");
  if (!build_dir.empty())
    lock_file_path_ = build_dir + "/" + lock_file_path_;
  status_->SetExplanations(explanations_.get());
}

Builder::~Builder() {
  Cleanup();
  status_->SetExplanations(nullptr);
}

void Builder::Cleanup() {
  if (command_runner_.get()) {
    vector<Edge*> active_edges = command_runner_->GetActiveEdges();
    command_runner_->Abort();

    for (vector<Edge*>::iterator e = active_edges.begin();
         e != active_edges.end(); ++e) {
      string depfile = (*e)->GetUnescapedDepfile();
      for (vector<Node*>::iterator o = (*e)->outputs_.begin();
           o != (*e)->outputs_.end(); ++o) {
        // Only delete this output if it was actually modified.  This is
        // important for things like the generator where we don't want to
        // delete the manifest file if we can avoid it.  But if the rule
        // uses a depfile, always delete.  (Consider the case where we
        // need to rebuild an output because of a modified header file
        // mentioned in a depfile, and the command touches its depfile
        // but is interrupted before it touches its output file.)
        string err;
        TimeStamp new_mtime = disk_interface_->Stat((*o)->path(), &err);
        if (new_mtime == -1)  // Log and ignore Stat() errors.
          status_->Error("%s", err.c_str());
        if (!depfile.empty() || (*o)->mtime() != new_mtime)
          disk_interface_->RemoveFile((*o)->path());
      }
      if (!depfile.empty())
        disk_interface_->RemoveFile(depfile);
    }
  }

  string err;
  if (disk_interface_->Stat(lock_file_path_, &err) > 0)
    disk_interface_->RemoveFile(lock_file_path_);
}

// 想象你在工地上找包工头说：“我要建个东西，叫 main.o。”  
// 包工头先翻翻蓝图（state_），看看有没有这个东西。  
// 如果没有，他说：“没听说过这个！”然后走人。  
// 如果有，他就交给助手（另一个 AddTarget 函数）去安排，安排好了再把东西给你
Node* Builder::AddTarget(const string& name, string* err) {
  Node* node = state_->LookupNode(name);
  if (!node) {
    *err = "unknown target: '" + name + "'";
    return NULL;
  }
  if (!AddTarget(node, err))
    return NULL;
  return node;
}

bool Builder::AddTarget(Node* target, string* err) {
  std::vector<Node*> validation_nodes;
// 做什么：让侦探（scan_）检查 target 是不是“脏了”（需要重建）。  
// scan_ 是啥：一个依赖检查工具，能看文件的修改时间和依赖关系。  
// RecomputeDirty 干啥：  
// 检查 target 的依赖（比如 main.c、header.h）有没有变。  
// 如果变了，target 就“脏了”，需要重建。  
// 顺便把相关的验证目标填到 validation_nodes 里。
// 出错咋办：如果检查失败（比如文件读不了），返回 false，错误写到 err
  if (!scan_.RecomputeDirty(target, &validation_nodes, err))
    return false;

  Edge* in_edge = target->in_edge();
  if (!in_edge || !in_edge->outputs_ready()) {
    if (!plan_.AddTarget(target, err)) {
      return false;
    }
  }

  // Also add any validation nodes found during RecomputeDirty as top level
  // targets.
// 做什么：如果验证目标过时了，也加到任务清单。  
// !outputs_ready()：验证目标需要重建。  
// !plan_.AddTarget(*n, err)：加不进去就失败。  
// 例子：test.o 的依赖变了，加到清单里
  for (std::vector<Node*>::iterator n = validation_nodes.begin();
       n != validation_nodes.end(); ++n) {
    if (Edge* validation_in_edge = (*n)->in_edge()) {
      if (!validation_in_edge->outputs_ready() &&
          !plan_.AddTarget(*n, err)) {
        return false;
      }
    }
  }

  return true;
}

bool Builder::AlreadyUpToDate() const {
  return !plan_.more_to_do();
}

ExitStatus Builder::Build(string* err) {
  assert(!AlreadyUpToDate());

  // 准备任务队列
  profiler.start("Prepare Queue");
  plan_.PrepareQueue();
  profiler.end();

  int pending_commands = 0;
  int failures_allowed = config_.failures_allowed;

  // 设置命令运行器
  profiler.start("Setup Command Runner");
  if (!command_runner_.get()) {
    if (config_.dry_run)
      command_runner_.reset(new DryRunCommandRunner);
    else
      command_runner_.reset(CommandRunner::factory(config_));
  }
  profiler.end();

  // 构建开始
  profiler.start("Build Start");
  status_->BuildStarted();
  profiler.end();

  // 主构建循环
  profiler.start("Build Loop");
  while (plan_.more_to_do()) {
    // 启动命令
    if (failures_allowed) {
      profiler.start("Check Runner Capacity");
      size_t capacity = command_runner_->CanRunMore();
      profiler.end();

      while (capacity > 0) {
        profiler.start("Find Work");
        Edge* edge = plan_.FindWork();
        profiler.end();

        if (!edge)
          break;

        profiler.start("Handle Generator Edge");
        if (edge->GetBindingBool("generator")) {
          scan_.build_log()->Close();
        }
        profiler.end();

        profiler.start("Start Edge");
        if (!StartEdge(edge, err)) {
          Cleanup();
          status_->BuildFinished();
          profiler.end();  // Start Edge
          profiler.end();  // Build Loop
          return ExitFailure;
        }
        profiler.end();  // Start Edge

        if (edge->is_phony()) {
          profiler.start("Finish Phony Edge");
          if (!plan_.EdgeFinished(edge, Plan::kEdgeSucceeded, err)) {
            Cleanup();
            status_->BuildFinished();
            profiler.end();  // Finish Phony Edge
            profiler.end();  // Build Loop
            return ExitFailure;
          }
          profiler.end();  // Finish Phony Edge
        } else {
          ++pending_commands;

          --capacity;

          // Re-evaluate capacity.
          profiler.start("Re-evaluate Capacity");
          size_t current_capacity = command_runner_->CanRunMore();
          if (current_capacity < capacity)
            capacity = current_capacity;
          profiler.end();  // Re-evaluate Capacity
        }
      }

       // We are finished with all work items and have no pending
       // commands. Therefore, break out of the main loop.
      profiler.start("Check Early Exit");
      if (pending_commands == 0 && !plan_.more_to_do()) {
        profiler.end();  // Check Early Exit
        break;  // 提前退出循环
      }
      profiler.end();  // Check Early Exit
    }

    // See if we can reap any finished commands.
    if (pending_commands) {
      profiler.start("Wait For Command");
      CommandRunner::Result result;
      if (!command_runner_->WaitForCommand(&result) ||
          result.status == ExitInterrupted) {
        Cleanup();
        status_->BuildFinished();
        *err = "interrupted by user";
        profiler.end();  // Wait For Command
        profiler.end();  // Build Loop
        return result.status;
      }
      profiler.end();  // Wait For Command

      --pending_commands;

      profiler.start("Finish Command");
      bool command_finished = FinishCommand(&result, err);
      SetFailureCode(result.status);
      if (!command_finished) {
        Cleanup();
        status_->BuildFinished();
        profiler.end();  // Finish Command
        profiler.end();  // Build Loop
        return result.status;
      }
      profiler.end();  // Finish Command

      profiler.start("Handle Command Result");
      if (!result.success()) {
        if (failures_allowed)
          failures_allowed--;
      }
      profiler.end();  // Handle Command Result
    } else {
      // 无进展处理
      profiler.start("Handle No Progress");
      status_->BuildFinished();
      if (failures_allowed == 0) {
        if (config_.failures_allowed > 1)
          *err = "subcommands failed";
        else
          *err = "subcommand failed";
      } else if (failures_allowed < config_.failures_allowed)
        *err = "cannot make progress due to previous errors";
      else
        *err = "stuck [this is a bug]";
      profiler.end();  // Handle No Progress
      profiler.end();  // Build Loop
      return GetExitCode();
    }

    // 主循环继续
  }
  profiler.end();  // Build Loop

  // 构建结束
  profiler.start("Build Finish");
  status_->BuildFinished();
  profiler.end();
  return ExitSuccess;
}

bool Builder::StartEdge(Edge* edge, string* err) {
  METRIC_RECORD("StartEdge");
  profiler.StartEdgeRecord();
  std::cout << "Starting Edge - Rule: " << edge->rule().name() << std::endl;
    
  std::cout << "Inputs:" << std::endl;
  for (std::vector<Node*>::iterator in = edge->inputs_.begin(); 
       in != edge->inputs_.end(); ++in) {
      std::cout << "  " << (*in)->path() << std::endl;
  }

  std::cout << "Outputs:" << std::endl;
  for (std::vector<Node*>::iterator out = edge->outputs_.begin(); 
       out != edge->outputs_.end(); ++out) {
      const std::string& path = (*out)->path();
      std::cout << "  " << path << std::endl;
  }

  // 包工头先看看是不是“假活”（phony），如果是就说“OK”走人
  // phony 是啥：一种不真干活的规则，比如 build all: phony main.o，只是个标记
  if (edge->is_phony())
    return true;

  int64_t start_time_millis = GetTimeMillis() - start_time_millis_;
  // 把这条规则加到“正在干活”的列表。  
// running_edges_：一个表格，记录当前跑的规则和开始时间
  running_edges_.insert(make_pair(edge, start_time_millis));

  status_->BuildEdgeStarted(edge, start_time_millis);

  TimeStamp build_start = config_.dry_run ? 0 : -1;

  // Create directories necessary for outputs and remember the current
  // filesystem mtime to record later
  // 访问文件系统（硬盘）通常比内存操作慢，可能要等一小会儿才能完成。这种等待就叫“阻塞”，因为程序得停下来，不能立刻干别的。
  // XXX: this will block; do we care?
  /// TODO: 这个很耗时间，需要注意
//   做什么：给输出文件建目录。  
// edge->outputs_：这条规则要生成的文件列表（比如 main.o）。  
// disk_interface_->MakeDirs((*o)->path())：创建目录，比如 main.o 在 obj/ 下，就建 obj/。  
// 出错咋办：建不了目录就返回 false。  
// 例子：main.o 在 obj/main.o，确保 obj/ 存在。
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    if (!disk_interface_->MakeDirs((*o)->path()))
      return false;
    if (build_start == -1) {
      disk_interface_->WriteFile(lock_file_path_, "");
      build_start = disk_interface_->Stat(lock_file_path_, err);
      if (build_start == -1)
        build_start = 0;
    }
  }

  // 做什么：把时间记到规则里。  
  // 例子：edge 知道自己从 10:01 开始。
  edge->command_start_time_ = build_start;

  // Create depfile directory if needed.
  // XXX: this may also block; do we care?
// 做什么：处理依赖文件（depfile）。  
// depfile：规则可能生成依赖文件（比如 gcc 的 main.d）。  
// MakeDirs(depfile)：确保 depfile 的目录存在。  
// 出错咋办：建不了就返回 false。  
// 例子：main.d 在 deps/，建好 deps/
  std::string depfile = edge->GetUnescapedDepfile();
  if (!depfile.empty() && !disk_interface_->MakeDirs(depfile))
    return false;

  // Create response file, if needed
  // XXX: this may also block; do we care?
// 做什么：处理响应文件（rspfile）。  
// rspfile：有些命令参数太长，写到文件里（比如 gcc @args.rsp）。  
// content：响应文件的内容，从规则里拿。  
// WriteFile：把内容写到 rspfile。  
// 出错咋办：写不了就返回 false。  
// 例子：写 args.rsp，里面是编译参数。
  string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty()) {
    string content = edge->GetBinding("rspfile_content");
    if (!disk_interface_->WriteFile(rspfile, content))
      return false;
  }

  // start command computing and run it
  if (!command_runner_->StartCommand(edge)) {
    err->assign("command '" + edge->EvaluateCommand() + "' failed.");
    return false;
  }

  return true;
}

bool Builder::FinishCommand(CommandRunner::Result* result, string* err) {
  METRIC_RECORD("FinishCommand");

  Edge* edge = result->edge;
  std::cout << "Finishing Edge - Rule: " << edge->rule().name() << std::endl;
    
  std::cout << "Inputs:" << std::endl;
  for (std::vector<Node*>::iterator in = edge->inputs_.begin(); 
       in != edge->inputs_.end(); ++in) {
      std::cout << "  " << (*in)->path() << std::endl;
  }

  std::cout << "Outputs:" << std::endl;
  for (std::vector<Node*>::iterator out = edge->outputs_.begin(); 
       out != edge->outputs_.end(); ++out) {
      const std::string& path = (*out)->path();
      std::cout << "  " << path << std::endl;
  }
  profiler.FinishEdgeRecord();
  // First try to extract dependencies from the result, if any.
  // This must happen first as it filters the command output (we want
  // to filter /showIncludes output, even on compile failure) and
  // extraction itself can fail, which makes the command fail from a
  // build perspective.
//   做什么：检查命令有没有生成依赖信息（比如头文件列表）。  
// deps_type：规则里定义的依赖类型（比如 "gcc" 或 "msvc"）。  
// deps_prefix：MSVC 编译器用的前缀（比如 Note: including file:）。  
// ExtractDeps：一个函数（下面会讲），从命令输出或文件里提取依赖，存到 deps_nodes。  
// 出错咋办：如果提取失败（比如文件读不了），但命令本身成功，就把错误加到输出里，标记命令失败。  
// 例子：编译 main.c，deps_type = "gcc"，提取出 header.h 放进 deps_nodes。
  vector<Node*> deps_nodes;
  string deps_type = edge->GetBinding("deps");
  const string deps_prefix = edge->GetBinding("msvc_deps_prefix");
  if (!deps_type.empty()) {
    string extract_err;
    if (!ExtractDeps(result, deps_type, deps_prefix, &deps_nodes,
                     &extract_err) &&
        result->success()) {
      if (!result->output.empty())
        result->output.append("\n");
      result->output.append(extract_err);
      result->status = ExitFailure;
    }
  }

  int64_t start_time_millis, end_time_millis;
  RunningEdgeMap::iterator it = running_edges_.find(edge);
  start_time_millis = it->second;
  end_time_millis = GetTimeMillis() - start_time_millis_;
  running_edges_.erase(it);

  status_->BuildEdgeFinished(edge, start_time_millis, end_time_millis,
                             result->status, result->output);

  // The rest of this function only applies to successful commands.
  if (!result->success()) {
    return plan_.EdgeFinished(edge, Plan::kEdgeFailed, err);
  }

  // Restat the edge outputs
  TimeStamp record_mtime = 0;
  if (!config_.dry_run) {
    const bool restat = edge->GetBindingBool("restat");
    const bool generator = edge->GetBindingBool("generator");
    bool node_cleaned = false;
    record_mtime = edge->command_start_time_;

    // restat and generator rules must restat the outputs after the build
    // has finished. if record_mtime == 0, then there was an error while
    // attempting to touch/stat the temp file when the edge started and
    // we should fall back to recording the outputs' current mtime in the
    // log.
    // 决定要不要检查输出文件时间
    if (record_mtime == 0 || restat || generator) {
      for (vector<Node*>::iterator o = edge->outputs_.begin();
           o != edge->outputs_.end(); ++o) {
        TimeStamp new_mtime = disk_interface_->Stat((*o)->path(), err);
        if (new_mtime == -1)
          return false;
        if (new_mtime > record_mtime)
          record_mtime = new_mtime;
        if ((*o)->mtime() == new_mtime && restat) {
          // The rule command did not change the output.  Propagate the clean
          // state through the build graph.
          // Note that this also applies to nonexistent outputs (mtime == 0).
          // 做什么：标记文件为“干净”。  
          // CleanNode：告诉 plan_ 这个文件没变，后续不用重建。  
          // 出错咋办：失败就返回 false。  
          // 例子：main.o 没变，标记干净，优化后续构建。
          if (!plan_.CleanNode(&scan_, *o, err))
            return false;
          node_cleaned = true;
        }
      }
    }
    if (node_cleaned) {
      // 做什么：如果有文件干净，恢复开始时间。  
      // 逻辑：文件没变，就用命令开始前的时间（避免误以为变了）
      record_mtime = edge->command_start_time_;
    }
  }

  if (!plan_.EdgeFinished(edge, Plan::kEdgeSucceeded, err))
    return false;

  // Delete any left over response file.
//   做什么：删掉临时响应文件（如果有）。  
// rspfile：命令用的参数文件。  
// g_keep_rsp：全局选项，是否保留。  
// 例子：删掉 args.rsp。
  string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty() && !g_keep_rsp)
    disk_interface_->RemoveFile(rspfile);

  if (scan_.build_log()) {
    if (!scan_.build_log()->RecordCommand(edge, start_time_millis,
                                          end_time_millis, record_mtime)) {
      *err = string("Error writing to build log: ") + strerror(errno);
      return false;
    }
  }

  if (!deps_type.empty() && !config_.dry_run) {
    assert(!edge->outputs_.empty() && "should have been rejected by parser");
    for (std::vector<Node*>::const_iterator o = edge->outputs_.begin();
         o != edge->outputs_.end(); ++o) {
      TimeStamp deps_mtime = disk_interface_->Stat((*o)->path(), err);
      if (deps_mtime == -1)
        return false;
      if (!scan_.deps_log()->RecordDeps(*o, deps_mtime, deps_nodes)) {
        *err = std::string("Error writing to deps log: ") + strerror(errno);
        return false;
      }
    }
  }
  return true;
}

// 你干完活，包工头说：“你用了哪些砖（依赖），告诉我！”  
// 你用的是 MSVC 工具，他就看你的报告单（输出）。  
// 你用的是 GCC 工具，他就看你的记录本（depfile）。  
// 他把用到的砖记下来（deps_nodes）
bool Builder::ExtractDeps(CommandRunner::Result* result,
                          const string& deps_type,
                          const string& deps_prefix,
                          vector<Node*>* deps_nodes,
                          string* err) {
  // 例子：输出有 Note: including file: header.h，提取出 header.h
  if (deps_type == "msvc") {
    CLParser parser;
    string output;
    if (!parser.Parse(result->output, deps_prefix, &output, err))
      return false;
    result->output = output;
    for (set<string>::iterator i = parser.includes_.begin();
         i != parser.includes_.end(); ++i) {
      // ~0 is assuming that with MSVC-parsed headers, it's ok to always make
      // all backslashes (as some of the slashes will certainly be backslashes
      // anyway). This could be fixed if necessary with some additional
      // complexity in IncludesNormalize::Relativize.
      deps_nodes->push_back(state_->GetNode(*i, ~0u));
    }
  } else if (deps_type == "gcc") {
    string depfile = result->edge->GetUnescapedDepfile();
    if (depfile.empty()) {
      *err = string("edge with deps=gcc but no depfile makes no sense");
      return false;
    }

    // 做什么：读 depfile 内容。  
    // 情况：读到了就继续，没找到就当空，读出错就失败。  
    // 例子：读 main.d，内容是 main.o: main.c header.h。
    // Read depfile content.  Treat a missing depfile as empty.
    string content;
    switch (disk_interface_->ReadFile(depfile, &content, err)) {
    case DiskInterface::Okay:
      break;
    case DiskInterface::NotFound:
      err->clear();
      break;
    case DiskInterface::OtherError:
      return false;
    }
    if (content.empty())
      return true;

    DepfileParser deps(config_.depfile_parser_options);
    if (!deps.Parse(&content, err))
      return false;

    // XXX check depfile matches expected output.
    deps_nodes->reserve(deps.ins_.size());
    for (vector<StringPiece>::iterator i = deps.ins_.begin();
         i != deps.ins_.end(); ++i) {
      uint64_t slash_bits;
      CanonicalizePath(const_cast<char*>(i->str_), &i->len_, &slash_bits);
      deps_nodes->push_back(state_->GetNode(*i, slash_bits));
    }

    // 做什么：如果不保留 depfile，就删掉。  
    // 例子：删 main.d。
    if (!g_keep_depfile) {
      if (disk_interface_->RemoveFile(depfile) < 0) {
        *err = string("deleting depfile: ") + strerror(errno) + string("\n");
        return false;
      }
    }
    // 如果 deps_type 不是 "msvc" 或 "gcc"，报错退出。
  } else {
    Fatal("unknown deps type '%s'", deps_type.c_str());
  }

  return true;
}

// 你在实验室拿到一张新配方表（node）：  
// 助手（scan_）读表（ddf）。  
// 包工头（plan_）根据表更新实验计划
bool Builder::LoadDyndeps(Node* node, string* err) {
  // Load the dyndep information provided by this node.
  DyndepFile ddf;
  if (!scan_.LoadDyndeps(node, &ddf, err))
    return false;

  // Update the build plan to account for dyndep modifications to the graph.
  if (!plan_.DyndepsLoaded(&scan_, node, ddf, err))
    return false;

  return true;
}

void Builder::SetFailureCode(ExitStatus code) {
  // ExitSuccess should not overwrite any error
  if (code != ExitSuccess) {
    exit_code_ = code;
  }
}
