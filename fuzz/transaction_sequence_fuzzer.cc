#include "queryforge/database.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  queryforge::Limits limits;
  limits.max_rows = 4096;
  limits.max_output_rows = 4096;
  limits.max_undo_entries = 4096;
  queryforge::Database database(limits);
  (void)database.execute(
      "CREATE TABLE items(id INTEGER PRIMARY KEY, value INTEGER, "
      "label TEXT);");
  const size_t operations = size < 512 ? size : 512;
  for (size_t index = 0; index < operations; ++index) {
    const uint8_t byte = data[index];
    const unsigned id = (byte >> 3) & 31u;
    const unsigned value = (byte * 17u + static_cast<unsigned>(index)) & 1023u;
    switch (byte & 7u) {
    case 0:
      (void)database.execute("BEGIN;");
      break;
    case 1:
      (void)database.execute(
          "INSERT INTO items VALUES(" + std::to_string(id) + "," +
          std::to_string(value) + ",'v" + std::to_string(byte) + "');");
      break;
    case 2:
      (void)database.execute(
          "UPDATE items SET value=" + std::to_string(value) +
          " WHERE id=" + std::to_string(id) + ";");
      break;
    case 3:
      (void)database.execute("DELETE FROM items WHERE id=" +
                             std::to_string(id) + ";");
      break;
    case 4:
      (void)database.execute("COMMIT;");
      break;
    case 5:
      (void)database.execute("ROLLBACK;");
      break;
    case 6: {
      queryforge::Error error;
      auto image = database.serialize(error);
      if (!error && !image.empty()) {
        queryforge::Database reopened(limits);
        queryforge::Error open_error;
        if (reopened.open(image.data(), image.size(), open_error))
          database = std::move(reopened);
      }
      break;
    }
    case 7:
      (void)database.execute(
          "SELECT id,value,label FROM items WHERE value >= 0 ORDER BY id;");
      break;
    }
    (void)database.verify();
  }
  if (database.transaction_state() == queryforge::TransactionState::active)
    (void)database.execute("ROLLBACK;");
  (void)database.invariant_hash();
  return 0;
}
