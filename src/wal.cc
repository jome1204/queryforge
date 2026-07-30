#include "internal.h"

namespace queryforge {

WalCodec::WalCodec(Limits limits) : limits_(limits) {}

std::vector<uint8_t> WalCodec::encode(const std::vector<WalRecord> &records,
                                      Error &error) const {
  error.clear();
  if (records.size() > limits_.max_wal_records) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "WAL record count exceeds limit");
    return {};
  }
  std::vector<uint8_t> output;
  output.insert(output.end(), {'Q', 'F', 'W', 'A', 'L', '1', 0, 0});
  internal::append32(output, 1);
  internal::append32(output, static_cast<uint32_t>(records.size()));
  uint64_t previous_lsn = 0;
  for (const WalRecord &record : records) {
    if (record.lsn == 0 || record.lsn <= previous_lsn ||
        record.payload.size() > limits_.max_record_bytes) {
      internal::fail(error, ErrorCode::invalid_value, record.lsn,
                     "WAL record ordering or payload is invalid");
      return {};
    }
    size_t start = output.size();
    internal::append32(output, 0);
    internal::append16(output, static_cast<uint16_t>(record.type));
    internal::append16(output, 0);
    internal::append64(output, record.lsn);
    internal::append64(output, record.transaction_id);
    internal::append32(output, record.page_id);
    internal::append32(output, record.generation);
    internal::append32(output, static_cast<uint32_t>(record.payload.size()));
    output.insert(output.end(), record.payload.begin(), record.payload.end());
    internal::append32(
        output, crc32(output.data() + start + 4, output.size() - start - 4));
    uint64_t length = output.size() - start;
    if (length > UINT32_MAX || output.size() > limits_.max_wal_bytes) {
      internal::fail(error, ErrorCode::resource_limit, start,
                     "encoded WAL exceeds limit");
      return {};
    }
    internal::patch32(output, start, static_cast<uint32_t>(length));
    previous_lsn = record.lsn;
  }
  return output;
}

std::optional<std::vector<WalRecord>>
WalCodec::decode(const uint8_t *data, size_t size, Error &error) const {
  error.clear();
  if (size < 16 || size > limits_.max_wal_bytes ||
      std::memcmp(data, "QFWAL1\0\0", 8) != 0) {
    internal::fail(
        error, size < 16 ? ErrorCode::truncated : ErrorCode::invalid_signature,
        0, "WAL file header is invalid");
    return std::nullopt;
  }
  uint32_t version = internal::le32(data + 8);
  uint32_t count = internal::le32(data + 12);
  if (version != 1 || count > limits_.max_wal_records) {
    internal::fail(error, ErrorCode::invalid_version, 8,
                   "WAL version or record count is invalid");
    return std::nullopt;
  }
  size_t position = 16;
  uint64_t previous_lsn = 0;
  std::vector<WalRecord> output;
  output.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    if (size - position < 40) {
      internal::fail(error, ErrorCode::truncated, position,
                     "WAL record header is truncated");
      return std::nullopt;
    }
    uint32_t length = internal::le32(data + position);
    if (length < 40 || length > size - position) {
      internal::fail(error, ErrorCode::invalid_offset, position,
                     "WAL record length exceeds file");
      return std::nullopt;
    }
    uint16_t raw_type = internal::le16(data + position + 4);
    if (raw_type < static_cast<uint16_t>(WalRecordType::begin) ||
        raw_type > static_cast<uint16_t>(WalRecordType::checkpoint)) {
      internal::fail(error, ErrorCode::invalid_value, position + 4,
                     "WAL record type is invalid");
      return std::nullopt;
    }
    uint32_t payload_size = internal::le32(data + position + 32);
    if (payload_size > limits_.max_record_bytes ||
        static_cast<uint64_t>(payload_size) + 40 != length) {
      internal::fail(error, ErrorCode::invalid_offset, position + 32,
                     "WAL payload length is inconsistent");
      return std::nullopt;
    }
    uint32_t stored = internal::le32(data + position + length - 4);
    uint32_t calculated = crc32(data + position + 4, length - 8);
    if (stored != calculated) {
      internal::fail(error, ErrorCode::checksum_mismatch, position + length - 4,
                     "WAL record checksum mismatch");
      return std::nullopt;
    }
    WalRecord record;
    record.type = static_cast<WalRecordType>(raw_type);
    record.lsn = internal::le64(data + position + 8);
    record.transaction_id = internal::le64(data + position + 16);
    record.page_id = internal::le32(data + position + 24);
    record.generation = internal::le32(data + position + 28);
    if (record.lsn == 0 || record.lsn <= previous_lsn) {
      internal::fail(error, ErrorCode::invalid_value, position + 8,
                     "WAL LSN ordering is invalid");
      return std::nullopt;
    }
    record.payload.assign(data + position + 36,
                          data + position + 36 + payload_size);
    output.push_back(std::move(record));
    previous_lsn = output.back().lsn;
    position += length;
  }
  if (position != size) {
    internal::fail(error, ErrorCode::invalid_offset, position,
                   "WAL contains trailing bytes");
    return std::nullopt;
  }
  return output;
}

} // namespace queryforge
