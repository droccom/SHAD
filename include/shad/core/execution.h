//===------------------------------------------------------------*- C++ -*-===//
//
//                                     SHAD
//
//      The Scalable High-performance Algorithms and Data Structure Library
//
//===----------------------------------------------------------------------===//
//
// Copyright 2018 Battelle Memorial Institute
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy
// of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
//
//===----------------------------------------------------------------------===//

#ifndef INCLUDE_SHAD_CORE_EXECUTION_H
#define INCLUDE_SHAD_CORE_EXECUTION_H

#include <type_traits>

namespace shad {

struct distributed_sequential_tag {};
struct distributed_parallel_tag {};

template <class ExecutionPolicy>
struct is_execution_policy :
         std::integral_constant<bool,
                                std::is_same<ExecutionPolicy,
                                          distributed_sequential_tag>::value ||
                                std::is_same<ExecutionPolicy,
                                          distributed_parallel_tag>::value>{
};

}  // namespace shad

#endif /* INCLUDE_SHAD_CORE_EXECUTION_H */