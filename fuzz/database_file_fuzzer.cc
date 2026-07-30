#include "queryforge/database.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  queryforge::Limits limits;
  limits.max_file_bytes = 4 * 1024 * 1024;
  limits.max_record_bytes = 256 * 1024;
  limits.max_rows = 16384;
  limits.max_tables = 256;
  if (size > limits.max_file_bytes)
    return 0;
  queryforge::Database database(limits);
  queryforge::Error error;
  if (database.open(data, size, error)) {
    (void)database.verify();
    (void)database.invariant_hash();
    queryforge::IntegrityChecker checker(limits);
    (void)checker.check_database(database);
    queryforge::Error encode_error;
    (void)database.serialize(encode_error);
  }
  queryforge::Pager pager(limits);
  queryforge::Error page_error;
  if (pager.open(data, size, page_error)) {
    for (uint32_t id = 0; id < pager.page_count(); ++id) {
      if (const auto *page = pager.page(id)) {
        queryforge::Error verify_error;
        (void)page->verify(verify_error);
      }
    }
  }
  return 0;
}
