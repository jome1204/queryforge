#ifndef QUERYFORGE_DATABASE_H
#define QUERYFORGE_DATABASE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace queryforge {

enum class ErrorCode {
  none,
  truncated,
  invalid_signature,
  invalid_version,
  lexical_error,
  syntax_error,
  semantic_error,
  type_error,
  constraint_error,
  not_found,
  duplicate,
  invalid_state,
  invalid_offset,
  overflow,
  resource_limit,
  checksum_mismatch,
  transaction_conflict,
  unsupported
};

struct Error {
  ErrorCode code = ErrorCode::none;
  uint64_t offset = 0;
  std::string message;
  explicit operator bool() const { return code != ErrorCode::none; }
  void clear() {
    code = ErrorCode::none;
    offset = 0;
    message.clear();
  }
};

struct Limits {
  uint64_t max_file_bytes = 256ull * 1024 * 1024;
  uint64_t max_wal_bytes = 64ull * 1024 * 1024;
  uint64_t max_record_bytes = 4ull * 1024 * 1024;
  uint64_t max_rows = 1'000'000;
  uint64_t max_output_rows = 100'000;
  uint64_t max_pages = 1'000'000;
  uint64_t max_wal_records = 1'000'000;
  uint64_t max_undo_entries = 1'000'000;
  uint32_t max_columns = 1024;
  uint32_t max_tables = 4096;
  uint32_t max_indexes = 8192;
  uint32_t max_identifier_bytes = 256;
  uint32_t max_string_bytes = 1024 * 1024;
  uint32_t max_sql_bytes = 8 * 1024 * 1024;
  uint32_t max_statements = 4096;
  uint32_t max_parser_depth = 128;
  uint32_t max_expression_nodes = 65'536;
  uint32_t max_plan_nodes = 16'384;
  uint32_t max_join_tables = 32;
  uint32_t max_index_fanout = 1024;
  uint32_t page_size = 4096;
};

enum class DataType { null_type, integer, real, text, boolean, blob };

class Value {
public:
  using Blob = std::vector<uint8_t>;
  using Storage =
      std::variant<std::monostate, int64_t, double, std::string, bool, Blob>;
  Value() = default;
  explicit Value(int64_t value);
  explicit Value(double value);
  explicit Value(std::string value);
  explicit Value(const char *value);
  explicit Value(bool value);
  explicit Value(Blob value);
  DataType type() const;
  bool is_null() const;
  const Storage &storage() const { return storage_; }
  std::optional<int64_t> as_integer() const;
  std::optional<double> as_real() const;
  std::optional<bool> as_boolean() const;
  std::optional<std::string_view> as_text() const;
  const Blob *as_blob() const;
  std::string display() const;
  uint64_t memory_usage() const;
  friend bool operator==(const Value &left, const Value &right);
  friend bool operator!=(const Value &left, const Value &right) {
    return !(left == right);
  }

private:
  Storage storage_;
};

enum class CompareResult { less, equal, greater, unordered };
CompareResult compare_values(const Value &left, const Value &right);

enum class TokenKind {
  end,
  invalid,
  identifier,
  integer,
  real,
  string,
  blob,
  comma,
  dot,
  semicolon,
  left_parenthesis,
  right_parenthesis,
  star,
  plus,
  minus,
  slash,
  percent,
  equal,
  not_equal,
  less,
  less_equal,
  greater,
  greater_equal,
  keyword_select,
  keyword_from,
  keyword_where,
  keyword_insert,
  keyword_into,
  keyword_values,
  keyword_update,
  keyword_set,
  keyword_delete,
  keyword_create,
  keyword_table,
  keyword_index,
  keyword_on,
  keyword_drop,
  keyword_and,
  keyword_or,
  keyword_not,
  keyword_null,
  keyword_true,
  keyword_false,
  keyword_as,
  keyword_order,
  keyword_by,
  keyword_asc,
  keyword_desc,
  keyword_limit,
  keyword_offset,
  keyword_join,
  keyword_inner,
  keyword_left,
  keyword_begin,
  keyword_commit,
  keyword_rollback,
  keyword_checkpoint,
  keyword_primary,
  keyword_key,
  keyword_unique,
  keyword_default,
  keyword_integer,
  keyword_real,
  keyword_text,
  keyword_boolean,
  keyword_blob,
  keyword_is,
  keyword_in,
  keyword_like,
  keyword_group,
  keyword_having
};

struct Token {
  TokenKind kind = TokenKind::invalid;
  std::string text;
  uint64_t offset = 0;
  uint64_t length = 0;
};

class Lexer {
public:
  explicit Lexer(Limits limits = {});
  std::optional<std::vector<Token>> tokenize(std::string_view sql,
                                             Error &error) const;

private:
  Limits limits_;
};

enum class UnaryOperator {
  positive,
  negative,
  logical_not,
  is_null,
  is_not_null
};
enum class BinaryOperator {
  add,
  subtract,
  multiply,
  divide,
  modulo,
  equal,
  not_equal,
  less,
  less_equal,
  greater,
  greater_equal,
  logical_and,
  logical_or,
  like
};

struct Expression {
  enum class Kind { literal, column, unary, binary, function, list };
  Kind kind = Kind::literal;
  Value literal;
  std::string qualifier;
  std::string name;
  UnaryOperator unary = UnaryOperator::positive;
  BinaryOperator binary = BinaryOperator::add;
  std::unique_ptr<Expression> left;
  std::unique_ptr<Expression> right;
  std::vector<std::unique_ptr<Expression>> arguments;
  uint64_t offset = 0;
  Expression() = default;
  Expression(const Expression &other);
  Expression &operator=(const Expression &other);
  Expression(Expression &&) noexcept = default;
  Expression &operator=(Expression &&) noexcept = default;
};

struct ColumnDefinition {
  std::string name;
  DataType type = DataType::null_type;
  bool nullable = true;
  bool primary_key = false;
  bool unique = false;
  std::optional<Value> default_value;
};

struct SelectItem {
  std::unique_ptr<Expression> expression;
  std::string alias;
  bool wildcard = false;
};

struct OrderItem {
  std::unique_ptr<Expression> expression;
  bool ascending = true;
};

struct JoinClause {
  enum class Kind { inner, left };
  Kind kind = Kind::inner;
  std::string table;
  std::string alias;
  std::unique_ptr<Expression> condition;
};

struct SelectStatement {
  std::vector<SelectItem> items;
  std::string table;
  std::string alias;
  std::vector<JoinClause> joins;
  std::unique_ptr<Expression> where;
  std::vector<std::unique_ptr<Expression>> group_by;
  std::unique_ptr<Expression> having;
  std::vector<OrderItem> order_by;
  std::optional<uint64_t> limit;
  uint64_t offset = 0;
};

struct InsertStatement {
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<std::unique_ptr<Expression>>> rows;
};

struct UpdateAssignment {
  std::string column;
  std::unique_ptr<Expression> expression;
};

struct UpdateStatement {
  std::string table;
  std::vector<UpdateAssignment> assignments;
  std::unique_ptr<Expression> where;
};

struct DeleteStatement {
  std::string table;
  std::unique_ptr<Expression> where;
};

struct CreateTableStatement {
  std::string table;
  std::vector<ColumnDefinition> columns;
};

struct CreateIndexStatement {
  std::string index;
  std::string table;
  std::vector<std::string> columns;
  bool unique = false;
};

struct DropStatement {
  enum class Kind { table, index };
  Kind kind = Kind::table;
  std::string name;
};

enum class TransactionCommand { begin, commit, rollback, checkpoint };

using StatementData =
    std::variant<SelectStatement, InsertStatement, UpdateStatement,
                 DeleteStatement, CreateTableStatement, CreateIndexStatement,
                 DropStatement, TransactionCommand>;

struct Statement {
  StatementData data;
  uint64_t offset = 0;
};

struct ParseResult {
  std::optional<std::vector<Statement>> statements;
  Error error;
  explicit operator bool() const { return statements.has_value(); }
};

class Parser {
public:
  explicit Parser(Limits limits = {});
  ParseResult parse(std::string_view sql) const;

private:
  Limits limits_;
};

struct Column {
  std::string name;
  DataType type = DataType::null_type;
  bool nullable = true;
  bool primary_key = false;
  bool unique = false;
  std::optional<Value> default_value;
};

struct Schema {
  std::string name;
  uint64_t identifier = 0;
  uint64_t generation = 0;
  std::vector<Column> columns;
  std::optional<size_t> column_index(std::string_view name) const;
};

using Row = std::vector<Value>;

struct RowRecord {
  uint64_t row_id = 0;
  uint64_t generation = 0;
  bool deleted = false;
  Row values;
};

struct IndexDefinition {
  std::string name;
  std::string table;
  std::vector<size_t> columns;
  bool unique = false;
};

class Catalog {
public:
  explicit Catalog(Limits limits = {});
  bool create_table(Schema schema, Error &error);
  bool drop_table(std::string_view name, Error &error);
  bool create_index(IndexDefinition index, Error &error);
  bool drop_index(std::string_view name, Error &error);
  const Schema *table(std::string_view name) const;
  Schema *table(std::string_view name);
  const IndexDefinition *index(std::string_view name) const;
  const std::map<std::string, Schema> &tables() const { return tables_; }
  const std::map<std::string, IndexDefinition> &indexes() const {
    return indexes_;
  }

private:
  Limits limits_;
  std::map<std::string, Schema> tables_;
  std::map<std::string, IndexDefinition> indexes_;
};

struct Binding {
  std::string qualifier;
  std::string name;
  DataType type = DataType::null_type;
  size_t row_index = 0;
};

class SemanticAnalyzer {
public:
  explicit SemanticAnalyzer(Limits limits = {});
  bool analyze(const Statement &statement, const Catalog &catalog,
               Error &error) const;
  std::optional<DataType> expression_type(const Expression &expression,
                                          const std::vector<Binding> &bindings,
                                          Error &error) const;

private:
  Limits limits_;
};

class ExpressionEvaluator {
public:
  explicit ExpressionEvaluator(Limits limits = {});
  std::optional<Value> evaluate(const Expression &expression, const Row &row,
                                const std::vector<Binding> &bindings,
                                Error &error) const;
  std::optional<bool> predicate(const Expression &expression, const Row &row,
                                const std::vector<Binding> &bindings,
                                Error &error) const;

private:
  Limits limits_;
};

enum class PlanKind {
  constant,
  table_scan,
  index_scan,
  filter,
  projection,
  nested_loop_join,
  aggregate,
  sort,
  limit,
  insert,
  update,
  remove
};

struct PlanNode {
  PlanKind kind = PlanKind::constant;
  std::string table;
  std::string index;
  std::unique_ptr<Expression> expression;
  std::vector<std::unique_ptr<Expression>> projections;
  std::vector<std::unique_ptr<PlanNode>> children;
  uint64_t estimated_rows = 0;
  double estimated_cost = 0.0;
};

class QueryPlanner {
public:
  explicit QueryPlanner(Limits limits = {});
  std::optional<PlanNode> plan(const Statement &statement,
                               const Catalog &catalog, Error &error) const;

private:
  Limits limits_;
};

struct QueryResult {
  std::vector<std::string> columns;
  std::vector<Row> rows;
  uint64_t affected_rows = 0;
  std::string message;
};

struct ExecutionResult {
  std::optional<QueryResult> result;
  Error error;
  explicit operator bool() const { return result.has_value(); }
};

enum class PageType : uint16_t {
  free = 0,
  catalog = 1,
  table_leaf = 2,
  btree_internal = 3,
  btree_leaf = 4,
  overflow = 5
};

struct PageHeader {
  uint32_t page_id = 0;
  PageType type = PageType::free;
  uint16_t flags = 0;
  uint32_t generation = 0;
  uint16_t cell_count = 0;
  uint16_t free_start = 0;
  uint16_t free_end = 0;
  uint32_t right_page = 0;
  uint32_t checksum = 0;
};

class Page {
public:
  Page() = default;
  explicit Page(uint32_t page_size);
  uint32_t size() const { return static_cast<uint32_t>(bytes_.size()); }
  const uint8_t *data() const { return bytes_.data(); }
  uint8_t *data() { return bytes_.data(); }
  const std::vector<uint8_t> &bytes() const { return bytes_; }
  std::vector<uint8_t> &bytes() { return bytes_; }
  std::optional<PageHeader> header(Error &error) const;
  bool initialize(uint32_t page_id, PageType type, uint32_t generation,
                  Error &error);
  bool verify(Error &error) const;
  bool update_checksum(Error &error);

private:
  std::vector<uint8_t> bytes_;
};

class RecordCodec {
public:
  explicit RecordCodec(Limits limits = {});
  std::vector<uint8_t> encode(const RowRecord &record, Error &error) const;
  std::optional<RowRecord> decode(const uint8_t *data, size_t size,
                                  Error &error) const;
  std::vector<uint8_t> encode_key(const Row &row,
                                  const std::vector<size_t> &columns,
                                  Error &error) const;

private:
  Limits limits_;
};

class Pager {
public:
  explicit Pager(Limits limits = {});
  bool create(Error &error);
  bool open(const uint8_t *data, size_t size, Error &error);
  uint32_t allocate(PageType type, Error &error);
  Page *page(uint32_t identifier);
  const Page *page(uint32_t identifier) const;
  bool free(uint32_t identifier, Error &error);
  std::vector<uint8_t> serialize(Error &error) const;
  uint64_t page_count() const { return pages_.size(); }
  uint64_t generation() const { return generation_; }

private:
  Limits limits_;
  uint64_t generation_ = 1;
  std::vector<Page> pages_;
  std::vector<uint32_t> free_pages_;
};

class BTree {
public:
  explicit BTree(Limits limits = {});
  bool initialize(uint32_t root_page, bool unique, Error &error);
  bool insert(std::vector<uint8_t> key, uint64_t row_id, Error &error);
  bool remove(const std::vector<uint8_t> &key, uint64_t row_id, Error &error);
  std::vector<uint64_t> find(const std::vector<uint8_t> &key) const;
  std::vector<uint64_t> range(const std::optional<std::vector<uint8_t>> &lower,
                              const std::optional<std::vector<uint8_t>> &upper,
                              uint64_t limit) const;
  bool verify(Error &error) const;
  uint64_t entry_count() const;

private:
  struct Entry {
    std::vector<uint8_t> key;
    std::vector<uint64_t> row_ids;
  };
  Limits limits_;
  uint32_t root_page_ = 0;
  bool unique_ = false;
  std::vector<Entry> entries_;
};

enum class WalRecordType : uint16_t {
  begin = 1,
  page_before = 2,
  page_after = 3,
  row_insert = 4,
  row_update = 5,
  row_delete = 6,
  catalog_change = 7,
  commit = 8,
  rollback = 9,
  checkpoint = 10
};

struct WalRecord {
  WalRecordType type = WalRecordType::begin;
  uint64_t lsn = 0;
  uint64_t transaction_id = 0;
  uint32_t page_id = 0;
  uint32_t generation = 0;
  std::vector<uint8_t> payload;
};

class WalCodec {
public:
  explicit WalCodec(Limits limits = {});
  std::vector<uint8_t> encode(const std::vector<WalRecord> &records,
                              Error &error) const;
  std::optional<std::vector<WalRecord>> decode(const uint8_t *data, size_t size,
                                               Error &error) const;

private:
  Limits limits_;
};

enum class TransactionState { idle, active, committed, rolled_back, failed };

struct UndoEntry {
  enum class Kind {
    insert_row,
    update_row,
    delete_row,
    create_table,
    drop_table
  };
  Kind kind = Kind::insert_row;
  std::string table;
  uint64_t row_id = 0;
  std::optional<RowRecord> before;
  std::optional<Schema> schema;
};

class TransactionManager {
public:
  explicit TransactionManager(Limits limits = {});
  bool begin(Error &error);
  bool record(UndoEntry entry, Error &error);
  bool commit(Error &error);
  bool rollback(std::map<std::string, std::vector<RowRecord>> &rows,
                Catalog &catalog, Error &error);
  void fail();
  TransactionState state() const { return state_; }
  uint64_t identifier() const { return identifier_; }
  const std::vector<WalRecord> &wal() const { return wal_; }
  void clear_wal();

private:
  Limits limits_;
  TransactionState state_ = TransactionState::idle;
  uint64_t identifier_ = 0;
  uint64_t next_identifier_ = 1;
  uint64_t next_lsn_ = 1;
  std::vector<UndoEntry> undo_;
  std::vector<WalRecord> wal_;
};

struct RecoveryReport {
  uint64_t records_seen = 0;
  uint64_t records_applied = 0;
  uint64_t transactions_committed = 0;
  uint64_t transactions_rolled_back = 0;
  uint64_t last_lsn = 0;
  std::vector<std::string> warnings;
};

class RecoveryManager {
public:
  explicit RecoveryManager(Limits limits = {});
  std::optional<RecoveryReport> recover(Pager &pager,
                                        const std::vector<WalRecord> &records,
                                        Error &error) const;

private:
  Limits limits_;
};

class Database {
public:
  explicit Database(Limits limits = {});
  ExecutionResult execute(std::string_view sql);
  ExecutionResult execute(const Statement &statement);
  bool open(const uint8_t *data, size_t size, Error &error);
  std::vector<uint8_t> serialize(Error &error) const;
  bool recover(const uint8_t *wal, size_t size, RecoveryReport &report,
               Error &error);
  bool checkpoint(Error &error);
  const Catalog &catalog() const { return catalog_; }
  const std::map<std::string, std::vector<RowRecord>> &rows() const {
    return rows_;
  }
  TransactionState transaction_state() const { return transaction_.state(); }
  uint64_t invariant_hash() const;
  std::vector<std::string> verify() const;

private:
  ExecutionResult execute_select(const SelectStatement &select);
  ExecutionResult execute_insert(const InsertStatement &insert);
  ExecutionResult execute_update(const UpdateStatement &update);
  ExecutionResult execute_delete(const DeleteStatement &remove);
  ExecutionResult execute_create_table(const CreateTableStatement &create);
  ExecutionResult execute_create_index(const CreateIndexStatement &create);
  ExecutionResult execute_drop(const DropStatement &drop);
  Limits limits_;
  Catalog catalog_;
  Pager pager_;
  TransactionManager transaction_;
  std::map<std::string, std::vector<RowRecord>> rows_;
  std::map<std::string, BTree> indexes_;
  uint64_t next_row_id_ = 1;
};

uint32_t crc32(const uint8_t *data, size_t size);
std::string error_code_name(ErrorCode code);
std::string data_type_name(DataType type);
std::string token_kind_name(TokenKind kind);

} // namespace queryforge

#endif
