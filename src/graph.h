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

#ifndef NINJA_GRAPH_H_
#define NINJA_GRAPH_H_

#include <algorithm>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "dyndep.h"
#include "eval_env.h"
#include "explanations.h"
#include "timestamp.h"
#include "util.h"

struct BuildLog;
struct DepfileParserOptions;
struct DiskInterface;
struct DepsLog;
struct Edge;
struct Node;
struct Pool;
struct State;

// Node是最边界的数据结构,ninja语法中的input,output,target,default的底层保存都是Node
/// Information about a node in the dependency graph: the file, whether
/// it's dirty, mtime, etc.
// 简单来说，Ninja 中的 Node 就是一个“文件”或“目标”的代表。比如源文件（main.cpp）、编译后的文件（main.o），或者最终程序（myprogram）。
// 它甚至可以是一个虚拟目标，比如“all”或“clean”。
// Ninja 用 Node 来记住每个文件的状态，决定哪些需要重新构建。
struct Node {
  // 名字
// 就是文件的路径，比如 src/main.cpp
  Node(const std::string& path, uint64_t slash_bits)
      : path_(path), slash_bits_(slash_bits) {}

  /// Return false on error.
  // 怎么干：通过 DiskInterface（一个跟文件系统打交道的工具）去看看文件还在不在，更新 mtime_ 和 exists_
  bool Stat(DiskInterface* disk_interface, std::string* err);

  /// If the file doesn't exist, set the mtime_ from its dependencies
  // 对于虚拟目标（phony，比如 all），没有真实文件，就用依赖的时间来代替
  void UpdatePhonyMtime(TimeStamp mtime);

  /// Return false on error.
  bool StatIfNecessary(DiskInterface* disk_interface, std::string* err) {
    if (status_known())
      return true;
    return Stat(disk_interface, err);
  }

  /// Mark as not-yet-stat()ed and not dirty.
  void ResetState() {
    mtime_ = -1;
    exists_ = ExistenceStatusUnknown;
    dirty_ = false;
  }

  /// Mark the Node as already-stat()ed and missing.
  void MarkMissing() {
    if (mtime_ == -1) {
      mtime_ = 0;
    }
    exists_ = ExistenceStatusMissing;
  }

  bool exists() const {
    return exists_ == ExistenceStatusExists;
  }

  bool status_known() const {
    return exists_ != ExistenceStatusUnknown;
  }

  const std::string& path() const { return path_; }
  /// Get |path()| but use slash_bits to convert back to original slash styles.
  std::string PathDecanonicalized() const {
    return PathDecanonicalized(path_, slash_bits_);
  }
  static std::string PathDecanonicalized(const std::string& path,
                                         uint64_t slash_bits);
  uint64_t slash_bits() const { return slash_bits_; }

  TimeStamp mtime() const { return mtime_; }

  bool dirty() const { return dirty_; }
  void set_dirty(bool dirty) { dirty_ = dirty; }
  void MarkDirty() { dirty_ = true; }

  bool dyndep_pending() const { return dyndep_pending_; }
  void set_dyndep_pending(bool pending) { dyndep_pending_ = pending; }

  Edge* in_edge() const { return in_edge_; }
  void set_in_edge(Edge* edge) { in_edge_ = edge; }

  /// Indicates whether this node was generated from a depfile or dyndep file,
  /// instead of being a regular input or output from the Ninja manifest.
  bool generated_by_dep_loader() const { return generated_by_dep_loader_; }

  void set_generated_by_dep_loader(bool value) {
    generated_by_dep_loader_ = value;
  }

  int id() const { return id_; }
  void set_id(int id) { id_ = id; }

  const std::vector<Edge*>& out_edges() const { return out_edges_; }
  const std::vector<Edge*>& validation_out_edges() const { return validation_out_edges_; }
  void AddOutEdge(Edge* edge) { out_edges_.push_back(edge); }
  void AddValidationOutEdge(Edge* edge) { validation_out_edges_.push_back(edge); }

  void Dump(const char* prefix="") const;

private:
// 就是文件的路径，比如 src/main.cpp。
// Ninja 靠这个找到文件。
  std::string path_;

  /// Set bits starting from lowest for backslashes that were normalized to
  /// forward slashes by CanonicalizePath. See |PathDecanonicalized|.
// 是什么：一个数字，记录路径里哪些斜杠是反斜杠（\）变来的。  
// 干什么用：Windows 上路径可能用反斜杠（src\main.cpp），但 Ninja 内部都用正斜杠（src/main.cpp）。
// 这个 slash_bits_ 就像个“备忘录”，记住哪些地方本来是反斜杠，必要时可以还原回去。
  uint64_t slash_bits_ = 0;

  /// Possible values of mtime_:
  ///   -1: file hasn't been examined
  ///   0:  we looked, and file doesn't exist
  ///   >0: actual file's mtime, or the latest mtime of its dependencies if it doesn't exist
  // 修改时间
  // 是什么：文件的修改时间。  
  // 干什么用：记录文件上次改动的时间，Ninja 用它判断文件有没有变。  
  // -1：还没检查过这个文件。  
  // 0：检查了，但文件没了。  
  // 大于 0：文件存在，这是它的真实修改时间；或者文件不存在，这是它依赖的文件里最晚的修改时间。
  TimeStamp mtime_ = -1;

// 文件存不存在
// 检查文件是还在、没了，还是没检查过。
// 这告诉 Ninja 能不能用这个文件
  enum ExistenceStatus {
    /// The file hasn't been examined.
    ExistenceStatusUnknown,
    /// The file doesn't exist. mtime_ will be the latest mtime of its dependencies.
    ExistenceStatusMissing,
    /// The path is an actual file. mtime_ will be the file's mtime.
    ExistenceStatusExists
  };
  ExistenceStatus exists_ = ExistenceStatusUnknown;

  /// Dirty is true when the underlying file is out-of-date.
  /// But note that Edge::outputs_ready_ is also used in judging which
  /// edges to build.
  // 干什么用：如果 true，说明文件过时了，需要重新构建。比如源文件改了，但目标文件还没更新，Ninja 就知道要动手了。
  bool dirty_ = false;

  /// Store whether dyndep information is expected from this node but
  /// has not yet been loaded.
  // 有没有在等动态依赖信息。  
  // 干什么用：有些文件（比如 .d 文件）会告诉 Ninja 额外的依赖，这个标志表示还在等这些信息加载完
  bool dyndep_pending_ = false;

  /// Set to true when this node comes from a depfile, a dyndep file or the
  /// deps log. If it does not have a producing edge, the build should not
  /// abort if it is missing (as for regular source inputs). By default
  /// all nodes have this flag set to true, since the deps and build logs
  /// can be loaded before the manifest.
// 特殊行为：
// 如果一个节点的 generated_by_dep_loader_ 为 true，而且它没有生成它的边（即 in_edge_ 为 NULL，表示没有规则直接生成这个节点），那么在构建过程中，如果这个节点对应的文件缺失，Ninja 不会中止构建。
// 这跟普通的源文件（如 main.c）类似：如果源文件缺失，构建可能会失败，但对于这些“依赖加载器”生成的节点，Ninja 会宽容处理。
// 默认值：所有节点的 generated_by_dep_loader_ 默认都设为 true。这是因为依赖文件（deps）和构建日志（build logs）可能在 manifest 文件加载之前就被解析，Ninja 假设这些节点都可能来自外部依赖。
// 举例：
// 假设 gcc 编译时生成了一个 main.d 文件，里面列出了 main.c 依赖 header.h。Ninja 解析 main.d 时会创建一个表示 header.h 的节点，并将它的 generated_by_dep_loader_ 设为 true。
// 如果 header.h 没有对应的生成规则（in_edge_ 为 NULL），而且构建时发现 header.h 文件缺失，Ninja 不会报错中止，而是继续执行。
// 总结：
// generated_by_dep_loader_ 标记了节点是否来自依赖文件，并影响缺失节点时的构建行为。它的默认值和宽容策略是为了兼容依赖加载的优先性和灵活性。
  bool generated_by_dep_loader_ = true;

  /// The Edge that produces this Node, or NULL when there is no
  /// known edge to produce it.
  Edge* in_edge_ = nullptr;

  /// All Edges that use this Node as an input.
  std::vector<Edge*> out_edges_;

// 是什么：一个列表，存着所有用这个文件做验证的构建规则。  
// 干什么用：有些规则会有验证步骤（比如检查文件是否有效），这里记录这些关系。
/// TODO: 这里不懂，为什么要验证
  /// All Edges that use this Node as a validation.
  std::vector<Edge*> validation_out_edges_;

  /// A dense integer id for the node, assigned and used by DepsLog.
  int id_ = -1;
};

// Edge是最核心的数据结构,会将Node Rule Binding等数据结构组合起来
/// An edge in the dependency graph; links between Nodes using Rules.



// Edge 是什么？
// Edge 是 Ninja 依赖图里的一条“边”。想象一个流程图：
// 有起点（输入文件，比如代码文件 .c）
// 有终点（输出文件，比如可执行文件 .exe）
// 中间有一条线告诉你“怎么从起点走到终点”（比如编译命令）
// 这条“边”就是 Edge。它不仅记录了起点和终点，还记录了“怎么走”的规则（Rule）和一些额外的细节。
struct Edge {
  enum VisitMark {
    VisitNone,
    VisitInStack,
    VisitDone
  };

  Edge() = default;

  /// Return true if all inputs' in-edges are ready.
  bool AllInputsReady() const;

  /// Expand all variables in a command and return it as a string.
  /// If incl_rsp_file is enabled, the string will also contain the
  /// full contents of a response file (if applicable)
  std::string EvaluateCommand(bool incl_rsp_file = false) const;

  /// Returns the shell-escaped value of |key|.
  std::string GetBinding(const std::string& key) const;
  bool GetBindingBool(const std::string& key) const;

  /// Like GetBinding("depfile"), but without shell escaping.
  std::string GetUnescapedDepfile() const;
  /// Like GetBinding("dyndep"), but without shell escaping.
  std::string GetUnescapedDyndep() const;
  /// Like GetBinding("rspfile"), but without shell escaping.
  std::string GetUnescapedRspfile() const;

  void Dump(const char* prefix="") const;

  // critical_path_weight is the priority during build scheduling. The
  // "critical path" between this edge's inputs and any target node is
  // the path which maximises the sum oof weights along that path.
  // NOTE: Defaults to -1 as a marker smaller than any valid weight
  int64_t critical_path_weight() const { return critical_path_weight_; }
  void set_critical_path_weight(int64_t critical_path_weight) {
    critical_path_weight_ = critical_path_weight;
  }

  const Rule* rule_ = nullptr;
  Pool* pool_ = nullptr;
  std::vector<Node*> inputs_;
  std::vector<Node*> outputs_;
  std::vector<Node*> validations_;
  Node* dyndep_ = nullptr;
  BindingEnv* env_ = nullptr;
  VisitMark mark_ = VisitNone;
  size_t id_ = 0;
  int64_t critical_path_weight_ = -1;
  // 使用场景：在决定是否要构建某条边时，Ninja 不仅仅会检查输入文件是否“脏”（比如输入文件被修改了，标记为 dirty_），
  // 还会检查 outputs_ready_ 的值。如果 outputs_ready_ 是 true，即使输入有变化，Ninja 也可能跳过构建，认为输出已经满足要求。
// 举例：假设有一条规则 gcc -o main.o main.c，其中 main.c 是输入，main.o 是输出。
// 如果 main.o 已存在且未过期（即 outputs_ready_ 为 true），Ninja 就不会重新运行这条编译命令
  bool outputs_ready_ = false;
  bool deps_loaded_ = false;
  bool deps_missing_ = false;
  bool generated_by_dep_loader_ = false;
  TimeStamp command_start_time_ = 0;

  const Rule& rule() const { return *rule_; }
  Pool* pool() const { return pool_; }
  int weight() const { return 1; }
  bool outputs_ready() const { return outputs_ready_; }

  //如何区分显隐式,input和output会按照按照 显式 -> 隐式 -> order-only(仅依赖) 的顺序进行push_back()
// 根据当前的值的位置与显隐式的数量做对比就可以知道
  // There are three types of inputs.
  // 1) explicit deps, which show up as $in on the command line;
  //    显式依赖（explicit deps）：直接写在命令里的依赖，比如 $in，通常是主要源文件。
  // 2) implicit deps, which the target depends on implicitly (e.g. C headers),
  //                   and changes in them cause the target to rebuild;
  //    隐式依赖（implicit deps）：没写在命令里，但目标依赖它们，比如头文件（.h）。如果这些文件变了，目标要重新构建。
  // 3) order-only deps, which are needed before the target builds but which
  //                     don't cause the target to rebuild.
  // These are stored in inputs_ in that order, and we keep counts of
  // #2 and #3 when we need to access the various subsets.
  //  怎么区分？用两个数字记录：
//    implicit_deps_：隐式依赖的数量。
//    order_only_deps_：order-only 依赖的数量。
//    方法 is_implicit() 和 is_order_only() 会根据位置判断某个输入属于哪种类型
  int implicit_deps_ = 0;
  int order_only_deps_ = 0;
  bool is_implicit(size_t index) {
    return index >= inputs_.size() - order_only_deps_ - implicit_deps_ &&
        !is_order_only(index);
  }
  bool is_order_only(size_t index) {
    return index >= inputs_.size() - order_only_deps_;
  }

  // There are two types of outputs.
  // 1) explicit outs, which show up as $out on the command line;
  // 2) implicit outs, which the target generates but are not part of $out.
  //    隐式输出（implicit outs）：构建时顺便生成的文件，但不在 $out 里
  // These are stored in outputs_ in that order, and we keep a count of
  // #2 to use when we need to access the various subsets.
  // 方法 is_implicit_out() 判断某个输出是不是隐式的
  int implicit_outs_ = 0;
  bool is_implicit_out(size_t index) const {
    return index >= outputs_.size() - implicit_outs_;
  }

  bool is_phony() const;
  bool use_console() const;
  bool maybe_phonycycle_diagnostic() const;

  // Historical info: how long did this edge take last time,
  // as per .ninja_log, if known? Defaults to -1 if unknown.
  // env_：变量环境，存着一些键值对（比如 CC=gcc），可以在命令里用。
  int64_t prev_elapsed_time_millis = -1;
};

struct EdgeCmp {
  bool operator()(const Edge* a, const Edge* b) const {
    return a->id_ < b->id_;
  }
};

// 把一堆 Edge 按 id_ 从小到大自动排好序，方便管理
typedef std::set<Edge*, EdgeCmp> EdgeSet;

/// ImplicitDepLoader loads implicit dependencies, as referenced via the
/// "depfile" attribute in build files.
// 是什么：一个类，专门加载“隐式依赖”（比如编译器生成的 .d 文件里的依赖）。
// 干什么用：读取依赖文件（depfile），把里面的信息加到 Ninja 的构建图里
struct ImplicitDepLoader {
  ImplicitDepLoader(State* state, DepsLog* deps_log,
                    DiskInterface* disk_interface,
                    DepfileParserOptions const* depfile_parser_options,
                    Explanations* explanations)
      : state_(state), disk_interface_(disk_interface), deps_log_(deps_log),
        depfile_parser_options_(depfile_parser_options),
        explanations_(explanations) {}

  /// Load implicit dependencies for \a edge.
  /// @return false on error (without filling \a err if info is just missing
  //                          or out of date).
  // LoadDeps：给某个 Edge 加载隐式依赖
  bool LoadDeps(Edge* edge, std::string* err);

  DepsLog* deps_log() const {
    return deps_log_;
  }

 protected:
  /// Process loaded implicit dependencies for \a edge and update the graph
  /// @return false on error (without filling \a err if info is just missing)
  virtual bool ProcessDepfileDeps(Edge* edge,
                                  std::vector<StringPiece>* depfile_ins,
                                  std::string* err);

  /// Load implicit dependencies for \a edge from a depfile attribute.
  /// @return false on error (without filling \a err if info is just missing).
  bool LoadDepFile(Edge* edge, const std::string& path, std::string* err);

  /// Load implicit dependencies for \a edge from the DepsLog.
  /// @return false on error (without filling \a err if info is just missing).
  bool LoadDepsFromLog(Edge* edge, std::string* err);

  /// Preallocate \a count spaces in the input array on \a edge, returning
  /// an iterator pointing at the first new space.
  std::vector<Node*>::iterator PreallocateSpace(Edge* edge, int count);

  State* state_;
  DiskInterface* disk_interface_;
  DepsLog* deps_log_;
  DepfileParserOptions const* depfile_parser_options_;
  OptionalExplanations explanations_;
};


/// DependencyScan manages the process of scanning the files in a graph
/// and updating the dirty/outputs_ready state of all the nodes and edges.
// 干什么用：判断哪些节点或边的输出需要重新构建（也就是“脏”了）。
struct DependencyScan {
  DependencyScan(State* state, BuildLog* build_log, DepsLog* deps_log,
                 DiskInterface* disk_interface,
                 DepfileParserOptions const* depfile_parser_options,
                 Explanations* explanations)
      : build_log_(build_log), disk_interface_(disk_interface),
        dep_loader_(state, deps_log, disk_interface, depfile_parser_options,
                    explanations),
        dyndep_loader_(state, disk_interface), explanations_(explanations) {}

  /// Update the |dirty_| state of the given nodes by transitively inspecting
  /// their input edges.
  /// Examine inputs, outputs, and command lines to judge whether an edge
  /// needs to be re-run, and update outputs_ready_ and each outputs' |dirty_|
  /// state accordingly.
  /// Appends any validation nodes found to the nodes parameter.
  /// Returns false on failure.
  bool RecomputeDirty(Node* node, std::vector<Node*>* validation_nodes, std::string* err);

  /// Recompute whether any output of the edge is dirty, if so sets |*dirty|.
  /// Returns false on failure.
  bool RecomputeOutputsDirty(Edge* edge, Node* most_recent_input,
                             bool* dirty, std::string* err);

  BuildLog* build_log() const {
    return build_log_;
  }
  void set_build_log(BuildLog* log) {
    build_log_ = log;
  }

  DepsLog* deps_log() const {
    return dep_loader_.deps_log();
  }

  /// Load a dyndep file from the given node's path and update the
  /// build graph with the new information.  One overload accepts
  /// a caller-owned 'DyndepFile' object in which to store the
  /// information loaded from the dyndep file.
  bool LoadDyndeps(Node* node, std::string* err) const;
  bool LoadDyndeps(Node* node, DyndepFile* ddf, std::string* err) const;

 private:
  bool RecomputeNodeDirty(Node* node, std::vector<Node*>* stack,
                          std::vector<Node*>* validation_nodes, std::string* err);
  bool VerifyDAG(Node* node, std::vector<Node*>* stack, std::string* err);

  /// Recompute whether a given single output should be marked dirty.
  /// Returns true if so.
  bool RecomputeOutputDirty(const Edge* edge, const Node* most_recent_input,
                            const std::string& command, Node* output);

  void RecordExplanation(const Node* node, const char* fmt, ...);

  BuildLog* build_log_;
  DiskInterface* disk_interface_;
  ImplicitDepLoader dep_loader_;
  DyndepLoader dyndep_loader_;
  OptionalExplanations explanations_;
};

// Implements a less comparison for edges by priority, where highest
// priority is defined lexicographically first by largest critical
// time, then lowest ID.
//
// Including ID means that wherever the critical path weights are the
// same, the edges are executed in ascending ID order which was
// historically how all tasks were scheduled.
// EdgePriorityLess：权重（critical_path_weight）小的优先级低；如果权重一样，id_ 大的优先级低。
// EdgePriorityGreater：反过来，权重大的优先级高，id_ 小的优先级高。
struct EdgePriorityLess {
  bool operator()(const Edge* e1, const Edge* e2) const {
    const int64_t cw1 = e1->critical_path_weight();
    const int64_t cw2 = e2->critical_path_weight();
    if (cw1 != cw2) {
      return cw1 < cw2;
    }
    return e1->id_ > e2->id_;
  }
};

// Reverse of EdgePriorityLess, e.g. to sort by highest priority first
struct EdgePriorityGreater {
  bool operator()(const Edge* e1, const Edge* e2) const {
    return EdgePriorityLess()(e2, e1);
  }
};

// A priority queue holding non-owning Edge pointers. top() will
// return the edge with the largest critical path weight, and lowest
// ID if more than one edge has the same critical path weight.
// 按优先级管理 Edge，每次拿出权重最大、id_ 最小的那个
class EdgePriorityQueue:
  public std::priority_queue<Edge*, std::vector<Edge*>, EdgePriorityLess>{
public:
  void clear() {
    c.clear();
  }
};

/// A class used to collect the transitive set of inputs from a given set
/// of starting nodes. Used to implement the `inputs` tool.
///
/// When collecting inputs, the outputs of phony edges are always ignored
/// from the result, but are followed by the dependency walk.
///
/// Usage is:
/// - Create instance.
/// - Call VisitNode() for each root node to collect inputs from.
/// - Call inputs() to retrieve the list of input node pointers.
/// - Call GetInputsAsStrings() to retrieve the list of inputs as a string
/// vector.
///
// 是什么：一个类，用来收集某个目标的所有输入文件。
// 干什么用：从某个节点开始，找到它依赖的所有输入节点。
struct InputsCollector {
  /// Visit a single @arg node during this collection.
  void VisitNode(const Node* node);

  /// Retrieve list of visited input nodes. A dependency always appears
  /// before its dependents in the result, but final order depends on the
  /// order of the VisitNode() calls performed before this.
  const std::vector<const Node*>& inputs() const { return inputs_; }

  /// Same as inputs(), but returns the list of visited nodes as a list of
  /// strings, with optional shell escaping.
  std::vector<std::string> GetInputsAsStrings(bool shell_escape = false) const;

  /// Reset collector state.
  void Reset() {
    inputs_.clear();
    visited_nodes_.clear();
  }

 private:
  std::vector<const Node*> inputs_;
  std::set<const Node*> visited_nodes_;
};

#endif  // NINJA_GRAPH_H_
