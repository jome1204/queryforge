#include "internal.h"

#include <charconv>

namespace queryforge {
namespace {
const std::unordered_map<std::string, TokenKind> &keywords() {
  static const std::unordered_map<std::string, TokenKind> values = {
      {"SELECT", TokenKind::keyword_select},
      {"FROM", TokenKind::keyword_from},
      {"WHERE", TokenKind::keyword_where},
      {"INSERT", TokenKind::keyword_insert},
      {"INTO", TokenKind::keyword_into},
      {"VALUES", TokenKind::keyword_values},
      {"UPDATE", TokenKind::keyword_update},
      {"SET", TokenKind::keyword_set},
      {"DELETE", TokenKind::keyword_delete},
      {"CREATE", TokenKind::keyword_create},
      {"TABLE", TokenKind::keyword_table},
      {"INDEX", TokenKind::keyword_index},
      {"ON", TokenKind::keyword_on},
      {"DROP", TokenKind::keyword_drop},
      {"AND", TokenKind::keyword_and},
      {"OR", TokenKind::keyword_or},
      {"NOT", TokenKind::keyword_not},
      {"NULL", TokenKind::keyword_null},
      {"TRUE", TokenKind::keyword_true},
      {"FALSE", TokenKind::keyword_false},
      {"AS", TokenKind::keyword_as},
      {"ORDER", TokenKind::keyword_order},
      {"BY", TokenKind::keyword_by},
      {"ASC", TokenKind::keyword_asc},
      {"DESC", TokenKind::keyword_desc},
      {"LIMIT", TokenKind::keyword_limit},
      {"OFFSET", TokenKind::keyword_offset},
      {"JOIN", TokenKind::keyword_join},
      {"INNER", TokenKind::keyword_inner},
      {"LEFT", TokenKind::keyword_left},
      {"BEGIN", TokenKind::keyword_begin},
      {"COMMIT", TokenKind::keyword_commit},
      {"ROLLBACK", TokenKind::keyword_rollback},
      {"CHECKPOINT", TokenKind::keyword_checkpoint},
      {"PRIMARY", TokenKind::keyword_primary},
      {"KEY", TokenKind::keyword_key},
      {"UNIQUE", TokenKind::keyword_unique},
      {"DEFAULT", TokenKind::keyword_default},
      {"INTEGER", TokenKind::keyword_integer},
      {"REAL", TokenKind::keyword_real},
      {"TEXT", TokenKind::keyword_text},
      {"BOOLEAN", TokenKind::keyword_boolean},
      {"BLOB", TokenKind::keyword_blob},
      {"IS", TokenKind::keyword_is},
      {"IN", TokenKind::keyword_in},
      {"LIKE", TokenKind::keyword_like},
      {"GROUP", TokenKind::keyword_group},
      {"HAVING", TokenKind::keyword_having}};
  return values;
}
bool identifier_start(unsigned char c) { return std::isalpha(c) || c == '_'; }
bool identifier_continue(unsigned char c) {
  return std::isalnum(c) || c == '_';
}
} // namespace

Lexer::Lexer(Limits limits) : limits_(limits) {}

std::optional<std::vector<Token>> Lexer::tokenize(std::string_view sql,
                                                  Error &error) const {
  error.clear();
  if (sql.size() > limits_.max_sql_bytes) {
    internal::fail(error, ErrorCode::resource_limit, 0,
                   "SQL input exceeds byte limit");
    return std::nullopt;
  }
  std::vector<Token> tokens;
  size_t position = 0;
  auto add = [&](TokenKind kind, size_t begin, size_t end,
                 std::string text = {}) {
    if (text.empty())
      text.assign(sql.substr(begin, end - begin));
    tokens.push_back({kind, std::move(text), begin, end - begin});
  };
  while (position < sql.size()) {
    unsigned char c = static_cast<unsigned char>(sql[position]);
    if (std::isspace(c)) {
      ++position;
      continue;
    }
    if (c == '-' && position + 1 < sql.size() && sql[position + 1] == '-') {
      position += 2;
      while (position < sql.size() && sql[position] != '\n')
        ++position;
      continue;
    }
    if (c == '/' && position + 1 < sql.size() && sql[position + 1] == '*') {
      size_t begin = position;
      position += 2;
      uint32_t depth = 1;
      while (position < sql.size() && depth) {
        if (position + 1 < sql.size() && sql[position] == '/' &&
            sql[position + 1] == '*') {
          if (++depth > limits_.max_parser_depth) {
            internal::fail(error, ErrorCode::resource_limit, begin,
                           "comment nesting exceeds limit");
            return std::nullopt;
          }
          position += 2;
        } else if (position + 1 < sql.size() && sql[position] == '*' &&
                   sql[position + 1] == '/') {
          --depth;
          position += 2;
        } else {
          ++position;
        }
      }
      if (depth) {
        internal::fail(error, ErrorCode::lexical_error, begin,
                       "unterminated block comment");
        return std::nullopt;
      }
      continue;
    }
    size_t begin = position;
    if (identifier_start(c)) {
      while (position < sql.size() &&
             identifier_continue(static_cast<unsigned char>(sql[position])))
        ++position;
      if (position - begin > limits_.max_identifier_bytes) {
        internal::fail(error, ErrorCode::resource_limit, begin,
                       "identifier exceeds byte limit");
        return std::nullopt;
      }
      std::string text(sql.substr(begin, position - begin));
      std::string normalized = internal::normalize(text);
      auto keyword = keywords().find(normalized);
      add(keyword == keywords().end() ? TokenKind::identifier : keyword->second,
          begin, position, std::move(text));
      continue;
    }
    if (std::isdigit(c)) {
      bool real = false;
      while (position < sql.size() &&
             std::isdigit(static_cast<unsigned char>(sql[position])))
        ++position;
      if (position < sql.size() && sql[position] == '.') {
        real = true;
        ++position;
        while (position < sql.size() &&
               std::isdigit(static_cast<unsigned char>(sql[position])))
          ++position;
      }
      if (position < sql.size() &&
          (sql[position] == 'e' || sql[position] == 'E')) {
        real = true;
        ++position;
        if (position < sql.size() &&
            (sql[position] == '+' || sql[position] == '-'))
          ++position;
        size_t exponent = position;
        while (position < sql.size() &&
               std::isdigit(static_cast<unsigned char>(sql[position])))
          ++position;
        if (position == exponent) {
          internal::fail(error, ErrorCode::lexical_error, begin,
                         "numeric exponent has no digits");
          return std::nullopt;
        }
      }
      add(real ? TokenKind::real : TokenKind::integer, begin, position);
      continue;
    }
    if (c == '\'' || c == '"' || c == '`' || c == '[') {
      char opening = static_cast<char>(c);
      char closing = opening == '[' ? ']' : opening;
      bool identifier = opening != '\'';
      ++position;
      std::string value;
      while (position < sql.size()) {
        char current = sql[position++];
        if (current == closing) {
          if (position < sql.size() && sql[position] == closing &&
              opening != '[') {
            value.push_back(closing);
            ++position;
            continue;
          }
          break;
        }
        value.push_back(current);
        if (value.size() > (identifier ? limits_.max_identifier_bytes
                                       : limits_.max_string_bytes)) {
          internal::fail(error, ErrorCode::resource_limit, begin,
                         "quoted token exceeds byte limit");
          return std::nullopt;
        }
      }
      if (position > sql.size() || sql[position - 1] != closing) {
        internal::fail(error, ErrorCode::lexical_error, begin,
                       "unterminated quoted token");
        return std::nullopt;
      }
      add(identifier ? TokenKind::identifier : TokenKind::string, begin,
          position, std::move(value));
      continue;
    }
    ++position;
    switch (c) {
    case ',':
      add(TokenKind::comma, begin, position);
      break;
    case '.':
      add(TokenKind::dot, begin, position);
      break;
    case ';':
      add(TokenKind::semicolon, begin, position);
      break;
    case '(':
      add(TokenKind::left_parenthesis, begin, position);
      break;
    case ')':
      add(TokenKind::right_parenthesis, begin, position);
      break;
    case '*':
      add(TokenKind::star, begin, position);
      break;
    case '+':
      add(TokenKind::plus, begin, position);
      break;
    case '-':
      add(TokenKind::minus, begin, position);
      break;
    case '/':
      add(TokenKind::slash, begin, position);
      break;
    case '%':
      add(TokenKind::percent, begin, position);
      break;
    case '=':
      add(TokenKind::equal, begin, position);
      break;
    case '!':
      if (position < sql.size() && sql[position] == '=') {
        ++position;
        add(TokenKind::not_equal, begin, position);
      } else {
        internal::fail(error, ErrorCode::lexical_error, begin,
                       "unexpected exclamation mark");
        return std::nullopt;
      }
      break;
    case '<':
      if (position < sql.size() && sql[position] == '=') {
        ++position;
        add(TokenKind::less_equal, begin, position);
      } else if (position < sql.size() && sql[position] == '>') {
        ++position;
        add(TokenKind::not_equal, begin, position);
      } else {
        add(TokenKind::less, begin, position);
      }
      break;
    case '>':
      if (position < sql.size() && sql[position] == '=') {
        ++position;
        add(TokenKind::greater_equal, begin, position);
      } else {
        add(TokenKind::greater, begin, position);
      }
      break;
    default:
      internal::fail(error, ErrorCode::lexical_error, begin,
                     "unexpected byte in SQL input");
      return std::nullopt;
    }
    if (tokens.size() >
        static_cast<uint64_t>(limits_.max_expression_nodes) * 8) {
      internal::fail(error, ErrorCode::resource_limit, begin,
                     "token count exceeds limit");
      return std::nullopt;
    }
  }
  tokens.push_back({TokenKind::end, {}, sql.size(), 0});
  return tokens;
}

std::string token_kind_name(TokenKind kind) {
  switch (kind) {
  case TokenKind::end:
    return "end";
  case TokenKind::invalid:
    return "invalid";
  case TokenKind::identifier:
    return "identifier";
  case TokenKind::integer:
    return "integer";
  case TokenKind::real:
    return "real";
  case TokenKind::string:
    return "string";
  case TokenKind::blob:
    return "blob";
  case TokenKind::comma:
    return "comma";
  case TokenKind::dot:
    return "dot";
  case TokenKind::semicolon:
    return "semicolon";
  case TokenKind::left_parenthesis:
    return "left_parenthesis";
  case TokenKind::right_parenthesis:
    return "right_parenthesis";
  case TokenKind::star:
    return "star";
  case TokenKind::plus:
    return "plus";
  case TokenKind::minus:
    return "minus";
  case TokenKind::slash:
    return "slash";
  case TokenKind::percent:
    return "percent";
  case TokenKind::equal:
    return "equal";
  case TokenKind::not_equal:
    return "not_equal";
  case TokenKind::less:
    return "less";
  case TokenKind::less_equal:
    return "less_equal";
  case TokenKind::greater:
    return "greater";
  case TokenKind::greater_equal:
    return "greater_equal";
  default:
    return "keyword";
  }
}

} // namespace queryforge
