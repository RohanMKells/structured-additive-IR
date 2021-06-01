// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SAIR_SEQUENCE_H_
#define SAIR_SEQUENCE_H_

#include <map>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/iterator_range.h"
#include "sair_op_interfaces.h"
#include "sair_ops.h"
#include "util.h"

namespace sair {

// A set of ops of OpTy that preserves the insertion order. Practically, this is
// an llvm::SetVector with additional casting to OpTy since llvm::SetVector
// (precisely, llvm::DenseSet inside it) cannot be constructed for op interface
// classes because their constructors need a non-null operation.
template <typename OpTy>
class ConcreteOpSet {
 public:
  ConcreteOpSet() {}

  // Inserts into the set.
  bool insert(OpTy op) { return contents_.insert(op.getOperation()); }
  template <typename Iterator>
  void insert(Iterator first, Iterator last) {
    contents_.insert(first, last);
  }

  // Merges the given set of ops into the this set of ops.
  void merge(const ConcreteOpSet<OpTy> &other) {
    contents_.insert(other.contents_.begin(), other.contents_.end());
  }

  // Returns `true` if the set has no elements.
  bool empty() const { return contents_.empty(); }

  // Returns the number of ops in this set.
  size_t size() const { return contents_.size(); }

  // Returns `true` if the set contains the given element.
  bool contains(OpTy op) const { return contents_.contains(op.getOperation()); }

  // Removes the most recently added unique element from the set and returns it.
  OpTy pop_back_val() { return cast<OpTy>(contents_.pop_back_val()); }

  // Returns the most recently added unique element of the set.
  OpTy back() const { return cast<OpTy>(contents_.back()); };

  // Returns an iterator range over the elements.
  auto Ops() const {
    return llvm::map_range(contents_,
                           [](Operation *op) { return cast<OpTy>(op); });
  }

  // Erases the given element from the set.
  void erase(OpTy op) { contents_.remove(op.getOperation()); }

 private:
  llvm::SetVector<Operation *> contents_;
};

using ComputeOpSet = ConcreteOpSet<ComputeOp>;

// A point in the execution of the program. A point can be:
// - Immediately before or after a Sair operation.
// - Immediately before entering the Sair program.
// - Immediately after exiting the Sair program.
class ProgramPoint {
 public:
  // Constructs a program point that is before or after the whole program.
  ProgramPoint(SairProgramOp program, Direction direction)
      : program_(program), direction_(direction) {}

  // Constructs a program point that is before or after `op`. Saves a reference
  // to `loop_nest`.
  ProgramPoint(ComputeOp op, Direction direction,
               llvm::ArrayRef<mlir::StringAttr> loop_nest);

  // If null, the point is outside of the sair program. If non-null the point is
  // immediately before or after this operation.
  ComputeOp operation() const { return op_; }

  // Indicates if the point is before or after operation() or before or after
  // the Sair program.
  Direction direction() const { return direction_; }

  // Loop nest the point is nested in.
  llvm::ArrayRef<mlir::StringAttr> loop_nest() const { return loop_nest_; }

  // Reduces the number of loops in loop_nest().
  void TrimLoopNest(int num_loops);

  // Number of common loops between two program points.
  int NumCommonLoops(const ProgramPoint &other) const;

 private:
  SairProgramOp program_;
  ComputeOp op_ = nullptr;
  Direction direction_;
  llvm::ArrayRef<mlir::StringAttr> loop_nest_;
};

// An analysis of the relative positions of Sair operations indicated by their
// sequence attributes.
class SequenceAnalysis {
 public:
  // We use a standard multimap because (a) the sequence numbers can be shared
  // and (b) we need a deterministic increasing order that is provided by this
  // map and not provided by hash table-based maps.
  using MapType = std::multimap<int64_t, ComputeOp>;
  using ConstRangeType = llvm::iterator_range<MapType::const_iterator>;

  // Performs the analysis in the given Sair program.
  explicit SequenceAnalysis(SairProgramOp program_op);

  // Creates and returns the analysis for the given Sair program, or `nullopt`
  // if the analysis cannot be performed, e.g., if the program has use-def
  // cycles between compute ops.
  static std::optional<SequenceAnalysis> Create(SairProgramOp program_op,
                                                bool report_errors = false);

  // Returns an iterator range for traversing operations in their relative
  // order. All operations are given a relative order even if they don't have a
  // sequence attribute attached. The sequence number returned in this iteration
  // may differ from that of the sequence attribute if the Sair program hasn't
  // been canonicalized.
  ConstRangeType Ops() const;

  // Assings inferred (contiguous) sequence numbers to operations by setting
  // their "sequence" attributes.
  void AssignInferred() const;

  // Returns an iterator range of all operations sequenced before the given one,
  // in their relative order. All operations are given a relative order even if
  // they don't have a sequence attribute attached. The sequence number returned
  // in this iteration may differ from that of the sequence attribute if the
  // Sair program hasn't been canonicalized.
  ConstRangeType OpsBefore(ComputeOp op) const;

  // Returns true if `first` is known to be sequenced before `second`, false
  // otherwise. Note that this currently relies on the default implicit order of
  // sequenced ops so even the ops that do not need to be sequenced in the
  // relative order may be sequenced. This is likely to change in the future.
  bool IsBefore(ComputeOp first, SairOp second) const;
  template <typename OpTy>
  bool IsBefore(ComputeOp first, OpTy second) const {
    return IsBefore(first, cast<SairOp>(second.getOperation()));
  }

  // Returns true if the program point is sequenced before the given op.
  bool IsBefore(ProgramPoint point, ComputeOp op) const;

  // Returns true if the program point is sequenced after the given op.
  bool IsAfter(ProgramPoint point, ComputeOp op) const;

  // Inserts the given `op` into the analysis, sequencing before or after the
  // `reference` op, depending on `direction`.
  void Insert(ComputeOp op, ComputeOp reference, Direction direction);
  void Insert(ComputeOp op, SairOp reference, Direction direction);

  // Erases the given `op` from the analysis.
  void Erase(ComputeOp op);

  // Returns the Sair operation of the given kind preceding `op` if any; steps
  // over the operations of other kinds.
  ComputeOp PrevOp(ComputeOp op) const {
    if (op == nullptr) return nullptr;
    auto iter = FindSequencedOp(op);
    assert(iter != sequenced_ops_.end() && "op not in sequence analysis");
    if (iter == sequenced_ops_.begin()) return nullptr;
    return (--iter)->second;
  }

  // Returns the Sair operation of the given kind preceding `op` if any; steps
  // over the operations of other kinds.
  ComputeOp NextOp(ComputeOp op) const {
    if (op == nullptr) return nullptr;
    auto iter = FindSequencedOp(op);
    assert(iter != sequenced_ops_.end() && "op not in sequence analysis");
    if (++iter == sequenced_ops_.end()) return nullptr;
    return iter->second;
  }

  // Returns the pair (first, last) of the given ops according to their sequence
  // numbers.
  std::pair<ComputeOp, ComputeOp> GetSpan(llvm::ArrayRef<ComputeOp> ops) const;

  // Finds the first point in the program where it is possible to insert an
  // operation nested in the first `num_loops` of `current_loop_nest`, when
  // starting from `start`.
  InsertionPoint FindInsertionPoint(
      SairOp start, llvm::ArrayRef<mlir::Attribute> current_loop_nest,
      int num_loops, Direction direction = Direction::kBefore) const;

  // An iterator that visits explicitly and implicitly sequenced ops in their
  // sequence order. Implicitly sequenced ops are additionally visited in the
  // order that respects their use-def chains.
  class SairOpIterator {
    friend class SequenceAnalysis;

   public:
    // Increments the iterator.
    SairOpIterator &operator++() {
      // This is essentially a nested iterator: if we reached the last element
      // in implicitly sequenced vector, take the next explicitly sequenced
      // operation.
      if (implicitly_sequenced_next_pos_ == implicitly_sequenced_.size()) {
        ++compute_iterator_;
        RepopulateImplicitlySequenced();
      } else {
        ++implicitly_sequenced_next_pos_;
      }
      return *this;
    }

    // Dereferences the iterator.
    SairOp operator*() const {
      if (implicitly_sequenced_next_pos_ == 0) {
        ComputeOp compute_op = compute_iterator_->second;
        return cast<SairOp>(compute_op.getOperation());
      }
      return implicitly_sequenced_[implicitly_sequenced_next_pos_ - 1];
    }

    // Compares this iterator with `other`.
    bool operator==(const SairOpIterator &other) const {
      assert(&other.sequence_analysis_ == &sequence_analysis_);
      return compute_iterator_ == other.compute_iterator_ &&
             implicitly_sequenced_next_pos_ ==
                 other.implicitly_sequenced_next_pos_;
    }
    bool operator!=(const SairOpIterator &other) const {
      return !(*this == other);
    }

   private:
    // Constructs an iterator pointing to the first explicitly sequenced
    // operation.
    explicit SairOpIterator(const SequenceAnalysis &sequence_analysis)
        : sequence_analysis_(sequence_analysis) {
      compute_iterator_ = sequence_analysis.Ops().begin();
      RepopulateImplicitlySequenced();
    }

    // Token structure to construct end iterators.
    struct End {};

    // Constructs an end iterator.
    SairOpIterator(const SequenceAnalysis &sequence_analysis, End)
        : sequence_analysis_(sequence_analysis) {
      SetEnd();
    }

    // Updates `implicitly_sequenced_` to contain the ordered list of implicitly
    // sequenced operations that can be placed after the current explicitly
    // sequenced operation.
    void RepopulateImplicitlySequenced() {
      if (compute_iterator_ != sequence_analysis_.sequenced_ops_.end()) {
        sequence_analysis_.ImplicitlySequencedOps(compute_iterator_->first,
                                                  implicitly_sequenced_);
      } else {
        implicitly_sequenced_.clear();
      }
      implicitly_sequenced_next_pos_ = 0;
    }

    // Sets the current iterator to be the end iterator.
    void SetEnd() {
      compute_iterator_ = sequence_analysis_.Ops().end();
      implicitly_sequenced_.clear();
      implicitly_sequenced_next_pos_ = 0;
    }

    // Iterator over explicitly sequenced operations.
    MapType::const_iterator compute_iterator_;

    // Ordered list of implicitly sequenced operations to place after the one
    // pointer to be `compute_iterator_`.
    llvm::SmallVector<SairOp> implicitly_sequenced_;

    // The position of the _next_ element to be taken from the
    // `implicitly_sequenced_` list (the current element is this value minus
    // one). When set to zero, the iterator will take the explicitly sequenced
    // element isntead.
    size_t implicitly_sequenced_next_pos_;

    // Back-reference to the parent analysis.
    const SequenceAnalysis &sequence_analysis_;
  };

  // Returns an iterator range covering all explicitly and implicitly sequenced
  // operations.
  llvm::iterator_range<SairOpIterator> AllOps() const {
    return llvm::make_range(SairOpIterator(*this),
                            SairOpIterator(*this, SairOpIterator::End{}));
  }

 private:
  // Default noop constructor. Init must be called separately.
  SequenceAnalysis() = default;

  // Initializes the analysis for the given program op. This may fail if the
  // program contains use-def loops between compute operations (loops are
  // allowed only through the non-compute by operation).
  mlir::LogicalResult Init(SairProgramOp program_op, bool report_errors);

  // Updates `sequenced_ops_` to have sequence numbers for all compute
  // operations in the program, inferring their relative order from the
  // available sequence attribtues and use-def chains. The relative order is
  // preserved but not the absolute sequence numbers. The traversal order is
  // deterministic but otherwise unspecified for operations that do not have
  // "sequence" attribute and belong to different connected components of the
  // use-def dependency graph.
  mlir::LogicalResult ComputeDefaultSequence(SairProgramOp program,
                                             bool report_errors);

  // Returns the sequence number of the given op.
  int64_t ExplicitSequenceNumber(ComputeOp op) const;

  // Returns the sequence number of the last explicitly sequenceable op that
  // (transitively) produces the operands for this implicitly sequenceable op.
  // In other words, the given op should be sequenced between result and
  // result+1.
  int64_t ImplicitSequenceNumber(SairOp op) const;

  // Returns the iterator pointing to the given op in the sequence map.
  MapType::const_iterator FindSequencedOp(ComputeOp op) const;

  // Populates `ops` with non-compute ops that are implicitly sequenced after
  // `sequence_number` and before `sequence_number` + 1.
  void ImplicitlySequencedOps(int64_t sequence_number,
                              llvm::SmallVectorImpl<SairOp> &ops) const;

  MapType sequenced_ops_;
  llvm::SmallVector<SairFbyOp> fby_ops_to_cut_;
};

}  // namespace sair

#endif  // SAIR_SEQUENCE_H_