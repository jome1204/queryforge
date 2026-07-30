#include "internal.h"

#include <charconv>

namespace queryforge {
namespace {
bool keyword(TokenKind kind) { return kind >= TokenKind::keyword_select; }
bool punctuation(TokenKind kind) {
  return kind == TokenKind::comma || kind == TokenKind::dot ||
         kind == TokenKind::semicolon || kind == TokenKind::left_parenthesis ||
         kind == TokenKind::right_parenthesis;
}
std::string quote_csv(std::string value) {
  bool quote = value.find_first_of(",\"\r\n") != std::string::npos;
  if (!quote)
    return value;
  std::string output = "\"";
  for (char c : value) {
    if (c == '"')
      output.push_back('"');
    output.push_back(c);
  }
  output.push_back('"');
  return output;
}
std::optional<Value> csv_value(std::string value, DataType type, Error &error) {
  if (value.empty())
    return Value();
  if (type == DataType::text)
    return Value(std::move(value));
  if (type == DataType::integer) {
    int64_t number = 0;
    auto result =
        std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec == std::errc{} && result.ptr == value.data() + value.size())
      return Value(number);
  } else if (type == DataType::real) {
    char *end = nullptr;
    double number = std::strtod(value.c_str(), &end);
    if (end == value.data() + value.size() && std::isfinite(number))
      return Value(number);
  } else if (type == DataType::boolean) {
    std::string normalized = internal::normalize(value);
    if (normalized == "TRUE" || normalized == "1")
      return Value(true);
    if (normalized == "FALSE" || normalized == "0")
      return Value(false);
  } else if (type == DataType::blob) {
    return Value(Value::Blob(value.begin(), value.end()));
  }
  internal::fail(error, ErrorCode::type_error, 0,
                 "CSV value cannot be converted to column type");
  return std::nullopt;
}
uint64_t fnv(const std::vector<uint8_t> &bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}
void append_string(std::vector<uint8_t> &output, std::string_view value) {
  internal::append32(output, static_cast<uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}
bool take_string(const uint8_t *data, size_t size, size_t &position,
                 uint32_t maximum, std::string &output, Error &error) {
  if (size - position < 4)
    return internal::fail(error, ErrorCode::truncated, position,
                          "backup string length is truncated");
  uint32_t length = internal::le32(data + position);
  position += 4;
  if (length > maximum || length > size - position)
    return internal::fail(error, ErrorCode::resource_limit, position,
                          "backup string exceeds bounds");
  output.assign(reinterpret_cast<const char *>(data + position), length);
  position += length;
  return true;
}
} // namespace

SqlFormatter::SqlFormatter(Limits limits) : limits_(limits) {}
std::optional<std::string> SqlFormatter::format(std::string_view sql,
                                                Error &error) const {
  auto tokens = Lexer(limits_).tokenize(sql, error);
  if (!tokens)
    return std::nullopt;
  std::string output;
  TokenKind previous = TokenKind::end;
  uint32_t indent = 0;
  for (const Token &token : *tokens) {
    if (token.kind == TokenKind::end)
      break;
    std::string text =
        keyword(token.kind) ? internal::normalize(token.text) : token.text;
    bool newline_keyword = token.kind == TokenKind::keyword_select ||
                           token.kind == TokenKind::keyword_from ||
                           token.kind == TokenKind::keyword_where ||
                           token.kind == TokenKind::keyword_group ||
                           token.kind == TokenKind::keyword_having ||
                           token.kind == TokenKind::keyword_order ||
                           token.kind == TokenKind::keyword_limit ||
                           token.kind == TokenKind::keyword_join ||
                           token.kind == TokenKind::keyword_left ||
                           token.kind == TokenKind::keyword_inner;
    if (token.kind == TokenKind::right_parenthesis && indent)
      --indent;
    if (newline_keyword && !output.empty() && output.back() != '\n')
      output.push_back('\n');
    if (output.empty() || output.back() == '\n')
      output.append(indent * 2, ' ');
    bool need_space = !output.empty() && output.back() != '\n' &&
                      output.back() != ' ' && token.kind != TokenKind::comma &&
                      token.kind != TokenKind::right_parenthesis &&
                      token.kind != TokenKind::dot &&
                      previous != TokenKind::left_parenthesis &&
                      previous != TokenKind::dot;
    if (need_space)
      output.push_back(' ');
    if (token.kind == TokenKind::string) {
      output.push_back('\'');
      for (char c : text) {
        output.push_back(c);
        if (c == '\'')
          output.push_back('\'');
      }
      output.push_back('\'');
    } else {
      output += text;
    }
    if (token.kind == TokenKind::comma)
      output.push_back(' ');
    if (token.kind == TokenKind::semicolon)
      output.push_back('\n');
    if (token.kind == TokenKind::left_parenthesis)
      ++indent;
    previous = token.kind;
  }
  while (!output.empty() && (output.back() == ' ' || output.back() == '\n'))
    output.pop_back();
  return output;
}
std::optional<std::vector<std::string>>
SqlFormatter::split(std::string_view sql, Error &error) const {
  auto tokens = Lexer(limits_).tokenize(sql, error);
  if (!tokens)
    return std::nullopt;
  std::vector<std::string> output;
  size_t begin = 0;
  for (const Token &token : *tokens) {
    if (token.kind != TokenKind::semicolon && token.kind != TokenKind::end)
      continue;
    size_t end = token.kind == TokenKind::semicolon
                     ? static_cast<size_t>(token.offset + token.length)
                     : sql.size();
    std::string statement(sql.substr(begin, end - begin));
    if (std::any_of(statement.begin(), statement.end(),
                    [](unsigned char c) { return !std::isspace(c); }))
      output.push_back(std::move(statement));
    begin = end;
  }
  if (output.size() > limits_.max_statements) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "split statement count exceeds limit");
    return std::nullopt;
  }
  return output;
}

SchemaComparator::SchemaComparator(Limits limits) : limits_(limits) {}
std::vector<SchemaDifference>
SchemaComparator::compare(const Catalog &source, const Catalog &target) const {
  std::vector<SchemaDifference> output;
  for (const auto &table : source.tables()) {
    const Schema *destination = target.table(table.second.name);
    if (!destination) {
      output.push_back({SchemaDifference::Kind::drop_table, table.second.name,
                        "table absent from target", true});
      continue;
    }
    for (const Column &column : table.second.columns) {
      auto target_column = destination->column_index(column.name);
      if (!target_column) {
        output.push_back({SchemaDifference::Kind::drop_column,
                          table.second.name + "." + column.name,
                          "column absent from target", true});
      } else {
        const Column &other = destination->columns[*target_column];
        if (column.type != other.type || column.nullable != other.nullable ||
            column.unique != other.unique ||
            column.primary_key != other.primary_key)
          output.push_back({SchemaDifference::Kind::alter_column,
                            table.second.name + "." + column.name,
                            "type or constraints differ", true});
      }
    }
    for (const Column &column : destination->columns)
      if (!table.second.column_index(column.name))
        output.push_back({SchemaDifference::Kind::add_column,
                          destination->name + "." + column.name,
                          data_type_name(column.type), !column.nullable});
  }
  for (const auto &table : target.tables())
    if (!source.table(table.second.name))
      output.push_back({SchemaDifference::Kind::create_table, table.second.name,
                        "table absent from source", false});
  for (const auto &index : source.indexes())
    if (!target.index(index.second.name))
      output.push_back({SchemaDifference::Kind::drop_index, index.second.name,
                        "index absent from target", false});
  for (const auto &index : target.indexes())
    if (!source.index(index.second.name))
      output.push_back({SchemaDifference::Kind::create_index, index.second.name,
                        index.second.table, false});
  if (output.size() > limits_.max_tables + limits_.max_indexes)
    output.resize(limits_.max_tables + limits_.max_indexes);
  return output;
}
std::string SchemaComparator::migration_sql(
    const std::vector<SchemaDifference> &differences) {
  std::ostringstream output;
  for (const SchemaDifference &difference : differences) {
    switch (difference.kind) {
    case SchemaDifference::Kind::drop_table:
      output << "DROP TABLE " << difference.object << ";\n";
      break;
    case SchemaDifference::Kind::drop_index:
      output << "DROP INDEX " << difference.object << ";\n";
      break;
    case SchemaDifference::Kind::create_table:
      output << "-- CREATE TABLE " << difference.object
             << " requires target column definitions\n";
      break;
    case SchemaDifference::Kind::create_index:
      output << "-- CREATE INDEX " << difference.object << " ON "
             << difference.detail << " requires target columns\n";
      break;
    case SchemaDifference::Kind::add_column:
      output << "ALTER TABLE "
             << difference.object.substr(0, difference.object.find('.'))
             << " ADD COLUMN "
             << difference.object.substr(difference.object.find('.') + 1) << ' '
             << difference.detail << ";\n";
      break;
    default:
      output << "-- destructive manual migration required for "
             << difference.object << ": " << difference.detail << '\n';
    }
  }
  return output.str();
}

CsvCodec::CsvCodec(Limits limits) : limits_(limits) {}
std::optional<std::vector<Row>> CsvCodec::parse(std::string_view csv,
                                                const Schema &schema,
                                                bool header,
                                                Error &error) const {
  error.clear();
  if (csv.size() > limits_.max_file_bytes ||
      schema.columns.size() > limits_.max_columns) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "CSV input or schema exceeds limits");
    return std::nullopt;
  }
  std::vector<std::vector<std::string>> raw_rows;
  std::vector<std::string> current_row;
  std::string field;
  bool quoted = false;
  for (size_t position = 0; position <= csv.size(); ++position) {
    char c = position < csv.size() ? csv[position] : '\n';
    if (quoted) {
      if (c == '"') {
        if (position + 1 < csv.size() && csv[position + 1] == '"') {
          field.push_back('"');
          ++position;
        } else {
          quoted = false;
        }
      } else {
        field.push_back(c);
      }
    } else if (c == '"' && field.empty()) {
      quoted = true;
    } else if (c == ',') {
      current_row.push_back(std::move(field));
      field.clear();
    } else if (c == '\n') {
      current_row.push_back(std::move(field));
      field.clear();
      if (!current_row.empty() &&
          !(current_row.size() == 1 && current_row[0].empty()))
        raw_rows.push_back(std::move(current_row));
      current_row.clear();
      if (raw_rows.size() > limits_.max_rows) {
        internal::fail(error, ErrorCode::resource_limit, position,
                       "CSV row count exceeds limit");
        return std::nullopt;
      }
    } else if (c != '\r') {
      field.push_back(c);
      if (field.size() > limits_.max_string_bytes) {
        internal::fail(error, ErrorCode::resource_limit, position,
                       "CSV field exceeds string limit");
        return std::nullopt;
      }
    }
  }
  if (quoted) {
    internal::fail(error, ErrorCode::syntax_error, csv.size(),
                   "CSV quoted field is unterminated");
    return std::nullopt;
  }
  size_t begin = header && !raw_rows.empty() ? 1 : 0;
  std::vector<Row> output;
  for (size_t row = begin; row < raw_rows.size(); ++row) {
    if (raw_rows[row].size() != schema.columns.size()) {
      internal::fail(error, ErrorCode::constraint_error, row,
                     "CSV row width differs from schema");
      return std::nullopt;
    }
    Row converted;
    for (size_t column = 0; column < schema.columns.size(); ++column) {
      auto value = csv_value(std::move(raw_rows[row][column]),
                             schema.columns[column].type, error);
      if (!value)
        return std::nullopt;
      if (value->is_null() && !schema.columns[column].nullable) {
        internal::fail(error, ErrorCode::constraint_error, row,
                       "CSV NULL violates non-null column");
        return std::nullopt;
      }
      converted.push_back(std::move(*value));
    }
    output.push_back(std::move(converted));
  }
  return output;
}
std::string CsvCodec::write(const std::vector<std::string> &columns,
                            const std::vector<Row> &rows, bool header,
                            Error &error) const {
  error.clear();
  if (columns.size() > limits_.max_columns || rows.size() > limits_.max_rows) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "CSV output dimensions exceed limits");
    return {};
  }
  std::ostringstream output;
  auto write_row = [&](const std::vector<std::string> &values) {
    for (size_t i = 0; i < values.size(); ++i) {
      if (i)
        output << ',';
      output << quote_csv(values[i]);
    }
    output << '\n';
  };
  if (header)
    write_row(columns);
  for (const Row &row : rows) {
    std::vector<std::string> values;
    for (const Value &value : row)
      values.push_back(value.is_null() ? std::string{} : value.display());
    write_row(values);
    if (static_cast<uint64_t>(output.tellp()) > limits_.max_file_bytes) {
      internal::fail(error, ErrorCode::resource_limit, rows.size(),
                     "CSV output exceeds file limit");
      return {};
    }
  }
  return output.str();
}

BackupCodec::BackupCodec(Limits limits) : limits_(limits) {}
std::vector<uint8_t> BackupCodec::create(const std::vector<uint8_t> &database,
                                         const std::vector<uint8_t> &wal,
                                         const BackupManifest &manifest,
                                         Error &error) const {
  error.clear();
  if (database.size() > limits_.max_file_bytes ||
      wal.size() > limits_.max_wal_bytes ||
      manifest.properties.size() > limits_.max_columns) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "backup components exceed limits");
    return {};
  }
  std::vector<uint8_t> output;
  output.insert(output.end(), {'Q', 'F', 'B', 'A', 'K', '1', 0, 0});
  internal::append32(output, 1);
  internal::append32(output, static_cast<uint32_t>(manifest.properties.size()));
  internal::append64(output, database.size());
  internal::append64(output, wal.size());
  internal::append64(output, fnv(database));
  internal::append64(output, fnv(wal));
  for (const auto &property : manifest.properties) {
    if (property.first.size() > limits_.max_identifier_bytes ||
        property.second.size() > limits_.max_string_bytes) {
      internal::fail(error, ErrorCode::resource_limit, 0,
                     "backup property exceeds limits");
      return {};
    }
    append_string(output, property.first);
    append_string(output, property.second);
  }
  output.insert(output.end(), database.begin(), database.end());
  output.insert(output.end(), wal.begin(), wal.end());
  internal::append32(output, crc32(output.data(), output.size()));
  return output;
}
std::optional<BackupCodec::Restored>
BackupCodec::restore(const uint8_t *data, size_t size, Error &error) const {
  error.clear();
  if (size < 52 || std::memcmp(data, "QFBAK1\0\0", 8) != 0) {
    internal::fail(
        error, size < 52 ? ErrorCode::truncated : ErrorCode::invalid_signature,
        0, "backup header is invalid");
    return std::nullopt;
  }
  if (internal::le32(data + 8) != 1) {
    internal::fail(error, ErrorCode::invalid_version, 8,
                   "backup version is unsupported");
    return std::nullopt;
  }
  uint32_t properties = internal::le32(data + 12);
  uint64_t database_bytes = internal::le64(data + 16);
  uint64_t wal_bytes = internal::le64(data + 24);
  uint64_t database_hash = internal::le64(data + 32);
  uint64_t wal_hash = internal::le64(data + 40);
  if (properties > limits_.max_columns ||
      database_bytes > limits_.max_file_bytes ||
      wal_bytes > limits_.max_wal_bytes ||
      crc32(data, size - 4) != internal::le32(data + size - 4)) {
    internal::fail(error, ErrorCode::checksum_mismatch, size - 4,
                   "backup dimensions or checksum are invalid");
    return std::nullopt;
  }
  Restored output;
  output.manifest.database_bytes = database_bytes;
  output.manifest.wal_bytes = wal_bytes;
  output.manifest.database_hash = database_hash;
  output.manifest.wal_hash = wal_hash;
  size_t position = 48;
  for (uint32_t index = 0; index < properties; ++index) {
    std::string key, value;
    if (!take_string(data, size - 4, position, limits_.max_identifier_bytes,
                     key, error) ||
        !take_string(data, size - 4, position, limits_.max_string_bytes, value,
                     error))
      return std::nullopt;
    output.manifest.properties.emplace(std::move(key), std::move(value));
  }
  uint64_t payload = 0;
  if (!internal::checked_add(database_bytes, wal_bytes, payload) ||
      payload != size - 4 - position) {
    internal::fail(error, ErrorCode::invalid_offset, position,
                   "backup payload sizes are inconsistent");
    return std::nullopt;
  }
  output.database.assign(data + position, data + position + database_bytes);
  position += static_cast<size_t>(database_bytes);
  output.wal.assign(data + position, data + position + wal_bytes);
  if (fnv(output.database) != database_hash || fnv(output.wal) != wal_hash) {
    internal::fail(error, ErrorCode::checksum_mismatch, position,
                   "backup component hash mismatch");
    return std::nullopt;
  }
  return output;
}

} // namespace queryforge
