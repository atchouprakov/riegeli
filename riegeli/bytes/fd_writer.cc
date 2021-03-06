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

// Make pwrite() and ftruncate() available.
#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < 500
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

// Make file offsets 64-bit even on 32-bit systems.
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

#include "riegeli/bytes/fd_writer.h"

#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <limits>
#include <string>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"
#include "riegeli/base/str_error.h"
#include "riegeli/bytes/buffered_writer.h"
#include "riegeli/bytes/fd_dependency.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

namespace internal {

FdWriterCommon::FdWriterCommon(size_t buffer_size)
    : BufferedWriter(UnsignedMin(
          buffer_size, Position{std::numeric_limits<off_t>::max()})) {}

void FdWriterCommon::SetFilename(int dest) {
  if (dest == 1) {
    filename_ = "/dev/stdout";
  } else if (dest == 2) {
    filename_ = "/dev/stderr";
  } else {
    filename_ = absl::StrCat("/proc/self/fd/", dest);
  }
}

int FdWriterCommon::OpenFd(absl::string_view filename, int flags,
                           mode_t permissions) {
  filename_.assign(filename.data(), filename.size());
again:
  const int dest = open(filename_.c_str(), flags, permissions);
  if (ABSL_PREDICT_FALSE(dest < 0)) {
    if (errno == EINTR) goto again;
    FailOperation("open()");
    return -1;
  }
  return dest;
}

bool FdWriterCommon::FailOperation(absl::string_view operation) {
  error_code_ = errno;
  return Fail(absl::StrCat(operation, " failed: ", StrError(error_code_),
                           ", writing ", filename_));
}

}  // namespace internal

void FdWriterBase::Initialize(int flags, int dest) {
  if (sync_pos_) {
    const off_t result = lseek(dest, 0, SEEK_CUR);
    if (ABSL_PREDICT_FALSE(result < 0)) {
      FailOperation("lseek()");
      return;
    }
    start_pos_ = IntCast<Position>(result);
  } else if ((flags & O_APPEND) != 0) {
    struct stat stat_info;
    if (ABSL_PREDICT_FALSE(fstat(dest, &stat_info) < 0)) {
      FailOperation("fstat()");
      return;
    }
    start_pos_ = IntCast<Position>(stat_info.st_size);
  }
}

bool FdWriterBase::SyncPos(int dest) {
  RIEGELI_ASSERT_EQ(written_to_buffer(), 0u)
      << "Failed precondition of FdWriterBase::SyncPos(): buffer not empty";
  if (sync_pos_) {
    if (ABSL_PREDICT_FALSE(lseek(dest, IntCast<off_t>(start_pos_), SEEK_SET) <
                           0)) {
      limit_ = start_;
      return FailOperation("lseek()");
    }
  }
  return true;
}

bool FdWriterBase::WriteInternal(absl::string_view src) {
  RIEGELI_ASSERT(!src.empty())
      << "Failed precondition of BufferedWriter::WriteInternal(): "
         "nothing to write";
  RIEGELI_ASSERT(healthy())
      << "Failed precondition of BufferedWriter::WriteInternal(): "
      << message();
  RIEGELI_ASSERT_EQ(written_to_buffer(), 0u)
      << "Failed precondition of BufferedWriter::WriteInternal(): "
         "buffer not empty";
  const int dest = dest_fd();
  if (ABSL_PREDICT_FALSE(src.size() >
                         Position{std::numeric_limits<off_t>::max()} -
                             start_pos_)) {
    limit_ = start_;
    return FailOverflow();
  }
  do {
  again:
    const ssize_t result = pwrite(
        dest, src.data(),
        UnsignedMin(src.size(), size_t{std::numeric_limits<ssize_t>::max()}),
        IntCast<off_t>(start_pos_));
    if (ABSL_PREDICT_FALSE(result < 0)) {
      if (errno == EINTR) goto again;
      limit_ = start_;
      return FailOperation("pwrite()");
    }
    RIEGELI_ASSERT_GT(result, 0) << "pwrite() returned 0";
    RIEGELI_ASSERT_LE(IntCast<size_t>(result), src.size())
        << "pwrite() wrote more than requested";
    start_pos_ += IntCast<size_t>(result);
    src.remove_prefix(IntCast<size_t>(result));
  } while (!src.empty());
  return true;
}

bool FdWriterBase::Flush(FlushType flush_type) {
  if (ABSL_PREDICT_FALSE(!PushInternal())) return false;
  const int dest = dest_fd();
  if (ABSL_PREDICT_FALSE(!SyncPos(dest))) return false;
  switch (flush_type) {
    case FlushType::kFromObject:
    case FlushType::kFromProcess:
      return true;
    case FlushType::kFromMachine:
      return fsync(dest) == 0;
  }
  RIEGELI_ASSERT_UNREACHABLE()
      << "Unknown flush type: " << static_cast<int>(flush_type);
}

bool FdWriterBase::SeekSlow(Position new_pos) {
  RIEGELI_ASSERT(new_pos < start_pos_ || new_pos > pos())
      << "Failed precondition of Writer::SeekSlow(): "
         "position in the buffer, use Seek() instead";
  if (ABSL_PREDICT_FALSE(!PushInternal())) return false;
  RIEGELI_ASSERT_EQ(written_to_buffer(), 0u)
      << "BufferedWriter::PushInternal() did not empty the buffer";
  if (new_pos >= start_pos_) {
    // Seeking forwards.
    const int dest = dest_fd();
    struct stat stat_info;
    if (ABSL_PREDICT_FALSE(fstat(dest, &stat_info) < 0)) {
      limit_ = start_;
      return FailOperation("fstat()");
    }
    if (ABSL_PREDICT_FALSE(new_pos > IntCast<Position>(stat_info.st_size))) {
      // File ends.
      start_pos_ = IntCast<Position>(stat_info.st_size);
      return false;
    }
  }
  start_pos_ = new_pos;
  return true;
}

bool FdWriterBase::Size(Position* size) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  const int dest = dest_fd();
  struct stat stat_info;
  if (ABSL_PREDICT_FALSE(fstat(dest, &stat_info) < 0)) {
    cursor_ = start_;
    limit_ = start_;
    return FailOperation("fstat()");
  }
  *size = UnsignedMax(IntCast<Position>(stat_info.st_size), pos());
  return true;
}

bool FdWriterBase::Truncate(Position new_size) {
  if (ABSL_PREDICT_FALSE(!PushInternal())) return false;
  RIEGELI_ASSERT_EQ(written_to_buffer(), 0u)
      << "BufferedWriter::PushInternal() did not empty the buffer";
  const int dest = dest_fd();
  if (new_size >= start_pos_) {
    // Seeking forwards.
    struct stat stat_info;
    if (ABSL_PREDICT_FALSE(fstat(dest, &stat_info) < 0)) {
      limit_ = start_;
      return FailOperation("fstat()");
    }
    if (ABSL_PREDICT_FALSE(new_size > IntCast<Position>(stat_info.st_size))) {
      // File ends.
      start_pos_ = IntCast<Position>(stat_info.st_size);
      return false;
    }
  }
again:
  if (ABSL_PREDICT_FALSE(ftruncate(dest, IntCast<off_t>(new_size)) < 0)) {
    if (errno == EINTR) goto again;
    limit_ = start_;
    return FailOperation("ftruncate()");
  }
  start_pos_ = new_size;
  return true;
}

void FdStreamWriterBase::Initialize(int flags, int dest) {
  if ((flags & O_APPEND) != 0) {
    struct stat stat_info;
    if (ABSL_PREDICT_FALSE(fstat(dest, &stat_info) < 0)) {
      FailOperation("fstat()");
      return;
    }
    start_pos_ = IntCast<Position>(stat_info.st_size);
  }
}

bool FdStreamWriterBase::WriteInternal(absl::string_view src) {
  RIEGELI_ASSERT(!src.empty())
      << "Failed precondition of BufferedWriter::WriteInternal(): "
         "nothing to write";
  RIEGELI_ASSERT(healthy())
      << "Failed precondition of BufferedWriter::WriteInternal(): "
      << message();
  RIEGELI_ASSERT_EQ(written_to_buffer(), 0u)
      << "Failed precondition of BufferedWriter::WriteInternal(): "
         "buffer not empty";
  const int dest = dest_fd();
  if (ABSL_PREDICT_FALSE(src.size() >
                         Position{std::numeric_limits<off_t>::max()} -
                             start_pos_)) {
    limit_ = start_;
    return FailOverflow();
  }
  do {
  again:
    const ssize_t result = write(
        dest, src.data(),
        UnsignedMin(src.size(), size_t{std::numeric_limits<ssize_t>::max()}));
    if (ABSL_PREDICT_FALSE(result < 0)) {
      if (errno == EINTR) goto again;
      limit_ = start_;
      return FailOperation("write()");
    }
    RIEGELI_ASSERT_GT(result, 0) << "write() returned 0";
    RIEGELI_ASSERT_LE(IntCast<size_t>(result), src.size())
        << "write() wrote more than requested";
    start_pos_ += IntCast<size_t>(result);
    src.remove_prefix(IntCast<size_t>(result));
  } while (!src.empty());
  return true;
}

bool FdStreamWriterBase::Flush(FlushType flush_type) {
  if (ABSL_PREDICT_FALSE(!PushInternal())) return false;
  const int dest = dest_fd();
  switch (flush_type) {
    case FlushType::kFromObject:
    case FlushType::kFromProcess:
      return true;
    case FlushType::kFromMachine:
      return fsync(dest) == 0;
  }
  RIEGELI_ASSERT_UNREACHABLE()
      << "Unknown flush type: " << static_cast<int>(flush_type);
}

template class FdWriter<OwnedFd>;
template class FdWriter<int>;
template class FdStreamWriter<OwnedFd>;
template class FdStreamWriter<int>;

}  // namespace riegeli
