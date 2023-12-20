//
// Copyright (C) 2020 The Android Open Source_info Project
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
//

#include "writer_v3.h"

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <brotli/encode.h>
#include <libsnapshot/cow_format.h>
#include <libsnapshot/cow_reader.h>
#include <libsnapshot/cow_writer.h>
#include <lz4.h>
#include <zlib.h>

#include <fcntl.h>
#include <libsnapshot_cow/parser_v3.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <numeric>

// The info messages here are spammy, but as useful for update_engine. Disable
// them when running on the host.
#ifdef __ANDROID__
#define LOG_INFO LOG(INFO)
#else
#define LOG_INFO LOG(VERBOSE)
#endif

namespace android {
namespace snapshot {

static_assert(sizeof(off_t) == sizeof(uint64_t));

using android::base::unique_fd;

CowWriterV3::CowWriterV3(const CowOptions& options, unique_fd&& fd)
    : CowWriterBase(options, std::move(fd)), batch_size_(std::max<size_t>(options.cluster_ops, 1)) {
    SetupHeaders();
}

void CowWriterV3::SetupHeaders() {
    header_ = {};
    header_.prefix.magic = kCowMagicNumber;
    header_.prefix.major_version = 3;
    header_.prefix.minor_version = 0;
    header_.prefix.header_size = sizeof(CowHeaderV3);
    header_.footer_size = 0;
    header_.op_size = sizeof(CowOperationV3);
    header_.block_size = options_.block_size;
    header_.num_merge_ops = options_.num_merge_ops;
    header_.cluster_ops = 0;
    if (batch_size_ > 1) {
        LOG(INFO) << "Batch writes enabled with batch size of " << batch_size_;
    }
    if (options_.scratch_space) {
        header_.buffer_size = BUFFER_REGION_DEFAULT_SIZE;
    }

    // v3 specific fields
    // WIP: not quite sure how some of these are calculated yet, assuming buffer_size is determined
    // during COW size estimation
    header_.sequence_data_count = 0;
    header_.resume_point_count = 0;
    header_.resume_point_max = kNumResumePoints;
    header_.op_count = 0;
    header_.op_count_max = 0;
    header_.compression_algorithm = kCowCompressNone;
    return;
}

bool CowWriterV3::ParseOptions() {
    num_compress_threads_ = std::max(options_.num_compress_threads, 1);
    auto parts = android::base::Split(options_.compression, ",");
    if (parts.size() > 2) {
        LOG(ERROR) << "failed to parse compression parameters: invalid argument count: "
                   << parts.size() << " " << options_.compression;
        return false;
    }
    auto algorithm = CompressionAlgorithmFromString(parts[0]);
    if (!algorithm) {
        LOG(ERROR) << "unrecognized compression: " << options_.compression;
        return false;
    }
    header_.compression_algorithm = *algorithm;
    header_.op_count_max = options_.op_count_max;

    if (parts.size() > 1) {
        if (!android::base::ParseUint(parts[1], &compression_.compression_level)) {
            LOG(ERROR) << "failed to parse compression level invalid type: " << parts[1];
            return false;
        }
    } else {
        compression_.compression_level =
                CompressWorker::GetDefaultCompressionLevel(algorithm.value());
    }

    compression_.algorithm = *algorithm;
    if (compression_.algorithm != kCowCompressNone) {
        compressor_ = ICompressor::Create(compression_, header_.block_size);
        if (compressor_ == nullptr) {
            LOG(ERROR) << "Failed to create compressor for " << compression_.algorithm;
            return false;
        }
    }
    return true;
}

CowWriterV3::~CowWriterV3() {}

bool CowWriterV3::Initialize(std::optional<uint64_t> label) {
    if (!InitFd() || !ParseOptions()) {
        return false;
    }
    if (!label) {
        if (!OpenForWrite()) {
            return false;
        }
    } else {
        if (!OpenForAppend(*label)) {
            return false;
        }
    }

    return true;
}

bool CowWriterV3::OpenForWrite() {
    // This limitation is tied to the data field size in CowOperationV2.
    // Keeping this for V3 writer <- although we
    if (header_.block_size > std::numeric_limits<uint16_t>::max()) {
        LOG(ERROR) << "Block size is too large";
        return false;
    }

    if (lseek(fd_.get(), 0, SEEK_SET) < 0) {
        PLOG(ERROR) << "lseek failed";
        return false;
    }

    // Headers are not complete, but this ensures the file is at the right
    // position.
    if (!android::base::WriteFully(fd_, &header_, sizeof(header_))) {
        PLOG(ERROR) << "write failed";
        return false;
    }

    if (options_.scratch_space) {
        // Initialize the scratch space
        std::string data(header_.buffer_size, 0);
        if (!android::base::WriteFully(fd_, data.data(), header_.buffer_size)) {
            PLOG(ERROR) << "writing scratch space failed";
            return false;
        }
    }

    resume_points_ = std::make_shared<std::vector<ResumePoint>>();

    if (!Sync()) {
        LOG(ERROR) << "Header sync failed";
        return false;
    }
    next_data_pos_ = GetDataOffset(header_);
    return true;
}

bool CowWriterV3::OpenForAppend(uint64_t label) {
    CowHeaderV3 header_v3{};
    if (!ReadCowHeader(fd_, &header_v3)) {
        LOG(ERROR) << "Couldn't read Cow Header";
        return false;
    }

    header_ = header_v3;

    CHECK(label >= 0);
    CowParserV3 parser;
    if (!parser.Parse(fd_, header_, label)) {
        PLOG(ERROR) << "unable to parse with given label: " << label;
        return false;
    }

    resume_points_ = parser.resume_points();
    options_.block_size = header_.block_size;
    next_data_pos_ = GetDataOffset(header_);

    TranslatedCowOps ops;
    parser.Translate(&ops);
    header_.op_count = ops.ops->size();

    for (const auto& op : *ops.ops) {
        next_data_pos_ += op.data_length;
    }

    return true;
}

bool CowWriterV3::CheckOpCount(size_t op_count) {
    if (IsEstimating()) {
        return true;
    }
    if (header_.op_count + op_count > header_.op_count_max) {
        LOG(ERROR) << "Current number of ops on disk: " << header_.op_count
                   << ", number of ops attempting to write: " << op_count
                   << ", this will exceed max op count " << header_.op_count_max;
        return false;
    }
    return true;
}

bool CowWriterV3::EmitCopy(uint64_t new_block, uint64_t old_block, uint64_t num_blocks) {
    if (!CheckOpCount(num_blocks)) {
        return false;
    }
    std::vector<CowOperationV3> ops(num_blocks);
    for (size_t i = 0; i < num_blocks; i++) {
        CowOperationV3& op = ops[i];
        op.set_type(kCowCopyOp);
        op.new_block = new_block + i;
        op.set_source(old_block + i);
    }
    if (!WriteOperation({ops.data(), ops.size()}, {})) {
        return false;
    }

    return true;
}

bool CowWriterV3::EmitRawBlocks(uint64_t new_block_start, const void* data, size_t size) {
    if (!CheckOpCount(size / header_.block_size)) {
        return false;
    }
    return EmitBlocks(new_block_start, data, size, 0, 0, kCowReplaceOp);
}

bool CowWriterV3::EmitXorBlocks(uint32_t new_block_start, const void* data, size_t size,
                                uint32_t old_block, uint16_t offset) {
    if (!CheckOpCount(size / header_.block_size)) {
        return false;
    }
    return EmitBlocks(new_block_start, data, size, old_block, offset, kCowXorOp);
}

bool CowWriterV3::EmitBlocks(uint64_t new_block_start, const void* data, size_t size,
                             uint64_t old_block, uint16_t offset, CowOperationType type) {
    if (compression_.algorithm != kCowCompressNone && compressor_ == nullptr) {
        LOG(ERROR) << "Compression algorithm is " << compression_.algorithm
                   << " but compressor is uninitialized.";
        return false;
    }
    const size_t num_blocks = (size / header_.block_size);
    if (compression_.algorithm == kCowCompressNone) {
        std::vector<CowOperationV3> ops(num_blocks);
        for (size_t i = 0; i < num_blocks; i++) {
            CowOperation& op = ops[i];
            op.new_block = new_block_start + i;

            op.set_type(type);
            if (type == kCowXorOp) {
                op.set_source((old_block + i) * header_.block_size + offset);
            } else {
                op.set_source(next_data_pos_ + header_.block_size * i);
            }
            op.data_length = header_.block_size;
        }
        return WriteOperation({ops.data(), ops.size()}, data, size);
    }

    for (size_t i = 0; i < num_blocks; i += batch_size_) {
        const auto blocks_to_write = std::min<size_t>(batch_size_, num_blocks - i);
        std::vector<std::basic_string<uint8_t>> compressed_blocks(blocks_to_write);
        std::vector<CowOperationV3> ops(blocks_to_write);
        std::vector<struct iovec> vec(blocks_to_write);
        size_t compressed_bytes = 0;
        for (size_t j = 0; j < blocks_to_write; j++) {
            const uint8_t* const iter =
                    reinterpret_cast<const uint8_t*>(data) + (header_.block_size * (i + j));

            CowOperation& op = ops[j];
            op.new_block = new_block_start + i + j;

            op.set_type(type);
            if (type == kCowXorOp) {
                op.set_source((old_block + i + j) * header_.block_size + offset);
            } else {
                op.set_source(next_data_pos_ + compressed_bytes);
            }

            std::basic_string<uint8_t> compressed_data =
                    compressor_->Compress(iter, header_.block_size);
            if (compressed_data.empty()) {
                LOG(ERROR) << "Compression failed during EmitBlocks(" << new_block_start << ", "
                           << num_blocks << ");";
                return false;
            }
            if (compressed_data.size() >= header_.block_size) {
                compressed_data.resize(header_.block_size);
                std::memcpy(compressed_data.data(), iter, header_.block_size);
            }
            compressed_blocks[j] = std::move(compressed_data);
            vec[j] = {.iov_base = compressed_blocks[j].data(),
                      .iov_len = compressed_blocks[j].size()};
            op.data_length = vec[j].iov_len;
            compressed_bytes += op.data_length;
        }
        if (!WriteOperation({ops.data(), ops.size()}, {vec.data(), vec.size()})) {
            PLOG(ERROR) << "AddRawBlocks with compression: write failed. new block: "
                        << new_block_start << " compression: " << compression_.algorithm;
            return false;
        }
    }

    return true;
}

bool CowWriterV3::EmitZeroBlocks(uint64_t new_block_start, const uint64_t num_blocks) {
    if (!CheckOpCount(num_blocks)) {
        return false;
    }
    std::vector<CowOperationV3> ops(num_blocks);
    for (uint64_t i = 0; i < num_blocks; i++) {
        auto& op = ops[i];
        op.set_type(kCowZeroOp);
        op.new_block = new_block_start + i;
    }
    if (!WriteOperation({ops.data(), ops.size()})) {
        return false;
    }
    return true;
}

bool CowWriterV3::EmitLabel(uint64_t label) {
    // remove all labels greater than this current one. we want to avoid the situation of adding
    // in
    // duplicate labels with differing op values
    auto remove_if_callback = [&](const auto& resume_point) -> bool {
        if (resume_point.label >= label) return true;
        return false;
    };
    resume_points_->erase(
            std::remove_if(resume_points_->begin(), resume_points_->end(), remove_if_callback),
            resume_points_->end());

    resume_points_->push_back({label, header_.op_count});
    header_.resume_point_count++;
    // remove the oldest resume point if resume_buffer is full
    while (resume_points_->size() > header_.resume_point_max) {
        resume_points_->erase(resume_points_->begin());
    }

    CHECK_LE(resume_points_->size(), header_.resume_point_max);

    if (!android::base::WriteFullyAtOffset(fd_, resume_points_->data(),
                                           resume_points_->size() * sizeof(ResumePoint),
                                           GetResumeOffset(header_))) {
        PLOG(ERROR) << "writing resume buffer failed";
        return false;
    }
    return Finalize();
}

bool CowWriterV3::EmitSequenceData(size_t num_ops, const uint32_t* data) {
    // TODO: size sequence buffer based on options
    if (header_.op_count > 0) {
        LOG(ERROR)
                << "There's " << header_.op_count
                << " operations written to disk. Writing sequence data is only allowed before all "
                   "operation writes.";
        return false;
    }
    header_.sequence_data_count = num_ops;
    // In COW format v3, data section is placed after op section and sequence
    // data section. Therefore, changing the sequence data count has the effect
    // of moving op section and data section. Therefore we need to reset the
    // value of |next_data_pos|. This is also the reason why writing sequence
    // data is only allowed if there's no operation written.
    next_data_pos_ = GetDataOffset(header_);
    if (!android::base::WriteFullyAtOffset(fd_, data, sizeof(data[0]) * num_ops,
                                           GetSequenceOffset(header_))) {
        PLOG(ERROR) << "writing sequence buffer failed";
        return false;
    }
    return true;
}

bool CowWriterV3::WriteOperation(std::basic_string_view<CowOperationV3> op, const void* data,
                                 size_t size) {
    struct iovec vec {
        .iov_len = size
    };
    // Dear C++ god, this is effectively a const_cast. I had to do this because
    // pwritev()'s struct iovec requires a non-const pointer. The input data
    // will not be modified, as the iovec is only used for a write operation.
    std::memcpy(&vec.iov_base, &data, sizeof(data));
    return WriteOperation(op, {&vec, 1});
}

bool CowWriterV3::WriteOperation(std::basic_string_view<CowOperationV3> ops,
                                 std::basic_string_view<struct iovec> data) {
    const auto total_data_size =
            std::transform_reduce(data.begin(), data.end(), 0, std::plus<size_t>{},
                                  [](const struct iovec& a) { return a.iov_len; });
    if (IsEstimating()) {
        header_.op_count += ops.size();
        if (header_.op_count > header_.op_count_max) {
            // If we increment op_count_max, the offset of data section would
            // change. So need to update |next_data_pos_|
            next_data_pos_ += (header_.op_count - header_.op_count_max) * sizeof(CowOperationV3);
            header_.op_count_max = header_.op_count;
        }
        next_data_pos_ += total_data_size;
        return true;
    }

    if (header_.op_count + ops.size() > header_.op_count_max) {
        LOG(ERROR) << "Current op count " << header_.op_count << ", attempting to write "
                   << ops.size() << " ops will exceed the max of " << header_.op_count_max;
        return false;
    }

    const off_t offset = GetOpOffset(header_.op_count, header_);
    if (!android::base::WriteFullyAtOffset(fd_, ops.data(), ops.size() * sizeof(ops[0]), offset)) {
        PLOG(ERROR) << "Write failed for " << ops.size() << " ops at " << offset;
        return false;
    }
    if (!data.empty()) {
        const auto ret = pwritev(fd_, data.data(), data.size(), next_data_pos_);
        if (ret != total_data_size) {
            PLOG(ERROR) << "write failed for data of size: " << data.size()
                        << " at offset: " << next_data_pos_ << " " << ret;
            return false;
        }
    }
    header_.op_count += ops.size();
    next_data_pos_ += total_data_size;

    return true;
}

bool CowWriterV3::WriteOperation(const CowOperationV3& op, const void* data, size_t size) {
    return WriteOperation({&op, 1}, data, size);
}

bool CowWriterV3::Finalize() {
    CHECK_GE(header_.prefix.header_size, sizeof(CowHeaderV3));
    CHECK_LE(header_.prefix.header_size, sizeof(header_));
    if (!android::base::WriteFullyAtOffset(fd_, &header_, header_.prefix.header_size, 0)) {
        return false;
    }
    return Sync();
}

uint64_t CowWriterV3::GetCowSize() {
    return next_data_pos_;
}

}  // namespace snapshot
}  // namespace android
