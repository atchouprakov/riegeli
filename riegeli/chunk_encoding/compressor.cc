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

#include "riegeli/chunk_encoding/compressor.h"

#include <stdint.h>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/types/variant.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/brotli_writer.h"
#include "riegeli/bytes/chain_writer.h"
#include "riegeli/bytes/writer.h"
#include "riegeli/bytes/writer_utils.h"
#include "riegeli/bytes/zstd_writer.h"
#include "riegeli/chunk_encoding/compressor_options.h"
#include "riegeli/chunk_encoding/constants.h"

namespace riegeli {
namespace internal {

Compressor::Compressor(CompressorOptions options, uint64_t size_hint)
    : Object(State::kOpen),
      options_(std::move(options)),
      size_hint_(size_hint) {
  Reset();
}

void Compressor::Reset() {
  MarkHealthy();
  compressed_.Clear();
  ChainWriter<> compressed_writer(
      &compressed_,
      ChainWriterBase::Options().set_size_hint(
          options_.compression_type() == CompressionType::kNone ? size_hint_
                                                                : uint64_t{0}));
  switch (options_.compression_type()) {
    case CompressionType::kNone:
      writer_ = std::move(compressed_writer);
      return;
    case CompressionType::kBrotli:
      writer_ = BrotliWriter<ChainWriter<>>(
          std::move(compressed_writer),
          BrotliWriterBase::Options()
              .set_compression_level(options_.compression_level())
              .set_window_log(options_.window_log())
              .set_size_hint(size_hint_));
      return;
    case CompressionType::kZstd:
      writer_ = ZstdWriter<ChainWriter<>>(
          std::move(compressed_writer),
          ZstdWriterBase::Options()
              .set_compression_level(options_.compression_level())
              .set_window_log(options_.window_log())
              .set_size_hint(size_hint_));
      return;
  }
  RIEGELI_ASSERT_UNREACHABLE()
      << "Unknown compression type: "
      << static_cast<unsigned>(options_.compression_type());
}

void Compressor::Done() {
  if (ABSL_PREDICT_FALSE(!writer()->Close())) Fail(*writer());
  compressed_ = Chain();
}

bool Compressor::EncodeAndClose(Writer* dest) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  const Position uncompressed_size = writer()->pos();
  if (ABSL_PREDICT_FALSE(!writer()->Close())) return Fail(*writer());
  if (options_.compression_type() != CompressionType::kNone) {
    if (ABSL_PREDICT_FALSE(
            !WriteVarint64(dest, IntCast<uint64_t>(uncompressed_size)))) {
      return Fail(*dest);
    }
  }
  if (ABSL_PREDICT_FALSE(!dest->Write(std::move(compressed_)))) {
    return Fail(*dest);
  }
  return Close();
}

}  // namespace internal
}  // namespace riegeli
