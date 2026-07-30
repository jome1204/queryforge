#include "internal.h"

#include <charconv>
#include <cstdlib>

namespace queryforge {
namespace {

class ParserImpl {
public:
  ParserImpl(const std::vector<Token> &tokens, Limits limits, Error &error)
      : tokens_(tokens), limits_(limits), error_(error) {}

  std::optional<std::vector<Statement>> statements() {
    std::vector<Statement> output;
    while (!at(TokenKind::end)) {
      while (match(TokenKind::semicolon)) {
      }
      if (at(TokenKind::end))
        break;
      if (output.size() >= limits_.max_statements) {
        fail("statement count exceeds limit");
        return std::nullopt;
      }
      auto statement = parse_statement();
      if (!statement)
        return std::nullopt;
      output.push_back(std::move(*statement));
      if (!at(TokenKind::end) && !match(TokenKind::semicolon)) {
        fail("expected semicolon between statements");
        return std::nullopt;
      }
    }
    return output;
  }

private:
  const Token &current() const { return tokens_[position_]; }
  bool at(TokenKind kind) const { return current().kind == kind; }
  bool match(TokenKind kind) {
    if (!at(kind))
      return false;
    ++position_;
    return true;
  }
  bool expect(TokenKind kind, std::string_view message) {
    if (match(kind))
      return true;
    fail(message);
    return false;
  }
  bool fail(std::string_view message) {
    return internal::fail(error_, ErrorCode::syntax_error, current().offset,
                          std::string(message));
  }
  std::optional<std::string> identifier() {
    if (!at(TokenKind::identifier)) {
      fail("expected identifier");
      return std::nullopt;
    }
    std::string output = current().text;
    ++position_;
    return output;
  }
  std::optional<uint64_t> unsigned_integer() {
    if (!at(TokenKind::integer)) {
      fail("expected nonnegative integer");
      return std::nullopt;
    }
    uint64_t output = 0;
    auto converted =
        std::from_chars(current().text.data(),
                        current().text.data() + current().text.size(), output);
    if (converted.ec != std::errc{}) {
      fail("integer literal overflows");
      return std::nullopt;
    }
    ++position_;
    return output;
  }

  std::optional<Statement> parse_statement() {
    uint64_t offset = current().offset;
    Statement statement;
    statement.offset = offset;
    if (at(TokenKind::keyword_select)) {
      auto value = select_statement();
      if (!value)
        return std::nullopt;
      statement.data = std::move(*value);
    } else if (at(TokenKind::keyword_insert)) {
      auto value = insert_statement();
      if (!value)
        return std::nullopt;
      statement.data = std::move(*value);
    } else if (at(TokenKind::keyword_update)) {
      auto value = update_statement();
      if (!value)
        return std::nullopt;
      statement.data = std::move(*value);
    } else if (at(TokenKind::keyword_delete)) {
      auto value = delete_statement();
      if (!value)
        return std::nullopt;
      statement.data = std::move(*value);
    } else if (at(TokenKind::keyword_create)) {
      ++position_;
      if (at(TokenKind::keyword_table)) {
        auto value = create_table();
        if (!value)
          return std::nullopt;
        statement.data = std::move(*value);
      } else {
        bool unique = match(TokenKind::keyword_unique);
        auto value = create_index(unique);
        if (!value)
          return std::nullopt;
        statement.data = std::move(*value);
      }
    } else if (at(TokenKind::keyword_drop)) {
      auto value = drop_statement();
      if (!value)
        return std::nullopt;
      statement.data = std::move(*value);
    } else if (match(TokenKind::keyword_begin)) {
      statement.data = TransactionCommand::begin;
    } else if (match(TokenKind::keyword_commit)) {
      statement.data = TransactionCommand::commit;
    } else if (match(TokenKind::keyword_rollback)) {
      statement.data = TransactionCommand::rollback;
    } else if (match(TokenKind::keyword_checkpoint)) {
      statement.data = TransactionCommand::checkpoint;
    } else {
      fail("unsupported statement");
      return std::nullopt;
    }
    return statement;
  }

  std::optional<SelectStatement> select_statement() {
    SelectStatement output;
    expect(TokenKind::keyword_select, "expected SELECT");
    do {
      if (output.items.size() >= limits_.max_columns) {
        fail("projection count exceeds limit");
        return std::nullopt;
      }
      SelectItem item;
      if (match(TokenKind::star)) {
        item.wildcard = true;
      } else {
        item.expression = expression();
        if (!item.expression)
          return std::nullopt;
        if (match(TokenKind::keyword_as)) {
          auto alias = identifier();
          if (!alias)
            return std::nullopt;
          item.alias = std::move(*alias);
        } else if (at(TokenKind::identifier) &&
                   current().kind != TokenKind::keyword_from) {
          item.alias = current().text;
          ++position_;
        }
      }
      output.items.push_back(std::move(item));
    } while (match(TokenKind::comma));
    if (match(TokenKind::keyword_from)) {
      auto table = identifier();
      if (!table)
        return std::nullopt;
      output.table = std::move(*table);
      if (match(TokenKind::keyword_as)) {
        auto alias = identifier();
        if (!alias)
          return std::nullopt;
        output.alias = std::move(*alias);
      } else if (at(TokenKind::identifier)) {
        output.alias = current().text;
        ++position_;
      }
      while (at(TokenKind::keyword_join) || at(TokenKind::keyword_inner) ||
             at(TokenKind::keyword_left)) {
        if (output.joins.size() >= limits_.max_join_tables) {
          fail("join count exceeds limit");
          return std::nullopt;
        }
        JoinClause join;
        if (match(TokenKind::keyword_left)) {
          join.kind = JoinClause::Kind::left;
          expect(TokenKind::keyword_join, "expected JOIN after LEFT");
        } else {
          match(TokenKind::keyword_inner);
          expect(TokenKind::keyword_join, "expected JOIN");
        }
        auto table_name = identifier();
        if (!table_name)
          return std::nullopt;
        join.table = std::move(*table_name);
        if (match(TokenKind::keyword_as)) {
          auto alias = identifier();
          if (!alias)
            return std::nullopt;
          join.alias = std::move(*alias);
        } else if (at(TokenKind::identifier)) {
          join.alias = current().text;
          ++position_;
        }
        if (!expect(TokenKind::keyword_on, "expected ON in join"))
          return std::nullopt;
        join.condition = expression();
        if (!join.condition)
          return std::nullopt;
        output.joins.push_back(std::move(join));
      }
    }
    if (match(TokenKind::keyword_where)) {
      output.where = expression();
      if (!output.where)
        return std::nullopt;
    }
    if (match(TokenKind::keyword_group)) {
      if (!expect(TokenKind::keyword_by, "expected BY after GROUP"))
        return std::nullopt;
      do {
        auto item = expression();
        if (!item)
          return std::nullopt;
        output.group_by.push_back(std::move(item));
      } while (match(TokenKind::comma));
    }
    if (match(TokenKind::keyword_having)) {
      output.having = expression();
      if (!output.having)
        return std::nullopt;
    }
    if (match(TokenKind::keyword_order)) {
      if (!expect(TokenKind::keyword_by, "expected BY after ORDER"))
        return std::nullopt;
      do {
        OrderItem item;
        item.expression = expression();
        if (!item.expression)
          return std::nullopt;
        if (match(TokenKind::keyword_desc))
          item.ascending = false;
        else
          match(TokenKind::keyword_asc);
        output.order_by.push_back(std::move(item));
      } while (match(TokenKind::comma));
    }
    if (match(TokenKind::keyword_limit)) {
      output.limit = unsigned_integer();
      if (!output.limit)
        return std::nullopt;
      if (match(TokenKind::keyword_offset)) {
        auto offset = unsigned_integer();
        if (!offset)
          return std::nullopt;
        output.offset = *offset;
      }
    }
    return output;
  }

  std::optional<InsertStatement> insert_statement() {
    InsertStatement output;
    expect(TokenKind::keyword_insert, "expected INSERT");
    if (!expect(TokenKind::keyword_into, "expected INTO"))
      return std::nullopt;
    auto table = identifier();
    if (!table)
      return std::nullopt;
    output.table = std::move(*table);
    if (match(TokenKind::left_parenthesis)) {
      do {
        auto column = identifier();
        if (!column)
          return std::nullopt;
        output.columns.push_back(std::move(*column));
        if (output.columns.size() > limits_.max_columns) {
          fail("insert column count exceeds limit");
          return std::nullopt;
        }
      } while (match(TokenKind::comma));
      if (!expect(TokenKind::right_parenthesis,
                  "expected closing parenthesis after columns"))
        return std::nullopt;
    }
    if (!expect(TokenKind::keyword_values, "expected VALUES"))
      return std::nullopt;
    do {
      if (!expect(TokenKind::left_parenthesis,
                  "expected opening parenthesis for row"))
        return std::nullopt;
      std::vector<std::unique_ptr<Expression>> row;
      if (!at(TokenKind::right_parenthesis)) {
        do {
          auto value = expression();
          if (!value)
            return std::nullopt;
          row.push_back(std::move(value));
          if (row.size() > limits_.max_columns) {
            fail("insert row width exceeds limit");
            return std::nullopt;
          }
        } while (match(TokenKind::comma));
      }
      if (!expect(TokenKind::right_parenthesis,
                  "expected closing parenthesis after row"))
        return std::nullopt;
      output.rows.push_back(std::move(row));
      if (output.rows.size() > limits_.max_output_rows) {
        fail("insert row count exceeds limit");
        return std::nullopt;
      }
    } while (match(TokenKind::comma));
    return output;
  }

  std::optional<UpdateStatement> update_statement() {
    UpdateStatement output;
    expect(TokenKind::keyword_update, "expected UPDATE");
    auto table = identifier();
    if (!table)
      return std::nullopt;
    output.table = std::move(*table);
    if (!expect(TokenKind::keyword_set, "expected SET"))
      return std::nullopt;
    do {
      auto column = identifier();
      if (!column || !expect(TokenKind::equal, "expected equals in assignment"))
        return std::nullopt;
      auto value = expression();
      if (!value)
        return std::nullopt;
      output.assignments.push_back({std::move(*column), std::move(value)});
      if (output.assignments.size() > limits_.max_columns) {
        fail("update assignment count exceeds limit");
        return std::nullopt;
      }
    } while (match(TokenKind::comma));
    if (match(TokenKind::keyword_where)) {
      output.where = expression();
      if (!output.where)
        return std::nullopt;
    }
    return output;
  }

  std::optional<DeleteStatement> delete_statement() {
    DeleteStatement output;
    expect(TokenKind::keyword_delete, "expected DELETE");
    if (!expect(TokenKind::keyword_from, "expected FROM"))
      return std::nullopt;
    auto table = identifier();
    if (!table)
      return std::nullopt;
    output.table = std::move(*table);
    if (match(TokenKind::keyword_where)) {
      output.where = expression();
      if (!output.where)
        return std::nullopt;
    }
    return output;
  }

  std::optional<CreateTableStatement> create_table() {
    CreateTableStatement output;
    expect(TokenKind::keyword_table, "expected TABLE");
    auto table = identifier();
    if (!table)
      return std::nullopt;
    output.table = std::move(*table);
    if (!expect(TokenKind::left_parenthesis,
                "expected opening parenthesis after table"))
      return std::nullopt;
    do {
      ColumnDefinition column;
      auto name = identifier();
      if (!name)
        return std::nullopt;
      column.name = std::move(*name);
      if (match(TokenKind::keyword_integer))
        column.type = DataType::integer;
      else if (match(TokenKind::keyword_real))
        column.type = DataType::real;
      else if (match(TokenKind::keyword_text))
        column.type = DataType::text;
      else if (match(TokenKind::keyword_boolean))
        column.type = DataType::boolean;
      else if (match(TokenKind::keyword_blob))
        column.type = DataType::blob;
      else {
        fail("expected column data type");
        return std::nullopt;
      }
      bool modifiers = true;
      while (modifiers) {
        if (match(TokenKind::keyword_not)) {
          if (!expect(TokenKind::keyword_null, "expected NULL after NOT"))
            return std::nullopt;
          column.nullable = false;
        } else if (match(TokenKind::keyword_primary)) {
          if (!expect(TokenKind::keyword_key, "expected KEY after PRIMARY"))
            return std::nullopt;
          column.primary_key = true;
          column.unique = true;
          column.nullable = false;
        } else if (match(TokenKind::keyword_unique)) {
          column.unique = true;
        } else if (match(TokenKind::keyword_default)) {
          auto value = primary_expression();
          if (!value || value->kind != Expression::Kind::literal) {
            fail("DEFAULT requires a literal");
            return std::nullopt;
          }
          column.default_value = value->literal;
        } else {
          modifiers = false;
        }
      }
      output.columns.push_back(std::move(column));
      if (output.columns.size() > limits_.max_columns) {
        fail("table column count exceeds limit");
        return std::nullopt;
      }
    } while (match(TokenKind::comma));
    if (!expect(TokenKind::right_parenthesis,
                "expected closing parenthesis after table columns"))
      return std::nullopt;
    return output;
  }

  std::optional<CreateIndexStatement> create_index(bool unique) {
    CreateIndexStatement output;
    output.unique = unique;
    if (!expect(TokenKind::keyword_index, "expected INDEX"))
      return std::nullopt;
    auto name = identifier();
    if (!name)
      return std::nullopt;
    output.index = std::move(*name);
    if (!expect(TokenKind::keyword_on, "expected ON"))
      return std::nullopt;
    auto table = identifier();
    if (!table)
      return std::nullopt;
    output.table = std::move(*table);
    if (!expect(TokenKind::left_parenthesis, "expected index column list"))
      return std::nullopt;
    do {
      auto column = identifier();
      if (!column)
        return std::nullopt;
      output.columns.push_back(std::move(*column));
    } while (match(TokenKind::comma));
    if (!expect(TokenKind::right_parenthesis,
                "expected closing parenthesis after index columns"))
      return std::nullopt;
    return output;
  }

  std::optional<DropStatement> drop_statement() {
    DropStatement output;
    expect(TokenKind::keyword_drop, "expected DROP");
    if (match(TokenKind::keyword_table))
      output.kind = DropStatement::Kind::table;
    else if (match(TokenKind::keyword_index))
      output.kind = DropStatement::Kind::index;
    else {
      fail("expected TABLE or INDEX after DROP");
      return std::nullopt;
    }
    auto name = identifier();
    if (!name)
      return std::nullopt;
    output.name = std::move(*name);
    return output;
  }

  std::unique_ptr<Expression> expression() { return logical_or(); }
  std::unique_ptr<Expression> logical_or() {
    auto left = logical_and();
    while (left && match(TokenKind::keyword_or))
      left = binary(std::move(left), BinaryOperator::logical_or, logical_and());
    return left;
  }
  std::unique_ptr<Expression> logical_and() {
    auto left = comparison();
    while (left && match(TokenKind::keyword_and))
      left = binary(std::move(left), BinaryOperator::logical_and, comparison());
    return left;
  }
  std::unique_ptr<Expression> comparison() {
    auto left = additive();
    if (!left)
      return nullptr;
    BinaryOperator operation;
    bool found = true;
    if (match(TokenKind::equal))
      operation = BinaryOperator::equal;
    else if (match(TokenKind::not_equal))
      operation = BinaryOperator::not_equal;
    else if (match(TokenKind::less))
      operation = BinaryOperator::less;
    else if (match(TokenKind::less_equal))
      operation = BinaryOperator::less_equal;
    else if (match(TokenKind::greater))
      operation = BinaryOperator::greater;
    else if (match(TokenKind::greater_equal))
      operation = BinaryOperator::greater_equal;
    else if (match(TokenKind::keyword_like))
      operation = BinaryOperator::like;
    else
      found = false;
    if (found)
      return binary(std::move(left), operation, additive());
    if (match(TokenKind::keyword_is)) {
      bool negate = match(TokenKind::keyword_not);
      if (!expect(TokenKind::keyword_null, "expected NULL after IS"))
        return nullptr;
      auto result = std::make_unique<Expression>();
      result->kind = Expression::Kind::unary;
      result->unary =
          negate ? UnaryOperator::is_not_null : UnaryOperator::is_null;
      result->left = std::move(left);
      return result;
    }
    return left;
  }
  std::unique_ptr<Expression> additive() {
    auto left = multiplicative();
    while (left && (at(TokenKind::plus) || at(TokenKind::minus))) {
      BinaryOperator operation =
          match(TokenKind::plus)
              ? BinaryOperator::add
              : (match(TokenKind::minus), BinaryOperator::subtract);
      left = binary(std::move(left), operation, multiplicative());
    }
    return left;
  }
  std::unique_ptr<Expression> multiplicative() {
    auto left = unary_expression();
    while (left && (at(TokenKind::star) || at(TokenKind::slash) ||
                    at(TokenKind::percent))) {
      BinaryOperator operation;
      if (match(TokenKind::star))
        operation = BinaryOperator::multiply;
      else if (match(TokenKind::slash))
        operation = BinaryOperator::divide;
      else {
        match(TokenKind::percent);
        operation = BinaryOperator::modulo;
      }
      left = binary(std::move(left), operation, unary_expression());
    }
    return left;
  }
  std::unique_ptr<Expression> unary_expression() {
    if (at(TokenKind::plus) || at(TokenKind::minus) ||
        at(TokenKind::keyword_not)) {
      TokenKind token = current().kind;
      ++position_;
      auto child = unary_expression();
      if (!child)
        return nullptr;
      auto result = std::make_unique<Expression>();
      result->kind = Expression::Kind::unary;
      result->unary =
          token == TokenKind::plus
              ? UnaryOperator::positive
              : (token == TokenKind::minus ? UnaryOperator::negative
                                           : UnaryOperator::logical_not);
      result->left = std::move(child);
      return result;
    }
    return primary_expression();
  }
  std::unique_ptr<Expression> primary_expression() {
    if (++nodes_ > limits_.max_expression_nodes) {
      fail("expression node count exceeds limit");
      return nullptr;
    }
    uint64_t offset = current().offset;
    auto result = std::make_unique<Expression>();
    result->offset = offset;
    if (match(TokenKind::keyword_null)) {
      result->literal = Value();
    } else if (match(TokenKind::keyword_true)) {
      result->literal = Value(true);
    } else if (match(TokenKind::keyword_false)) {
      result->literal = Value(false);
    } else if (at(TokenKind::string)) {
      result->literal = Value(current().text);
      ++position_;
    } else if (at(TokenKind::integer)) {
      int64_t value = 0;
      auto converted =
          std::from_chars(current().text.data(),
                          current().text.data() + current().text.size(), value);
      if (converted.ec != std::errc{}) {
        fail("integer literal overflows");
        return nullptr;
      }
      result->literal = Value(value);
      ++position_;
    } else if (at(TokenKind::real)) {
      char *end = nullptr;
      double value = std::strtod(current().text.c_str(), &end);
      if (end != current().text.data() + current().text.size() ||
          !std::isfinite(value)) {
        fail("real literal is invalid");
        return nullptr;
      }
      result->literal = Value(value);
      ++position_;
    } else if (at(TokenKind::identifier)) {
      std::string name = current().text;
      ++position_;
      if (match(TokenKind::left_parenthesis)) {
        result->kind = Expression::Kind::function;
        result->name = std::move(name);
        if (!at(TokenKind::right_parenthesis)) {
          do {
            if (match(TokenKind::star)) {
              auto wildcard = std::make_unique<Expression>();
              wildcard->kind = Expression::Kind::column;
              wildcard->name = "*";
              result->arguments.push_back(std::move(wildcard));
            } else {
              auto argument = expression();
              if (!argument)
                return nullptr;
              result->arguments.push_back(std::move(argument));
            }
          } while (match(TokenKind::comma));
        }
        if (!expect(TokenKind::right_parenthesis,
                    "expected closing function parenthesis"))
          return nullptr;
      } else {
        result->kind = Expression::Kind::column;
        if (match(TokenKind::dot)) {
          result->qualifier = std::move(name);
          auto column = identifier();
          if (!column)
            return nullptr;
          result->name = std::move(*column);
        } else {
          result->name = std::move(name);
        }
      }
    } else if (match(TokenKind::left_parenthesis)) {
      if (++depth_ > limits_.max_parser_depth) {
        fail("expression nesting exceeds limit");
        return nullptr;
      }
      result = expression();
      --depth_;
      if (!result || !expect(TokenKind::right_parenthesis,
                             "expected closing expression parenthesis"))
        return nullptr;
    } else {
      fail("expected expression");
      return nullptr;
    }
    return result;
  }
  std::unique_ptr<Expression> binary(std::unique_ptr<Expression> left,
                                     BinaryOperator operation,
                                     std::unique_ptr<Expression> right) {
    if (!right)
      return nullptr;
    auto result = std::make_unique<Expression>();
    result->kind = Expression::Kind::binary;
    result->binary = operation;
    result->left = std::move(left);
    result->right = std::move(right);
    return result;
  }

  const std::vector<Token> &tokens_;
  Limits limits_;
  Error &error_;
  size_t position_ = 0;
  uint32_t depth_ = 0;
  uint32_t nodes_ = 0;
};

} // namespace

Parser::Parser(Limits limits) : limits_(limits) {}
ParseResult Parser::parse(std::string_view sql) const {
  ParseResult result;
  auto tokens = Lexer(limits_).tokenize(sql, result.error);
  if (!tokens)
    return result;
  result.statements = ParserImpl(*tokens, limits_, result.error).statements();
  return result;
}

} // namespace queryforge
