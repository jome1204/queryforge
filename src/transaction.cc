#include "internal.h"

namespace queryforge {

TransactionManager::TransactionManager(Limits limits) : limits_(limits) {}

bool TransactionManager::begin(Error &error) {
  error.clear();
  if (state_ == TransactionState::active)
    return internal::fail(error, ErrorCode::invalid_state, identifier_,
                          "transaction is already active");
  state_ = TransactionState::active;
  identifier_ = next_identifier_++;
  undo_.clear();
  wal_.push_back({WalRecordType::begin, next_lsn_++, identifier_, 0, 0, {}});
  return true;
}
bool TransactionManager::record(UndoEntry entry, Error &error) {
  error.clear();
  if (state_ != TransactionState::active)
    return internal::fail(error, ErrorCode::invalid_state, identifier_,
                          "cannot record undo outside transaction");
  if (undo_.size() >= limits_.max_undo_entries)
    return internal::fail(error, ErrorCode::resource_limit, undo_.size(),
                          "transaction undo count exceeds limit");
  WalRecordType type = WalRecordType::catalog_change;
  if (entry.kind == UndoEntry::Kind::insert_row)
    type = WalRecordType::row_insert;
  else if (entry.kind == UndoEntry::Kind::update_row)
    type = WalRecordType::row_update;
  else if (entry.kind == UndoEntry::Kind::delete_row)
    type = WalRecordType::row_delete;
  std::vector<uint8_t> payload(entry.table.begin(), entry.table.end());
  wal_.push_back({type, next_lsn_++, identifier_, 0, 0, std::move(payload)});
  undo_.push_back(std::move(entry));
  return true;
}
bool TransactionManager::commit(Error &error) {
  error.clear();
  if (state_ != TransactionState::active)
    return internal::fail(error, ErrorCode::invalid_state, identifier_,
                          "no active transaction to commit");
  wal_.push_back({WalRecordType::commit, next_lsn_++, identifier_, 0, 0, {}});
  undo_.clear();
  state_ = TransactionState::committed;
  return true;
}
bool TransactionManager::rollback(
    std::map<std::string, std::vector<RowRecord>> &rows, Catalog &catalog,
    Error &error) {
  error.clear();
  if (state_ != TransactionState::active && state_ != TransactionState::failed)
    return internal::fail(error, ErrorCode::invalid_state, identifier_,
                          "no active transaction to roll back");
  for (auto iterator = undo_.rbegin(); iterator != undo_.rend(); ++iterator) {
    auto table = rows.find(internal::normalize(iterator->table));
    if (iterator->kind == UndoEntry::Kind::insert_row) {
      if (table != rows.end()) {
        table->second.erase(
            std::remove_if(table->second.begin(), table->second.end(),
                           [&](const RowRecord &row) {
                             return row.row_id == iterator->row_id;
                           }),
            table->second.end());
      }
    } else if (iterator->kind == UndoEntry::Kind::update_row ||
               iterator->kind == UndoEntry::Kind::delete_row) {
      if (!iterator->before)
        continue;
      if (table == rows.end())
        table = rows.emplace(internal::normalize(iterator->table),
                             std::vector<RowRecord>{})
                    .first;
      auto row = std::find_if(table->second.begin(), table->second.end(),
                              [&](const RowRecord &candidate) {
                                return candidate.row_id == iterator->row_id;
                              });
      if (row == table->second.end())
        table->second.push_back(*iterator->before);
      else
        *row = *iterator->before;
    } else if (iterator->kind == UndoEntry::Kind::create_table) {
      Error ignored;
      catalog.drop_table(iterator->table, ignored);
      rows.erase(internal::normalize(iterator->table));
    } else if (iterator->kind == UndoEntry::Kind::drop_table &&
               iterator->schema) {
      Error restore_error;
      if (!catalog.create_table(*iterator->schema, restore_error)) {
        state_ = TransactionState::failed;
        error = std::move(restore_error);
        return false;
      }
    }
  }
  wal_.push_back({WalRecordType::rollback, next_lsn_++, identifier_, 0, 0, {}});
  undo_.clear();
  state_ = TransactionState::rolled_back;
  return true;
}
void TransactionManager::fail() {
  if (state_ == TransactionState::active)
    state_ = TransactionState::failed;
}
void TransactionManager::clear_wal() { wal_.clear(); }

RecoveryManager::RecoveryManager(Limits limits) : limits_(limits) {}

std::optional<RecoveryReport>
RecoveryManager::recover(Pager &pager, const std::vector<WalRecord> &records,
                         Error &error) const {
  error.clear();
  if (records.size() > limits_.max_wal_records) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "recovery record count exceeds limit");
    return std::nullopt;
  }
  RecoveryReport report;
  std::map<uint64_t, std::vector<const WalRecord *>> transactions;
  std::set<uint64_t> committed;
  std::set<uint64_t> rolled_back;
  uint64_t previous_lsn = 0;
  for (const WalRecord &record : records) {
    ++report.records_seen;
    if (record.lsn <= previous_lsn) {
      internal::fail(error, ErrorCode::invalid_value, record.lsn,
                     "recovery LSN order is invalid");
      return std::nullopt;
    }
    previous_lsn = record.lsn;
    report.last_lsn = record.lsn;
    transactions[record.transaction_id].push_back(&record);
    if (record.type == WalRecordType::commit)
      committed.insert(record.transaction_id);
    else if (record.type == WalRecordType::rollback)
      rolled_back.insert(record.transaction_id);
  }
  for (const auto &transaction : transactions) {
    if (rolled_back.count(transaction.first)) {
      ++report.transactions_rolled_back;
      continue;
    }
    if (!committed.count(transaction.first)) {
      ++report.transactions_rolled_back;
      report.warnings.push_back("incomplete transaction ignored");
      continue;
    }
    ++report.transactions_committed;
    for (const WalRecord *record : transaction.second) {
      if (record->type != WalRecordType::page_after)
        continue;
      Page *page = pager.page(record->page_id);
      if (!page || record->payload.size() != page->size()) {
        report.warnings.push_back("page-after record targets invalid page");
        continue;
      }
      auto header = page->header(error);
      if (!header)
        return std::nullopt;
      if (header->generation > record->generation)
        continue;
      std::memcpy(page->data(), record->payload.data(), page->size());
      if (!page->verify(error)) {
        error.offset = record->page_id;
        return std::nullopt;
      }
      ++report.records_applied;
    }
  }
  return report;
}

} // namespace queryforge
