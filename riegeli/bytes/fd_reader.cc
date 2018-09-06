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

// Make pread() available.
#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE < 500
#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

// Make file offsets 64-bit even on 32-bit systems.
#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

#include "riegeli/bytes/fd_reader.h"

#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
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
#include "riegeli/base/chain.h"
#include "riegeli/base/memory_estimator.h"
#include "riegeli/base/object.h"
#include "riegeli/base/str_error.h"
#include "riegeli/bytes/backward_writer.h"
#include "riegeli/bytes/buffered_reader.h"
#include "riegeli/bytes/fd_holder.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

namespace {

class MMapRef {
 public:
  MMapRef(void* data, size_t size) : data_(data), size_(size) {}

  MMapRef(MMapRef&& src) noexcept;
  MMapRef& operator=(MMapRef&& src) noexcept;

  ~MMapRef();

  absl::string_view data() const {
    return absl::string_view(static_cast<const char*>(data_), size_);
  }
  void RegisterSubobjects(absl::string_view data,
                          MemoryEstimator* memory_estimator) const;
  void DumpStructure(absl::string_view data, std::ostream& out) const;

 private:
  void* data_;
  size_t size_;
};

MMapRef::MMapRef(MMapRef&& src) noexcept
    : data_(riegeli::exchange(src.data_, nullptr)),
      size_(riegeli::exchange(src.size_, 0)) {}

MMapRef& MMapRef::operator=(MMapRef&& src) noexcept {
  // Exchange data_ early to support self-assignment.
  void* const data = riegeli::exchange(src.data_, nullptr);
  if (data_ != nullptr) {
    const int result = munmap(data_, size_);
    RIEGELI_CHECK_EQ(result, 0) << "munmap() failed: " << StrError(errno);
  }
  data_ = data;
  size_ = riegeli::exchange(src.size_, 0);
  return *this;
}

MMapRef::~MMapRef() {
  if (data_ != nullptr) {
    const int result = munmap(data_, size_);
    RIEGELI_CHECK_EQ(result, 0) << "munmap() failed: " << StrError(errno);
  }
}

void MMapRef::RegisterSubobjects(absl::string_view data,
                                 MemoryEstimator* memory_estimator) const {}

void MMapRef::DumpStructure(absl::string_view data, std::ostream& out) const {
  out << "mmap";
}

}  // namespace

namespace internal {

inline FdReaderBase::FdReaderBase(int fd, bool owns_fd, size_t buffer_size)
    : BufferedReader(UnsignedMin(buffer_size,
                                 Position{std::numeric_limits<off_t>::max()})),
      owned_fd_(owns_fd ? fd : -1),
      fd_(fd),
      filename_(fd == 0 ? "/dev/stdin" : absl::StrCat("/proc/self/fd/", fd)) {
  RIEGELI_ASSERT_GE(fd, 0)
      << "Failed precondition of FdReaderBase::FdReaderBase(int): "
         "negative file descriptor";
}

inline FdReaderBase::FdReaderBase(absl::string_view filename, int flags,
                                  bool owns_fd, size_t buffer_size)
    : BufferedReader(UnsignedMin(buffer_size,
                                 Position{std::numeric_limits<off_t>::max()})),
      filename_(filename) {
  RIEGELI_ASSERT((flags & O_ACCMODE) == O_RDONLY ||
                 (flags & O_ACCMODE) == O_RDWR)
      << "Failed precondition of FdReaderBase::FdReaderBase(string_view): "
         "flags must include O_RDONLY or O_RDWR";
again:
  fd_ = open(filename_.c_str(), flags, 0666);
  if (ABSL_PREDICT_FALSE(fd_ < 0)) {
    const int error_code = errno;
    if (error_code == EINTR) goto again;
    FailOperation("open()", error_code);
    return;
  }
  if (owns_fd) owned_fd_ = FdHolder(fd_);
}

void FdReaderBase::Done() {
  if (ABSL_PREDICT_TRUE(healthy())) MaybeSyncPos();
  if (owned_fd_.fd() >= 0) {
    const int error_code = owned_fd_.Close();
    if (ABSL_PREDICT_FALSE(error_code != 0) && ABSL_PREDICT_TRUE(healthy())) {
      FailOperation(FdHolder::CloseFunctionName(), error_code);
    }
    fd_ = -1;
  }
  BufferedReader::Done();
}

inline bool FdReaderBase::FailOperation(absl::string_view operation,
                                        int error_code) {
  error_code_ = error_code;
  return Fail(absl::StrCat(operation, " failed: ", StrError(error_code),
                           ", reading ", filename_));
}

}  // namespace internal

FdReader::FdReader(int fd, Options options)
    : FdReaderBase(fd, options.owns_fd_, options.buffer_size_),
      sync_pos_(options.sync_pos_) {
  InitializePos();
}

FdReader::FdReader(absl::string_view filename, int flags, Options options)
    : FdReaderBase(filename, flags, options.owns_fd_, options.buffer_size_),
      sync_pos_(options.sync_pos_) {
  if (ABSL_PREDICT_TRUE(healthy())) InitializePos();
}

inline void FdReader::InitializePos() {
  if (sync_pos_) {
    const off_t result = lseek(fd_, 0, SEEK_CUR);
    if (ABSL_PREDICT_FALSE(result < 0)) {
      FailOperation("lseek()", errno);
      return;
    }
    limit_pos_ = IntCast<Position>(result);
  }
}

bool FdReader::MaybeSyncPos() {
  if (sync_pos_) {
    if (ABSL_PREDICT_FALSE(lseek(fd_, IntCast<off_t>(pos()), SEEK_SET) < 0)) {
      return FailOperation("lseek()", errno);
    }
  }
  return true;
}

bool FdReader::ReadInternal(char* dest, size_t min_length, size_t max_length) {
  RIEGELI_ASSERT_GT(min_length, 0u)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "nothing to read";
  RIEGELI_ASSERT_GE(max_length, min_length)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "max_length < min_length";
  RIEGELI_ASSERT(healthy())
      << "Failed precondition of BufferedReader::ReadInternal(): " << message();
  if (ABSL_PREDICT_FALSE(max_length >
                         Position{std::numeric_limits<off_t>::max()} -
                             limit_pos_)) {
    return FailOverflow();
  }
  for (;;) {
  again:
    const ssize_t result = pread(
        fd_, dest,
        UnsignedMin(max_length, size_t{std::numeric_limits<ssize_t>::max()}),
        IntCast<off_t>(limit_pos_));
    if (ABSL_PREDICT_FALSE(result < 0)) {
      const int error_code = errno;
      if (error_code == EINTR) goto again;
      return FailOperation("pread()", error_code);
    }
    if (ABSL_PREDICT_FALSE(result == 0)) return false;
    RIEGELI_ASSERT_LE(IntCast<size_t>(result), max_length)
        << "pread() read more than requested";
    limit_pos_ += IntCast<size_t>(result);
    if (IntCast<size_t>(result) >= min_length) return true;
    dest += result;
    min_length -= IntCast<size_t>(result);
    max_length -= IntCast<size_t>(result);
  }
}

bool FdReader::SeekSlow(Position new_pos) {
  RIEGELI_ASSERT(new_pos < start_pos() || new_pos > limit_pos_)
      << "Failed precondition of Reader::SeekSlow(): "
         "position in the buffer, use Seek() instead";
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  if (new_pos > limit_pos_) {
    // Seeking forwards.
    struct stat stat_info;
    if (ABSL_PREDICT_FALSE(fstat(fd_, &stat_info) < 0)) {
      const int error_code = errno;
      return FailOperation("fstat()", error_code);
    }
    if (ABSL_PREDICT_FALSE(new_pos > IntCast<Position>(stat_info.st_size))) {
      // File ends.
      ClearBuffer();
      limit_pos_ = IntCast<Position>(stat_info.st_size);
      return false;
    }
  }
  ClearBuffer();
  limit_pos_ = new_pos;
  PullSlow();
  return true;
}

bool FdReader::Size(Position* size) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  struct stat stat_info;
  if (ABSL_PREDICT_FALSE(fstat(fd_, &stat_info) < 0)) {
    const int error_code = errno;
    return FailOperation("fstat()", error_code);
  }
  *size = IntCast<Position>(stat_info.st_size);
  return true;
}

FdStreamReader::FdStreamReader(int fd, Options options)
    : FdReaderBase(fd, true, options.buffer_size_) {
  RIEGELI_ASSERT(options.assumed_pos_.has_value())
      << "Failed precondition of FdStreamReader::FdStreamReader(int): "
         "assumed file position must be specified "
         "if FdStreamReader does not open the file";
  limit_pos_ = *options.assumed_pos_;
}

FdStreamReader::FdStreamReader(absl::string_view filename, int flags,
                               Options options)
    : FdReaderBase(filename, flags, true, options.buffer_size_) {
  if (ABSL_PREDICT_FALSE(!healthy())) return;
  limit_pos_ = options.assumed_pos_.value_or(0);
}

bool FdStreamReader::ReadInternal(char* dest, size_t min_length,
                                  size_t max_length) {
  RIEGELI_ASSERT_GT(min_length, 0u)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "nothing to read";
  RIEGELI_ASSERT_GE(max_length, min_length)
      << "Failed precondition of BufferedReader::ReadInternal(): "
         "max_length < min_length";
  RIEGELI_ASSERT(healthy())
      << "Failed precondition of BufferedReader::ReadInternal(): " << message();
  if (ABSL_PREDICT_FALSE(max_length >
                         Position{std::numeric_limits<off_t>::max()} -
                             limit_pos_)) {
    return FailOverflow();
  }
  for (;;) {
  again:
    const ssize_t result = read(
        fd_, dest,
        UnsignedMin(max_length, size_t{std::numeric_limits<ssize_t>::max()}));
    if (ABSL_PREDICT_FALSE(result < 0)) {
      const int error_code = errno;
      if (error_code == EINTR) goto again;
      return FailOperation("read()", error_code);
    }
    if (ABSL_PREDICT_FALSE(result == 0)) return false;
    RIEGELI_ASSERT_LE(IntCast<size_t>(result), max_length)
        << "read() read more than requested";
    limit_pos_ += IntCast<size_t>(result);
    if (IntCast<size_t>(result) >= min_length) return true;
    dest += result;
    min_length -= IntCast<size_t>(result);
    max_length -= IntCast<size_t>(result);
  }
}

FdMMapReader::FdMMapReader(int fd, Options options)
    : ChainReader(Chain()),
      owned_fd_(options.owns_fd_ ? fd : -1),
      fd_(fd),
      filename_(fd == 0 ? "/dev/stdin" : absl::StrCat("/proc/self/fd/", fd)),
      sync_pos_(options.sync_pos_) {
  RIEGELI_ASSERT_GE(fd, 0)
      << "Failed precondition of FdMMapReader::FdMMapReader(int): "
         "negative file descriptor";
  Initialize(options);
}

FdMMapReader::FdMMapReader(absl::string_view filename, int flags,
                           Options options)
    : ChainReader(Chain()), filename_(filename) {
  RIEGELI_ASSERT((flags & O_ACCMODE) == O_RDONLY ||
                 (flags & O_ACCMODE) == O_RDWR)
      << "Failed precondition of FdMMapReader::FdMMapReader(string_view): "
         "flags must include O_RDONLY or O_RDWR";
again:
  fd_ = open(filename_.c_str(), flags, 0666);
  if (ABSL_PREDICT_FALSE(fd_ < 0)) {
    const int error_code = errno;
    if (error_code == EINTR) goto again;
    FailOperation("open()", error_code);
    return;
  }
  if (options.owns_fd_) owned_fd_ = internal::FdHolder(fd_);
  Initialize(options);
}

void FdMMapReader::Done() {
  if (ABSL_PREDICT_TRUE(healthy()) && sync_pos_) {
    if (ABSL_PREDICT_FALSE(lseek(fd_, IntCast<off_t>(pos()), SEEK_SET) < 0)) {
      FailOperation("lseek()", errno);
    }
  }
  limit_pos_ = pos();
  // TODO: Do it without const_cast by providing a non-const accessor
  // to the source of an owning ChainReader.
  const_cast<Chain&>(src()) = Chain();
  if (owned_fd_.fd() >= 0) {
    const int error_code = owned_fd_.Close();
    if (ABSL_PREDICT_FALSE(error_code != 0) && ABSL_PREDICT_TRUE(healthy())) {
      FailOperation(internal::FdHolder::CloseFunctionName(), error_code);
    }
    fd_ = -1;
  }
  Reader::Done();
}

inline void FdMMapReader::Initialize(Options options) {
  struct stat stat_info;
  if (ABSL_PREDICT_FALSE(fstat(fd_, &stat_info) < 0)) {
    const int error_code = errno;
    FailOperation("fstat()", error_code);
    return;
  }
  if (ABSL_PREDICT_FALSE(IntCast<Position>(stat_info.st_size) >
                         std::numeric_limits<size_t>::max())) {
    Fail("File is too large for mmap()");
    return;
  }
  if (stat_info.st_size != 0) {
    void* const data = mmap(nullptr, IntCast<size_t>(stat_info.st_size),
                            PROT_READ, MAP_SHARED, fd_, 0);
    if (ABSL_PREDICT_FALSE(data == MAP_FAILED)) {
      const int error_code = errno;
      FailOperation("mmap()", error_code);
      return;
    }
    Chain contents;
    contents.AppendExternal(MMapRef(data, IntCast<size_t>(stat_info.st_size)));
    ChainReader::operator=(ChainReader(std::move(contents)));
    if (sync_pos_) {
      const off_t result = lseek(fd_, 0, SEEK_CUR);
      if (ABSL_PREDICT_FALSE(result < 0)) {
        FailOperation("lseek()", errno);
        return;
      }
      cursor_ += UnsignedMin(IntCast<Position>(result), available());
    }
  }
}

inline bool FdMMapReader::FailOperation(absl::string_view operation,
                                        int error_code) {
  error_code_ = error_code;
  return Fail(absl::StrCat(operation, " failed: ", StrError(error_code),
                           ", reading ", filename_));
}

}  // namespace riegeli
