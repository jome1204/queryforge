#include "internal.h"

namespace queryforge {

BTree::BTree(Limits limits) : limits_(limits) {}
bool BTree::initialize(uint32_t root_page, bool unique, Error &error) {
  error.clear();
  if (root_page == 0)
    return internal::fail(error, ErrorCode::invalid_offset, root_page,
                          "B-tree root page cannot be zero");
  root_page_ = root_page;
  unique_ = unique;
  entries_.clear();
  return true;
}
bool BTree::insert(std::vector<uint8_t> key, uint64_t row_id, Error &error) {
  error.clear();
  if (root_page_ == 0 || key.size() > limits_.max_record_bytes || row_id == 0)
    return internal::fail(error, ErrorCode::invalid_state, root_page_,
                          "B-tree insert parameters are invalid");
  auto found = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const Entry &entry, const std::vector<uint8_t> &value) {
        return entry.key < value;
      });
  if (found != entries_.end() && found->key == key) {
    if (unique_ && !found->row_ids.empty())
      return internal::fail(error, ErrorCode::constraint_error, row_id,
                            "unique index key already exists");
    auto row =
        std::lower_bound(found->row_ids.begin(), found->row_ids.end(), row_id);
    if (row == found->row_ids.end() || *row != row_id)
      found->row_ids.insert(row, row_id);
    return true;
  }
  if (entries_.size() >= limits_.max_rows)
    return internal::fail(error, ErrorCode::resource_limit, entries_.size(),
                          "B-tree entry count exceeds limit");
  entries_.insert(found, {std::move(key), {row_id}});
  return true;
}
bool BTree::remove(const std::vector<uint8_t> &key, uint64_t row_id,
                   Error &error) {
  error.clear();
  auto found = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const Entry &entry, const std::vector<uint8_t> &value) {
        return entry.key < value;
      });
  if (found == entries_.end() || found->key != key)
    return internal::fail(error, ErrorCode::not_found, row_id,
                          "B-tree key does not exist");
  auto row =
      std::lower_bound(found->row_ids.begin(), found->row_ids.end(), row_id);
  if (row == found->row_ids.end() || *row != row_id)
    return internal::fail(error, ErrorCode::not_found, row_id,
                          "B-tree row identifier does not exist");
  found->row_ids.erase(row);
  if (found->row_ids.empty())
    entries_.erase(found);
  return true;
}
std::vector<uint64_t> BTree::find(const std::vector<uint8_t> &key) const {
  auto found = std::lower_bound(
      entries_.begin(), entries_.end(), key,
      [](const Entry &entry, const std::vector<uint8_t> &value) {
        return entry.key < value;
      });
  return found != entries_.end() && found->key == key ? found->row_ids
                                                      : std::vector<uint64_t>{};
}
std::vector<uint64_t>
BTree::range(const std::optional<std::vector<uint8_t>> &lower,
             const std::optional<std::vector<uint8_t>> &upper,
             uint64_t limit) const {
  std::vector<uint64_t> output;
  auto begin = lower
                   ? std::lower_bound(entries_.begin(), entries_.end(), *lower,
                                      [](const Entry &entry,
                                         const std::vector<uint8_t> &value) {
                                        return entry.key < value;
                                      })
                   : entries_.begin();
  for (auto entry = begin; entry != entries_.end(); ++entry) {
    if (upper && entry->key > *upper)
      break;
    for (uint64_t row : entry->row_ids) {
      if (output.size() >= limit)
        return output;
      output.push_back(row);
    }
  }
  return output;
}
bool BTree::verify(Error &error) const {
  error.clear();
  if (root_page_ == 0)
    return internal::fail(error, ErrorCode::invalid_state, 0,
                          "B-tree has no root");
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (i && !(entries_[i - 1].key < entries_[i].key))
      return internal::fail(error, ErrorCode::invalid_state, i,
                            "B-tree keys are not strictly ordered");
    if (entries_[i].row_ids.empty() ||
        !std::is_sorted(entries_[i].row_ids.begin(),
                        entries_[i].row_ids.end()) ||
        std::adjacent_find(entries_[i].row_ids.begin(),
                           entries_[i].row_ids.end()) !=
            entries_[i].row_ids.end())
      return internal::fail(error, ErrorCode::invalid_state, i,
                            "B-tree row identifiers are invalid");
    if (unique_ && entries_[i].row_ids.size() != 1)
      return internal::fail(error, ErrorCode::constraint_error, i,
                            "unique B-tree key has multiple rows");
  }
  return true;
}
uint64_t BTree::entry_count() const {
  uint64_t count = 0;
  for (const Entry &entry : entries_)
    count += entry.row_ids.size();
  return count;
}

} // namespace queryforge
