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

#ifndef INCLUDE_SHAD_CORE_IMPL_MODIFYING_SEQUENCE_OPS_H
#define INCLUDE_SHAD_CORE_IMPL_MODIFYING_SEQUENCE_OPS_H

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <tuple>

#include "shad/core/execution.h"
#include "shad/core/impl/utils.h"
#include "shad/distributed_iterator_traits.h"
#include "shad/runtime/runtime.h"

namespace shad {
namespace impl {

template <typename ForwardIt, typename T>
void fill(distributed_parallel_tag&& policy, ForwardIt first, ForwardIt last,
          const T& value) {
  using itr_traits = distributed_iterator_traits<ForwardIt>;

  // distributed map
  distributed_map_void(
      // range
      first, last,
      // kernel
      [](ForwardIt first, ForwardIt last, const T& value) {
        using local_iterator_t = typename itr_traits::local_iterator_type;

        // local map
        auto lrange = itr_traits::local_range(first, last);

        local_map_void(
            // range
            lrange.begin(), lrange.end(),
            // kernel
            [&](local_iterator_t b, local_iterator_t e) {
              std::fill(b, e, value);
            });
      },
      // map arguments
      value);
}

template <typename ForwardIt, typename T>
void fill(distributed_sequential_tag&& policy, ForwardIt first, ForwardIt last,
          const T& value) {
  using itr_traits = distributed_iterator_traits<ForwardIt>;

  distributed_folding_map_void(
      // range
      first, last,
      // kernel
      [](ForwardIt first, ForwardIt last, const T& value) {
        // local processing
        auto lrange = itr_traits::local_range(first, last);
        std::fill(lrange.begin(), lrange.end(), value);
      },
      // map arguments
      value);
}

namespace transform_impl {
template <typename ForwardIt>
struct gen_args_t {
  static constexpr size_t buf_size =
      (2 << 10) / sizeof(typename ForwardIt::value_type);
  typename ForwardIt::value_type buf[buf_size];
  ForwardIt w_first;
  size_t size;
};

template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
void block_contiguous_local(ForwardIt1 first, ForwardIt1 last,
                            ForwardIt2 d_first, UnaryOperation op) {
  using itr_traits2 = distributed_iterator_traits<ForwardIt2>;
  using args_t = gen_args_t<ForwardIt2>;
  auto size = std::distance(first, last);
  auto d_last = d_first;
  std::advance(d_last, size);

  // local assign
  auto local_d_range = itr_traits2::local_range(d_first, d_last);
  auto loc_res = std::transform(first, last, local_d_range.begin(), op);
}

template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
void block_contiguous_local_par(rt::Handle& h, ForwardIt1 first,
                                ForwardIt1 last, ForwardIt2 d_first,
                                UnaryOperation op) {
  using itr_traits1 = std::iterator_traits<ForwardIt1>;
  using itr_traits2 = distributed_iterator_traits<ForwardIt2>;
  using args_t = gen_args_t<ForwardIt2>;
  auto size = std::distance(first, last);
  auto d_last = d_first;
  std::advance(d_last, size);

  // local map
  auto local_d_range = itr_traits2::local_range(d_first, d_last);
  using offset_t = typename std::iterator_traits<ForwardIt1>::difference_type;
  async_local_map_void_offset(h,
                              // range
                              first, last,
                              // kernel
                              [&](ForwardIt1 b, ForwardIt1 e, offset_t offset) {
                                std::transform(
                                    b, e, local_d_range.begin() + offset, op);
                              });
  rt::waitForCompletion(h); //FIXME should be moved outside
}

template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
void block_contiguous_remote(rt::Locality l, rt::Handle& h, ForwardIt1 first,
                             ForwardIt1 last, ForwardIt2 d_first,
                             UnaryOperation op) {
  using itr_traits2 = distributed_iterator_traits<ForwardIt2>;
  using args_t = gen_args_t<ForwardIt2>;

  // remote assign
  std::shared_ptr<uint8_t> args_buf(new uint8_t[sizeof(args_t)],
                                    std::default_delete<uint8_t[]>());
  auto typed_args_buf = reinterpret_cast<args_t*>(args_buf.get());
  auto block_last = first;
  while (first != last) {
    typed_args_buf->w_first = d_first;
    typed_args_buf->size =
        std::min(args_t::buf_size, (size_t)std::distance(first, last));
    std::advance(block_last, typed_args_buf->size);
    std::transform(first, block_last, typed_args_buf->buf, op);
    rt::asyncExecuteAt(
        h, l,
        [](rt::Handle&, const uint8_t* args_buf, const uint32_t) {
          const args_t& args = *reinterpret_cast<const args_t*>(args_buf);
          using val_t = typename ForwardIt2::value_type;
          ForwardIt2 w_last = args.w_first;
          std::advance(w_last, args.size);
          auto w_range = itr_traits2::local_range(args.w_first, w_last);
          std::memcpy(w_range.begin(), args.buf, sizeof(val_t) * args.size);
        },
        args_buf, sizeof(args_t));
    std::advance(first, typed_args_buf->size);
    std::advance(d_first, typed_args_buf->size);
  }
}

// distributed-sequential kernel for block-contiguous output-iterators
template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
ForwardIt2 dseq_kernel(std::true_type, ForwardIt1 first, ForwardIt1 last,
                       ForwardIt2 d_first, UnaryOperation op) {
  using itr_traits1 = distributed_iterator_traits<ForwardIt1>;
  using itr_traits2 = distributed_random_access_iterator_trait<ForwardIt2>;
  auto loc_range = itr_traits1::local_range(first, last);
  auto loc_first = loc_range.begin();
  auto d_last = d_first;
  std::advance(d_last, std::distance(loc_first, loc_range.end()));
  auto dmap = itr_traits2::distribution(d_first, d_last);
  auto loc_last = loc_first;
  rt::Handle h;
  for (auto i : dmap) {
    auto l = i.first;
    std::advance(loc_last, i.second);
    if (rt::thisLocality() == l)
      block_contiguous_local(loc_first, loc_last, d_first, op);
    else {
      block_contiguous_remote(l, h, loc_first, loc_last, d_first, op);
      rt::waitForCompletion(h);
    }
    std::advance(loc_first, i.second);
    std::advance(d_first, i.second);
  }
  return d_last;
}

// distributed-parallel kernel for block-contiguous output-iterators
template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
ForwardIt2 dpar_kernel(std::true_type, ForwardIt1 first, ForwardIt1 last,
                       ForwardIt2 d_first, UnaryOperation op) {
  using itr_traits1 = distributed_iterator_traits<ForwardIt1>;
  using itr_traits2 = distributed_random_access_iterator_trait<ForwardIt2>;
  auto loc_range = itr_traits1::local_range(first, last);
  auto loc_first = loc_range.begin();
  auto first_ = itr_traits1::iterator_from_local(first, last, loc_first);
  std::advance(d_first, std::distance(first, first_));
  auto d_last = d_first;
  std::advance(d_last, std::distance(loc_first, loc_range.end()));
  auto dmap = itr_traits2::distribution(d_first, d_last);
  auto loc_last = loc_first;
  rt::Handle h;
  auto coloc_first = loc_first, coloc_last = loc_first;
  auto coloc_d_first = d_first;
  // create remote tasks
  for (auto i : dmap) {
    auto l = i.first;
    std::advance(loc_last, i.second);
    if (rt::thisLocality() == l) {
      coloc_first = loc_first;
      coloc_last = loc_last;
      coloc_d_first = d_first;
    } else
      block_contiguous_remote(l, h, loc_first, loc_last, d_first, op);
    std::advance(loc_first, i.second);
    std::advance(d_first, i.second);
  }
  // process local portion
  if (coloc_first != coloc_last)
    block_contiguous_local_par(h, coloc_first, coloc_last, coloc_d_first, op);
  // join
  if (!h.IsNull()) rt::waitForCompletion(h); //FIXME should be enough
  return d_last;
}

// distributed-parallel kernel for non-block-contiguous output-iterators
template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
ForwardIt2 dpar_kernel(std::false_type, ForwardIt1 first, ForwardIt1 last,
                       ForwardIt2 d_first, UnaryOperation op) {
  using itr_traits1 = distributed_iterator_traits<ForwardIt1>;
  auto local_range = itr_traits1::local_range(first, last);
  auto begin = local_range.begin();
  auto end = local_range.end();
  auto res = std::transform(begin, end, d_first, op);
  flush_iterator(res);
  return res;
}

// distributed-sequential kernel for non-block-contiguous output-iterators
template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
ForwardIt2 dseq_kernel(std::false_type, ForwardIt1 first, ForwardIt1 last,
                       ForwardIt2 d_first, UnaryOperation op) {
  using itr_traits1 = distributed_iterator_traits<ForwardIt1>;
  auto local_range = itr_traits1::local_range(first, last);
  auto begin = local_range.begin();
  auto end = local_range.end();
  auto res = std::transform(begin, end, d_first, op);
  flush_iterator(res);
  return res;
}

// dispatchers
template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
ForwardIt2 dseq_kernel(ForwardIt1 first, ForwardIt1 last, ForwardIt2 d_first,
                       UnaryOperation op) {
  return dseq_kernel(is_block_contiguous<ForwardIt2>::value, first, last,
                     d_first, op);
}

template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
ForwardIt2 dpar_kernel(ForwardIt1 first, ForwardIt1 last, ForwardIt2 d_first,
                       UnaryOperation op) {
  return dpar_kernel(is_block_contiguous<ForwardIt2>::value, first, last,
                     d_first, op);
}

}  // namespace transform_impl

template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
ForwardIt2 transform(distributed_parallel_tag&& policy, ForwardIt1 first1,
                     ForwardIt1 last1, ForwardIt2 d_first,
                     UnaryOperation unary_op) {
  using itr_traits = distributed_iterator_traits<ForwardIt1>;

  // distributed map
  auto map_res = distributed_map_init(
      // range
      first1, last1,
      // kernel
      [](ForwardIt1 first1, ForwardIt1 last1, ForwardIt2 d_first,
         UnaryOperation unary_op) {
        return transform_impl::dpar_kernel(first1, last1, d_first, unary_op);
      },
      // init value
      d_first,
      // map arguments
      d_first, unary_op);

  // reduce
  return map_res.back();
}

template <class ForwardIt1, class ForwardIt2, class UnaryOperation>
ForwardIt2 transform(distributed_sequential_tag&& policy, ForwardIt1 first1,
                     ForwardIt1 last1, ForwardIt2 d_first,
                     UnaryOperation unary_op) {
  using itr_traits = distributed_iterator_traits<ForwardIt1>;

  return distributed_folding_map(
      // range
      first1, last1,
      // kernel
      [](ForwardIt1 first1, ForwardIt1 last1, ForwardIt2 d_first,
         UnaryOperation unary_op) {
        // local processing
        auto lrange = itr_traits::local_range(first1, last1);
        auto local_res =
            transform_impl::dseq_kernel(first1, last1, d_first, unary_op);
        // update the partial solution
        return local_res;
      },
      // initial solution
      d_first,
      // map arguments
      unary_op);
}

template <typename ForwardIt, typename Generator>
void generate(distributed_parallel_tag&& policy, ForwardIt first,
              ForwardIt last, Generator generator) {
  using itr_traits = distributed_iterator_traits<ForwardIt>;

  // distributed map
  distributed_map_void(
      // range
      first, last,
      // kernel
      [](ForwardIt first, ForwardIt last, Generator generator) {
        using local_iterator_t = typename itr_traits::local_iterator_type;

        // local map
        auto lrange = itr_traits::local_range(first, last);

        local_map_void(
            // range
            lrange.begin(), lrange.end(),
            // kernel
            [&](local_iterator_t b, local_iterator_t e) {
              std::generate(b, e, generator);
            });
      },
      // map arguments
      generator);
}

template <typename ForwardIt, typename Generator>
void generate(distributed_sequential_tag&& policy, ForwardIt first,
              ForwardIt last, Generator generator) {
  using itr_traits = distributed_iterator_traits<ForwardIt>;

  distributed_folding_map_void(
      // range
      first, last,
      // kernel
      [](ForwardIt first, ForwardIt last, Generator generator) {
        // local processing
        auto lrange = itr_traits::local_range(first, last);
        std::generate(lrange.begin(), lrange.end(), generator);
      },
      // map arguments
      generator);
}

template <typename ForwardIt, typename T>
void replace(distributed_parallel_tag&& policy, ForwardIt first, ForwardIt last,
             const T& old_value, const T& new_value) {
  using itr_traits = distributed_iterator_traits<ForwardIt>;

  // distributed map
  distributed_map_void(
      // range
      first, last,
      // kernel
      [](ForwardIt first, ForwardIt last, const T& old_value,
         const T& new_value) {
        using local_iterator_t = typename itr_traits::local_iterator_type;

        // local map
        auto lrange = itr_traits::local_range(first, last);

        local_map_void(
            // range
            lrange.begin(), lrange.end(),
            // kernel
            [&](local_iterator_t b, local_iterator_t e) {
              std::replace(b, e, old_value, new_value);
            });
      },
      // map arguments
      old_value, new_value);
}

template <typename ForwardIt, typename T>
void replace(distributed_sequential_tag&& policy, ForwardIt first,
             ForwardIt last, const T& old_value, const T& new_value) {
  using itr_traits = distributed_iterator_traits<ForwardIt>;

  distributed_folding_map_void(
      // range
      first, last,
      // kernel
      [](ForwardIt first, ForwardIt last, const T& old_value,
         const T& new_value) {
        // local processing
        auto lrange = itr_traits::local_range(first, last);
        std::replace(lrange.begin(), lrange.end(), old_value, new_value);
      },
      // map arguments
      old_value, new_value);
}

template <typename ForwardIt, typename UnaryPredicate, typename T>
void replace_if(distributed_parallel_tag&& policy, ForwardIt first,
                ForwardIt last, UnaryPredicate p, const T& new_value) {
  using itr_traits = distributed_iterator_traits<ForwardIt>;

  // distributed map
  distributed_map_void(
      // range
      first, last,
      // kernel
      [](ForwardIt first, ForwardIt last, UnaryPredicate p,
         const T& new_value) {
        using local_iterator_t = typename itr_traits::local_iterator_type;

        // local map
        auto lrange = itr_traits::local_range(first, last);

        local_map_void(
            // range
            lrange.begin(), lrange.end(),
            // kernel
            [&](local_iterator_t b, local_iterator_t e) {
              std::replace_if(b, e, p, new_value);
            });
      },
      // map arguments
      p, new_value);
}

template <typename ForwardIt, typename UnaryPredicate, typename T>
void replace_if(distributed_sequential_tag&& policy, ForwardIt first,
                ForwardIt last, UnaryPredicate p, const T& new_value) {
  using itr_traits = distributed_iterator_traits<ForwardIt>;

  distributed_folding_map_void(
      // range
      first, last,
      // kernel
      [](ForwardIt first, ForwardIt last, UnaryPredicate p,
         const T& new_value) {
        // local processing
        auto lrange = itr_traits::local_range(first, last);
        std::replace_if(lrange.begin(), lrange.end(), p, new_value);
      },
      // map arguments
      p, new_value);
}

}  // namespace impl
}  // namespace shad

#endif /* INCLUDE_SHAD_CORE_IMPL_MODIFYING_SEQUENCE_OPS_H */
