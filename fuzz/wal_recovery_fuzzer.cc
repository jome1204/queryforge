#include "queryforge/database.h"

#include <cstddef>
#include <cstdint>

namespace {
uint32_t read32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 4 || size > 8 * 1024 * 1024)
    return 0;
  size_t database_size = read32(data);
  if (database_size > size - 4)
    return 0;
  queryforge::Limits limits;
  limits.max_file_bytes = 4 * 1024 * 1024;
  limits.max_wal_bytes = 4 * 1024 * 1024;
  limits.max_wal_records = 8192;
  queryforge::Database database(limits);
  queryforge::Error open_error;
  if (!database.open(data + 4, database_size, open_error))
    return 0;
  queryforge::RecoveryReport report;
  queryforge::Error recovery_error;
  (void)database.recover(data + 4 + database_size,
                         size - 4 - database_size, report, recovery_error);
  (void)database.verify();
  queryforge::Error checkpoint_error;
  (void)database.checkpoint(checkpoint_error);
  return 0;
}
