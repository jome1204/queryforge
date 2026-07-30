// QueryForge interactive and batch command-line client.
#include "queryforge/database.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct Options {
  std::string database;
  std::string script;
  std::string command;
  std::string output;
  bool interactive = false;
  bool verify = false;
  bool report = false;
  bool help = false;
};

void usage(std::ostream &output) {
  output
      << "Usage: queryforge_cli [OPTIONS]\n"
      << "  --database FILE   Open a QueryForge database image\n"
      << "  --script FILE     Execute a UTF-8 SQL script\n"
      << "  --execute SQL     Execute SQL supplied on the command line\n"
      << "  --output FILE     Save the resulting database image\n"
      << "  --interactive     Read SQL from standard input\n"
      << "  --verify          Print database integrity findings\n"
      << "  --report          Print a database statistics report\n"
      << "  --help            Show this help\n";
}

std::optional<Options> parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    std::string argument = argv[index];
    auto value = [&](const char *name) -> std::optional<std::string> {
      if (index + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return std::nullopt;
      }
      return std::string(argv[++index]);
    };
    if (argument == "--database") {
      auto item = value("--database");
      if (!item)
        return std::nullopt;
      options.database = std::move(*item);
    } else if (argument == "--script") {
      auto item = value("--script");
      if (!item)
        return std::nullopt;
      options.script = std::move(*item);
    } else if (argument == "--execute") {
      auto item = value("--execute");
      if (!item)
        return std::nullopt;
      options.command = std::move(*item);
    } else if (argument == "--output") {
      auto item = value("--output");
      if (!item)
        return std::nullopt;
      options.output = std::move(*item);
    } else if (argument == "--interactive") {
      options.interactive = true;
    } else if (argument == "--verify") {
      options.verify = true;
    } else if (argument == "--report") {
      options.report = true;
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else {
      std::cerr << "unknown option: " << argument << '\n';
      return std::nullopt;
    }
  }
  return options;
}

std::optional<std::vector<uint8_t>> read_binary(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "cannot open " << path << '\n';
    return std::nullopt;
  }
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0 || length > 256ll * 1024 * 1024) {
    std::cerr << "invalid or excessive file length: " << path << '\n';
    return std::nullopt;
  }
  input.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  if (!bytes.empty())
    input.read(reinterpret_cast<char *>(bytes.data()), length);
  if (!input) {
    std::cerr << "failed to read " << path << '\n';
    return std::nullopt;
  }
  return bytes;
}

std::optional<std::string> read_text(const std::string &path) {
  auto bytes = read_binary(path);
  if (!bytes)
    return std::nullopt;
  if (bytes->size() > 8 * 1024 * 1024) {
    std::cerr << "SQL script exceeds the 8 MiB limit\n";
    return std::nullopt;
  }
  return std::string(bytes->begin(), bytes->end());
}

bool write_binary(const std::string &path,
                  const std::vector<uint8_t> &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "cannot create " << path << '\n';
    return false;
  }
  if (!bytes.empty())
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  if (!output) {
    std::cerr << "failed to write " << path << '\n';
    return false;
  }
  return true;
}

std::string cell_text(const queryforge::Value &value) {
  if (value.is_null())
    return "NULL";
  if (value.type() == queryforge::DataType::blob) {
    const auto *blob = value.as_blob();
    std::ostringstream output;
    output << "X'";
    for (uint8_t byte : *blob)
      output << std::hex << std::setw(2) << std::setfill('0')
             << static_cast<unsigned>(byte);
    return output.str() + "'";
  }
  return value.display();
}

void print_result(const queryforge::QueryResult &result) {
  if (!result.columns.empty()) {
    for (size_t column = 0; column < result.columns.size(); ++column) {
      if (column)
        std::cout << " | ";
      std::cout << result.columns[column];
    }
    std::cout << '\n';
    for (size_t column = 0; column < result.columns.size(); ++column) {
      if (column)
        std::cout << "-+-";
      std::cout << std::string(result.columns[column].size(), '-');
    }
    std::cout << '\n';
  }
  for (const auto &row : result.rows) {
    for (size_t column = 0; column < row.size(); ++column) {
      if (column)
        std::cout << " | ";
      std::cout << cell_text(row[column]);
    }
    std::cout << '\n';
  }
  if (!result.message.empty())
    std::cout << result.message;
  if (result.affected_rows)
    std::cout << " (" << result.affected_rows << " affected)";
  if (!result.message.empty() || result.affected_rows)
    std::cout << '\n';
}

bool execute(queryforge::Database &database, std::string_view sql) {
  auto result = database.execute(sql);
  if (!result) {
    std::cerr << queryforge::error_code_name(result.error.code) << " at "
              << result.error.offset << ": " << result.error.message << '\n';
    return false;
  }
  print_result(*result.result);
  return true;
}

bool interactive(queryforge::Database &database) {
  std::string pending;
  std::string line;
  while (true) {
    std::cout << (pending.empty() ? "queryforge> " : "       ...> ")
              << std::flush;
    if (!std::getline(std::cin, line))
      break;
    if (pending.empty() && (line == ".quit" || line == ".exit"))
      break;
    if (pending.empty() && line == ".verify") {
      for (const auto &issue : database.verify())
        std::cout << issue << '\n';
      continue;
    }
    if (pending.empty() && line == ".tables") {
      for (const auto &table : database.catalog().tables())
        std::cout << table.second.name << '\n';
      continue;
    }
    pending += line;
    pending.push_back('\n');
    if (line.find(';') == std::string::npos)
      continue;
    (void)execute(database, pending);
    pending.clear();
  }
  if (!pending.empty())
    return execute(database, pending);
  return true;
}

bool print_verification(const queryforge::Database &database) {
  auto issues = database.verify();
  if (issues.empty()) {
    std::cout << "Integrity check: OK\n";
    return true;
  }
  std::cout << "Integrity check: " << issues.size() << " issue(s)\n";
  for (const auto &issue : issues)
    std::cout << "  " << issue << '\n';
  return false;
}

void print_report(const queryforge::Database &database) {
  queryforge::ReportBuilder builder;
  queryforge::Error error;
  auto report = builder.build(database, error);
  if (error) {
    std::cerr << "report failed: " << error.message << '\n';
    return;
  }
  std::cout << queryforge::ReportBuilder::text(report);
}
} // namespace

int main(int argc, char **argv) {
  auto options = parse_options(argc, argv);
  if (!options) {
    usage(std::cerr);
    return 2;
  }
  if (options->help) {
    usage(std::cout);
    return 0;
  }

  queryforge::Database database;
  if (!options->database.empty()) {
    auto image = read_binary(options->database);
    if (!image)
      return 1;
    queryforge::Error error;
    if (!database.open(image->data(), image->size(), error)) {
      std::cerr << "open failed: " << error.message << '\n';
      return 1;
    }
  }

  bool successful = true;
  if (!options->script.empty()) {
    auto script = read_text(options->script);
    if (!script)
      return 1;
    successful = execute(database, *script) && successful;
  }
  if (!options->command.empty())
    successful = execute(database, options->command) && successful;
  if (options->interactive)
    successful = interactive(database) && successful;
  if (options->verify)
    successful = print_verification(database) && successful;
  if (options->report)
    print_report(database);

  if (!options->output.empty()) {
    queryforge::Error error;
    auto image = database.serialize(error);
    if (error) {
      std::cerr << "serialization failed: " << error.message << '\n';
      return 1;
    }
    successful = write_binary(options->output, image) && successful;
  }
  return successful ? 0 : 1;
}
