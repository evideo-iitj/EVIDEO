// fstdeterminize.cc

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
// Modified: jpr@google.com (Jake Ratkiewicz) to use FstClass
//
// \file
// Determinizes an FST.
//

#include <fst/script/determinize.h>

DEFINE_double(delta, fst::kDelta, "Comparison/quantization delta");
DEFINE_int64(nstate, fst::kNoStateId, "State number parameter");
DEFINE_string(weight, "", "Weight parameter");
DEFINE_int64(subsequential_label, 0,
             "Input label of arc corresponding to residual final output when"
             " producing a subsequential transducer");

int main(int argc, char **argv) {
  namespace s = fst::script;
  using fst::script::FstClass;
  using fst::script::MutableFstClass;
  using fst::script::VectorFstClass;
  using fst::script::WeightClass;

  string usage = "Determinizes an FST.\n\n  Usage: ";
  usage += argv[0];
  usage += " [in.fst [out.fst]]\n";

  std::set_new_handler(FailedNewHandler);
  SetFlags(usage.c_str(), &argc, &argv, true);
  if (argc > 3) {
    ShowUsage();
    return 1;
  }

  string in_name = (argc > 1 && strcmp(argv[1], "-") != 0) ? argv[1] : "";
  string out_name = argc > 2 ? argv[2] : "";

  FstClass *ifst = FstClass::Read(in_name);
  if (!ifst) return 1;

  VectorFstClass ofst(ifst->ArcType());

  s::DeterminizeOptions opts(
      FLAGS_delta, FLAGS_weight.empty() ?
      WeightClass::Zero() : WeightClass(ifst->WeightType(), FLAGS_weight),
      FLAGS_nstate, FLAGS_subsequential_label);

  s::Determinize(*ifst, &ofst, opts);

  ofst.Write(out_name);

  return 0;
}
