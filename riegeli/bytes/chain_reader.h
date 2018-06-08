// Copyright 2017 Google LLC
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

#ifndef RIEGELI_BYTES_CHAIN_READER_H_
#define RIEGELI_BYTES_CHAIN_READER_H_

#include <stddef.h>
#include <utility>

#include "absl/base/optimization.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/backward_writer.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

// A Reader which reads from a Chain. It supports random access.
class ChainReader final : public Reader {
 public:
  // Creates a closed ChainReader.
  ChainReader() noexcept : Reader(State::kClosed) {}

  // Will read from the Chain which is owned by this ChainReader.
  explicit ChainReader(Chain src);

  // Will read from the Chain which is not owned by this ChainReader and must be
  // kept alive but not changed until the ChainReader is closed.
  explicit ChainReader(const Chain* src);

  ChainReader(ChainReader&& src) noexcept;
  ChainReader& operator=(ChainReader&& src) noexcept;

  // Returns the Chain being read from. Unchanged by Close().
  const Chain& src() const { return *src_; }

  bool SupportsRandomAccess() const override { return true; }
  bool Size(Position* size) override;

 protected:
  void Done() override;
  bool PullSlow() override;
  bool ReadSlow(Chain* dest, size_t length) override;
  bool CopyToSlow(Writer* dest, Position length) override;
  bool CopyToSlow(BackwardWriter* dest, size_t length) override;
  bool SeekSlow(Position new_pos) override;

 private:
  ChainReader(ChainReader&& src, size_t block_index);

  Chain owned_src_;
  // Invariant: src_ != nullptr
  const Chain* src_ = &owned_src_;
  // Invariant:
  //   if healthy() then iter_.chain() == src_
  //                else iter_ == Chain::BlockIterator()
  Chain::BlockIterator iter_;

  // Invariants if healthy():
  //   start_ == (iter_ == src_->blocks().cend() ? nullptr : iter_->data())
  //   buffer_size() == (iter_ == src_->blocks().cend() ? 0 : iter_->size())
  //   start_pos() is the position of iter_ in *src_
};

// Implementation details follow.

inline ChainReader::ChainReader(Chain src)
    : Reader(State::kOpen),
      owned_src_(std::move(src)),
      iter_(src_->blocks().cbegin()) {
  if (iter_ != src_->blocks().cend()) {
    start_ = iter_->data();
    cursor_ = start_;
    limit_ = start_ + iter_->size();
    limit_pos_ = buffer_size();
  }
}

inline ChainReader::ChainReader(const Chain* src)
    : Reader(State::kOpen),
      src_(RIEGELI_ASSERT_NOTNULL(src)),
      iter_(src_->blocks().cbegin()) {
  if (iter_ != src_->blocks().cend()) {
    start_ = iter_->data();
    cursor_ = start_;
    limit_ = start_ + iter_->size();
    limit_pos_ = buffer_size();
  }
}

inline ChainReader::ChainReader(ChainReader&& src) noexcept
    : ChainReader(std::move(src), src.iter_.block_index()) {}

// block_index is computed early because if src.src_ == &src.owned_src_ then
// *src.src_ is moved, which invalidates src.iter_.
inline ChainReader::ChainReader(ChainReader&& src, size_t block_index)
    : Reader(std::move(src)),
      owned_src_(std::move(src.owned_src_)),
      src_(src.src_ == &src.owned_src_
               ? &owned_src_
               : riegeli::exchange(src.src_, &src.owned_src_)) {
  src.iter_ = Chain::BlockIterator();
  if (ABSL_PREDICT_TRUE(healthy())) {
    // If src_ == &owned_src_ then *src_ was moved, which invalidated iter_ and
    // buffer pointers.
    iter_ = Chain::BlockIterator(src_, block_index);
    if (src_ == &owned_src_ && start_ != nullptr) {
      const size_t cursor_index = read_from_buffer();
      start_ = iter_->data();
      cursor_ = start_ + cursor_index;
      limit_ = start_ + iter_->size();
    }
  }
}

inline ChainReader& ChainReader::operator=(ChainReader&& src) noexcept {
  // block_index is computed early because if src.src_ == &src.owned_src_ then
  // *src.src_ is moved, which invalidates src.iter_.
  const size_t block_index = src.iter_.block_index();
  Reader::operator=(std::move(src));
  owned_src_ = std::move(src.owned_src_);
  src_ = src.src_ == &src.owned_src_
             ? &owned_src_
             : riegeli::exchange(src.src_, &src.owned_src_);
  // Set src.iter_ before iter_ to support self-assignment.
  src.iter_ = Chain::BlockIterator();
  iter_ = Chain::BlockIterator();
  if (ABSL_PREDICT_TRUE(healthy())) {
    // If src_ == &owned_src_ then *src_ was moved, which invalidated iter_ and
    // buffer pointers.
    iter_ = Chain::BlockIterator(src_, block_index);
    if (src_ == &owned_src_ && start_ != nullptr) {
      const size_t cursor_index = read_from_buffer();
      start_ = iter_->data();
      cursor_ = start_ + cursor_index;
      limit_ = start_ + iter_->size();
    }
  }
  return *this;
}

inline bool ChainReader::Size(Position* size) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  *size = src_->size();
  return true;
}

}  // namespace riegeli

#endif  // RIEGELI_BYTES_CHAIN_READER_H_
