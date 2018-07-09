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

#ifndef RIEGELI_RECORDS_CHUNK_READER_H_
#define RIEGELI_RECORDS_CHUNK_READER_H_

#include <memory>

#include "absl/base/optimization.h"
#include "riegeli/base/base.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/chunk_encoding/chunk.h"
#include "riegeli/records/block.h"
#include "riegeli/records/skipped_region.h"

namespace riegeli {

// A ChunkReader reads chunks of a Riegeli/records file (rather than individual
// records, as RecordReader does).
//
// ChunkReader can be used together with ChunkWriter to rewrite Riegeli/records
// files without recompressing chunks, e.g. to concatenate files.
//
// TODO: If use cases arise, this could be made an abstract class,
// together with a default implementation, analogously to ChunkWriter.
class ChunkReader : public Object {
 public:
  // Will read chunks from the byte Reader which is owned by this ChunkReader
  // and will be closed and deleted when the ChunkReader is closed.
  explicit ChunkReader(std::unique_ptr<Reader> byte_reader);

  // Will read chunks from the byte Reader which is not owned by this
  // ChunkReader and must be kept alive but not accessed until closing the
  // ChunkReader.
  explicit ChunkReader(Reader* byte_reader);

  ChunkReader(const ChunkReader&) = delete;
  ChunkReader& operator=(const ChunkReader&) = delete;

  // Ensures that the file looks like a valid Riegeli/Records file.
  //
  // Reading the file already checks whether it is valid. CheckFileFormat() can
  // verify this before (or instead of) performing other operations.
  //
  // Return values:
  //  * true                    - success
  //  * false (when healthy())  - source ends
  //  * false (when !healthy()) - failure
  bool CheckFileFormat();

  // Reads the next chunk.
  //
  // Return values:
  //  * true                    - success (*chunk is set)
  //  * false (when healthy())  - source ends
  //  * false (when !healthy()) - failure
  bool ReadChunk(Chunk* chunk);

  // Reads the next chunk header, from same chunk which will be read by an
  // immediately following ReadChunk().
  //
  // If chunk_header != nullptr, *chunk_header is set to the chunk header, valid
  // until the next non-const function of the ChunkReader.
  //
  // Return values:
  //  * true                    - success (*chunk_header is set)
  //  * false (when healthy())  - source ends
  //  * false (when !healthy()) - failure
  bool PullChunkHeader(const ChunkHeader** chunk_header);

  // If !healthy() and the failure was caused by invalid file contents, then
  // Recover() tries to recover from the failure and allow reading again by
  // skipping over the invalid region.
  //
  // If Close() failed and the failure was caused by truncated file contents,
  // then Recover() returns true. The ChunkReader remains closed.
  //
  // If healthy(), or if !healthy() but the failure was not caused by invalid
  // file contents, then Recover() returns false.
  //
  // If skipped_region != nullptr, *skipped_region is set to the position of the
  // skipped region on success.
  //
  // Return values:
  //  * true  - success
  //  * false - failure not caused by invalid file contents
  bool Recover(SkippedRegion* skipped_region = nullptr);

  // Returns the current position, which is a chunk boundary (except that if
  // the source ends in a skipped region, it can be greater than file size and
  // it can be a block boundary).
  //
  // ReadChunk() and PullChunkHeader() return a chunk which begins at pos() if
  // they succeed.
  //
  // pos() is unchanged by Close().
  Position pos() const { return pos_; }

  // Returns true if this ChunkReader supports Seek(), SeekToChunkContaining(),
  // SeekToChunkAfter(), and Size().
  bool SupportsRandomAccess() const;

  // Seeks to new_pos, which should be a chunk boundary.
  //
  // Return values:
  //  * true  - success
  //  * false - failure (!healthy())
  bool Seek(Position new_pos);

  // Seeks to the nearest chunk boundary before or at new_pos if the position
  // corresponds to some numeric record position in the following chunk (i.e. is
  // less than num_records bytes after chunk beginning), otherwise seeks to the
  // nearest chunk boundary at or after the given position.
  //
  // Return values:
  //  * true  - success
  //  * false - failure (!healthy())
  bool SeekToChunkContaining(Position new_pos);

  // Seeks to the nearest chunk boundary at or before new_pos.
  //
  // Return values:
  //  * true  - success
  //  * false - failure (!healthy())
  bool SeekToChunkBefore(Position new_pos);

  // Seeks to the nearest chunk boundary at or after new_pos.
  //
  // Return values:
  //  * true  - success
  //  * false - failure (!healthy())
  bool SeekToChunkAfter(Position new_pos);

  // Returns the size of the file, i.e. the position corresponding to its end.
  //
  // Return values:
  //  * true  - success (*size is set, healthy())
  //  * false - failure (!healthy())
  bool Size(Position* size);

 protected:
  void Done() override;

 private:
  enum class Recoverable { kNo, kHaveChunk, kFindChunk };
  enum class WhichChunk { kContaining, kBefore, kAfter };

  // Interprets a false result from a byte_reader_ reading or seeking function.
  //
  // End of file (i.e. if healthy()) is propagated, setting truncated_ if it was
  // in the middle of a chunk.
  //
  // Always returns false.
  bool ReadingFailed();

  // Interprets a false result from a byte_reader_ reading or seeking function.
  //
  // End of file (i.e. if healthy()) fails the ChunkReader.
  //
  // Always returns false.
  bool SeekingFailed(Position new_pos);

  // Reads or continues reading chunk_.header.
  bool ReadChunkHeader();

  // Reads or continues reading block_header_.
  //
  // Precondition: internal::RemainingInBlockHeader(byte_reader_->pos()) > 0
  bool ReadBlockHeader();

  // Shared implementation of SeekToChunkContaining(), SeekToChunkBefore(), and
  // SeekToChunkAfter().
  template <WhichChunk which_chunk>
  bool SeekToChunk(Position new_pos);

  std::unique_ptr<Reader> owned_byte_reader_;
  // Invariant: if healthy() then byte_reader_ != nullptr
  Reader* byte_reader_;

  // If true, the source is truncated (in the middle of a chunk) at the current
  // position. If the source does not grow, Close() will fail.
  //
  // Invariant: if truncated_ then byte_reader_->pos() > pos_
  bool truncated_ = false;

  // Beginning of the current chunk.
  //
  // If pos_ > byte_reader_->pos(), the source ends in a skipped region. In this
  // case pos_ can be a block boundary instead of a chunk boundary.
  Position pos_;

  // Chunk header and chunk data, filled to the point derived from pos_ and
  // byte_reader_->pos().
  Chunk chunk_;

  // Block header, filled to the point derived from byte_reader_->pos().
  internal::BlockHeader block_header_;

  // Whether Recover() is applicable, and if so, how it should be performed:
  //
  //  * Recoverable::kNo        - Recover() is not applicable
  //  * Recoverable::kHaveChunk - Recover() assumes that a chunk starts
  //                              at recoverable_pos_
  //  * Recoverable::kFindChunk - Recover() finds a block after recoverable_pos_
  //                              and a chunk after the block
  //
  // Invariants:
  //   if healthy() then recoverable_ == Recoverable::kNo
  //   if closed() then recoverable_ == Recoverable::kNo ||
  //                    recoverable_ == Recoverable::kHaveChunk
  Recoverable recoverable_ = Recoverable::kNo;

  // If recoverable_ != Recoverable::kNo, the position to start recovery from.
  //
  // Invariant:
  //   if recoverable_ != Recoverable::kNo then recoverable_pos_ >= pos_
  Position recoverable_pos_ = 0;
};

// Implementation details follow.

inline bool ChunkReader::SupportsRandomAccess() const {
  return byte_reader_ != nullptr && byte_reader_->SupportsRandomAccess();
}

inline bool ChunkReader::Size(Position* size) {
  if (ABSL_PREDICT_FALSE(!healthy())) return false;
  if (ABSL_PREDICT_FALSE(!byte_reader_->Size(size))) return Fail(*byte_reader_);
  return true;
}

}  // namespace riegeli

#endif  // RIEGELI_RECORDS_CHUNK_READER_H_
