// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/elements_upload_data_stream.h"

#include <stdint.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/base/upload_data_stream.h"
#include "net/base/upload_file_element_reader.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"

using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::_;

namespace net {

namespace {

const char kTestData[] = "0123456789";
const size_t kTestDataSize = arraysize(kTestData) - 1;
const size_t kTestBufferSize = 1 << 14;  // 16KB.

// Reads data from the upload data stream, and returns the data as string.
std::string ReadFromUploadDataStream(UploadDataStream* stream) {
  std::string data_read;
  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestBufferSize);
  while (!stream->IsEOF()) {
    TestCompletionCallback callback;
    const int result =
        stream->Read(buf.get(), kTestBufferSize, callback.callback());
    const int bytes_read =
        result != ERR_IO_PENDING ? result : callback.WaitForResult();
    data_read.append(buf->data(), bytes_read);
  }
  return data_read;
}

// A mock class of UploadElementReader.
class MockUploadElementReader : public UploadElementReader {
 public:
  MockUploadElementReader(int content_length, bool is_in_memory)
      : content_length_(content_length),
        bytes_remaining_(content_length),
        is_in_memory_(is_in_memory),
        init_result_(OK),
        read_result_(OK) {}

  ~MockUploadElementReader() override {}

  // UploadElementReader overrides.
  MOCK_METHOD1(Init, int(const CompletionCallback& callback));
  uint64_t GetContentLength() const override { return content_length_; }
  uint64_t BytesRemaining() const override { return bytes_remaining_; }
  bool IsInMemory() const override { return is_in_memory_; }
  MOCK_METHOD3(Read, int(IOBuffer* buf,
                         int buf_length,
                         const CompletionCallback& callback));

  // Sets expectation to return the specified result from Init() asynchronously.
  void SetAsyncInitExpectation(int result) {
    init_result_ = result;
    EXPECT_CALL(*this, Init(_))
        .WillOnce(DoAll(Invoke(this, &MockUploadElementReader::OnInit),
                        Return(ERR_IO_PENDING)));
  }

  // Sets expectation to return the specified result from Read().
  void SetReadExpectation(int result) {
    read_result_ = result;
    EXPECT_CALL(*this, Read(_, _, _))
        .WillOnce(Invoke(this, &MockUploadElementReader::OnRead));
  }

 private:
  void OnInit(const CompletionCallback& callback) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::Bind(callback, init_result_));
  }

  int OnRead(IOBuffer* buf,
             int buf_length,
             const CompletionCallback& callback) {
    if (read_result_ > 0)
      bytes_remaining_ = std::max(0, bytes_remaining_ - read_result_);
    if (IsInMemory()) {
      return read_result_;
    } else {
      base::ThreadTaskRunnerHandle::Get()->PostTask(
          FROM_HERE, base::Bind(callback, read_result_));
      return ERR_IO_PENDING;
    }
  }

  int content_length_;
  int bytes_remaining_;
  bool is_in_memory_;

  // Result value returned from Init().
  int init_result_;

  // Result value returned from Read().
  int read_result_;
};

}  // namespace

class ElementsUploadDataStreamTest : public PlatformTest {
 public:
  void SetUp() override {
    PlatformTest::SetUp();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
  }
  ~ElementsUploadDataStreamTest() override {
    element_readers_.clear();
    base::RunLoop().RunUntilIdle();
  }

  void FileChangedHelper(const base::FilePath& file_path,
                         const base::Time& time,
                         bool error_expected);

  base::ScopedTempDir temp_dir_;
  std::vector<std::unique_ptr<UploadElementReader>> element_readers_;
};

TEST_F(ElementsUploadDataStreamTest, EmptyUploadData) {
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));
  ASSERT_EQ(OK, stream->Init(CompletionCallback()));
  EXPECT_TRUE(stream->IsInMemory());
  EXPECT_EQ(0U, stream->size());
  EXPECT_EQ(0U, stream->position());
  EXPECT_TRUE(stream->IsEOF());
}

TEST_F(ElementsUploadDataStreamTest, ConsumeAllBytes) {
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));
  ASSERT_EQ(OK, stream->Init(CompletionCallback()));
  EXPECT_TRUE(stream->IsInMemory());
  EXPECT_EQ(kTestDataSize, stream->size());
  EXPECT_EQ(0U, stream->position());
  EXPECT_FALSE(stream->IsEOF());
  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestBufferSize);
  while (!stream->IsEOF()) {
    int bytes_read =
        stream->Read(buf.get(), kTestBufferSize, CompletionCallback());
    ASSERT_LE(0, bytes_read);  // Not an error.
  }
  EXPECT_EQ(kTestDataSize, stream->position());
  ASSERT_TRUE(stream->IsEOF());
}

TEST_F(ElementsUploadDataStreamTest, File) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));

  element_readers_.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), temp_file_path, 0,
      std::numeric_limits<uint64_t>::max(), base::Time())));

  TestCompletionCallback init_callback;
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback.callback()));
  ASSERT_EQ(OK, init_callback.WaitForResult());
  EXPECT_FALSE(stream->IsInMemory());
  EXPECT_EQ(kTestDataSize, stream->size());
  EXPECT_EQ(0U, stream->position());
  EXPECT_FALSE(stream->IsEOF());
  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestBufferSize);
  while (!stream->IsEOF()) {
    TestCompletionCallback read_callback;
    ASSERT_EQ(
        ERR_IO_PENDING,
        stream->Read(buf.get(), kTestBufferSize, read_callback.callback()));
    ASSERT_LE(0, read_callback.WaitForResult());  // Not an error.
  }
  EXPECT_EQ(kTestDataSize, stream->position());
  ASSERT_TRUE(stream->IsEOF());
}

TEST_F(ElementsUploadDataStreamTest, FileSmallerThanLength) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));
  const uint64_t kFakeSize = kTestDataSize * 2;

  UploadFileElementReader::ScopedOverridingContentLengthForTests
      overriding_content_length(kFakeSize);

  element_readers_.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), temp_file_path, 0,
      std::numeric_limits<uint64_t>::max(), base::Time())));

  TestCompletionCallback init_callback;
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback.callback()));
  ASSERT_EQ(OK, init_callback.WaitForResult());
  EXPECT_FALSE(stream->IsInMemory());
  EXPECT_EQ(kFakeSize, stream->size());
  EXPECT_EQ(0U, stream->position());
  EXPECT_FALSE(stream->IsEOF());
  uint64_t read_counter = 0;
  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestBufferSize);
  while (!stream->IsEOF()) {
    TestCompletionCallback read_callback;
    ASSERT_EQ(
        ERR_IO_PENDING,
        stream->Read(buf.get(), kTestBufferSize, read_callback.callback()));
    int bytes_read = read_callback.WaitForResult();
    ASSERT_LE(0, bytes_read);  // Not an error.
    read_counter += bytes_read;
    EXPECT_EQ(read_counter, stream->position());
  }
  // UpdateDataStream will pad out the file with 0 bytes so that the HTTP
  // transaction doesn't hang.  Therefore we expected the full size.
  EXPECT_EQ(kFakeSize, read_counter);
  EXPECT_EQ(read_counter, stream->position());
}

TEST_F(ElementsUploadDataStreamTest, ReadErrorSync) {
  // This element cannot be read.
  std::unique_ptr<MockUploadElementReader> reader(
      new MockUploadElementReader(kTestDataSize, true));
  EXPECT_CALL(*reader, Init(_)).WillOnce(Return(OK));
  reader->SetReadExpectation(ERR_FAILED);
  element_readers_.push_back(std::move(reader));

  // This element is ignored because of the error from the previous reader.
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));

  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  // Run Init().
  ASSERT_EQ(OK, stream->Init(CompletionCallback()));
  EXPECT_EQ(kTestDataSize*2, stream->size());
  EXPECT_EQ(0U, stream->position());
  EXPECT_FALSE(stream->IsEOF());

  // Prepare a buffer filled with non-zero data.
  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestBufferSize);
  std::fill_n(buf->data(), kTestBufferSize, -1);

  // Read() results in success even when the reader returns error.
  EXPECT_EQ(static_cast<int>(kTestDataSize * 2),
            stream->Read(buf.get(), kTestBufferSize, CompletionCallback()));
  EXPECT_EQ(kTestDataSize * 2, stream->position());
  EXPECT_TRUE(stream->IsEOF());

  // The buffer is filled with zero.
  EXPECT_EQ(static_cast<int>(kTestDataSize*2),
            std::count(buf->data(), buf->data() + kTestBufferSize, 0));
}

TEST_F(ElementsUploadDataStreamTest, ReadErrorAsync) {
  // This element cannot be read.
  std::unique_ptr<MockUploadElementReader> reader(
      new MockUploadElementReader(kTestDataSize, false));
  reader->SetAsyncInitExpectation(OK);
  reader->SetReadExpectation(ERR_FAILED);
  element_readers_.push_back(std::move(reader));

  // This element is ignored because of the error from the previous reader.
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));

  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  // Run Init().
  TestCompletionCallback init_callback;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback.callback()));
  EXPECT_EQ(OK, init_callback.WaitForResult());
  EXPECT_EQ(kTestDataSize*2, stream->size());
  EXPECT_EQ(0U, stream->position());
  EXPECT_FALSE(stream->IsEOF());

  // Prepare a buffer filled with non-zero data.
  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestBufferSize);
  std::fill_n(buf->data(), kTestBufferSize, -1);

  // Read() results in success even when the reader returns error.
  TestCompletionCallback read_callback;
  ASSERT_EQ(ERR_IO_PENDING,
            stream->Read(buf.get(), kTestBufferSize, read_callback.callback()));
  EXPECT_EQ(static_cast<int>(kTestDataSize * 2), read_callback.WaitForResult());
  EXPECT_EQ(kTestDataSize*2, stream->position());
  EXPECT_TRUE(stream->IsEOF());

  // The buffer is filled with zero.
  EXPECT_EQ(static_cast<int>(kTestDataSize*2),
            std::count(buf->data(), buf->data() + kTestBufferSize, 0));
}

TEST_F(ElementsUploadDataStreamTest, FileAndBytes) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));

  const uint64_t kFileRangeOffset = 1;
  const uint64_t kFileRangeLength = 4;
  element_readers_.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), temp_file_path,
      kFileRangeOffset, kFileRangeLength, base::Time())));

  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));

  const uint64_t kStreamSize = kTestDataSize + kFileRangeLength;
  TestCompletionCallback init_callback;
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback.callback()));
  ASSERT_EQ(OK, init_callback.WaitForResult());
  EXPECT_FALSE(stream->IsInMemory());
  EXPECT_EQ(kStreamSize, stream->size());
  EXPECT_EQ(0U, stream->position());
  EXPECT_FALSE(stream->IsEOF());
  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestBufferSize);
  while (!stream->IsEOF()) {
    TestCompletionCallback read_callback;
    const int result =
        stream->Read(buf.get(), kTestBufferSize, read_callback.callback());
    const int bytes_read =
        result != ERR_IO_PENDING ? result : read_callback.WaitForResult();
    ASSERT_LE(0, bytes_read);  // Not an error.
  }
  EXPECT_EQ(kStreamSize, stream->position());
  ASSERT_TRUE(stream->IsEOF());
}

// Init() with on-memory and not-on-memory readers.
TEST_F(ElementsUploadDataStreamTest, InitAsync) {
  // Create UploadDataStream with mock readers.
  std::unique_ptr<MockUploadElementReader> reader(
      new MockUploadElementReader(kTestDataSize, true));
  EXPECT_CALL(*reader, Init(_)).WillOnce(Return(OK));
  element_readers_.push_back(std::move(reader));

  std::unique_ptr<MockUploadElementReader> reader2(
      new MockUploadElementReader(kTestDataSize, true));
  EXPECT_CALL(*reader2, Init(_)).WillOnce(Return(OK));
  element_readers_.push_back(std::move(reader2));

  std::unique_ptr<MockUploadElementReader> reader3(
      new MockUploadElementReader(kTestDataSize, false));
  reader3->SetAsyncInitExpectation(OK);
  element_readers_.push_back(std::move(reader3));

  std::unique_ptr<MockUploadElementReader> reader4(
      new MockUploadElementReader(kTestDataSize, false));
  reader4->SetAsyncInitExpectation(OK);
  element_readers_.push_back(std::move(reader4));

  std::unique_ptr<MockUploadElementReader> reader5(
      new MockUploadElementReader(kTestDataSize, true));
  EXPECT_CALL(*reader5, Init(_)).WillOnce(Return(OK));
  element_readers_.push_back(std::move(reader5));

  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  // Run Init().
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(callback.callback()));
  EXPECT_EQ(OK, callback.WaitForResult());
}

// Init() of a reader fails asynchronously.
TEST_F(ElementsUploadDataStreamTest, InitAsyncFailureAsync) {
  // Create UploadDataStream with a mock reader.
  std::unique_ptr<MockUploadElementReader> reader(
      new MockUploadElementReader(kTestDataSize, false));
  reader->SetAsyncInitExpectation(ERR_FAILED);
  element_readers_.push_back(std::move(reader));

  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  // Run Init().
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(callback.callback()));
  EXPECT_EQ(ERR_FAILED, callback.WaitForResult());
}

// Init() of a reader fails synchronously.
TEST_F(ElementsUploadDataStreamTest, InitAsyncFailureSync) {
  // Create UploadDataStream with mock readers.
  std::unique_ptr<MockUploadElementReader> reader(
      new MockUploadElementReader(kTestDataSize, false));
  reader->SetAsyncInitExpectation(OK);
  element_readers_.push_back(std::move(reader));

  std::unique_ptr<MockUploadElementReader> reader2(
      new MockUploadElementReader(kTestDataSize, true));
  EXPECT_CALL(*reader2, Init(_)).WillOnce(Return(ERR_FAILED));
  element_readers_.push_back(std::move(reader2));

  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  // Run Init().
  TestCompletionCallback callback;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(callback.callback()));
  EXPECT_EQ(ERR_FAILED, callback.WaitForResult());
}

// Read with a buffer whose size is same as the data.
TEST_F(ElementsUploadDataStreamTest, ReadAsyncWithExactSizeBuffer) {
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  ASSERT_EQ(OK, stream->Init(CompletionCallback()));
  EXPECT_TRUE(stream->IsInMemory());
  EXPECT_EQ(kTestDataSize, stream->size());
  EXPECT_EQ(0U, stream->position());
  EXPECT_FALSE(stream->IsEOF());
  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestDataSize);
  int bytes_read = stream->Read(buf.get(), kTestDataSize, CompletionCallback());
  ASSERT_EQ(static_cast<int>(kTestDataSize), bytes_read);  // Not an error.
  EXPECT_EQ(kTestDataSize, stream->position());
  ASSERT_TRUE(stream->IsEOF());
}

// Async Read() with on-memory and not-on-memory readers.
TEST_F(ElementsUploadDataStreamTest, ReadAsync) {
  // Create UploadDataStream with mock readers.
  std::unique_ptr<MockUploadElementReader> reader(
      new MockUploadElementReader(kTestDataSize, true));
  EXPECT_CALL(*reader, Init(_)).WillOnce(Return(OK));
  reader->SetReadExpectation(kTestDataSize);
  element_readers_.push_back(std::move(reader));

  std::unique_ptr<MockUploadElementReader> reader2(
      new MockUploadElementReader(kTestDataSize, false));
  reader2->SetAsyncInitExpectation(OK);
  reader2->SetReadExpectation(kTestDataSize);
  element_readers_.push_back(std::move(reader2));

  std::unique_ptr<MockUploadElementReader> reader3(
      new MockUploadElementReader(kTestDataSize, true));
  EXPECT_CALL(*reader3, Init(_)).WillOnce(Return(OK));
  reader3->SetReadExpectation(kTestDataSize);
  element_readers_.push_back(std::move(reader3));

  std::unique_ptr<MockUploadElementReader> reader4(
      new MockUploadElementReader(kTestDataSize, false));
  reader4->SetAsyncInitExpectation(OK);
  reader4->SetReadExpectation(kTestDataSize);
  element_readers_.push_back(std::move(reader4));

  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  // Run Init().
  TestCompletionCallback init_callback;
  EXPECT_EQ(ERR_IO_PENDING, stream->Init(init_callback.callback()));
  EXPECT_EQ(OK, init_callback.WaitForResult());

  scoped_refptr<IOBuffer> buf = new IOBuffer(kTestBufferSize);

  // Consume the first element.
  TestCompletionCallback read_callback1;
  EXPECT_EQ(static_cast<int>(kTestDataSize),
            stream->Read(buf.get(), kTestDataSize, read_callback1.callback()));
  base::MessageLoop::current()->RunUntilIdle();
  EXPECT_FALSE(read_callback1.have_result());

  // Consume the second element.
  TestCompletionCallback read_callback2;
  ASSERT_EQ(ERR_IO_PENDING,
            stream->Read(buf.get(), kTestDataSize, read_callback2.callback()));
  EXPECT_EQ(static_cast<int>(kTestDataSize), read_callback2.WaitForResult());

  // Consume the third and the fourth elements.
  TestCompletionCallback read_callback3;
  ASSERT_EQ(
      ERR_IO_PENDING,
      stream->Read(buf.get(), kTestDataSize * 2, read_callback3.callback()));
  EXPECT_EQ(static_cast<int>(kTestDataSize * 2),
            read_callback3.WaitForResult());
}

void ElementsUploadDataStreamTest::FileChangedHelper(
    const base::FilePath& file_path,
    const base::Time& time,
    bool error_expected) {
  // Don't use element_readers_ here, as this function is called twice, and
  // reusing element_readers_ is wrong.
  std::vector<std::unique_ptr<UploadElementReader>> element_readers;
  element_readers.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), file_path, 1, 2, time)));

  TestCompletionCallback init_callback;
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers), 0));
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback.callback()));
  int error_code = init_callback.WaitForResult();
  if (error_expected)
    ASSERT_EQ(ERR_UPLOAD_FILE_CHANGED, error_code);
  else
    ASSERT_EQ(OK, error_code);
}

TEST_F(ElementsUploadDataStreamTest, FileChanged) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));

  base::File::Info file_info;
  ASSERT_TRUE(base::GetFileInfo(temp_file_path, &file_info));

  // Test file not changed.
  FileChangedHelper(temp_file_path, file_info.last_modified, false);

  // Test file changed.
  FileChangedHelper(temp_file_path,
                    file_info.last_modified - base::TimeDelta::FromSeconds(1),
                    true);
}

TEST_F(ElementsUploadDataStreamTest, MultipleInit) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));

  // Prepare data.
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));
  element_readers_.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), temp_file_path, 0,
      std::numeric_limits<uint64_t>::max(), base::Time())));
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  std::string expected_data(kTestData, kTestData + kTestDataSize);
  expected_data += expected_data;

  // Call Init().
  TestCompletionCallback init_callback1;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback1.callback()));
  ASSERT_EQ(OK, init_callback1.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Read.
  EXPECT_EQ(expected_data, ReadFromUploadDataStream(stream.get()));
  EXPECT_TRUE(stream->IsEOF());

  // Call Init() again to reset.
  TestCompletionCallback init_callback2;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback2.callback()));
  ASSERT_EQ(OK, init_callback2.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Read again.
  EXPECT_EQ(expected_data, ReadFromUploadDataStream(stream.get()));
  EXPECT_TRUE(stream->IsEOF());
}

TEST_F(ElementsUploadDataStreamTest, MultipleInitAsync) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));
  TestCompletionCallback test_callback;

  // Prepare data.
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));
  element_readers_.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), temp_file_path, 0,
      std::numeric_limits<uint64_t>::max(), base::Time())));
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  std::string expected_data(kTestData, kTestData + kTestDataSize);
  expected_data += expected_data;

  // Call Init().
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(test_callback.callback()));
  EXPECT_EQ(OK, test_callback.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Read.
  EXPECT_EQ(expected_data, ReadFromUploadDataStream(stream.get()));
  EXPECT_TRUE(stream->IsEOF());

  // Call Init() again to reset.
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(test_callback.callback()));
  EXPECT_EQ(OK, test_callback.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Read again.
  EXPECT_EQ(expected_data, ReadFromUploadDataStream(stream.get()));
  EXPECT_TRUE(stream->IsEOF());
}

TEST_F(ElementsUploadDataStreamTest, InitToReset) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));

  // Prepare data.
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));
  element_readers_.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), temp_file_path, 0,
      std::numeric_limits<uint64_t>::max(), base::Time())));
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  std::vector<char> expected_data(kTestData, kTestData + kTestDataSize);
  expected_data.insert(expected_data.end(), kTestData,
                       kTestData + kTestDataSize);

  // Call Init().
  TestCompletionCallback init_callback1;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback1.callback()));
  EXPECT_EQ(OK, init_callback1.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Read some.
  TestCompletionCallback read_callback1;
  std::vector<char> buf(kTestDataSize + kTestDataSize/2);
  scoped_refptr<IOBuffer> wrapped_buffer = new WrappedIOBuffer(&buf[0]);
  EXPECT_EQ(
      ERR_IO_PENDING,
      stream->Read(wrapped_buffer.get(), buf.size(),
                   read_callback1.callback()));
  EXPECT_EQ(static_cast<int>(buf.size()), read_callback1.WaitForResult());
  EXPECT_EQ(buf.size(), stream->position());

  // Call Init to reset the state.
  TestCompletionCallback init_callback2;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback2.callback()));
  EXPECT_EQ(OK, init_callback2.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Read.
  TestCompletionCallback read_callback2;
  std::vector<char> buf2(kTestDataSize*2);
  scoped_refptr<IOBuffer> wrapped_buffer2 = new WrappedIOBuffer(&buf2[0]);
  EXPECT_EQ(ERR_IO_PENDING,
            stream->Read(
                wrapped_buffer2.get(), buf2.size(), read_callback2.callback()));
  EXPECT_EQ(static_cast<int>(buf2.size()), read_callback2.WaitForResult());
  EXPECT_EQ(expected_data, buf2);
}

TEST_F(ElementsUploadDataStreamTest, InitDuringAsyncInit) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));

  // Prepare data.
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));
  element_readers_.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), temp_file_path, 0,
      std::numeric_limits<uint64_t>::max(), base::Time())));
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  std::vector<char> expected_data(kTestData, kTestData + kTestDataSize);
  expected_data.insert(expected_data.end(), kTestData,
                       kTestData + kTestDataSize);

  // Start Init.
  TestCompletionCallback init_callback1;
  EXPECT_EQ(ERR_IO_PENDING, stream->Init(init_callback1.callback()));

  // Call Init again to cancel the previous init.
  TestCompletionCallback init_callback2;
  EXPECT_EQ(ERR_IO_PENDING, stream->Init(init_callback2.callback()));
  EXPECT_EQ(OK, init_callback2.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Read.
  TestCompletionCallback read_callback2;
  std::vector<char> buf2(kTestDataSize*2);
  scoped_refptr<IOBuffer> wrapped_buffer2 = new WrappedIOBuffer(&buf2[0]);
  EXPECT_EQ(ERR_IO_PENDING,
            stream->Read(
                wrapped_buffer2.get(), buf2.size(), read_callback2.callback()));
  EXPECT_EQ(static_cast<int>(buf2.size()), read_callback2.WaitForResult());
  EXPECT_EQ(expected_data, buf2);
  EXPECT_TRUE(stream->IsEOF());

  // Make sure callbacks are not called for cancelled operations.
  EXPECT_FALSE(init_callback1.have_result());
}

TEST_F(ElementsUploadDataStreamTest, InitDuringAsyncRead) {
  base::FilePath temp_file_path;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(),
                                             &temp_file_path));
  ASSERT_EQ(static_cast<int>(kTestDataSize),
            base::WriteFile(temp_file_path, kTestData, kTestDataSize));

  // Prepare data.
  element_readers_.push_back(
      base::WrapUnique(new UploadBytesElementReader(kTestData, kTestDataSize)));
  element_readers_.push_back(base::WrapUnique(new UploadFileElementReader(
      base::ThreadTaskRunnerHandle::Get().get(), temp_file_path, 0,
      std::numeric_limits<uint64_t>::max(), base::Time())));
  std::unique_ptr<UploadDataStream> stream(
      new ElementsUploadDataStream(std::move(element_readers_), 0));

  std::vector<char> expected_data(kTestData, kTestData + kTestDataSize);
  expected_data.insert(expected_data.end(), kTestData,
                       kTestData + kTestDataSize);

  // Call Init().
  TestCompletionCallback init_callback1;
  ASSERT_EQ(ERR_IO_PENDING, stream->Init(init_callback1.callback()));
  EXPECT_EQ(OK, init_callback1.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Start reading.
  TestCompletionCallback read_callback1;
  std::vector<char> buf(kTestDataSize*2);
  scoped_refptr<IOBuffer> wrapped_buffer = new WrappedIOBuffer(&buf[0]);
  EXPECT_EQ(
      ERR_IO_PENDING,
      stream->Read(wrapped_buffer.get(), buf.size(),
                   read_callback1.callback()));

  // Call Init to cancel the previous read.
  TestCompletionCallback init_callback2;
  EXPECT_EQ(ERR_IO_PENDING, stream->Init(init_callback2.callback()));
  EXPECT_EQ(OK, init_callback2.WaitForResult());
  EXPECT_FALSE(stream->IsEOF());
  EXPECT_EQ(kTestDataSize*2, stream->size());

  // Read.
  TestCompletionCallback read_callback2;
  std::vector<char> buf2(kTestDataSize*2);
  scoped_refptr<IOBuffer> wrapped_buffer2 = new WrappedIOBuffer(&buf2[0]);
  EXPECT_EQ(ERR_IO_PENDING,
            stream->Read(
                wrapped_buffer2.get(), buf2.size(), read_callback2.callback()));
  EXPECT_EQ(static_cast<int>(buf2.size()), read_callback2.WaitForResult());
  EXPECT_EQ(expected_data, buf2);
  EXPECT_TRUE(stream->IsEOF());

  // Make sure callbacks are not called for cancelled operations.
  EXPECT_FALSE(read_callback1.have_result());
}

}  // namespace net