#include "internal.h"

#include <charconv>
#include <iomanip>

namespace queryforge {

Value::Value(int64_t value) : storage_(value) {}
Value::Value(double value) : storage_(value) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(const char *value) : storage_(std::string(value)) {}
Value::Value(bool value) : storage_(value) {}
Value::Value(Blob value) : storage_(std::move(value)) {}

DataType Value::type() const {
  switch (storage_.index()) {
  case 0:
    return DataType::null_type;
  case 1:
    return DataType::integer;
  case 2:
    return DataType::real;
  case 3:
    return DataType::text;
  case 4:
    return DataType::boolean;
  case 5:
    return DataType::blob;
  default:
    return DataType::null_type;
  }
}
bool Value::is_null() const {
  return std::holds_alternative<std::monostate>(storage_);
}
std::optional<int64_t> Value::as_integer() const {
  if (auto value = std::get_if<int64_t>(&storage_))
    return *value;
  if (auto value = std::get_if<double>(&storage_)) {
    if (std::isfinite(*value) &&
        *value >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
        *value <= static_cast<double>(std::numeric_limits<int64_t>::max()))
      return static_cast<int64_t>(*value);
  }
  if (auto value = std::get_if<bool>(&storage_))
    return *value ? 1 : 0;
  return std::nullopt;
}
std::optional<double> Value::as_real() const {
  if (auto value = std::get_if<double>(&storage_))
    return *value;
  if (auto value = std::get_if<int64_t>(&storage_))
    return static_cast<double>(*value);
  if (auto value = std::get_if<bool>(&storage_))
    return *value ? 1.0 : 0.0;
  return std::nullopt;
}
std::optional<bool> Value::as_boolean() const {
  if (auto value = std::get_if<bool>(&storage_))
    return *value;
  if (auto value = std::get_if<int64_t>(&storage_))
    return *value != 0;
  if (auto value = std::get_if<double>(&storage_))
    return std::isfinite(*value) && *value != 0.0;
  if (auto value = std::get_if<std::string>(&storage_))
    return !value->empty();
  return std::nullopt;
}
std::optional<std::string_view> Value::as_text() const {
  if (auto value = std::get_if<std::string>(&storage_))
    return *value;
  return std::nullopt;
}
const Value::Blob *Value::as_blob() const {
  return std::get_if<Blob>(&storage_);
}
std::string Value::display() const {
  if (is_null())
    return "NULL";
  if (auto value = std::get_if<int64_t>(&storage_))
    return std::to_string(*value);
  if (auto value = std::get_if<double>(&storage_)) {
    std::ostringstream output;
    output << std::setprecision(17) << *value;
    return output.str();
  }
  if (auto value = std::get_if<std::string>(&storage_))
    return *value;
  if (auto value = std::get_if<bool>(&storage_))
    return *value ? "TRUE" : "FALSE";
  if (auto value = std::get_if<Blob>(&storage_)) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output = "X'";
    output.reserve(value->size() * 2 + 3);
    for (uint8_t byte : *value) {
      output.push_back(digits[byte >> 4]);
      output.push_back(digits[byte & 15]);
    }
    output.push_back('\'');
    return output;
  }
  return {};
}
uint64_t Value::memory_usage() const {
  if (auto value = std::get_if<std::string>(&storage_))
    return value->size();
  if (auto value = std::get_if<Blob>(&storage_))
    return value->size();
  return sizeof(storage_);
}
bool operator==(const Value &left, const Value &right) {
  if (left.type() == right.type())
    return left.storage_ == right.storage_;
  auto left_number = left.as_real();
  auto right_number = right.as_real();
  return left_number && right_number && *left_number == *right_number;
}

CompareResult compare_values(const Value &left, const Value &right) {
  if (left.is_null() || right.is_null())
    return CompareResult::unordered;
  auto left_number = left.as_real();
  auto right_number = right.as_real();
  if (left_number && right_number) {
    if (!std::isfinite(*left_number) || !std::isfinite(*right_number))
      return CompareResult::unordered;
    if (*left_number < *right_number)
      return CompareResult::less;
    if (*left_number > *right_number)
      return CompareResult::greater;
    return CompareResult::equal;
  }
  auto left_text = left.as_text();
  auto right_text = right.as_text();
  if (left_text && right_text) {
    if (*left_text < *right_text)
      return CompareResult::less;
    if (*left_text > *right_text)
      return CompareResult::greater;
    return CompareResult::equal;
  }
  if (left.type() != right.type())
    return CompareResult::unordered;
  if (auto left_blob = left.as_blob()) {
    const auto *right_blob = right.as_blob();
    if (*left_blob < *right_blob)
      return CompareResult::less;
    if (*left_blob > *right_blob)
      return CompareResult::greater;
    return CompareResult::equal;
  }
  return CompareResult::unordered;
}

Expression::Expression(const Expression &other)
    : kind(other.kind), literal(other.literal), qualifier(other.qualifier),
      name(other.name), unary(other.unary), binary(other.binary),
      left(internal::clone(other.left)), right(internal::clone(other.right)),
      offset(other.offset) {
  for (const auto &argument : other.arguments)
    arguments.push_back(internal::clone(argument));
}
Expression &Expression::operator=(const Expression &other) {
  if (this == &other)
    return *this;
  Expression copy(other);
  *this = std::move(copy);
  return *this;
}

std::optional<size_t> Schema::column_index(std::string_view name) const {
  std::string normalized = internal::normalize(name);
  for (size_t i = 0; i < columns.size(); ++i)
    if (internal::normalize(columns[i].name) == normalized)
      return i;
  return std::nullopt;
}

uint32_t crc32(const uint8_t *data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

std::string error_code_name(ErrorCode code) {
  switch (code) {
  case ErrorCode::none:
    return "none";
  case ErrorCode::truncated:
    return "truncated";
  case ErrorCode::invalid_signature:
    return "invalid_signature";
  case ErrorCode::invalid_version:
    return "invalid_version";
  case ErrorCode::lexical_error:
    return "lexical_error";
  case ErrorCode::syntax_error:
    return "syntax_error";
  case ErrorCode::semantic_error:
    return "semantic_error";
  case ErrorCode::type_error:
    return "type_error";
  case ErrorCode::constraint_error:
    return "constraint_error";
  case ErrorCode::not_found:
    return "not_found";
  case ErrorCode::duplicate:
    return "duplicate";
  case ErrorCode::invalid_value:
    return "invalid_value";
  case ErrorCode::invalid_state:
    return "invalid_state";
  case ErrorCode::invalid_offset:
    return "invalid_offset";
  case ErrorCode::overflow:
    return "overflow";
  case ErrorCode::resource_limit:
    return "resource_limit";
  case ErrorCode::checksum_mismatch:
    return "checksum_mismatch";
  case ErrorCode::transaction_conflict:
    return "transaction_conflict";
  case ErrorCode::unsupported:
    return "unsupported";
  }
  return "unknown";
}
std::string data_type_name(DataType type) {
  switch (type) {
  case DataType::null_type:
    return "NULL";
  case DataType::integer:
    return "INTEGER";
  case DataType::real:
    return "REAL";
  case DataType::text:
    return "TEXT";
  case DataType::boolean:
    return "BOOLEAN";
  case DataType::blob:
    return "BLOB";
  }
  return "UNKNOWN";
}

} // namespace queryforge
