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

#ifndef RIEGELI_BYTES_FD_READER_H_
#define RIEGELI_BYTES_FD_READER_H_

#include <fcntl.h>
#include <stddef.h>

#include <string>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "absl/utility/utility.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/backward_writer.h"
#include "riegeli/bytes/buffered_reader.h"
#include "riegeli/bytes/chain_reader.h"
#include "riegeli/bytes/fd_dependency.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/writer.h"

namespace riegeli {

namespace internal {

// Implementation shared between FdReader and FdStreamReader.
class FdReaderCommon : public BufferedReader {
 public:
  // Returns the fd being read from. If the fd is owned then changed to -1 by
  // Close(), otherwise unchanged.
  virtual int src_fd() const = 0;

  // Returns the original name of the file being read from (or /dev/stdin or
  // /proc/self/fd/<fd> if fd was given). Unchanged by Close().
  const std::string& filename() const { return filename_; }

 protected:
  FdReaderCommon() noexcept {}

  explicit FdReaderCommon(size_t buffer_size);

  FdReaderCommon(FdReaderCommon&& that) noexcept;
  FdReaderCommon& operator=(FdReaderCommon&& that) noexcept;

  void SetFilename(int src);
  int OpenFd(absl::string_view filename, int flags);
  ABSL_ATTRIBUTE_COLD bool FailOperation(absl::string_view operation);

  std::string filename_;
};

}  // namespace internal

// Template parameter invariant part of FdReader.
class FdReaderBase : public internal::FdReaderCommon {
 public:
  class Options {
   public:
    Options() noexcept {}

    // If nullopt, FdReader will initially get the current fd position, and will
    // set the fd position on Close().
    //
    // If not nullopt, reading will start from this position. The current fd
    // position will not be gotten or set. This is useful for multiple FdReaders
    // concurrently reading from the same fd.
    //
    // Default: nullopt.
    Options& set_initial_pos(absl::optional<Position> initial_pos) & {
      initial_pos_ = initial_pos;
      return *this;
    }
    Options&& set_initial_pos(absl::optional<Position> initial_pos) && {
      return std::move(set_initial_pos(initial_pos));
    }

    // Tunes how much data is buffered after reading from the file.
    //
    // Default: 64K
    Options& set_buffer_size(size_t buffer_size) & {
      RIEGELI_ASSERT_GT(buffer_size, 0u)
          << "Failed precondition of FdReaderBase::Options::set_buffer_size(): "
             "zero buffer size";
      buffer_size_ = buffer_size;
      return *this;
    }
    Options&& set_buffer_size(size_t buffer_size) && {
      return std::move(set_buffer_size(buffer_size));
    }

   private:
    template <typename Src>
    friend class FdReader;

    absl::optional<Position> initial_pos_;
    size_t buffer_size_ = kDefaultBufferSize;
  };

  bool SupportsRandomAccess() const override { return true; }
  bool Size(Position* size) override;

 protected:
  FdReaderBase() noexcept {}

  explicit FdReaderBase(size_t buffer_size, bool sync_pos)
      : FdReaderCommon(buffer_size), sync_pos_(sync_pos) {}

  FdReaderBase(FdReaderBase&& that) noexcept;
  FdReaderBase& operator=(FdReaderBase&& that) noexcept;

  void Initialize(absl::optional<Position> initial_pos, int src);
  void SyncPos(int src);
  bool ReadInternal(char* dest, size_t min_length, size_t max_length) override;
  bool SeekSlow(Position new_pos) override;

  bool sync_pos_ = false;

  // Invariant: limit_pos_ <= numeric_limits<off_t>::max()
};

// Template parameter invariant part of FdStreamReader.
class FdStreamReaderBase : public internal::FdReaderCommon {
 public:
  class Options {
   public:
    Options() noexcept {}

    // If not nullopt, this position will be assumed initially, to be reported
    // by pos(). This is required by the constructor from fd.
    //
    // If nullopt, which is allowed by the constructor from filename, the
    // position will be assumed to be 0.
    //
    // In any case reading will start from the current position.
    //
    // Default: nullopt.
    Options& set_assumed_pos(absl::optional<Position> assumed_pos) & {
      assumed_pos_ = assumed_pos;
      return *this;
    }
    Options&& set_assumed_pos(absl::optional<Position> assumed_pos) && {
      return std::move(set_assumed_pos(assumed_pos));
    }

    // Tunes how much data is buffered after reading from the file.
    //
    // Default: 64K
    Options& set_buffer_size(size_t buffer_size) & {
      RIEGELI_ASSERT_GT(buffer_size, 0u)
          << "Failed precondition of "
             "FdStreamReaderBase::Options::set_buffer_size()";
      buffer_size_ = buffer_size;
      return *this;
    }
    Options&& set_buffer_size(size_t buffer_size) && {
      return std::move(set_buffer_size(buffer_size));
    }

   private:
    template <typename Src>
    friend class FdStreamReader;

    absl::optional<Position> assumed_pos_;
    size_t buffer_size_ = kDefaultBufferSize;
  };

 protected:
  FdStreamReaderBase() noexcept {}

  explicit FdStreamReaderBase(size_t buffer_size)
      : FdReaderCommon(buffer_size) {}

  FdStreamReaderBase(FdStreamReaderBase&& that) noexcept;
  FdStreamReaderBase& operator=(FdStreamReaderBase&& that) noexcept;

  bool ReadInternal(char* dest, size_t min_length, size_t max_length) override;
};

// Template parameter invariant part of FdMMapReader.
class FdMMapReaderBase : public ChainReader<Chain> {
 public:
  class Options {
   public:
    Options() noexcept {}

    // If nullopt, FdMMapReader will initially get the current fd position, and
    // will set the fd position on Close().
    //
    // If not nullopt, reading will start from this position. The current fd
    // position will not be gotten or set. This is useful for multiple
    // FdMMapReaders concurrently reading from the same fd.
    //
    // Default: nullopt.
    Options& set_initial_pos(absl::optional<Position> initial_pos) & {
      initial_pos_ = initial_pos;
      return *this;
    }
    Options&& set_initial_pos(absl::optional<Position> initial_pos) && {
      return std::move(set_initial_pos(initial_pos));
    }

   private:
    template <typename Src>
    friend class FdMMapReader;

    absl::optional<Position> initial_pos_;
  };

  // Returns the fd being read from. If the fd is owned then changed to -1 by
  // Close(), otherwise unchanged.
  virtual int src_fd() const = 0;

  // Returns the original name of the file being read from (or /dev/stdin or
  // /proc/self/fd/<fd> if fd was given). Unchanged by Close().
  const std::string& filename() const { return filename_; }

 protected:
  FdMMapReaderBase() noexcept {}

  explicit FdMMapReaderBase(bool sync_pos)
      // Empty Chain here is a placeholder, it will be replaced in Initialize().
      : ChainReader(Chain()), sync_pos_(sync_pos) {}

  FdMMapReaderBase(FdMMapReaderBase&& that) noexcept;
  FdMMapReaderBase& operator=(FdMMapReaderBase&& that) noexcept;

  void SetFilename(int src);
  int OpenFd(absl::string_view filename, int flags);
  ABSL_ATTRIBUTE_COLD bool FailOperation(absl::string_view operation);
  void Initialize(absl::optional<Position> initial_pos, int src);
  void SyncPos(int src);

  std::string filename_;
  bool sync_pos_ = false;
};

// A Reader which reads from a file descriptor. It supports random access.
//
// The fd should support:
//  * close() - if the fd is owned
//  * pread()
//  * lseek() - unless Options::set_initial_pos(pos)
//  * fstat() - for Seek() or Size()
//
// The Src template parameter specifies the type of the object providing and
// possibly owning the fd being read from. Src must support
// Dependency<int, Src>, e.g. OwnedFd (owned, default), int (not owned).
//
// The fd must not be closed until the FdReader is closed or no longer used.
template <typename Src = OwnedFd>
class FdReader : public FdReaderBase {
 public:
  // Creates a closed FdReader.
  FdReader() noexcept {}

  // Will read from the fd provided by src.
  //
  // type_identity_t<Src> disables template parameter deduction (C++17), letting
  // FdReader(fd) mean FdReader<OwnedFd>(fd) rather than FdReader<int>(fd).
  explicit FdReader(type_identity_t<Src> src, Options options = Options());

  // Opens a file for reading.
  //
  // flags is the second argument of open, typically O_RDONLY.
  //
  // flags must include O_RDONLY or O_RDWR.
  explicit FdReader(absl::string_view filename, int flags,
                    Options options = Options());

  FdReader(FdReader&& that) noexcept;
  FdReader& operator=(FdReader&& that) noexcept;

  // Returns the object providing and possibly owning the fd being read from. If
  // the fd is owned then changed to -1 by Close(), otherwise unchanged.
  Src& src() { return src_.manager(); }
  const Src& src() const { return src_.manager(); }
  int src_fd() const override { return src_.get(); }

 protected:
  void Done() override;

 private:
  // The object providing and possibly owning the fd being read from.
  Dependency<int, Src> src_;
};

// A Reader which reads from a file descriptor which does not have to support
// random access.
//
// The fd should support:
//  * close() - if the fd is owned
//  * read()
//
// The Src template parameter specifies the type of the object providing and
// possibly owning the fd being read from. Src must support
// Dependency<int, Src>, e.g. OwnedFd (owned, default), int (not owned).
//
// Warning: if the fd is not owned, it will have an unpredictable amount of
// extra data consumed because of buffering.
//
// The fd must not be closed nor have its position changed until the
// FdStreamReader is closed or no longer used.
template <typename Src = OwnedFd>
class FdStreamReader : public FdStreamReaderBase {
 public:
  // Creates a closed FdStreamReader.
  FdStreamReader() noexcept {}

  // Will read from the fd provided by src.
  //
  // Requires Options::set_assumed_pos(pos).
  //
  // type_identity_t<Src> disables template parameter deduction (C++17),
  // letting FdStreamReader(fd) mean FdStreamReader<OwnedFd>(fd) rather than
  // FdStreamReader<int>(fd).
  explicit FdStreamReader(type_identity_t<Src> src, Options options);

  // Opens a file for reading.
  //
  // flags is the second argument of open, typically O_RDONLY.
  //
  // flags must include O_RDONLY or O_RDWR.
  explicit FdStreamReader(absl::string_view filename, int flags,
                          Options options = Options());

  FdStreamReader(FdStreamReader&& that) noexcept;
  FdStreamReader& operator=(FdStreamReader&& that) noexcept;

  // Returns the object providing and owning the fd being read from. Changed to
  // -1 by Close().
  Src& src() { return src_.manager(); }
  const Src& src() const { return src_.manager(); }
  int src_fd() const override { return src_.get(); }

 protected:
  void Done() override;

 private:
  // The object providing and possibly owning the fd being read from.
  Dependency<int, Src> src_;
};

// A Reader which reads from a file descriptor by mapping the whole file to
// memory. It supports random access.
//
// The fd should support:
//  * close() - if the fd is owned
//  * fstat()
//  * mmap()
//  * lseek() - unless Options::set_initial_pos(pos)
//
// The Src template parameter specifies the type of the object providing and
// possibly owning the fd being read from. Src must support
// Dependency<int, Src>, e.g. OwnedFd (owned, default), int (not owned).
//
// The fd must not be closed until the FdMMapReader is closed or no longer used.
// File contents must not be changed while data read from the file is accessed
// without a memory copy.
template <typename Src = OwnedFd>
class FdMMapReader : public FdMMapReaderBase {
 public:
  // Creates a closed FdMMapReader.
  FdMMapReader() noexcept {}

  // Will read from the fd provided by Src.
  //
  // type_identity_t<Src> disables template parameter deduction (C++17),
  // letting FdMMapReader(fd) mean FdMMapReader<OwnedFd>(fd) instead of
  // FdMMapReader<int>(fd).
  explicit FdMMapReader(type_identity_t<Src> src, Options options = Options());

  // Opens a file for reading.
  //
  // flags is the second argument of open, typically O_RDONLY.
  //
  // flags must include O_RDONLY or O_RDWR.
  explicit FdMMapReader(absl::string_view filename, int flags,
                        Options options = Options());

  FdMMapReader(FdMMapReader&& that) noexcept;
  FdMMapReader& operator=(FdMMapReader&& that) noexcept;

  // Returns the object providing and possibly owning the fd being read from. If
  // the fd is owned then changed to -1 by Close(), otherwise unchanged.
  Src& src() { return src_.manager(); }
  const Src& src() const { return src_.manager(); }
  int src_fd() const override { return src_.get(); }

 protected:
  void Done() override;

 private:
  // The object providing and possibly owning the fd being read from.
  Dependency<int, Src> src_;
};

// Implementation details follow.

namespace internal {

inline FdReaderCommon::FdReaderCommon(size_t buffer_size)
    : BufferedReader(buffer_size) {}

inline FdReaderCommon::FdReaderCommon(FdReaderCommon&& that) noexcept
    : BufferedReader(std::move(that)),
      filename_(absl::exchange(that.filename_, std::string())) {}

inline FdReaderCommon& FdReaderCommon::operator=(
    FdReaderCommon&& that) noexcept {
  BufferedReader::operator=(std::move(that));
  filename_ = absl::exchange(that.filename_, std::string());
  return *this;
}

}  // namespace internal

inline FdReaderBase::FdReaderBase(FdReaderBase&& that) noexcept
    : FdReaderCommon(std::move(that)),
      sync_pos_(absl::exchange(that.sync_pos_, false)) {}

inline FdReaderBase& FdReaderBase::operator=(FdReaderBase&& that) noexcept {
  FdReaderCommon::operator=(std::move(that));
  sync_pos_ = absl::exchange(that.sync_pos_, false);
  return *this;
}

inline FdStreamReaderBase::FdStreamReaderBase(
    FdStreamReaderBase&& that) noexcept
    : FdReaderCommon(std::move(that)) {}

inline FdStreamReaderBase& FdStreamReaderBase::operator=(
    FdStreamReaderBase&& that) noexcept {
  FdReaderCommon::operator=(std::move(that));
  return *this;
}

inline FdMMapReaderBase::FdMMapReaderBase(FdMMapReaderBase&& that) noexcept
    : ChainReader(std::move(that)),
      filename_(absl::exchange(that.filename_, std::string())),
      sync_pos_(absl::exchange(that.sync_pos_, false)) {}

inline FdMMapReaderBase& FdMMapReaderBase::operator=(
    FdMMapReaderBase&& that) noexcept {
  ChainReader::operator=(std::move(that));
  filename_ = absl::exchange(that.filename_, std::string());
  sync_pos_ = absl::exchange(that.sync_pos_, false);
  return *this;
}

template <typename Src>
FdReader<Src>::FdReader(type_identity_t<Src> src, Options options)
    : FdReaderBase(options.buffer_size_, !options.initial_pos_.has_value()),
      src_(std::move(src)) {
  RIEGELI_ASSERT_GE(src_.get(), 0)
      << "Failed precondition of FdReader<Src>::FdReader(Src): "
         "negative file descriptor";
  SetFilename(src_.get());
  Initialize(options.initial_pos_, src_.get());
}

template <typename Src>
FdReader<Src>::FdReader(absl::string_view filename, int flags, Options options)
    : FdReaderBase(options.buffer_size_, !options.initial_pos_.has_value()) {
  RIEGELI_ASSERT((flags & O_ACCMODE) == O_RDONLY ||
                 (flags & O_ACCMODE) == O_RDWR)
      << "Failed precondition of FdReader::FdReader(string_view): "
         "flags must include O_RDONLY or O_RDWR";
  const int src = OpenFd(filename, flags);
  if (ABSL_PREDICT_FALSE(src < 0)) return;
  src_ = Dependency<int, Src>(Src(src));
  Initialize(options.initial_pos_, src_.get());
}

template <typename Src>
inline FdReader<Src>::FdReader(FdReader&& that) noexcept
    : FdReaderBase(std::move(that)), src_(std::move(that.src_)) {}

template <typename Src>
inline FdReader<Src>& FdReader<Src>::operator=(FdReader&& that) noexcept {
  FdReaderBase::operator=(std::move(that));
  src_ = std::move(that.src_);
  return *this;
}

template <typename Src>
void FdReader<Src>::Done() {
  if (ABSL_PREDICT_TRUE(healthy())) SyncPos(src_.get());
  FdReaderBase::Done();
  if (src_.is_owning() && src_.get() >= 0) {
    const int src = src_.Release();
    if (ABSL_PREDICT_FALSE(internal::CloseFd(src) < 0) &&
        ABSL_PREDICT_TRUE(healthy())) {
      FailOperation(internal::CloseFunctionName());
    }
  }
}

template <typename Src>
FdStreamReader<Src>::FdStreamReader(type_identity_t<Src> src, Options options)
    : FdStreamReaderBase(options.buffer_size_), src_(std::move(src)) {
  RIEGELI_ASSERT_GE(src_.get(), 0)
      << "Failed precondition of FdStreamReader<Src>::FdStreamReader(Src): "
         "negative file descriptor";
  RIEGELI_CHECK(options.assumed_pos_.has_value())
      << "Failed precondition of FdStreamReader<Src>::FdStreamReader(Src): "
         "assumed file position must be specified "
         "if FdStreamReader does not open the file";
  SetFilename(src_.get());
  limit_pos_ = *options.assumed_pos_;
}

template <typename Src>
FdStreamReader<Src>::FdStreamReader(absl::string_view filename, int flags,
                                    Options options)
    : FdStreamReaderBase(options.buffer_size_) {
  RIEGELI_ASSERT((flags & O_ACCMODE) == O_RDONLY ||
                 (flags & O_ACCMODE) == O_RDWR)
      << "Failed precondition of FdStreamReader::FdStreamReader(string_view): "
         "flags must include O_RDONLY or O_RDWR";
  const int src = OpenFd(filename, flags);
  if (ABSL_PREDICT_FALSE(src < 0)) return;
  src_ = Dependency<int, Src>(Src(src));
  if (options.assumed_pos_.has_value()) limit_pos_ = *options.assumed_pos_;
}

template <typename Src>
inline FdStreamReader<Src>::FdStreamReader(FdStreamReader&& that) noexcept
    : FdStreamReaderBase(std::move(that)), src_(std::move(that.src_)) {}

template <typename Src>
inline FdStreamReader<Src>& FdStreamReader<Src>::operator=(
    FdStreamReader&& that) noexcept {
  FdStreamReaderBase::operator=(std::move(that));
  src_ = std::move(that.src_);
  return *this;
}

template <typename Src>
void FdStreamReader<Src>::Done() {
  FdStreamReaderBase::Done();
  if (src_.is_owning() && src_.get() >= 0) {
    const int src = src_.Release();
    if (ABSL_PREDICT_FALSE(internal::CloseFd(src) < 0) &&
        ABSL_PREDICT_TRUE(healthy())) {
      FailOperation(internal::CloseFunctionName());
    }
  }
}

template <typename Src>
FdMMapReader<Src>::FdMMapReader(type_identity_t<Src> src, Options options)
    : FdMMapReaderBase(!options.initial_pos_.has_value()),
      src_(std::move(src)) {
  RIEGELI_ASSERT_GE(src_.get(), 0)
      << "Failed precondition of FdMMapReader<Src>::FdMMapReader(Src): "
         "negative file descriptor";
  SetFilename(src_.get());
  Initialize(options.initial_pos_, src_.get());
}

template <typename Src>
FdMMapReader<Src>::FdMMapReader(absl::string_view filename, int flags,
                                Options options)
    : FdMMapReaderBase(!options.initial_pos_.has_value()) {
  RIEGELI_ASSERT((flags & O_ACCMODE) == O_RDONLY ||
                 (flags & O_ACCMODE) == O_RDWR)
      << "Failed precondition of FdMMapReader::FdMMapReader(string_view): "
         "flags must include O_RDONLY or O_RDWR";
  const int src = OpenFd(filename, flags);
  if (ABSL_PREDICT_FALSE(src < 0)) return;
  src_ = Dependency<int, Src>(Src(src));
  Initialize(options.initial_pos_, src_.get());
}

template <typename Src>
inline FdMMapReader<Src>::FdMMapReader(FdMMapReader&& that) noexcept
    : FdMMapReaderBase(std::move(that)), src_(std::move(that.src_)) {}

template <typename Src>
inline FdMMapReader<Src>& FdMMapReader<Src>::operator=(
    FdMMapReader&& that) noexcept {
  FdMMapReaderBase::operator=(std::move(that));
  src_ = std::move(that.src_);
  return *this;
}

template <typename Src>
void FdMMapReader<Src>::Done() {
  if (ABSL_PREDICT_TRUE(healthy())) SyncPos(src_.get());
  FdMMapReaderBase::Done();
  ChainReader::src().Clear();
  if (src_.is_owning() && src_.get() >= 0) {
    const int src = src_.Release();
    if (ABSL_PREDICT_FALSE(internal::CloseFd(src) < 0) &&
        ABSL_PREDICT_TRUE(healthy())) {
      FailOperation(internal::CloseFunctionName());
    }
  }
}

extern template class FdReader<OwnedFd>;
extern template class FdReader<int>;
extern template class FdStreamReader<OwnedFd>;
extern template class FdStreamReader<int>;
extern template class FdMMapReader<OwnedFd>;
extern template class FdMMapReader<int>;

}  // namespace riegeli

#endif  // RIEGELI_BYTES_FD_READER_H_
