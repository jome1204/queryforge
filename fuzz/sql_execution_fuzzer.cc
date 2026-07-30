#include "queryforge/database.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  queryforge::Limits limits;
  limits.max_sql_bytes = 128 * 1024;
  limits.max_statements = 128;
  limits.max_rows = 4096;
  limits.max_output_rows = 2048;
  limits.max_string_bytes = 64 * 1024;
  if (size > limits.max_sql_bytes)
    return 0;
  queryforge::Database database(limits);
  (void)database.execute(
      std::string_view(reinterpret_cast<const char *>(data), size));
  (void)database.verify();
  queryforge::Error error;
  auto encoded = database.serialize(error);
  if (!error && !encoded.empty()) {
    queryforge::Database reopened(limits);
    queryforge::Error open_error;
    if (reopened.open(encoded.data(), encoded.size(), open_error)) {
      (void)reopened.invariant_hash();
      (void)reopened.verify();
    }
  }
  return 0;
}
