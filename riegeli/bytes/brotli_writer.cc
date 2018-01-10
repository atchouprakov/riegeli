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

#include "riegeli/bytes/brotli_writer.h"

#include <stddef.h>
#include <stdint.h>
#include <limits>
#include <memory>
#include <utility>

#include "brotli/encode.h"
#include "riegeli/base/assert.h"
#include "riegeli/base/base.h"
#include "riegeli/base/string_view.h"
#include "riegeli/bytes/buffered_writer.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

BrotliWriter::BrotliWriter() : dest_(nullptr), compressor_(nullptr) {
  MarkCancelled();
}

BrotliWriter::BrotliWriter(std::unique_ptr<Writer> dest, Options options)
    : BrotliWriter(dest.get(), options) {
  owned_dest_ = std::move(dest);
}

BrotliWriter::BrotliWriter(Writer* dest, Options options)
    : BufferedWriter(options.buffer_size_),
      dest_(RIEGELI_ASSERT_NOTNULL(dest)),
      compressor_(BrotliEncoderCreateInstance(nullptr, nullptr, nullptr)) {
  if (RIEGELI_UNLIKELY(compressor_ == nullptr)) {
    Fail("BrotliEncoderCreateInstance() failed");
    return;
  }
  if (RIEGELI_UNLIKELY(!BrotliEncoderSetParameter(
          compressor_, BROTLI_PARAM_QUALITY, options.compression_level_))) {
    Fail("BrotliEncoderSetParameter() failed");
    return;
  }
  if (options.size_hint_ > 0) {
    // Ignore errors from tuning.
    BrotliEncoderSetParameter(
        compressor_, BROTLI_PARAM_SIZE_HINT,
        UnsignedMin(options.size_hint_, std::numeric_limits<uint32_t>::max()));
  }
}

BrotliWriter::BrotliWriter(BrotliWriter&& src) noexcept
    : BufferedWriter(std::move(src)),
      dest_(riegeli::exchange(src.dest_, nullptr)),
      owned_dest_(std::move(src.owned_dest_)),
      compressor_(riegeli::exchange(src.compressor_, nullptr)) {}

BrotliWriter& BrotliWriter::operator=(BrotliWriter&& src) noexcept {
  if (&src != this) {
    BufferedWriter::operator=(std::move(src));
    dest_ = riegeli::exchange(src.dest_, nullptr);
    owned_dest_ = std::move(src.owned_dest_);
    compressor_ = riegeli::exchange(src.compressor_, nullptr);
  }
  return *this;
}

BrotliWriter::~BrotliWriter() { Cancel(); }

void BrotliWriter::Done() {
  if (RIEGELI_LIKELY(healthy())) {
    WriteInternal(string_view(start_, written_to_buffer()),
                  BROTLI_OPERATION_FINISH);
  }
  if (RIEGELI_LIKELY(healthy())) {
    if (owned_dest_ != nullptr) {
      if (RIEGELI_UNLIKELY(!owned_dest_->Close())) Fail(owned_dest_->Message());
    }
  } else {
    dest_->Cancel();
  }
  dest_ = nullptr;
  owned_dest_.reset();
  BrotliEncoderDestroyInstance(compressor_);
  compressor_ = nullptr;
  BufferedWriter::Done();
}

bool BrotliWriter::Flush(FlushType flush_type) {
  if (RIEGELI_UNLIKELY(!healthy())) return false;
  const size_t buffered_length = written_to_buffer();
  cursor_ = start_;
  if (RIEGELI_UNLIKELY(!WriteInternal(string_view(start_, buffered_length),
                                      BROTLI_OPERATION_FLUSH))) {
    return false;
  }
  if (RIEGELI_UNLIKELY(!dest_->Flush(flush_type))) {
    if (dest_->healthy()) return false;
    return Fail(dest_->Message());
  }
  return true;
}

bool BrotliWriter::WriteInternal(string_view src) {
  RIEGELI_ASSERT(!src.empty());
  RIEGELI_ASSERT(healthy());
  return WriteInternal(src, BROTLI_OPERATION_PROCESS);
}

inline bool BrotliWriter::WriteInternal(string_view src,
                                        BrotliEncoderOperation op) {
  RIEGELI_ASSERT(healthy());
  size_t available_in = src.size();
  const uint8_t* next_in = reinterpret_cast<const uint8_t*>(src.data());
  size_t available_out = 0;
  for (;;) {
    if (RIEGELI_UNLIKELY(!BrotliEncoderCompressStream(
            compressor_, op, &available_in, &next_in, &available_out, nullptr,
            nullptr))) {
      return Fail("BrotliEncoderCompressStream() failed");
    }
    size_t length = 0;
    const char* const data = reinterpret_cast<const char*>(
        BrotliEncoderTakeOutput(compressor_, &length));
    if (length > 0) {
      if (RIEGELI_UNLIKELY(!dest_->Write(string_view(data, length)))) {
        RIEGELI_ASSERT(!dest_->healthy());
        return Fail(dest_->Message());
      }
    } else if (available_in == 0) {
      start_pos_ += src.size();
      return true;
    }
  }
}

}  // namespace riegeli