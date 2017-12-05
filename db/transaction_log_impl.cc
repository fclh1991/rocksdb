//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#ifndef ROCKSDB_LITE
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "db/transaction_log_impl.h"
#include "db/wal_manager.h"
#include "db/db_impl.h"
#include <inttypes.h>
#include "db/write_batch_internal.h"
#include "util/file_reader_writer.h"

namespace rocksdb {

TransactionLogIteratorImpl::TransactionLogIteratorImpl(
    const std::string& dir, const DBOptions* options,
    const TransactionLogIterator::ReadOptions& read_options,
    const EnvOptions& soptions, const SequenceNumber seq,
    std::unique_ptr<VectorLogPtr> files, DBImpl const* const db,
    WalManager *wal_manager)
    : dir_(dir),
      options_(options),
      read_options_(read_options),
      soptions_(soptions),
      startingSequenceNumber_(seq),
      files_(std::move(files)),
      started_(false),
      isValid_(false),
      currentFileIndex_(0),
      currentBatchSeq_(0),
      currentLastSeq_(0),
      db_(db),
      wal_manager_(wal_manager),
      async_wal_(false),
      last_batch_from_buffer_(false) {
  assert(files_ != nullptr);
  assert(db_ != nullptr);

  async_wal_ = db_->db_options_.async_wal;
  reporter_.env = options_->env;
  reporter_.info_log = options_->info_log.get();
  SeekToStartSequence(); // Seek till starting sequence
}

Status TransactionLogIteratorImpl::OpenLogFile(
    const LogFile* logFile, unique_ptr<SequentialFileReader>* file_reader) {
  Env* env = options_->env;
  unique_ptr<SequentialFile> file;
  Status s;
  if (logFile->Type() == kArchivedLogFile) {
    std::string fname = ArchivedLogFileName(dir_, logFile->LogNumber());
    s = env->NewSequentialFile(fname, &file, soptions_);
  } else {
    std::string fname = LogFileName(dir_, logFile->LogNumber());
    s = env->NewSequentialFile(fname, &file, soptions_);
    if (!s.ok()) {
      //  If cannot open file in DB directory.
      //  Try the archive dir, as it could have moved in the meanwhile.
      fname = ArchivedLogFileName(dir_, logFile->LogNumber());
      s = env->NewSequentialFile(fname, &file, soptions_);
    }
  }
  if (s.ok()) {
    file_reader->reset(new SequentialFileReader(std::move(file)));
  }
  return s;
}

BatchResult TransactionLogIteratorImpl::GetBatch()  {
  assert(isValid_);  //  cannot call in a non valid state.
  BatchResult result;
  result.sequence = currentBatchSeq_;
  result.writeBatchPtr = std::move(currentBatch_);
  return result;
}

Status TransactionLogIteratorImpl::status() {
  return currentStatus_;
}

bool TransactionLogIteratorImpl::Valid() {
  return started_ && isValid_;
}

bool TransactionLogIteratorImpl::RestrictedRead(
    Slice* record, std::string* scratch, SequenceNumber expectedSeq) {
  // Don't read if no more complete entries to read from logs
  if (expectedSeq >= db_->GetLatestSequenceNumber()) {
    return false;
  }
  if (async_wal_) {
    assert(db_->wal_writer_);
    auto batch = db_->wal_writer_->GetBatch(expectedSeq);
    if (batch) {
      assert(WriteBatchInternal::Sequence(batch.get()) == expectedSeq);
      *scratch = batch->Data();
      *record = Slice(*scratch);
      last_batch_from_buffer_ = true;
      return true;
    }
  }
  if (last_batch_from_buffer_) {
    return ReadTargetFromWAL(record, scratch, expectedSeq);
  } else {
    last_batch_from_buffer_ = false;
    return currentLogReader_->ReadRecord(record, scratch);
  }
}

void TransactionLogIteratorImpl::SeekToStartSequence(
    uint64_t startFileIndex,
    bool strict) {
  std::string scratch;
  Slice record;
  started_ = false;
  isValid_ = false;
  if (files_->size() <= startFileIndex && !async_wal_) {
    return;
  }
  if (startingSequenceNumber_ > db_->GetLatestSequenceNumber()) {
    return;
  }
  if (files_->size() > startFileIndex) {
    Status s = OpenLogReader(files_->at(startFileIndex).get());
    if (!s.ok()) {
      currentStatus_ = s;
      reporter_.Info(currentStatus_.ToString().c_str());
      return;
    }
    last_batch_from_buffer_ = false;
  } else {
    assert(async_wal_);
    last_batch_from_buffer_ = true;
  }
  while (RestrictedRead(&record, &scratch, startingSequenceNumber_)) {
    if (record.size() < WriteBatchInternal::kHeader) {
      reporter_.Corruption(
        record.size(), Status::Corruption("very small log record"));
      continue;
    }
    UpdateCurrentWriteBatch(record);
    if (currentLastSeq_ >= startingSequenceNumber_) {
      if (strict && currentBatchSeq_ != startingSequenceNumber_) {
        currentStatus_ = Status::Corruption("Gap in sequence number. Could not "
                                            "seek to required sequence number");
        reporter_.Info(currentStatus_.ToString().c_str());
        return;
      } else if (strict) {
        reporter_.Info("Could seek required sequence number. Iterator will "
                       "continue.");
      }
      isValid_ = true;
      started_ = true; // set started_ as we could seek till starting sequence
      return;
    } else {
      isValid_ = false;
    }
  }

  // Could not find start sequence in first file. Normally this must be the
  // only file. Otherwise log the error and let the iterator return next entry
  // If strict is set, we want to seek exactly till the start sequence and it
  // should have been present in the file we scanned above
  if (strict) {
    currentStatus_ = Status::Corruption("Gap in sequence number. Could not "
                                        "seek to required sequence number");
    reporter_.Info(currentStatus_.ToString().c_str());
  } else if (files_->size() > 1) {
    currentStatus_ = Status::Corruption("Start sequence was not found, "
                                        "skipping to the next available");
    reporter_.Info(currentStatus_.ToString().c_str());
    // Let NextImpl find the next available entry. started_ remains false
    // because we don't want to check for gaps while moving to start sequence
    NextImpl(true, startingSequenceNumber_);
  }
}

void TransactionLogIteratorImpl::Next() {
  SequenceNumber expectedSeq = 0;
  if (!started_) {
    expectedSeq = startingSequenceNumber_;
  } else {
    expectedSeq = currentLastSeq_ + 1;
  }
  return NextImpl(false, expectedSeq);
}

void TransactionLogIteratorImpl::NextImpl(bool internal,
                                          SequenceNumber expectedSeq) {
  std::string scratch;
  Slice record;
  isValid_ = false;
  if (!internal && !started_) {
    // Runs every time until we can seek to the start sequence
    return SeekToStartSequence();
  }
  while(true) {
    if (currentLogReader_ && currentLogReader_->IsEOF()) {
      currentLogReader_->UnmarkEOF();
    }
    while (RestrictedRead(&record, &scratch, expectedSeq)) {
      if (record.size() < WriteBatchInternal::kHeader) {
        reporter_.Corruption(
          record.size(), Status::Corruption("very small log record"));
        continue;
      } else {
        // started_ should be true if called by application
        assert(internal || started_);
        // started_ should be false if called internally
        assert(!internal || !started_);
        UpdateCurrentWriteBatch(record);
        if (internal && !started_) {
          started_ = true;
        }
        return;
      }
    }

    assert(!last_batch_from_buffer_);

    // Open the next file
    if (currentFileIndex_ < files_->size() - 1) {
      ++currentFileIndex_;
      Status s = OpenLogReader(files_->at(currentFileIndex_).get());
      if (!s.ok()) {
        isValid_ = false;
        currentStatus_ = s;
        return;
      }
    } else {
      auto log_number = wal_manager_->GetNextLogNumber(
          files_->back()->LogNumber());
      if (log_number != 0) {
        files_->clear();
        files_->emplace_back(new LogFileImpl(log_number, kAliveLogFile, 0, 0));
        currentFileIndex_ = 0;
        auto s = OpenLogReader(files_->at(currentFileIndex_).get());
        if (s.ok()) {
          continue;
        }
      }
      isValid_ = false;
      if (currentLastSeq_ == db_->GetLatestSequenceNumber()) {
        currentStatus_ = Status::OK();
      } else {
        currentStatus_ = Status::Corruption("NO MORE DATA LEFT");
      }
      return;
    }
  }
}

bool TransactionLogIteratorImpl::IsBatchExpected(
    const WriteBatch* batch,
    const SequenceNumber expectedSeq) {
  assert(batch);
  SequenceNumber batchSeq = WriteBatchInternal::Sequence(batch);
  if (batchSeq != expectedSeq) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "Discontinuity in log records. Got seq=%" PRIu64
             ", Expected seq=%" PRIu64 ", Last flushed seq=%" PRIu64
             ".Log iterator will reseek the correct batch.",
             batchSeq, expectedSeq, db_->GetLatestSequenceNumber());
    reporter_.Info(buf);
    return false;
  }
  return true;
}

void TransactionLogIteratorImpl::UpdateCurrentWriteBatch(const Slice& record) {
  std::unique_ptr<WriteBatch> batch(new WriteBatch());
  WriteBatchInternal::SetContents(batch.get(), record);

  SequenceNumber expectedSeq = currentLastSeq_ + 1;
  // If the iterator has started, then confirm that we get continuous batches
  if (started_ && !IsBatchExpected(batch.get(), expectedSeq)) {
    // Seek to the batch having expected sequence number
    if (expectedSeq < files_->at(currentFileIndex_)->StartSequence()) {
      // Expected batch must lie in the previous log file
      // Avoid underflow.
      if (currentFileIndex_ != 0) {
        currentFileIndex_--;
      }
    }
    startingSequenceNumber_ = expectedSeq;
    // currentStatus_ will be set to Ok if reseek succeeds
    currentStatus_ = Status::NotFound("Gap in sequence numbers");
    return SeekToStartSequence(currentFileIndex_, true);
  }

  currentBatchSeq_ = WriteBatchInternal::Sequence(batch.get());
  currentLastSeq_ = currentBatchSeq_ +
                    WriteBatchInternal::Count(batch.get()) - 1;
  // currentBatchSeq_ can only change here
  assert(currentLastSeq_ <= db_->GetLatestSequenceNumber());

  currentBatch_ = std::move(batch);
  isValid_ = true;
  currentStatus_ = Status::OK();
}

Status TransactionLogIteratorImpl::OpenLogReader(const LogFile* logFile) {
  unique_ptr<SequentialFileReader> file;
  Status s = OpenLogFile(logFile, &file);
  if (!s.ok()) {
    return s;
  }
  assert(file);
  currentLogReader_.reset(new log::Reader(
      options_->info_log, std::move(file), &reporter_,
      read_options_.verify_checksums_, 0, logFile->LogNumber()));
  return Status::OK();
}

bool TransactionLogIteratorImpl::ReadTargetFromWAL(
    Slice *record, std::string *scratch, SequenceNumber expectedSeq) {
  assert(async_wal_ && last_batch_from_buffer_);
  uint64_t target_log = wal_manager_->GetWALContainsBatch(expectedSeq);
  if (target_log == 0) {
    return false;
  }
  uint64_t current_log = 0;
  if (currentFileIndex_ < files_->size()) {
    current_log = files_->at(currentFileIndex_)->LogNumber();
  }
  bool openWAL = true;
  if (target_log != current_log) {
    files_->clear();
    files_->emplace_back(new LogFileImpl(target_log, kAliveLogFile, 0, 0));
    currentFileIndex_ = 0;
  } else if (currentLogReader_) {
    openWAL = false;
  }
  last_batch_from_buffer_ = false;
  if (openWAL) {
    Status s = OpenLogReader(files_->at(currentFileIndex_).get());
    if (!s.ok()) {
      currentStatus_ = s;
      reporter_.Info(currentStatus_.ToString().c_str());
      return false;
    }
  }
  assert(currentLogReader_);
  if (currentLogReader_->IsEOF()) {
    currentLogReader_->UnmarkEOF();
  }
  SequenceNumber batchSeq = 0;
  do {
    scratch->clear();
    record->clear();
    if (currentLogReader_->ReadRecord(record, scratch)) {
      if (record->size() < WriteBatchInternal::kHeader) {
        reporter_.Corruption(
          record->size(), Status::Corruption("very small log record"));
        continue;
      } else {
        std::unique_ptr<WriteBatch> batch(new WriteBatch());
        WriteBatchInternal::SetContents(batch.get(), *record);
        batchSeq = WriteBatchInternal::Sequence(batch.get());
      }
    } else {
      return false;
    }
  } while (batchSeq < expectedSeq);
  return true;
}

}  //  namespace rocksdb
#endif  // ROCKSDB_LITE
