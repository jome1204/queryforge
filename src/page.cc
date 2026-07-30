#include "internal.h"

namespace queryforge {
namespace {
constexpr size_t kPageHeaderSize = 32;

void write_header(std::vector<uint8_t> &bytes, const PageHeader &header) {
  internal::patch32(bytes, 4, header.page_id);
  bytes[8] = static_cast<uint8_t>(static_cast<uint16_t>(header.type));
  bytes[9] = static_cast<uint8_t>(static_cast<uint16_t>(header.type) >> 8);
  bytes[10] = static_cast<uint8_t>(header.flags);
  bytes[11] = static_cast<uint8_t>(header.flags >> 8);
  internal::patch32(bytes, 12, header.generation);
  bytes[16] = static_cast<uint8_t>(header.cell_count);
  bytes[17] = static_cast<uint8_t>(header.cell_count >> 8);
  bytes[18] = static_cast<uint8_t>(header.free_start);
  bytes[19] = static_cast<uint8_t>(header.free_start >> 8);
  bytes[20] = static_cast<uint8_t>(header.free_end);
  bytes[21] = static_cast<uint8_t>(header.free_end >> 8);
  internal::patch32(bytes, 24, header.right_page);
  internal::patch32(bytes, 28, header.checksum);
}
} // namespace

Page::Page(uint32_t page_size) : bytes_(page_size, 0) {}

std::optional<PageHeader> Page::header(Error &error) const {
  error.clear();
  if (bytes_.size() < kPageHeaderSize ||
      std::memcmp(bytes_.data(), "QFP1", 4) != 0) {
    internal::fail(error,
                   bytes_.size() < kPageHeaderSize
                       ? ErrorCode::truncated
                       : ErrorCode::invalid_signature,
                   0, "page header is invalid");
    return std::nullopt;
  }
  PageHeader result;
  result.page_id = internal::le32(bytes_.data() + 4);
  result.type = static_cast<PageType>(internal::le16(bytes_.data() + 8));
  result.flags = internal::le16(bytes_.data() + 10);
  result.generation = internal::le32(bytes_.data() + 12);
  result.cell_count = internal::le16(bytes_.data() + 16);
  result.free_start = internal::le16(bytes_.data() + 18);
  result.free_end = internal::le16(bytes_.data() + 20);
  result.right_page = internal::le32(bytes_.data() + 24);
  result.checksum = internal::le32(bytes_.data() + 28);
  if (static_cast<uint16_t>(result.type) >
          static_cast<uint16_t>(PageType::overflow) ||
      result.free_start < kPageHeaderSize ||
      result.free_start > result.free_end || result.free_end > bytes_.size()) {
    internal::fail(error, ErrorCode::invalid_offset, 8,
                   "page header fields are inconsistent");
    return std::nullopt;
  }
  return result;
}

bool Page::initialize(uint32_t page_id, PageType type, uint32_t generation,
                      Error &error) {
  error.clear();
  if (bytes_.size() < kPageHeaderSize || bytes_.size() > UINT16_MAX)
    return internal::fail(error, ErrorCode::resource_limit, 0,
                          "page size cannot be represented");
  std::fill(bytes_.begin(), bytes_.end(), 0);
  std::memcpy(bytes_.data(), "QFP1", 4);
  PageHeader value;
  value.page_id = page_id;
  value.type = type;
  value.generation = generation;
  value.free_start = static_cast<uint16_t>(kPageHeaderSize);
  value.free_end = static_cast<uint16_t>(bytes_.size());
  write_header(bytes_, value);
  return update_checksum(error);
}

bool Page::verify(Error &error) const {
  auto value = header(error);
  if (!value)
    return false;
  std::vector<uint8_t> copy = bytes_;
  std::fill(copy.begin() + 28, copy.begin() + 32, 0);
  if (crc32(copy.data(), copy.size()) != value->checksum)
    return internal::fail(error, ErrorCode::checksum_mismatch, 28,
                          "page checksum mismatch");
  return true;
}
bool Page::update_checksum(Error &error) {
  auto value = header(error);
  if (!value)
    return false;
  value->checksum = 0;
  write_header(bytes_, *value);
  value->checksum = crc32(bytes_.data(), bytes_.size());
  write_header(bytes_, *value);
  return true;
}

Pager::Pager(Limits limits) : limits_(limits) {}
bool Pager::create(Error &error) {
  error.clear();
  pages_.clear();
  free_pages_.clear();
  generation_ = 1;
  Page header(limits_.page_size);
  if (!header.initialize(0, PageType::catalog, 1, error))
    return false;
  pages_.push_back(std::move(header));
  return true;
}

bool Pager::open(const uint8_t *data, size_t size, Error &error) {
  error.clear();
  if (size < 32 || size > limits_.max_file_bytes ||
      std::memcmp(data, "QFDB1\0\0\0", 8) != 0) {
    return internal::fail(
        error, size < 32 ? ErrorCode::truncated : ErrorCode::invalid_signature,
        0, "database file header is invalid");
  }
  uint32_t version = internal::le32(data + 8);
  uint32_t page_size = internal::le32(data + 12);
  uint64_t page_count = internal::le64(data + 16);
  generation_ = internal::le64(data + 24);
  if (version != 1 || page_size != limits_.page_size || page_count == 0 ||
      page_count > limits_.max_pages) {
    return internal::fail(error, ErrorCode::invalid_version, 8,
                          "database version or dimensions are invalid");
  }
  uint64_t page_bytes = 0;
  if (!internal::checked_multiply(page_count, static_cast<uint64_t>(page_size),
                                  page_bytes) ||
      page_bytes != size - 32)
    return internal::fail(error, ErrorCode::invalid_offset, 16,
                          "database page area size is invalid");
  std::vector<Page> loaded;
  loaded.reserve(static_cast<size_t>(page_count));
  std::vector<uint32_t> free;
  for (uint64_t index = 0; index < page_count; ++index) {
    Page page(page_size);
    std::memcpy(page.data(), data + 32 + index * page_size, page_size);
    if (!page.verify(error)) {
      error.offset += 32 + index * page_size;
      return false;
    }
    auto header = page.header(error);
    if (!header || header->page_id != index)
      return internal::fail(error, ErrorCode::invalid_offset,
                            32 + index * page_size,
                            "database page identifier mismatch");
    if (header->type == PageType::free)
      free.push_back(static_cast<uint32_t>(index));
    loaded.push_back(std::move(page));
  }
  pages_ = std::move(loaded);
  free_pages_ = std::move(free);
  return true;
}

uint32_t Pager::allocate(PageType type, Error &error) {
  error.clear();
  uint32_t identifier = 0;
  if (!free_pages_.empty()) {
    identifier = free_pages_.back();
    free_pages_.pop_back();
  } else {
    if (pages_.size() >= limits_.max_pages) {
      internal::fail(error, ErrorCode::resource_limit, pages_.size(),
                     "page count exceeds limit");
      return 0;
    }
    identifier = static_cast<uint32_t>(pages_.size());
    pages_.emplace_back(limits_.page_size);
  }
  ++generation_;
  if (!pages_[identifier].initialize(identifier, type,
                                     static_cast<uint32_t>(generation_), error))
    return 0;
  return identifier;
}
Page *Pager::page(uint32_t identifier) {
  return identifier < pages_.size() ? &pages_[identifier] : nullptr;
}
const Page *Pager::page(uint32_t identifier) const {
  return identifier < pages_.size() ? &pages_[identifier] : nullptr;
}
bool Pager::free(uint32_t identifier, Error &error) {
  if (identifier == 0 || identifier >= pages_.size())
    return internal::fail(error, ErrorCode::invalid_offset, identifier,
                          "cannot free requested page");
  if (!pages_[identifier].initialize(identifier, PageType::free,
                                     static_cast<uint32_t>(++generation_),
                                     error))
    return false;
  if (std::find(free_pages_.begin(), free_pages_.end(), identifier) ==
      free_pages_.end())
    free_pages_.push_back(identifier);
  return true;
}
std::vector<uint8_t> Pager::serialize(Error &error) const {
  error.clear();
  uint64_t bytes = 0;
  if (!internal::checked_multiply(static_cast<uint64_t>(pages_.size()),
                                  static_cast<uint64_t>(limits_.page_size),
                                  bytes) ||
      bytes > limits_.max_file_bytes - 32) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "serialized pager exceeds file limit");
    return {};
  }
  std::vector<uint8_t> output(32, 0);
  std::memcpy(output.data(), "QFDB1\0\0\0", 8);
  internal::patch32(output, 8, 1);
  internal::patch32(output, 12, limits_.page_size);
  internal::append64(output, 0); // removed below
  output.resize(32);
  for (unsigned shift = 0; shift < 64; shift += 8)
    output[16 + shift / 8] = static_cast<uint8_t>(pages_.size() >> shift);
  for (unsigned shift = 0; shift < 64; shift += 8)
    output[24 + shift / 8] = static_cast<uint8_t>(generation_ >> shift);
  for (const Page &page : pages_)
    output.insert(output.end(), page.bytes().begin(), page.bytes().end());
  return output;
}

} // namespace queryforge
