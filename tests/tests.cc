#include "queryforge/database.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;

template <typename T> void check(const T &condition, const char *message) {
  if (!static_cast<bool>(condition)) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void test_parser() {
  queryforge::Parser parser;
  auto result = parser.parse(
      "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
      "INSERT INTO users VALUES(1,'Ada');"
      "SELECT id,name FROM users WHERE id=1 ORDER BY name;");
  check(static_cast<bool>(result), "parser accepts representative workload");
  check(result && result.statements->size() == 3,
        "parser returns all statements");
  auto invalid = parser.parse("SELECT (((((((;");
  check(!invalid, "parser rejects unbalanced expression");
}

void test_database_execution() {
  queryforge::Database database;
  check(database.execute(
      "CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
      "score REAL DEFAULT 0);"), "create table");
  check(database.execute(
      "INSERT INTO users(id,name,score) VALUES(1,'Ada',9.5);"),
      "insert first row");
  check(database.execute(
      "INSERT INTO users(id,name,score) VALUES(2,'Linus',8.0);"),
      "insert second row");
  auto result = database.execute(
      "SELECT id,name,score FROM users WHERE score >= 8 ORDER BY id;");
  check(result, "select rows");
  check(result && result.result->rows.size() == 2, "select row count");
  check(database.execute("UPDATE users SET score=10 WHERE id=1;"),
        "update row");
  check(database.execute("DELETE FROM users WHERE id=2;"), "delete row");
  auto count = database.execute("SELECT COUNT(*) FROM users;");
  check(count && count.result->rows.size() == 1, "aggregate result");
  check(database.verify().empty(), "database invariants after mutations");
}

void test_transactions() {
  queryforge::Database database;
  check(database.execute(
      "CREATE TABLE ledger(id INTEGER PRIMARY KEY, amount INTEGER);"),
      "transaction table");
  check(database.execute("INSERT INTO ledger VALUES(1,100);"),
        "transaction baseline row");
  const auto baseline = database.invariant_hash();
  check(database.execute("BEGIN;"), "begin transaction");
  check(database.execute("UPDATE ledger SET amount=25 WHERE id=1;"),
        "transaction update");
  check(database.execute("INSERT INTO ledger VALUES(2,75);"),
        "transaction insert");
  check(database.execute("ROLLBACK;"), "rollback transaction");
  check(database.invariant_hash() == baseline, "rollback restores state");
  check(database.execute("BEGIN;"), "begin commit transaction");
  auto committed_insert =
      database.execute("INSERT INTO ledger VALUES(2,75);");
  if (!committed_insert)
    std::cerr << "commit insert error: " << committed_insert.error.message
              << '\n';
  check(committed_insert, "commit insert");
  check(database.execute("COMMIT;"), "commit transaction");
  check(database.invariant_hash() != baseline, "commit changes state");
}

void test_serialization() {
  queryforge::Database original;
  check(original.execute(
      "CREATE TABLE values_table(id INTEGER PRIMARY KEY, text_value TEXT, "
      "flag BOOLEAN, payload BLOB);"), "serialization table");
  check(original.execute(
      "INSERT INTO values_table VALUES(1,'hello',NULL,NULL);"),
      "serialization row");
  queryforge::Error error;
  auto encoded = original.serialize(error);
  check(!error && !encoded.empty(), "database serialization");
  queryforge::Database reopened;
  queryforge::Error open_error;
  check(reopened.open(encoded.data(), encoded.size(), open_error),
        "database reopen");
  check(original.invariant_hash() == reopened.invariant_hash(),
        "round-trip invariant hash");
  if (!encoded.empty()) {
    encoded.back() ^= 0x80;
    queryforge::Database corrupted;
    queryforge::Error corrupted_error;
    check(!corrupted.open(encoded.data(), encoded.size(), corrupted_error),
          "database checksum rejects corruption");
  }
}

void test_records() {
  queryforge::RowRecord record;
  record.row_id = 42;
  record.generation = 7;
  record.values.emplace_back(int64_t{123});
  record.values.emplace_back(std::string("text"));
  record.values.emplace_back(true);
  record.values.emplace_back(queryforge::Value::Blob{0, 1, 2, 3});
  queryforge::RecordCodec codec;
  queryforge::Error error;
  auto bytes = codec.encode(record, error);
  check(!error && !bytes.empty(), "record encode");
  queryforge::Error decode_error;
  auto decoded = codec.decode(bytes.data(), bytes.size(), decode_error);
  check(decoded.has_value(), "record decode");
  check(decoded && decoded->row_id == 42, "record identifier");
  check(decoded && decoded->values == record.values, "record values");
  bytes[0] ^= 1;
  queryforge::Error invalid_error;
  check(!codec.decode(bytes.data(), bytes.size(), invalid_error),
        "record signature validation");
}

void test_pages() {
  queryforge::Error error;
  queryforge::Page page(4096);
  check(page.initialize(3, queryforge::PageType::table_leaf, 9, error),
        "page initialization");
  check(page.verify(error), "page verification");
  page.bytes()[100] ^= 1;
  queryforge::Error checksum_error;
  check(!page.verify(checksum_error), "page checksum validation");

  queryforge::Pager pager;
  queryforge::Error create_error;
  check(pager.create(create_error), "pager create");
  queryforge::Error allocate_error;
  uint32_t identifier =
      pager.allocate(queryforge::PageType::btree_leaf, allocate_error);
  check(!allocate_error && pager.page(identifier), "pager allocate");
  queryforge::Error serialize_error;
  auto image = pager.serialize(serialize_error);
  check(!serialize_error && !image.empty(), "pager serialize");
  queryforge::Pager reopened;
  queryforge::Error open_error;
  check(reopened.open(image.data(), image.size(), open_error), "pager reopen");
  check(reopened.page_count() == pager.page_count(), "pager page count");
}

void test_btree() {
  queryforge::BTree tree;
  queryforge::Error error;
  check(tree.initialize(1, true, error), "B-tree initialize");
  check(tree.insert({1, 2, 3}, 10, error), "B-tree insert");
  check(tree.insert({1, 2, 4}, 11, error), "B-tree second insert");
  check(!tree.insert({1, 2, 3}, 12, error), "unique B-tree constraint");
  auto found = tree.find({1, 2, 3});
  check(found.size() == 1 && found[0] == 10, "B-tree lookup");
  queryforge::Error verify_error;
  check(tree.verify(verify_error), "B-tree invariants");
  queryforge::Error remove_error;
  check(tree.remove({1, 2, 3}, 10, remove_error), "B-tree remove");
  check(tree.find({1, 2, 3}).empty(), "B-tree removal visible");
}

void test_wal() {
  std::vector<queryforge::WalRecord> records;
  records.push_back(
      {queryforge::WalRecordType::begin, 1, 7, 0, 0, {}});
  records.push_back(
      {queryforge::WalRecordType::page_after, 2, 7, 1, 1, {1, 2, 3}});
  records.push_back(
      {queryforge::WalRecordType::commit, 3, 7, 0, 0, {}});
  queryforge::WalCodec codec;
  queryforge::Error error;
  auto encoded = codec.encode(records, error);
  check(!error && !encoded.empty(), "WAL encode");
  queryforge::Error decode_error;
  auto decoded = codec.decode(encoded.data(), encoded.size(), decode_error);
  check(decoded && decoded->size() == records.size(), "WAL decode");
  encoded.back() ^= 1;
  queryforge::Error corrupted_error;
  check(!codec.decode(encoded.data(), encoded.size(), corrupted_error),
        "WAL checksum validation");
}

void test_utilities() {
  queryforge::Database database;
  check(database.execute(
      "CREATE TABLE metrics(id INTEGER PRIMARY KEY, label TEXT);"
      "INSERT INTO metrics VALUES(1,'one');"),
      "utility fixture");
  queryforge::StatisticsCollector collector;
  queryforge::Error statistics_error;
  const auto &schema = database.catalog().tables().begin()->second;
  const auto &table_rows = database.rows().begin()->second;
  auto statistics = collector.collect(schema, table_rows, statistics_error);
  check(statistics && statistics->live_rows == 1, "statistics collection");
  queryforge::IntegrityChecker checker;
  auto integrity = checker.check_database(database);
  check(integrity.empty(), "integrity checker");
  queryforge::ReportBuilder reports;
  queryforge::Error report_error;
  auto report = reports.build(database, report_error);
  check(!report_error && !queryforge::ReportBuilder::text(report).empty(),
        "text report");
  check(!queryforge::ReportBuilder::json(report).empty(), "JSON report");

  queryforge::CsvCodec csv;
  queryforge::Error encode_error;
  std::vector<std::string> columns{"id", "label"};
  std::vector<queryforge::Row> rows{
      {queryforge::Value(int64_t{1}), queryforge::Value("a,b")}};
  auto text = csv.write(columns, rows, true, encode_error);
  check(!encode_error && text.find("\"a,b\"") != std::string::npos,
        "CSV quoting");
  queryforge::Error decode_error;
  auto decoded = csv.parse(text, schema, true, decode_error);
  check(decoded && decoded->size() == 1, "CSV round trip");

  queryforge::BackupCodec backup;
  queryforge::Error backup_error;
  queryforge::Error image_error;
  auto database_image = database.serialize(image_error);
  queryforge::BackupManifest manifest;
  manifest.database_bytes = database_image.size();
  auto archive = backup.create(database_image, {}, manifest, backup_error);
  check(!backup_error && !archive.empty(), "backup create");
  queryforge::Error restore_error;
  auto restored = backup.restore(archive.data(), archive.size(), restore_error);
  check(restored && restored->database == database_image, "backup restore");
}
} // namespace

int main() {
  test_parser();
  test_database_execution();
  test_transactions();
  test_serialization();
  test_records();
  test_pages();
  test_btree();
  test_wal();
  test_utilities();
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All QueryForge tests passed\n";
  return EXIT_SUCCESS;
}
