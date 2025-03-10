// state-reachable.h

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
//
// Copyright 2005-2010 Google, Inc.
// Author: riley@google.com (Michael Riley)
//
// \file
// Class to determine whether a given (final) state can be reached from some
// other given state.

#ifndef FST_LIB_STATE_REACHABLE_H__
#define FST_LIB_STATE_REACHABLE_H__


#include <vector>
using std::vector;

#include <fst/dfs-visit.h>
#include <fst/fst.h>

#include <fst/interval-set.h>

namespace fst {

// Computes the (final) states reachable from a given state in an FST.
// After this visitor has been called, a final state f can be reached
// from a state s iff (*isets)[s].Member(state2index[f]) is true, where
// (*isets[s]) is a set of half-open inteval of final state indices
// and state2index[f] maps from a final state to its index.
//
// If state2index is empty, it is filled-in with suitable indices.
// If it is non-empty, those indices are used; in this case, the
// final states must have out-degree 0.
template <class A, typename I = typename A::StateId>
class IntervalReachVisitor {
 public:
  typedef typename A::StateId StateId;
  typedef typename A::Label Label;
  typedef typename A::Weight Weight;
  typedef typename IntervalSet<I>::Interval Interval;

  IntervalReachVisitor(const Fst<A> &fst,
                       vector< IntervalSet<I> > *isets,
                       vector<I> *state2index)
      : fst_(fst),
        isets_(isets),
        state2index_(state2index),
        index_(state2index->empty() ? 1 : -1) {
    isets_->clear();
  }

  void InitVisit(const Fst<A> &fst) {}

  bool InitState(StateId s, StateId r) {
    while (isets_->size() <= s)
      isets_->push_back(IntervalSet<Label>());
    while (state2index_->size() <= s)
      state2index_->push_back(-1);

    if (fst_.Final(s) != Weight::Zero()) {
      // Create tree interval
      vector<Interval> *intervals = (*isets_)[s].Intervals();
      if (index_ < 0) {  // Use state2index_ map to set index
        if (fst_.NumArcs(s) > 0)
          LOG(FATAL) << "IntervalReachVisitor: state2index map must be empty "
                     << "for this FST";
        I index = (*state2index_)[s];
        if (index < 0)
          LOG(FATAL) << "IntervalReachVisitor: state2index map incomplete";
        intervals->push_back(Interval(index, index + 1));
      } else {           // Use pre-order index
        intervals->push_back(Interval(index_, index_ + 1));
        (*state2index_)[s] = index_++;
      }
    }
    return true;
  }

  bool TreeArc(StateId s, const A &arc) {
    return true;
  }

  bool BackArc(StateId s, const A &arc) {
    LOG(FATAL) << "IntervalReachVisitor: cyclic input";
    return true;
  }

  bool ForwardOrCrossArc(StateId s, const A &arc) {
    // Non-tree interval
    (*isets_)[s].Union((*isets_)[arc.nextstate]);
    return true;
  }

  void FinishState(StateId s, StateId p, const A *arc) {
    if (index_ >= 0 && fst_.Final(s) != Weight::Zero()) {
      vector<Interval> *intervals = (*isets_)[s].Intervals();
      (*intervals)[0].end = index_;      // Update tree interval end
    }
    (*isets_)[s].Normalize();
    if (p != kNoStateId)
      (*isets_)[p].Union((*isets_)[s]);  // Propagate intervals to parent
  }

  void FinishVisit() {}

 private:
  const Fst<A> &fst_;
  vector< IntervalSet<I> > *isets_;
  vector<I> *state2index_;
  I index_;
};


// Tests reachability of final states from a given state. To test for
// reachability from a state s, first do SetState(s). Then a final
// state f can be reached from state s of FST iff Reach(f) is true.
template <class A, typename I = typename A::StateId>
class StateReachable {
 public:
  typedef A Arc;
  typedef I Index;
  typedef typename A::StateId StateId;
  typedef typename A::Label Label;
  typedef typename A::Weight Weight;
  typedef typename IntervalSet<I>::Interval Interval;

  StateReachable(const Fst<A> &fst) {
    IntervalReachVisitor<Arc> reach_visitor(fst, &isets_, &state2index_);
    DfsVisit(fst, &reach_visitor);
  }

  StateReachable(const StateReachable<A> &reachable) {
    LOG(FATAL) << "Copy constructor for state reachable class "
               << "not yet implemented.";
  }

  // Set current state.
  void SetState(StateId s) { s_ = s; }

  // Can reach this label from current state?
  bool Reach(StateId s) {
    if (s >= state2index_.size())
      return false;

    I i =  state2index_[s];
    if (i < 0)
      LOG(FATAL) << "StateReachable: state non-final: " << s;
    return isets_[s_].Member(i);
  }

  // Access to the state-to-index mapping. Unassigned states have index -1.
  vector<I> &State2Index() { return state2index_; }

  // Access to the interval sets. These specify the reachability
  // to the final states as intervals of the final state indices.
  const vector< IntervalSet<I> > &IntervalSets() { return isets_; }

 private:
  StateId s_;                                 // Current state
  vector< IntervalSet<I> > isets_;            // Interval sets per state
  vector<I> state2index_;                     // Finds index for a final state
  void operator=(const StateReachable<A> &);  // Disallow
};

}  // namespace fst

#endif  // FST_LIB_STATE_REACHABLE_H__
