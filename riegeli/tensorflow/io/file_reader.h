// Copyright 2019 Google LLC
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

#ifndef RIEGELI_TENSORFLOW_IO_FILE_READER_H_
#define RIEGELI_TENSORFLOW_IO_FILE_READER_H_

#include <stddef.h>

#include <memory>
#include <string>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/strings/string_view.h"
#include "absl/utility/utility.h"
#include "riegeli/base/base.h"
#include "riegeli/base/buffer.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/reader.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/file_system.h"

namespace riegeli {
namespace tensorflow {

// Template parameter invariant part of FileReader.
class FileReaderBase : public Reader {
 public:
  class Options {
   public:
    Options() noexcept {}

    // Overrides the TensorFlow environment.
    //
    // nullptr is interpreted as Env::Default().
    //
    // Default: nullptr.
    Options& set_env(::tensorflow::Env* env) & {
      env_ = env;
      return *this;
    }
    Options&& set_env(::tensorflow::Env* env) && {
      return std::move(set_env(env));
    }

    // Reading will start from this position.
    //
    // Default: 0.
    Options& set_initial_pos(Position initial_pos) & {
      initial_pos_ = initial_pos;
      return *this;
    }
    Options&& set_initial_pos(Position initial_pos) && {
      return std::move(set_initial_pos(initial_pos));
    }

    // Tunes how much data is buffered after reading from the file.
    //
    // Default: 64K
    Options& set_buffer_size(size_t buffer_size) & {
      RIEGELI_ASSERT_GT(buffer_size, 0u)
          << "Failed precondition of "
             "FileReaderBase::Options::set_buffer_size(): "
             "zero buffer size";
      buffer_size_ = buffer_size;
      return *this;
    }
    Options&& set_buffer_size(size_t buffer_size) && {
      return std::move(set_buffer_size(buffer_size));
    }

   private:
    template <typename Src>
    friend class FileReader;

    ::tensorflow::Env* env_ = nullptr;
    Position initial_pos_ = 0;
    size_t buffer_size_ = kDefaultBufferSize;
  };

  // Returns the RandomAccessFile being read from. If the RandomAccessFile is
  // owned then changed to nullptr by Close(), otherwise unchanged.
  virtual ::tensorflow::RandomAccessFile* src_file() const = 0;

  // Returns the name of the RandomAccessFile being read from. Unchanged by
  // Close().
  const std::string& filename() const { return filename_; }

  bool SupportsRandomAccess() const override { return !filename_.empty(); }
  bool Size(Position* size) override;

 protected:
  FileReaderBase() noexcept : Reader(State::kClosed) {}

  explicit FileReaderBase(size_t buffer_size);

  FileReaderBase(FileReaderBase&& that) noexcept;
  FileReaderBase& operator=(FileReaderBase&& that) noexcept;

  bool InitializeFilename(::tensorflow::Env* env,
                          ::tensorflow::RandomAccessFile* src);
  bool InitializeFilename(::tensorflow::Env* env, absl::string_view filename);
  std::unique_ptr<::tensorflow::RandomAccessFile> OpenFile();
  void InitializePos(Position initial_pos);
  ABSL_ATTRIBUTE_COLD bool FailOperation(const ::tensorflow::Status& status,
                                         absl::string_view operation);
  bool PullSlow() override;
  using Reader::ReadSlow;
  bool ReadSlow(char* dest, size_t length) override;
  bool SeekSlow(Position new_pos) override;

 private:
  // Minimum length for which it is better to append current contents of buffer_
  // and read the remaining data directly than to read the data through buffer_.
  size_t LengthToReadDirectly() const;

  std::string filename_;
  // Invariant: if healthy() && !filename_.empty() then file_system_ != nullptr
  ::tensorflow::FileSystem* file_system_ = nullptr;
  Buffer buffer_;
};

// A Reader which reads from a RandomAccessFile. It supports random access
// if RandomAccessFile::Name() is supported.
//
// The Src template parameter specifies the type of the object providing and
// possibly owning the RandomAccessFile being read from. Src must support
// Dependency<RandomAccessFile*, Src>, e.g.
// std::unique_ptr<RandomAccessFile> (owned, default),
// RandomAccessFile* (not owned).
//
// The RandomAccessFile must not be closed until the FileReader is
// closed or no longer used.
template <typename Src = std::unique_ptr<::tensorflow::RandomAccessFile>>
class FileReader : public FileReaderBase {
 public:
  // Creates a closed FileReader.
  FileReader() noexcept {}

  // Will read from the RandomAccessFile provided by src.
  explicit FileReader(Src src, Options options = Options());

  // Opens a RandomAccessFile for reading.
  explicit FileReader(absl::string_view filename, Options options = Options());

  FileReader(FileReader&& that) noexcept;
  FileReader& operator=(FileReader&& that) noexcept;

  // Returns the object providing and possibly owning the RandomAccessFile being
  // read from. If the RandomAccessFile is owned then changed to nullptr by
  // Close(), otherwise unchanged.
  Src& src() { return src_.manager(); }
  const Src& src() const { return src_.manager(); }
  ::tensorflow::RandomAccessFile* src_file() const override {
    return src_.get();
  }

 protected:
  void Done() override;

 private:
  // The object providing and possibly owning the RandomAccessFile being read
  // from.
  Dependency<::tensorflow::RandomAccessFile*, Src> src_;
};

// Implementation details follow.

inline FileReaderBase::FileReaderBase(size_t buffer_size)
    : Reader(State::kOpen), buffer_(buffer_size) {}

inline FileReaderBase::FileReaderBase(FileReaderBase&& that) noexcept
    : Reader(std::move(that)),
      filename_(absl::exchange(that.filename_, std::string())),
      file_system_(absl::exchange(that.file_system_, nullptr)),
      buffer_(std::move(that.buffer_)) {}

inline FileReaderBase& FileReaderBase::operator=(
    FileReaderBase&& that) noexcept {
  Reader::operator=(std::move(that));
  filename_ = absl::exchange(that.filename_, std::string());
  file_system_ = absl::exchange(that.file_system_, nullptr);
  buffer_ = std::move(that.buffer_);
  return *this;
}

template <typename Src>
FileReader<Src>::FileReader(Src src, Options options)
    : FileReaderBase(options.buffer_size_), src_(std::move(src)) {
  RIEGELI_ASSERT(src_.get() != nullptr)
      << "Failed precondition of FileReader<Src>::FileReader(Src): "
         "null RandomAccessFile pointer";
  if (ABSL_PREDICT_FALSE(!InitializeFilename(options.env_, src_.get()))) {
    return;
  }
  InitializePos(options.initial_pos_);
}

template <typename Src>
FileReader<Src>::FileReader(absl::string_view filename, Options options)
    : FileReaderBase(options.buffer_size_) {
  if (ABSL_PREDICT_FALSE(!InitializeFilename(options.env_, filename))) {
    return;
  }
  std::unique_ptr<::tensorflow::RandomAccessFile> src = OpenFile();
  if (ABSL_PREDICT_FALSE(src == nullptr)) return;
  src_ = Dependency<::tensorflow::RandomAccessFile*, Src>(Src(src.release()));
  InitializePos(options.initial_pos_);
}

template <typename Src>
inline FileReader<Src>::FileReader(FileReader&& that) noexcept
    : FileReaderBase(std::move(that)), src_(std::move(that.src_)) {}

template <typename Src>
inline FileReader<Src>& FileReader<Src>::operator=(FileReader&& that) noexcept {
  FileReaderBase::operator=(std::move(that));
  src_ = std::move(that.src_);
  return *this;
}

template <typename Src>
void FileReader<Src>::Done() {
  FileReaderBase::Done();
  if (src_.is_owning() && src_.get() != nullptr) {
    // The only way to close a RandomAccessFile is to delete it.
    src_ = Dependency<::tensorflow::RandomAccessFile*, Src>();
  }
}

extern template class FileReader<
    std::unique_ptr<::tensorflow::RandomAccessFile>>;
extern template class FileReader<::tensorflow::RandomAccessFile*>;

}  // namespace tensorflow
}  // namespace riegeli

#endif  // RIEGELI_TENSORFLOW_IO_FILE_READER_H_
