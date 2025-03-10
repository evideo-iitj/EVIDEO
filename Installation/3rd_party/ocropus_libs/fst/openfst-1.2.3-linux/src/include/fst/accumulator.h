// accumulator.h

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
// Classes to accumulate arc weights. Useful for weight lookahead.

#ifndef FST_LIB_ACCUMULATOR_H__
#define FST_LIB_ACCUMULATOR_H__

#include <algorithm>
#include <tr1/unordered_map>
using std::tr1::unordered_map;
#include <vector>
using std::vector;

#include <fst/arcfilter.h>
#include <fst/arcsort.h>
#include <fst/dfs-visit.h>
#include <fst/expanded-fst.h>
#include <fst/replace.h>

namespace fst {

// This class accumulates arc weights using the semiring Plus().
template <class A>
class DefaultAccumulator {
 public:
  typedef A Arc;
  typedef typename A::StateId StateId;
  typedef typename A::Weight Weight;

  DefaultAccumulator() {}

  DefaultAccumulator(const DefaultAccumulator<A> &acc) {}

  void Init(const Fst<A>& fst, bool copy = false) {}

  void SetState(StateId) {}

  Weight Sum(Weight w, Weight v) {
    return Plus(w, v);
  }

  template <class ArcIterator>
  Weight Sum(Weight w, ArcIterator *aiter, ssize_t begin,
             ssize_t end) {
    Weight sum = w;
    aiter->Seek(begin);
    for (ssize_t pos = begin; pos < end; aiter->Next(), ++pos)
      sum = Plus(sum, aiter->Value().weight);
    return sum;
  }

 private:
  void operator=(const DefaultAccumulator<A> &);   // Disallow
};


// This class accumulates arc weights using the log semiring Plus()
// assuming an arc weight that has a member 'float Value()' and a float
// constructor.
template <class A>
class LogAccumulator {
 public:
  typedef A Arc;
  typedef typename A::StateId StateId;
  typedef typename A::Weight Weight;

  LogAccumulator() {}

  LogAccumulator(const LogAccumulator<A> &acc) {}

  void Init(const Fst<A>& fst, bool copy = false) {}

  void SetState(StateId) {}

  Weight Sum(Weight w, Weight v) {
    return LogPlus(w, v);
  }

  template <class ArcIterator>
  Weight Sum(Weight w, ArcIterator *aiter, ssize_t begin,
             ssize_t end) {
    Weight sum = w;
    aiter->Seek(begin);
    for (ssize_t pos = begin; pos < end; aiter->Next(), ++pos)
      sum = LogPlus(sum, aiter->Value().weight);
    return sum;
  }

 private:
  double LogPosExp(double x) { return log(1.0F + exp(-x)); }

  Weight LogPlus(Weight w, Weight v) {
    float f1 = w.Value();
    float f2 = v.Value();
    if (f1 > f2)
      return Weight(f2 - LogPosExp(f1 - f2));
    else
      return Weight(f1 - LogPosExp(f2 - f1));
  }

  void operator=(const LogAccumulator<A> &);   // Disallow
};

// Stores shareable data for fast log accumulator copies.
class FastLogAccumulatorData {
 public:
  FastLogAccumulatorData() {}

  vector<double> *Weights() { return &weights_; }
  vector<ssize_t> *WeightPositions() { return &weight_positions_; }
  double *WeightEnd() { return &(weights_[weights_.size() - 1]); };
  int RefCount() const { return ref_count_.count(); }
  int IncrRefCount() { return ref_count_.Incr(); }
  int DecrRefCount() { return ref_count_.Decr(); }

 private:
  // Cummulative weight per state for all states s.t. # of arcs >
  // arc_limit_ with arcs in order. Special first element per state
  // being LogWeight::Zero();
  vector<double> weights_;
  // Maps from state to corresponding beginning weight position in
  // weights_. Position -1 means no pre-computed weights for that
  // state.
  vector<ssize_t> weight_positions_;
  RefCounter ref_count_;                  // Reference count.

  DISALLOW_COPY_AND_ASSIGN(FastLogAccumulatorData);
};


// This class accumulates arc weights using the log semiring Plus()
// assuming an arc weight that has a member 'float Value()' and a float
// constructor. The member function Init(fst) has to be called to
// setup pre-computed weight information.
template <class A>
class FastLogAccumulator {
 public:
  typedef A Arc;
  typedef typename A::StateId StateId;
  typedef typename A::Weight Weight;

  explicit FastLogAccumulator(ssize_t arc_limit = 20, ssize_t arc_period = 10)
      : arc_limit_(arc_limit),
        arc_period_(arc_period),
        data_(new FastLogAccumulatorData()) {
  }

  FastLogAccumulator(const FastLogAccumulator<A> &acc)
      : arc_limit_(acc.arc_limit_),
        arc_period_(acc.arc_period_),
        data_(acc.data_) {
    data_->IncrRefCount();
  }

  ~FastLogAccumulator() {
    if (!data_->DecrRefCount())
      delete data_;
  }

  void SetState(StateId s) {
    vector<double> &weights = *data_->Weights();
    vector<ssize_t> &weight_positions = *data_->WeightPositions();

    CHECK(weight_positions.size() > s);

    ssize_t pos = weight_positions[s];
    if (pos >= 0)
      state_weights_ = &(weights[pos]);
    else
      state_weights_ = 0;
  }

  Weight Sum(Weight w, Weight v) {
    return LogPlus(w, v);
  }

  template <class ArcIterator>
  Weight Sum(Weight w, ArcIterator *aiter, ssize_t begin,
             ssize_t end) {
    Weight sum = w;
    // Finds begin and end of pre-stored weights
    ssize_t index_begin = -1, index_end = -1;
    ssize_t stored_begin = end, stored_end = end;
    if (state_weights_ != 0) {
      index_begin = begin > 0 ? (begin - 1)/ arc_period_ + 1 : 0;
      index_end = end / arc_period_;
      stored_begin = index_begin * arc_period_;
      stored_end = index_end * arc_period_;
    }
    // Computes sum before pre-stored weights
    if (begin < stored_begin) {
      ssize_t pos_end = min(stored_begin, end);
      aiter->Seek(begin);
      for (ssize_t pos = begin; pos < pos_end; aiter->Next(), ++pos)
        sum = LogPlus(sum, aiter->Value().weight);
    }
    // Computes sum between pre-stored weights
    if (stored_begin < stored_end) {
      sum = LogPlus(sum, LogMinus(state_weights_[index_end],
                                  state_weights_[index_begin]));
    }
    // Computes sum after pre-stored weights
    if (stored_end < end) {
      ssize_t pos_start = max(stored_begin, stored_end);
      aiter->Seek(pos_start);
      for (ssize_t pos = pos_start; pos < end; aiter->Next(), ++pos)
        sum = LogPlus(sum, aiter->Value().weight);
    }
    return sum;
  }

  template <class F>
  void Init(const F &fst, bool copy = false) {
    if (copy)
      return;
    vector<double> &weights = *data_->Weights();
    vector<ssize_t> &weight_positions = *data_->WeightPositions();
    CHECK(weights.empty());
    CHECK_GE(arc_limit_, arc_period_);
    weight_positions.reserve(CountStates(fst));

    ssize_t weight_position = 0;
    for(StateIterator<F> siter(fst); !siter.Done(); siter.Next()) {
      StateId s = siter.Value();
      if (fst.NumArcs(s) >= arc_limit_) {
        double sum = FloatLimits<double>::kPosInfinity;
        weight_positions.push_back(weight_position);
        weights.push_back(sum);
        ++weight_position;
        ssize_t narcs = 0;
        for(ArcIterator<F> aiter(fst, s); !aiter.Done(); aiter.Next()) {
          const A &arc = aiter.Value();
          sum = LogPlus(sum, arc.weight);
          // Stores cumulative weight distribution per arc_period_.
          if (++narcs % arc_period_ == 0) {
            weights.push_back(sum);
            ++weight_position;
          }
        }
      } else {
        weight_positions.push_back(-1);
      }
    }
  }

 private:
  double LogPosExp(double x) {
    return x == FloatLimits<double>::kPosInfinity ?
        0.0 : log(1.0F + exp(-x));
  }

  double LogMinusExp(double x) {
    return x == FloatLimits<double>::kPosInfinity ?
        0.0 : log(1.0F - exp(-x));
  }

  Weight LogPlus(Weight w, Weight v) {
    float f1 = w.Value();
    float f2 = v.Value();
    if (f1 > f2)
      return Weight(f2 - LogPosExp(f1 - f2));
    else
      return Weight(f1 - LogPosExp(f2 - f1));
  }

  double LogPlus(double f1, Weight v) {
    float f2 = v.Value();
    if (f1 == FloatLimits<double>::kPosInfinity)
      return f2;
    else if (f1 > f2)
      return f2 - LogPosExp(f1 - f2);
    else
      return f1 - LogPosExp(f2 - f1);
  }

  Weight LogMinus(double f1, double f2) {
    CHECK_LT(f1, f2);
    if (f2 == FloatLimits<double>::kPosInfinity)
      return f1;
    else
      return Weight(f1 - LogMinusExp(f2 - f1));
  }
  ssize_t arc_limit_;     // Minimum # of arcs to pre-compute state
  ssize_t arc_period_;    // Save cumulative weights per 'arc_period_'.
  bool init_;             // Cumulative weights initialized?
  FastLogAccumulatorData *data_;
  double *state_weights_;

  void operator=(const FastLogAccumulator<A> &);   // Disallow
};


// Stores shareable data for cache log accumulator copies.
// All copies share the same cache.
template <class A>
class CacheLogAccumulatorData {
 public:
  typedef A Arc;
  typedef typename A::StateId StateId;
  typedef typename A::Weight Weight;

  CacheLogAccumulatorData(bool gc, size_t gc_limit)
      : cache_gc_(gc), cache_limit_(gc_limit), cache_size_(0) {}

  ~CacheLogAccumulatorData() {
    for(typename unordered_map<StateId, CacheState>::iterator it = cache_.begin();
        it != cache_.end();
        ++it)
      delete it->second.weights;
  }

  bool CacheDisabled() const { return cache_gc_ && cache_limit_ == 0; }

  vector<double> *GetWeights(StateId s) {
    typename unordered_map<StateId, CacheState>::iterator it = cache_.find(s);
    if (it != cache_.end()) {
      it->second.recent = true;
      return it->second.weights;
    } else {
      return 0;
    }
  }

  void AddWeights(StateId s, vector<double> *weights) {
    if (cache_gc_ && cache_size_ >= cache_limit_)
      GC(false);
    cache_.insert(make_pair(s, CacheState(weights, true)));
    if (cache_gc_)
      cache_size_ += weights->capacity() * sizeof(double);
  }

  int RefCount() const { return ref_count_.count(); }
  int IncrRefCount() { return ref_count_.Incr(); }
  int DecrRefCount() { return ref_count_.Decr(); }

 private:
  // Cached information for a given state.
  struct CacheState {
    vector<double>* weights;  // Accumulated weights for this state.
    bool recent;              // Has this state been accessed since last GC?

    CacheState(vector<double> *w, bool r) : weights(w), recent(r) {}
  };

  // Garbage collect: Delete from cache states that have not been
  // accessed since the last GC ('free_recent = false') until
  // 'cache_size_' is 2/3 of 'cache_limit_'. If it does not free enough
  // memory, start deleting recently accessed states.
  void GC(bool free_recent) {
    size_t cache_target = (2 * cache_limit_)/3 + 1;
    typename unordered_map<StateId, CacheState>::iterator it = cache_.begin();
    while (it != cache_.end() && cache_size_ > cache_target) {
      CacheState &cs = it->second;
      if (free_recent || !cs.recent) {
        cache_size_ -= cs.weights->capacity() * sizeof(double);
        delete cs.weights;
        cache_.erase(it++);
      } else {
        cs.recent = false;
        ++it;
      }
    }
    if (!free_recent && cache_size_ > cache_target)
      GC(true);
  }

  unordered_map<StateId, CacheState> cache_;  // Cache
  bool cache_gc_;                        // Enable garbage collection
  size_t cache_limit_;                   // # of bytes cached
  size_t cache_size_;                    // # of bytes allowed before GC
  RefCounter ref_count_;

  DISALLOW_COPY_AND_ASSIGN(CacheLogAccumulatorData);
};

// This class accumulates arc weights using the log semiring Plus()
// assuming an arc weight that has a member 'float Value()' and a
// float constructor. It is similar to the FastLogAccumator. However
// here, the accumulated weights are pre-computed and stored only for
// the states that are visited. The member function Init(fst) has to
// be called to setup this accumulator.
template <class A>
class CacheLogAccumulator {
 public:
  typedef A Arc;
  typedef typename A::StateId StateId;
  typedef typename A::Weight Weight;

  explicit CacheLogAccumulator(ssize_t arc_limit = 10, bool gc = false,
                               size_t gc_limit = 10 * 1024 * 1024)
      : arc_limit_(arc_limit), fst_(0), data_(
          new CacheLogAccumulatorData<A>(gc, gc_limit)), s_(kNoStateId) {}

  CacheLogAccumulator(const CacheLogAccumulator<A> &acc)
      : arc_limit_(acc.arc_limit_), fst_(acc.fst_ ? acc.fst_->Copy() : 0),
        data_(acc.data_), s_(kNoStateId) {
    data_->IncrRefCount();
  }

  ~CacheLogAccumulator() {
    if (fst_)
      delete fst_;
    if (!data_->DecrRefCount())
      delete data_;
  }

  // Arg 'arc_limit' specifies minimum # of arcs to pre-compute state.
  void Init(const Fst<A> &fst, bool copy = false) {
    if (copy)
      delete fst_;
    else
      CHECK(!fst_);
    fst_ = fst.Copy();
  }

  void SetState(StateId s, int depth = 0) {
    if (s == s_)
      return;
    s_ = s;

    if (data_->CacheDisabled()) {
      weights_ = 0;
      return;
    }

    CHECK(fst_);

    weights_ = data_->GetWeights(s);
    if ((weights_ == 0) && (fst_->NumArcs(s) >= arc_limit_)) {
      weights_ = new vector<double>;
      weights_->reserve(fst_->NumArcs(s) + 1);
      weights_->push_back(FloatLimits<double>::kPosInfinity);
      data_->AddWeights(s, weights_);
    }
  }

  Weight Sum(Weight w, Weight v) {
    return LogPlus(w, v);
  }

  template <class Iterator>
  Weight Sum(Weight w, Iterator *aiter, ssize_t begin,
             ssize_t end) {
    if (weights_ == 0) {
      Weight sum = w;
      aiter->Seek(begin);
      for (ssize_t pos = begin; pos < end; aiter->Next(), ++pos)
        sum = LogPlus(sum, aiter->Value().weight);
      return sum;
    } else {
      if (weights_->size() <= end)
        for (aiter->Seek(weights_->size() - 1);
             weights_->size() <= end;
             aiter->Next())
          weights_->push_back(LogPlus(weights_->back(),
                                      aiter->Value().weight));
      return LogPlus(w, LogMinus((*weights_)[end], (*weights_)[begin]));
    }
  }

  template <class Iterator>
  size_t LowerBound(double w, Iterator *aiter) {
    if (weights_ != 0) {
      return lower_bound(weights_->begin() + 1,
                         weights_->end(),
                         w,
                         std::greater<double>())
          - weights_->begin() - 1;
    } else {
      size_t n = 0;
      double x =  FloatLimits<double>::kPosInfinity;
      for(aiter->Reset(); !aiter->Done(); aiter->Next(), ++n) {
        x = LogPlus(x, aiter->Value().weight.Value());
        if (x < w) break;
      }
      return n;
    }
  }

 private:
  double LogPosExp(double x) {
    return x == FloatLimits<double>::kPosInfinity ?
        0.0 : log(1.0F + exp(-x));
  }

  double LogMinusExp(double x) {
    return x == FloatLimits<double>::kPosInfinity ?
        0.0 : log(1.0F - exp(-x));
  }

  Weight LogPlus(Weight w, Weight v) {
    float f1 = w.Value();
    float f2 = v.Value();
    if (f1 > f2)
      return Weight(f2 - LogPosExp(f1 - f2));
    else
      return Weight(f1 - LogPosExp(f2 - f1));
  }

  double LogPlus(double f1, Weight v) {
    float f2 = v.Value();
    if (f1 == FloatLimits<double>::kPosInfinity)
      return f2;
    else if (f1 > f2)
      return f2 - LogPosExp(f1 - f2);
    else
      return f1 - LogPosExp(f2 - f1);
  }

  Weight LogMinus(double f1, double f2) {
    CHECK_LT(f1, f2);
    if (f2 == FloatLimits<double>::kPosInfinity)
      return f1;
    else
      return Weight(f1 - LogMinusExp(f2 - f1));
  }

  ssize_t arc_limit_;                    // Minimum # of arcs to cache a state
  vector<double> *weights_;              // Accumulated weights for cur. state
  const Fst<A>* fst_;                    // Input fst
  CacheLogAccumulatorData<A> *data_;     // Cache data
  StateId s_;                            // Current state

  void operator=(const CacheLogAccumulator<A> &);   // Disallow
};


// Stores shareable data for replace accumulator copies.
template <class Accumulator, class T>
class ReplaceAccumulatorData {
 public:
  typedef typename Accumulator::Arc Arc;
  typedef typename Arc::StateId StateId;
  typedef typename Arc::Label Label;
  typedef T StateTable;
  typedef typename T::StateTuple StateTuple;

  ReplaceAccumulatorData() : state_table_(0) {}

  ReplaceAccumulatorData(const vector<Accumulator*> &accumulators)
      : state_table_(0), accumulators_(accumulators) {}

  ~ReplaceAccumulatorData() {
    for (size_t i = 0; i < fst_array_.size(); ++i)
      delete fst_array_[i];
    for (size_t i = 0; i < accumulators_.size(); ++i)
      delete accumulators_[i];
  }

  void Init(const vector<pair<Label, const Fst<Arc>*> > &fst_tuples,
       const StateTable *state_table) {
    state_table_ = state_table;
    accumulators_.resize(fst_tuples.size());
    for (size_t i = 0; i < accumulators_.size(); ++i) {
      if (!accumulators_[i])
        accumulators_[i] = new Accumulator;
      accumulators_[i]->Init(*(fst_tuples[i].second));
      fst_array_.push_back(fst_tuples[i].second->Copy());
    }
  }

  const StateTuple &GetTuple(StateId s) const {
    return state_table_->Tuple(s);
  }

  Accumulator *GetAccumulator(size_t i) { return accumulators_[i]; }

  const Fst<Arc> *GetFst(size_t i) const { return fst_array_[i]; }

  int RefCount() const { return ref_count_.count(); }
  int IncrRefCount() { return ref_count_.Incr(); }
  int DecrRefCount() { return ref_count_.Decr(); }

 private:
  const T * state_table_;
  vector<Accumulator*> accumulators_;
  vector<const Fst<Arc>*> fst_array_;
  RefCounter ref_count_;

  DISALLOW_COPY_AND_ASSIGN(ReplaceAccumulatorData);
};

// This class accumulates weights in a ReplaceFst.  The 'Init' method
// takes as input the argument used to build the ReplaceFst and the
// ReplaceFst state table. It uses accumulators of type 'Accumulator'
// in the underlying FSTs.
template <class Accumulator,
          class T = DefaultReplaceStateTable<typename Accumulator::Arc> >
class ReplaceAccumulator {
 public:
  typedef typename Accumulator::Arc Arc;
  typedef typename Arc::StateId StateId;
  typedef typename Arc::Label Label;
  typedef typename Arc::Weight Weight;
  typedef T StateTable;
  typedef typename T::StateTuple StateTuple;

  ReplaceAccumulator()
      : init_(false), data_(new ReplaceAccumulatorData<Accumulator, T>()) {}

  ReplaceAccumulator(const vector<Accumulator*> &accumulators)
      : init_(false),
        data_(new ReplaceAccumulatorData<Accumulator, T>(accumulators)) {}

  ReplaceAccumulator(const ReplaceAccumulator<Accumulator, T> &acc)
      : init_(acc.init_), data_(acc.data_) {
    if (!init_)
      LOG(FATAL) << "ReplaceAccumulator: can't copy unintialized accumulator";
    data_->IncrRefCount();
  }

  ~ReplaceAccumulator() {
     if (!data_->DecrRefCount())
      delete data_;
  }

  // Does not take ownership of the state table, the state table
  // is own by the ReplaceFst
  void Init(const vector<pair<Label, const Fst<Arc>*> > &fst_tuples,
            const StateTable *state_table) {
    init_ = true;
    data_->Init(fst_tuples, state_table);
  }

  void SetState(StateId s) {
    CHECK(init_);
    StateTuple tuple = data_->GetTuple(s);
    fst_id_ = tuple.fst_id - 1;  // Replace FST ID is 1-based
    data_->GetAccumulator(fst_id_)->SetState(tuple.fst_state);
    if ((tuple.prefix_id != 0) &&
        (data_->GetFst(fst_id_)->Final(tuple.fst_state) != Weight::Zero())) {
      offset_ = 1;
      offset_weight_ = data_->GetFst(fst_id_)->Final(tuple.fst_state);
    } else {
      offset_ = 0;
      offset_weight_ = Weight::Zero();
    }
  }

  Weight Sum(Weight w, Weight v) {
    return data_->GetAccumulator(fst_id_)->Sum(w, v);
  }

  template <class ArcIterator>
  Weight Sum(Weight w, ArcIterator *aiter, ssize_t begin,
             ssize_t end) {
    Weight sum = begin == end ? Weight::Zero()
        : data_->GetAccumulator(fst_id_)->Sum(
            w, aiter, begin ? begin - offset_ : 0, end - offset_);
    if (begin == 0 && end != 0 && offset_ > 0)
      sum = Sum(offset_weight_, sum);
    return sum;
  }


 private:
  bool init_;
  ReplaceAccumulatorData<Accumulator, T> *data_;
  Label fst_id_;
  size_t offset_;
  Weight offset_weight_;

  void operator=(const ReplaceAccumulator<Accumulator, T> &);   // Disallow
};

}  // namespace fst

#endif  // FST_LIB_ACCUMULATOR_H__
