#include "internal.h"

namespace queryforge {
namespace {
bool append_value(std::vector<uint8_t> &output, const Value &value,
                  const Limits &limits, Error &error) {
  output.push_back(static_cast<uint8_t>(value.type()));
  switch (value.type()) {
  case DataType::null_type:
    return true;
  case DataType::integer:
    internal::append64(output, static_cast<uint64_t>(*value.as_integer()));
    return true;
  case DataType::real: {
    double number = *value.as_real();
    uint64_t bits = 0;
    std::memcpy(&bits, &number, sizeof(bits));
    internal::append64(output, bits);
    return true;
  }
  case DataType::boolean:
    output.push_back(*value.as_boolean() ? 1 : 0);
    return true;
  case DataType::text: {
    auto text = value.as_text();
    if (text->size() > limits.max_string_bytes)
      return internal::fail(error, ErrorCode::resource_limit, output.size(),
                            "record text exceeds limit");
    internal::append32(output, static_cast<uint32_t>(text->size()));
    output.insert(output.end(), text->begin(), text->end());
    return true;
  }
  case DataType::blob: {
    const auto *blob = value.as_blob();
    if (blob->size() > limits.max_record_bytes)
      return internal::fail(error, ErrorCode::resource_limit, output.size(),
                            "record blob exceeds limit");
    internal::append32(output, static_cast<uint32_t>(blob->size()));
    output.insert(output.end(), blob->begin(), blob->end());
    return true;
  }
  }
  return false;
}

std::optional<Value> read_value(const uint8_t *data, size_t size,
                                size_t &position, const Limits &limits,
                                Error &error) {
  if (position >= size) {
    internal::fail(error, ErrorCode::truncated, position,
                   "record value type is truncated");
    return std::nullopt;
  }
  DataType type = static_cast<DataType>(data[position++]);
  if (type == DataType::null_type)
    return Value();
  if (type == DataType::integer || type == DataType::real) {
    if (size - position < 8) {
      internal::fail(error, ErrorCode::truncated, position,
                     "record numeric value is truncated");
      return std::nullopt;
    }
    uint64_t raw = internal::le64(data + position);
    position += 8;
    if (type == DataType::integer)
      return Value(static_cast<int64_t>(raw));
    double number = 0;
    std::memcpy(&number, &raw, sizeof(number));
    if (!std::isfinite(number)) {
      internal::fail(error, ErrorCode::type_error, position - 8,
                     "record real value is nonfinite");
      return std::nullopt;
    }
    return Value(number);
  }
  if (type == DataType::boolean) {
    if (position >= size || data[position] > 1) {
      internal::fail(error, ErrorCode::type_error, position,
                     "record boolean value is invalid");
      return std::nullopt;
    }
    return Value(data[position++] != 0);
  }
  if (type == DataType::text || type == DataType::blob) {
    if (size - position < 4) {
      internal::fail(error, ErrorCode::truncated, position,
                     "record variable length is truncated");
      return std::nullopt;
    }
    uint32_t length = internal::le32(data + position);
    position += 4;
    uint64_t maximum = type == DataType::text ? limits.max_string_bytes
                                              : limits.max_record_bytes;
    if (length > maximum || length > size - position) {
      internal::fail(error, ErrorCode::resource_limit, position,
                     "record variable value exceeds bounds");
      return std::nullopt;
    }
    if (type == DataType::text) {
      std::string text(reinterpret_cast<const char *>(data + position), length);
      position += length;
      return Value(std::move(text));
    }
    Value::Blob blob(data + position, data + position + length);
    position += length;
    return Value(std::move(blob));
  }
  internal::fail(error, ErrorCode::type_error, position - 1,
                 "record value has unknown type");
  return std::nullopt;
}
} // namespace

RecordCodec::RecordCodec(Limits limits) : limits_(limits) {}

std::vector<uint8_t> RecordCodec::encode(const RowRecord &record,
                                         Error &error) const {
  error.clear();
  if (record.values.size() > limits_.max_columns) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "record column count exceeds limit");
    return {};
  }
  std::vector<uint8_t> output;
  output.insert(output.end(), {'Q', 'F', 'R', '1'});
  internal::append64(output, record.row_id);
  internal::append64(output, record.generation);
  output.push_back(record.deleted ? 1 : 0);
  internal::append16(output, static_cast<uint16_t>(record.values.size()));
  for (const Value &value : record.values)
    if (!append_value(output, value, limits_, error))
      return {};
  if (output.size() + 4 > limits_.max_record_bytes) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "encoded record exceeds limit");
    return {};
  }
  internal::append32(output, crc32(output.data(), output.size()));
  return output;
}

std::optional<RowRecord> RecordCodec::decode(const uint8_t *data, size_t size,
                                             Error &error) const {
  error.clear();
  if (size < 27 || size > limits_.max_record_bytes ||
      std::memcmp(data, "QFR1", 4) != 0) {
    internal::fail(
        error, size < 27 ? ErrorCode::truncated : ErrorCode::invalid_signature,
        0, "record header is invalid");
    return std::nullopt;
  }
  uint32_t stored = internal::le32(data + size - 4);
  if (crc32(data, size - 4) != stored) {
    internal::fail(error, ErrorCode::checksum_mismatch, size - 4,
                   "record checksum mismatch");
    return std::nullopt;
  }
  RowRecord output;
  output.row_id = internal::le64(data + 4);
  output.generation = internal::le64(data + 12);
  output.deleted = data[20] != 0;
  uint16_t columns = internal::le16(data + 21);
  if (columns > limits_.max_columns) {
    internal::fail(error, ErrorCode::resource_limit, 21,
                   "record column count exceeds limit");
    return std::nullopt;
  }
  size_t position = 23;
  for (uint16_t column = 0; column < columns; ++column) {
    auto value = read_value(data, size - 4, position, limits_, error);
    if (!value)
      return std::nullopt;
    output.values.push_back(std::move(*value));
  }
  if (position != size - 4) {
    internal::fail(error, ErrorCode::invalid_offset, position,
                   "record has trailing bytes");
    return std::nullopt;
  }
  return output;
}

std::vector<uint8_t> RecordCodec::encode_key(const Row &row,
                                             const std::vector<size_t> &columns,
                                             Error &error) const {
  error.clear();
  if (columns.empty() || columns.size() > limits_.max_columns) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "index key column count is invalid");
    return {};
  }
  std::vector<uint8_t> output;
  for (size_t column : columns) {
    if (column >= row.size()) {
      internal::fail(error, ErrorCode::invalid_offset, column,
                     "index key column exceeds row");
      return {};
    }
    if (!append_value(output, row[column], limits_, error))
      return {};
  }
  if (output.size() > limits_.max_record_bytes) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "index key exceeds record limit");
    return {};
  }
  return output;
}

} // namespace queryforge
