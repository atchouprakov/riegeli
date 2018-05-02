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

#include "riegeli/records/chunk_reader.h"

#include <stddef.h>
#include <memory>
#include <new>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/strings/str_cat.h"
#include "riegeli/base/base.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/chunk_encoding/chunk.h"
#include "riegeli/chunk_encoding/hash.h"
#include "riegeli/chunk_encoding/types.h"
#include "riegeli/records/block.h"

namespace riegeli {

ChunkReader::ChunkReader(std::unique_ptr<Reader> byte_reader)
    : ChunkReader(byte_reader.get()) {
  owned_byte_reader_ = std::move(byte_reader);
}

ChunkReader::ChunkReader(Reader* byte_reader)
    : Object(State::kOpen),
      byte_reader_(RIEGELI_ASSERT_NOTNULL(byte_reader)),
      pos_(byte_reader_->pos()) {
  if (ABSL_PREDICT_FALSE(!internal::IsPossibleChunkBoundary(pos_))) {
    recoverable_ = Recoverable::kFindChunk;
    recoverable_pos_ = pos_;
    Fail(absl::StrCat("Invalid chunk boundary: ", pos_));
  }
}

void ChunkReader::Done() {
  recoverable_ = Recoverable::kNo;
  recoverable_pos_ = 0;
  if (ABSL_PREDICT_TRUE(healthy()) &&
      ABSL_PREDICT_FALSE(current_chunk_is_incomplete_)) {
    RIEGELI_ASSERT_GT(byte_reader_->pos(), pos_)
        << "Failed invariant of ChunkReader: a chunk beginning must have been "
           "read for the chunk to be considered incomplete";
    recoverable_ = Recoverable::kReportSkippedBytes;
    recoverable_pos_ = byte_reader_->pos() - pos_;
    Fail(absl::StrCat("Truncated Riegeli/records file, incomplete chunk at ",
                      pos_, " with length ", recoverable_pos_));
  }
  if (owned_byte_reader_ != nullptr) {
    if (ABSL_PREDICT_TRUE(healthy())) {
      if (ABSL_PREDICT_FALSE(!owned_byte_reader_->Close())) {
        Fail(*owned_byte_reader_);
      }
    }
    owned_byte_reader_.reset();
  }
  byte_reader_ = nullptr;
  pos_ = 0;
  chunk_.Close();
  current_chunk_is_incomplete_ = false;
}

inline bool ChunkReader::ReadingFailed() {
  if (ABSL_PREDICT_TRUE(byte_reader_->healthy())) {
    if (ABSL_PREDICT_FALSE(byte_reader_->pos() > pos_)) {
      current_chunk_is_incomplete_ = true;
    }
    return false;
  }
  return Fail(*byte_reader_);
}

bool ChunkReader::CheckFileFormat() { return PullChunkHeader(nullptr); }

bool ChunkReader::ReadChunk(Chunk* chunk) {
  if (ABSL_PREDICT_FALSE(!PullChunkHeader(nullptr))) return false;

  while (chunk_.data.size() < chunk_.header.data_size()) {
    const Position pos_before = byte_reader_->pos();
    if (ABSL_PREDICT_FALSE(!ReadBlockHeader())) return false;
    if (internal::IsBlockBoundary(pos_before) &&
        ABSL_PREDICT_FALSE(block_header_.previous_chunk() !=
                           pos_before - pos_)) {
      recoverable_ = Recoverable::kFindChunk;
      recoverable_pos_ = byte_reader_->pos();
      return Fail(absl::StrCat(
          "Invalid Riegeli/records file: chunk boundary is ", pos_,
          " but block header at ", pos_before,
          " implies a different previous chunk boundary: ",
          pos_before >= block_header_.previous_chunk()
              ? absl::StrCat(pos_before - block_header_.previous_chunk())
              : absl::StrCat("-",
                             block_header_.previous_chunk() - pos_before)));
    }
    if (ABSL_PREDICT_FALSE(!byte_reader_->Read(
            &chunk_.data,
            IntCast<size_t>(UnsignedMin(
                chunk_.header.data_size() - chunk_.data.size(),
                internal::RemainingInBlock(byte_reader_->pos())))))) {
      return ReadingFailed();
    }
  }

  const Position chunk_end = internal::ChunkEnd(chunk_.header, pos_);

  const uint64_t computed_data_hash = internal::Hash(chunk_.data);
  if (ABSL_PREDICT_FALSE(computed_data_hash != chunk_.header.data_hash())) {
    // Recoverable::kHaveChunk, not Recoverable::kFindChunk, because while chunk
    // data are invalid, chunk header has a correct hash, and thus the next
    // chunk is believed to be present after this chunk.
    recoverable_ = Recoverable::kHaveChunk;
    recoverable_pos_ = chunk_end;
    return Fail(absl::StrCat(
        "Corrupted Riegeli/records file: chunk data hash mismatch (computed 0x",
        absl::Hex(computed_data_hash, absl::PadSpec::kZeroPad16), ", stored 0x",
        absl::Hex(chunk_.header.data_hash(), absl::PadSpec::kZeroPad16),
        "), chunk at ", pos_, " with length ", chunk_end - pos_));
  }

  *chunk = std::move(chunk_);
  pos_ = chunk_end;
  chunk_.Reset();
  return true;
}

bool ChunkReader::PullChunkHeader(const ChunkHeader** chunk_header) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  current_chunk_is_incomplete_ = false;

  if (ABSL_PREDICT_FALSE(byte_reader_->pos() < pos_)) {
    if (ABSL_PREDICT_FALSE(!byte_reader_->Seek(pos_))) return ReadingFailed();
  }

  const Position chunk_header_read =
      internal::DistanceWithoutOverhead(pos_, byte_reader_->pos());
  if (chunk_header_read < chunk_.header.size()) {
    if (ABSL_PREDICT_FALSE(!ReadChunkHeader())) return false;
  }
  if (chunk_header != nullptr) *chunk_header = &chunk_.header;
  return true;
}

inline bool ChunkReader::ReadChunkHeader() {
  RIEGELI_ASSERT(healthy())
      << "Failed precondition of ChunkReader::ReadChunkReader(): " << message();
  RIEGELI_ASSERT_LT(
      internal::DistanceWithoutOverhead(pos_, byte_reader_->pos()),
      chunk_.header.size())
      << "Failed precondition of ChunkReader::ReadChunkHeader(): "
         "chunk header already read";
  size_t remaining_length;
  size_t length_to_read;
  do {
    const Position pos_before = byte_reader_->pos();
    if (ABSL_PREDICT_FALSE(!ReadBlockHeader())) return false;
    if (internal::IsBlockBoundary(pos_before) &&
        ABSL_PREDICT_FALSE(block_header_.previous_chunk() !=
                           pos_before - pos_)) {
      recoverable_ = Recoverable::kFindChunk;
      recoverable_pos_ = byte_reader_->pos();
      return Fail(absl::StrCat(
          "Invalid Riegeli/records file: chunk boundary is ", pos_,
          " but block header at ", pos_before,
          " implies a different previous chunk boundary: ",
          pos_before >= block_header_.previous_chunk()
              ? absl::StrCat(pos_before - block_header_.previous_chunk())
              : absl::StrCat("-",
                             block_header_.previous_chunk() - pos_before)));
    }
    const size_t chunk_header_read = IntCast<size_t>(
        internal::DistanceWithoutOverhead(pos_, byte_reader_->pos()));
    remaining_length = chunk_.header.size() - chunk_header_read;
    length_to_read = UnsignedMin(
        remaining_length, internal::RemainingInBlock(byte_reader_->pos()));
    if (ABSL_PREDICT_FALSE(!byte_reader_->Read(
            chunk_.header.bytes() + chunk_header_read, length_to_read))) {
      return ReadingFailed();
    }
  } while (length_to_read < remaining_length);

  const uint64_t computed_header_hash = chunk_.header.computed_header_hash();
  if (ABSL_PREDICT_FALSE(computed_header_hash !=
                         chunk_.header.stored_header_hash())) {
    recoverable_ = Recoverable::kFindChunk;
    recoverable_pos_ = byte_reader_->pos();
    return Fail(absl::StrCat(
        "Corrupted Riegeli/records file: chunk header hash mismatch "
        "(computed 0x",
        absl::Hex(computed_header_hash, absl::PadSpec::kZeroPad16),
        ", stored 0x",
        absl::Hex(chunk_.header.stored_header_hash(),
                  absl::PadSpec::kZeroPad16),
        "), chunk at ", pos_));
  }
  if (pos_ == 0) {
    // Verify file signature.
    if (ABSL_PREDICT_FALSE(chunk_.header.data_size() != 0 ||
                           chunk_.header.chunk_type() !=
                               ChunkType::kFileSignature ||
                           chunk_.header.num_records() != 0 ||
                           chunk_.header.decoded_data_size() != 0)) {
      recoverable_ = Recoverable::kFindChunk;
      recoverable_pos_ = byte_reader_->pos();
      return Fail("Invalid Riegeli/records file: missing file signature");
    }
  }
  return true;
}

inline bool ChunkReader::ReadBlockHeader() {
  const size_t remaining_length =
      internal::RemainingInBlockHeader(byte_reader_->pos());
  if (remaining_length == 0) return true;
  if (ABSL_PREDICT_FALSE(!byte_reader_->Read(
          block_header_.bytes() + block_header_.size() - remaining_length,
          remaining_length))) {
    return ReadingFailed();
  }
  const uint64_t computed_header_hash = block_header_.computed_header_hash();
  if (ABSL_PREDICT_FALSE(computed_header_hash !=
                         block_header_.stored_header_hash())) {
    recoverable_ = Recoverable::kFindChunk;
    recoverable_pos_ = byte_reader_->pos();
    return Fail(absl::StrCat(
        "Corrupted Riegeli/records file: block header hash mismatch "
        "(computed 0x",
        absl::Hex(computed_header_hash, absl::PadSpec::kZeroPad16),
        ", stored 0x",
        absl::Hex(block_header_.stored_header_hash(),
                  absl::PadSpec::kZeroPad16),
        "), block at ",
        recoverable_pos_ - recoverable_pos_ % internal::kBlockSize()));
  }
  return true;
}

bool ChunkReader::Recover(Position* skipped_bytes) {
  if (recoverable_ == Recoverable::kNo) return false;
again:
  RIEGELI_ASSERT(!healthy()) << "Failed invariant of ChunkReader: "
                                "recovery applicable but ChunkReader healthy";
  const Recoverable recoverable = recoverable_;
  recoverable_ = Recoverable::kNo;
  Position recoverable_pos = recoverable_pos_;
  recoverable_pos_ = 0;
  RIEGELI_ASSERT_GT(recoverable_pos, pos_)
      << "Failed invariant of ChunkReader: recovery must be forwards";
  if (recoverable == Recoverable::kFindChunk) {
    recoverable_pos += internal::RemainingInBlock(recoverable_pos);
  }
  if (skipped_bytes != nullptr) {
    *skipped_bytes = SaturatingAdd(*skipped_bytes, recoverable_pos - pos_);
  }
  MarkNotFailed();
  if (recoverable == Recoverable::kReportSkippedBytes) {
    RIEGELI_ASSERT(closed()) << "Failed invariant of ChunkReader: "
                                "recovery only reports skipped bytes "
                                "but ChunkReader is not closed";
    RIEGELI_ASSERT_EQ(pos_, 0u) << "Failed invariant of ChunkReader: "
                                   "ChunkReader is closed "
                                   "but the current position is not 0";
    return true;
  }
  RIEGELI_ASSERT(!closed()) << "Failed invariant of ChunkReader: "
                               "recovery does not only report skipped bytes "
                               "but ChunkReader is closed";
  pos_ = recoverable_pos;
  chunk_.Reset();
  if (recoverable == Recoverable::kHaveChunk) return true;

find_chunk:
  if (ABSL_PREDICT_FALSE(!byte_reader_->Seek(pos_))) {
    if (ABSL_PREDICT_TRUE(byte_reader_->healthy())) {
      // pos_ is a block boundary, so the next read will verify whether this is
      // also a chunk boundary, asking to recover again if not.
      return true;
    }
    return Fail(*byte_reader_);
  }

  if (ABSL_PREDICT_FALSE(!ReadBlockHeader())) {
    if (recoverable_ != Recoverable::kNo) goto again;
    return true;
  }
  if (block_header_.previous_chunk() == 0) {
    // A chunk boundary coincides with block boundary. Recovery is done.
  } else {
    const Position next_chunk =
        ABSL_PREDICT_FALSE(block_header_.next_chunk() == 0)
            ? internal::kBlockSize()
            : block_header_.next_chunk();
    if (skipped_bytes != nullptr) {
      *skipped_bytes = SaturatingAdd(*skipped_bytes, next_chunk);
    }
    pos_ += next_chunk;
    if (ABSL_PREDICT_FALSE(block_header_.next_chunk() == 0 ||
                           !internal::IsPossibleChunkBoundary(pos_))) {
      goto find_chunk;
    }
  }
  return true;
}

bool ChunkReader::Seek(Position new_pos) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  pos_ = new_pos;
  chunk_.Reset();
  current_chunk_is_incomplete_ = false;
  if (ABSL_PREDICT_FALSE(!byte_reader_->Seek(pos_))) {
    if (ABSL_PREDICT_FALSE(!byte_reader_->healthy())) {
      return Fail(*byte_reader_);
    }
  }
  if (ABSL_PREDICT_FALSE(!internal::IsPossibleChunkBoundary(pos_))) {
    recoverable_ = Recoverable::kFindChunk;
    recoverable_pos_ = pos_;
    return Fail(absl::StrCat("Invalid chunk boundary: ", pos_));
  }
  return true;
}

bool ChunkReader::SeekToChunkContaining(Position new_pos) {
  return SeekToChunk(new_pos, true);
}

bool ChunkReader::SeekToChunkAfter(Position new_pos) {
  return SeekToChunk(new_pos, false);
}

inline bool ChunkReader::SeekToChunk(Position new_pos, bool containing) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  chunk_.Reset();
  current_chunk_is_incomplete_ = false;
  Position block_begin = new_pos - new_pos % internal::kBlockSize();
  if (block_begin > 0) {
    Position size;
    if (ABSL_PREDICT_TRUE(byte_reader_->Size(&size))) {
      Position max_block_begin =
          SaturatingSub(size, internal::BlockHeader::size());
      block_begin = UnsignedMin(
          block_begin,
          max_block_begin - max_block_begin % internal::kBlockSize());
    }
  }
  if (pos_ <= new_pos) {
    // The current chunk begins at or before new_pos. If it also ends at or
    // after block_begin, it is better to start searching from the current
    // position than to seek back to block_begin.
    if (pos_ == new_pos) return true;
    if (ABSL_PREDICT_FALSE(!PullChunkHeader(nullptr))) return false;
    const Position chunk_end = internal::ChunkEnd(chunk_.header, pos_);
    if (chunk_end < block_begin) {
      // The current chunk ends too early. Skip to block_begin.
      goto read_block_header;
    }
    if (containing && pos_ + chunk_.header.num_records() > new_pos) {
      return true;
    }
    pos_ = chunk_end;
  } else {
  read_block_header:
    pos_ = block_begin;
    if (ABSL_PREDICT_FALSE(!byte_reader_->Seek(pos_))) {
      if (ABSL_PREDICT_TRUE(byte_reader_->healthy())) return false;
      return Fail(*byte_reader_);
    }
    if (ABSL_PREDICT_FALSE(!ReadBlockHeader())) return false;
    if (block_header_.previous_chunk() == 0) {
      // A chunk boundary coincides with block boundary. The current position is
      // already before the chunk header; start searching from this chunk,
      // skipping seeking back and reading the block header again.
      goto check_current_chunk;
    }
    pos_ = block_begin + block_header_.next_chunk();
    if (containing && pos_ > new_pos) {
      // new_pos is inside the chunk which contains this block boundary, so
      // start the search from this chunk instead of the next chunk.
      if (ABSL_PREDICT_FALSE(block_header_.previous_chunk() > block_begin)) {
        recoverable_ = Recoverable::kFindChunk;
        recoverable_pos_ = byte_reader_->pos();
        return Fail(absl::StrCat(
            "Invalid Riegeli/records file: block header at ", block_begin,
            " implies a negative previous chunk boundary: -",
            block_header_.previous_chunk() - block_begin));
      }
      pos_ = block_begin - block_header_.previous_chunk();
    }
    if (ABSL_PREDICT_FALSE(!internal::IsPossibleChunkBoundary(pos_))) {
      recoverable_ = Recoverable::kFindChunk;
      recoverable_pos_ = byte_reader_->pos();
      return Fail(absl::StrCat("Invalid Riegeli/records file: block header at ",
                               block_begin,
                               " implies an invalid chunk boundary: ", pos_));
    }
  }

  for (;;) {
    if (ABSL_PREDICT_FALSE(!byte_reader_->Seek(pos_))) {
      if (ABSL_PREDICT_TRUE(byte_reader_->healthy())) return false;
      return Fail(*byte_reader_);
    }
  check_current_chunk:
    if (pos_ >= new_pos) return true;
    if (ABSL_PREDICT_FALSE(!ReadChunkHeader())) return false;
    if (containing && pos_ + chunk_.header.num_records() > new_pos) {
      return true;
    }
    pos_ = internal::ChunkEnd(chunk_.header, pos_);
  }
}

}  // namespace riegeli
