// const-fst.h

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
// Simple concrete immutable FST whose states and arcs are each stored
// in single arrays.

#ifndef FST_LIB_CONST_FST_H__
#define FST_LIB_CONST_FST_H__

#include <string>
#include <vector>
using std::vector;
#include <fst/expanded-fst.h>
#include <fst/fst-decl.h>  // For optional argument declarations
#include <fst/test-properties.h>
#include <fst/util.h>

namespace fst {

template <class A, class U> class ConstFst;
template <class F, class G> void Cast(const F &, G *);

// States and arcs each implemented by single arrays, templated on the
// Arc definition. The unsigned type U is used to represent indices into
// the arc array.
template <class A, class U>
class ConstFstImpl : public FstImpl<A> {
 public:
  using FstImpl<A>::SetInputSymbols;
  using FstImpl<A>::SetOutputSymbols;
  using FstImpl<A>::SetType;
  using FstImpl<A>::SetProperties;
  using FstImpl<A>::Properties;
  using FstImpl<A>::WriteHeader;

  typedef A Arc;
  typedef typename A::Weight Weight;
  typedef typename A::StateId StateId;
  typedef U Unsigned;

  ConstFstImpl()
      : states_(0), arcs_(0), nstates_(0), narcs_(0), start_(kNoStateId) {
    string type = "const";
    if (sizeof(U) != sizeof(uint32)) {
      string size;
      Int64ToStr(8 * sizeof(U), &size);
      type += size;
    }
    SetType(type);
    SetProperties(kNullProperties | kStaticProperties);
  }

  explicit ConstFstImpl(const Fst<A> &fst);

  ~ConstFstImpl() {
    delete[] states_;
    delete[] arcs_;
  }

  StateId Start() const { return start_; }

  Weight Final(StateId s) const { return states_[s].final; }

  StateId NumStates() const { return nstates_; }

  size_t NumArcs(StateId s) const { return states_[s].narcs; }

  size_t NumInputEpsilons(StateId s) const { return states_[s].niepsilons; }

  size_t NumOutputEpsilons(StateId s) const { return states_[s].noepsilons; }

  static ConstFstImpl<A, U> *Read(istream &strm, const FstReadOptions &opts);

  bool Write(ostream &strm, const FstWriteOptions &opts) const;

  A *Arcs(StateId s) { return arcs_ + states_[s].pos; }

  // Provide information needed for generic state iterator
  void InitStateIterator(StateIteratorData<A> *data) const {
    data->base = 0;
    data->nstates = nstates_;
  }

  // Provide information needed for the generic arc iterator
  void InitArcIterator(StateId s, ArcIteratorData<A> *data) const {
    data->base = 0;
    data->arcs = arcs_ + states_[s].pos;
    data->narcs = states_[s].narcs;
    data->ref_count = 0;
  }

 private:
  // States implemented by array *states_ below, arcs by (single) *arcs_.
  struct State {
    Weight final;                // Final weight
    Unsigned pos;                // Start of state's arcs in *arcs_
    Unsigned narcs;              // Number of arcs (per state)
    Unsigned niepsilons;         // # of input epsilons
    Unsigned noepsilons;         // # of output epsilons
    State() : final(Weight::Zero()), niepsilons(0), noepsilons(0) {}
  };

  // Properties always true of this Fst class
  static const uint64 kStaticProperties = kExpanded;
  // Current file format version
  static const int kFileVersion = 1;
  // Minimum file format version supported
  static const int kMinFileVersion = 1;
  // Byte alignment for states and arcs in file format
  static const int kFileAlign = 16;

  State *states_;                // States represenation
  A *arcs_;                      // Arcs representation
  StateId nstates_;              // Number of states
  size_t narcs_;                 // Number of arcs (per FST)
  StateId start_;                // Initial state

  DISALLOW_COPY_AND_ASSIGN(ConstFstImpl);
};

template <class A, class U>
const uint64 ConstFstImpl<A, U>::kStaticProperties;
template <class A, class U>
const int ConstFstImpl<A, U>::kFileVersion;
template <class A, class U>
const int ConstFstImpl<A, U>::kMinFileVersion;
template <class A, class U>
const int ConstFstImpl<A, U>::kFileAlign;


template<class A, class U>
ConstFstImpl<A, U>::ConstFstImpl(const Fst<A> &fst) : nstates_(0), narcs_(0) {
  string type = "const";
  if (sizeof(U) != sizeof(uint32)) {
    string size;
    Int64ToStr(sizeof(U) * 8, &size);
    type += size;
  }
  SetType(type);
  uint64 copy_properties = fst.Properties(kCopyProperties, true);
  SetProperties(copy_properties | kStaticProperties);
  SetInputSymbols(fst.InputSymbols());
  SetOutputSymbols(fst.OutputSymbols());
  start_ = fst.Start();

  // Count # of states and arcs.
  for (StateIterator< Fst<A> > siter(fst);
       !siter.Done();
       siter.Next()) {
    ++nstates_;
    StateId s = siter.Value();
    for (ArcIterator< Fst<A> > aiter(fst, s);
         !aiter.Done();
         aiter.Next())
      ++narcs_;
  }
  states_ = new State[nstates_];
  arcs_ = new A[narcs_];
  size_t pos = 0;
  for (StateId s = 0; s < nstates_; ++s) {
    states_[s].final = fst.Final(s);
    states_[s].pos = pos;
    states_[s].narcs = 0;
    states_[s].niepsilons = 0;
    states_[s].noepsilons = 0;
    for (ArcIterator< Fst<A> > aiter(fst, s);
         !aiter.Done();
         aiter.Next()) {
      const A &arc = aiter.Value();
      ++states_[s].narcs;
      if (arc.ilabel == 0)
        ++states_[s].niepsilons;
      if (arc.olabel == 0)
        ++states_[s].noepsilons;
      arcs_[pos++] = arc;
    }
  }
}

template<class A, class U>
ConstFstImpl<A, U> *ConstFstImpl<A, U>::Read(istream &strm,
                                             const FstReadOptions &opts) {
  ConstFstImpl<A, U> *impl = new ConstFstImpl<A, U>;
  FstHeader hdr;
  if (!impl->ReadHeader(strm, opts, kMinFileVersion, &hdr))
    return 0;
  impl->start_ = hdr.Start();
  impl->nstates_ = hdr.NumStates();
  impl->narcs_ = hdr.NumArcs();
  impl->states_ = new State[impl->nstates_];
  impl->arcs_ = new A[impl->narcs_];

  char c;
  for (int i = 0; i < kFileAlign && strm.tellg() % kFileAlign; ++i)
    strm.read(&c, 1);
  // TODO: memory map this
  size_t b = impl->nstates_ * sizeof(typename ConstFstImpl<A, U>::State);
  strm.read(reinterpret_cast<char *>(impl->states_), b);
  if (!strm) {
    LOG(ERROR) << "ConstFst::Read: Read failed: " << opts.source;
    return 0;
  }
  // TODO: memory map this
  for (int i = 0; i < kFileAlign && strm.tellg() % kFileAlign; ++i)
    strm.read(&c, 1);
  b = impl->narcs_ * sizeof(A);
  strm.read(reinterpret_cast<char *>(impl->arcs_), b);
  if (!strm) {
    LOG(ERROR) << "ConstFst::Read: Read failed: " << opts.source;
    return 0;
  }
  return impl;
}

template<class A, class U>
bool ConstFstImpl<A, U>::Write(ostream &strm,
                               const FstWriteOptions &opts) const {
  FstHeader hdr;
  hdr.SetStart(start_);
  hdr.SetNumStates(nstates_);
  hdr.SetNumArcs(narcs_);
  WriteHeader(strm, opts, kFileVersion, &hdr);

  for (int i = 0; i < kFileAlign && strm.tellp() % kFileAlign; ++i)
    strm.write("", 1);
  strm.write(reinterpret_cast<char *>(states_), nstates_ * sizeof(State));
  for (int i = 0; i < kFileAlign && strm.tellp() % kFileAlign; ++i)
    strm.write("", 1);
  strm.write(reinterpret_cast<char *>(arcs_), narcs_ * sizeof(A));

  strm.flush();
  if (!strm) {
    LOG(ERROR) << "ConstFst::Write: Write failed: " << opts.source;
    return false;
  }
  return true;
}


// Simple concrete immutable FST.  This class attaches interface to
// implementation and handles reference counting, delegating most
// methods to ImplToExpandedFst. The unsigned type U is used to
// represent indices into the arc array (uint32 by default, declared
// in fst-decl.h).
template <class A, class U>
class ConstFst : public ImplToExpandedFst< ConstFstImpl<A, U> > {
 public:
  friend class StateIterator< ConstFst<A, U> >;
  friend class ArcIterator< ConstFst<A, U> >;
  template <class F, class G> void friend Cast(const F &, G *);

  typedef A Arc;
  typedef typename A::StateId StateId;
  typedef ConstFstImpl<A, U> Impl;
  typedef U Unsigned;

  ConstFst() : ImplToExpandedFst<Impl>(new Impl()) {}

  explicit ConstFst(const Fst<A> &fst)
      : ImplToExpandedFst<Impl>(new Impl(fst)) {}

  ConstFst(const ConstFst<A, U> &fst) : ImplToExpandedFst<Impl>(fst) {}

  // Get a copy of this ConstFst. See Fst<>::Copy() for further doc.
  virtual ConstFst<A, U> *Copy(bool safe = false) const {
    return new ConstFst<A, U>(*this);
  }

  // Read a ConstFst from an input stream; return NULL on error
  static ConstFst<A, U> *Read(istream &strm, const FstReadOptions &opts) {
    Impl* impl = Impl::Read(strm, opts);
    return impl ? new ConstFst<A, U>(impl) : 0;
  }

  // Read a ConstFst from a file; return NULL on error
  // Empty filename reads from standard input
  static ConstFst<A, U> *Read(const string &filename) {
    Impl* impl = ImplToExpandedFst<Impl>::Read(filename);
    return impl ? new ConstFst<A, U>(impl) : 0;
  }

  virtual void InitStateIterator(StateIteratorData<Arc> *data) const {
    GetImpl()->InitStateIterator(data);
  }

  virtual void InitArcIterator(StateId s, ArcIteratorData<Arc> *data) const {
    GetImpl()->InitArcIterator(s, data);
  }

 private:
  explicit ConstFst(Impl *impl) : ImplToExpandedFst<Impl>(impl) {}

  // Makes visible to friends.
  Impl *GetImpl() const { return ImplToFst<Impl, ExpandedFst<A> >::GetImpl(); }

  void SetImpl(Impl *impl, bool own_impl = true) {
    ImplToFst< Impl, ExpandedFst<A> >::SetImpl(impl, own_impl);
  }

  void operator=(const ConstFst<A, U> &fst);  // disallow
};


// Specialization for ConstFst; see generic version in fst.h
// for sample usage (but use the ConstFst type!). This version
// should inline.
template <class A, class U>
class StateIterator< ConstFst<A, U> > {
 public:
  typedef typename A::StateId StateId;

  explicit StateIterator(const ConstFst<A, U> &fst)
      : nstates_(fst.GetImpl()->NumStates()), s_(0) {}

  bool Done() const { return s_ >= nstates_; }

  StateId Value() const { return s_; }

  void Next() { ++s_; }

  void Reset() { s_ = 0; }

 private:
  StateId nstates_;
  StateId s_;

  DISALLOW_COPY_AND_ASSIGN(StateIterator);
};


// Specialization for ConstFst; see generic version in fst.h
// for sample usage (but use the ConstFst type!). This version
// should inline.
template <class A, class U>
class ArcIterator< ConstFst<A, U> > {
 public:
  typedef typename A::StateId StateId;

  ArcIterator(const ConstFst<A, U> &fst, StateId s)
      : arcs_(fst.GetImpl()->Arcs(s)),
        narcs_(fst.GetImpl()->NumArcs(s)), i_(0) {}

  bool Done() const { return i_ >= narcs_; }

  const A& Value() const { return arcs_[i_]; }

  void Next() { ++i_; }

  size_t Position() const { return i_; }

  void Reset() { i_ = 0; }

  void Seek(size_t a) { i_ = a; }

  uint32 Flags() const {
    return kArcValueFlags;
  }

  void SetFlags(uint32 f, uint32 m) {}

 private:
  const A *arcs_;
  size_t narcs_;
  size_t i_;

  DISALLOW_COPY_AND_ASSIGN(ArcIterator);
};

// A useful alias when using StdArc.
typedef ConstFst<StdArc> StdConstFst;

}  // namespace fst;

#endif  // FST_LIB_CONST_FST_H__
