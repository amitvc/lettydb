#include <gtest/gtest.h>
#include "sql/lexer.h"
#include "sql/token.h"
#include "test_utils.h"
#include <vector>

using namespace letty;

class LexerTest : public ::testing::Test {};


TEST_F(LexerTest, SimpleSelectAll) {
    std::string query = "SELECT * FROM users;";
    Lexer lexer(query);
    std::vector<Token> tokens = lexer.tokenize();

    std::vector<Token> expected_tokens = {
            {TokenType::SELECT, "SELECT"},
            {TokenType::STAR, "*"},
            {TokenType::FROM, "FROM"},
            {TokenType::IDENTIFIER, "users"},
            {TokenType::SEMICOLON, ";"},
            {TokenType::EOF_FILE, ""}
    };

    assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, SelectWithColumnName) {
    std::string query = "SELECT name,age,sex FROM users;";
    Lexer lexer(query);
    std::vector<Token> tokens = lexer.tokenize();

    std::vector<Token> expected_tokens = {
            {TokenType::SELECT, "SELECT"},
            {TokenType::IDENTIFIER, "name"},
            {TokenType::COMMA, ","},
            {TokenType::IDENTIFIER, "age"},
            {TokenType::COMMA, ","},
            {TokenType::IDENTIFIER, "sex"},
            {TokenType::FROM, "FROM"},
            {TokenType::IDENTIFIER, "users"},
            {TokenType::SEMICOLON, ";"},
            {TokenType::EOF_FILE, ""}
    };
    assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, SelectWithColumnAndWhereClauseName) {
    std::string query = "SELECT name,age,sex FROM users where age >= 40;";
    Lexer lexer(query);
    std::vector<Token> tokens = lexer.tokenize();

    std::vector<Token> expected_tokens = {
            {TokenType::SELECT, "SELECT"},
            {TokenType::IDENTIFIER, "name"},
            {TokenType::COMMA, ","},
            {TokenType::IDENTIFIER, "age"},
            {TokenType::COMMA, ","},
            {TokenType::IDENTIFIER, "sex"},
            {TokenType::FROM, "FROM"},
            {TokenType::IDENTIFIER, "users"},
            {TokenType::WHERE, "where"},
            {TokenType::IDENTIFIER, "age"},
            {TokenType::GTE, ">="},
            {TokenType::INT_LITERAL, "40"},
            {TokenType::SEMICOLON, ";"},
            {TokenType::EOF_FILE, ""}
    };
    assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, SelectJoinTwoTablesWithColumnAlias) {
    std::string query = "SELECT u.id as user_id, p.name \n"
                        "FROM users u\n"
                        "JOIN products p ON u.id = p.user_id\n"
                        "WHERE p.price < 50;";
    Lexer lexer(query);
    std::vector<Token> tokens = lexer.tokenize();
    std::vector<Token> expected_tokens = {
            {TokenType::SELECT, "SELECT"},
            {TokenType::IDENTIFIER, "u"},
            {TokenType::DOT, "."},
            {TokenType::IDENTIFIER, "id"},
            {TokenType::AS, "as"},
            {TokenType::IDENTIFIER, "user_id"},
            {TokenType::COMMA, ","},
            {TokenType::IDENTIFIER, "p"},
            {TokenType::DOT, "."},
            {TokenType::IDENTIFIER, "name"},
            {TokenType::FROM, "FROM"},
            {TokenType::IDENTIFIER, "users"},
            {TokenType::IDENTIFIER, "u"},
            {TokenType::JOIN, "JOIN"},
            {TokenType::IDENTIFIER, "products"},
            {TokenType::IDENTIFIER, "p"},
            {TokenType::ON, "ON"},
            {TokenType::IDENTIFIER, "u"},
            {TokenType::DOT, "."},
            {TokenType::IDENTIFIER, "id"},
            {TokenType::EQ, "="},
            {TokenType::IDENTIFIER, "p"},
            {TokenType::DOT, "."},
            {TokenType::IDENTIFIER, "user_id"},

            {TokenType::WHERE, "WHERE"},


            {TokenType::IDENTIFIER, "p"},
            {TokenType::DOT, "."},
            {TokenType::IDENTIFIER, "price"},

            {TokenType::LT, "<"},
            {TokenType::INT_LITERAL, "50"},

            {TokenType::SEMICOLON, ";"},
            {TokenType::EOF_FILE, ""}
    };
    assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, InsertStatement) {
    std::string query = "INSERT INTO users VALUES (1, 'Alice');";
    Lexer lexer(query);
    std::vector<Token> tokens = lexer.tokenize();

    std::vector<Token> expected_tokens = {
            {TokenType::INSERT, "INSERT"},
            {TokenType::INTO, "INTO"},
            {TokenType::IDENTIFIER, "users"},
            {TokenType::VALUES, "VALUES"},
            {TokenType::LPAREN, "("},
            {TokenType::INT_LITERAL, "1"},
            {TokenType::COMMA, ","},
            {TokenType::STRING_LITERAL, "Alice"},
            {TokenType::RPAREN, ")"},
            {TokenType::SEMICOLON, ";"},
            {TokenType::EOF_FILE, ""}
    };
    assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, FloatLiteral) {
    std::string query = "SELECT 3.14 FROM products;";
    Lexer lexer(query);
    std::vector<Token> tokens = lexer.tokenize();

    std::vector<Token> expected_tokens = {
            {TokenType::SELECT, "SELECT"},
            {TokenType::FLOAT_LITERAL, "3.14"},
            {TokenType::FROM, "FROM"},
            {TokenType::IDENTIFIER, "products"},
            {TokenType::SEMICOLON, ";"},
            {TokenType::EOF_FILE, ""}
    };

    assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, DateLiteral) {
    std::string query = "SELECT '2025-10-31' FROM events;";
    Lexer lexer(query);
    std::vector<Token> tokens = lexer.tokenize();

    std::vector<Token> expected_tokens = {
            {TokenType::SELECT, "SELECT"},
            {TokenType::DATE_LITERAL, "2025-10-31"},
            {TokenType::FROM, "FROM"},
            {TokenType::IDENTIFIER, "events"},
            {TokenType::SEMICOLON, ";"},
            {TokenType::EOF_FILE, ""}
    };

    assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, TimestampLiteral) {
    std::string query = "SELECT '2025-10-31 12:30:00' FROM events;";
    Lexer lexer(query);
    std::vector<Token> tokens = lexer.tokenize();

    std::vector<Token> expected_tokens = {
            {TokenType::SELECT, "SELECT"},
            {TokenType::TIMESTAMP_LITERAL, "2025-10-31 12:30:00"},
            {TokenType::FROM, "FROM"},
            {TokenType::IDENTIFIER, "events"},
            {TokenType::SEMICOLON, ";"},
            {TokenType::EOF_FILE, ""}
    };

    assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, EmptyInput) {
  std::string query = "";
  Lexer lexer(query);
  std::vector<Token> tokens = lexer.tokenize();

  std::vector<Token> expected_tokens = {
	  {TokenType::EOF_FILE, ""}
  };
  assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, WhitespaceOnly) {
  std::string query = "   \t\n  ";
  Lexer lexer(query);
  std::vector<Token> tokens = lexer.tokenize();

  std::vector<Token> expected_tokens = {
	  {TokenType::EOF_FILE, ""}
  };
  assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, UnterminatedString) {
  // This tests the fix we made for unterminated strings
  std::string query = "SELECT 'dangling";
  Lexer lexer(query);
  std::vector<Token> tokens = lexer.tokenize();

  // Expecting UNKNOWN token for the unterminated string
  // Note: The value depends on your implementation, here assuming "'dangling"
  ASSERT_FALSE(tokens.empty());
  EXPECT_EQ(tokens[0].type, TokenType::SELECT);
  EXPECT_EQ(tokens[1].type, TokenType::UNKNOWN);
  EXPECT_EQ(tokens.back().type, TokenType::EOF_FILE);
}

TEST_F(LexerTest, UnknownCharacter) {
  // '@' is not a valid character in our SQL dialect
  std::string query = "SELECT @ FROM users;";
  Lexer lexer(query);
  std::vector<Token> tokens = lexer.tokenize();

  std::vector<Token> expected_tokens = {
	  {TokenType::SELECT, "SELECT"},
	  {TokenType::UNKNOWN, "@"},
	  {TokenType::FROM, "FROM"},
	  {TokenType::IDENTIFIER, "users"},
	  {TokenType::SEMICOLON, ";"},
	  {TokenType::EOF_FILE, ""}
  };
  assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, ArithmeticOperators) {
  std::string query = "SELECT -5, +10 FROM numbers;";
  Lexer lexer(query);
  std::vector<Token> tokens = lexer.tokenize();

  std::vector<Token> expected_tokens = {
	  {TokenType::SELECT, "SELECT"},
	  {TokenType::MINUS, "-"},
	  {TokenType::INT_LITERAL, "5"},
	  {TokenType::COMMA, ","},
	  {TokenType::PLUS, "+"},
	  {TokenType::INT_LITERAL, "10"},
	  {TokenType::FROM, "FROM"},
	  {TokenType::IDENTIFIER, "numbers"},
	  {TokenType::SEMICOLON, ";"},
	  {TokenType::EOF_FILE, ""}
  };
  assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, CaseInsensitivity) {
  std::string query = "SeLeCt * fRoM Users;";
  Lexer lexer(query);
  std::vector<Token> tokens = lexer.tokenize();

  std::vector<Token> expected_tokens = {
	  {TokenType::SELECT, "SeLeCt"},
	  {TokenType::STAR, "*"},
	  {TokenType::FROM, "fRoM"},
	  {TokenType::IDENTIFIER, "Users"},
	  {TokenType::SEMICOLON, ";"},
	  {TokenType::EOF_FILE, ""}
  };
  assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, BooleanAndNullLiterals) {
  std::string query = "SELECT TRUE, FALSE, NULL;";
  Lexer lexer(query);
  std::vector<Token> tokens = lexer.tokenize();

  std::vector<Token> expected_tokens = {
	  {TokenType::SELECT, "SELECT"},
	  {TokenType::TRUE, "TRUE"},
	  {TokenType::COMMA, ","},
	  {TokenType::FALSE, "FALSE"},
	  {TokenType::COMMA, ","},
	  {TokenType::NULL_LITERAL, "NULL"},
	  {TokenType::SEMICOLON, ";"},
	  {TokenType::EOF_FILE, ""}
  };
  assert_tokens_equal(tokens, expected_tokens);
}

TEST_F(LexerTest, ConstructFromTemporaryString) {
  auto make_query = []() { return std::string("SELECT * FROM users"); };
  Lexer lexer(make_query());
  auto tokens = lexer.tokenize();
  EXPECT_EQ(tokens[0].type, TokenType::SELECT);
  EXPECT_EQ(tokens[2].type, TokenType::FROM);
  EXPECT_EQ(tokens[3].type, TokenType::IDENTIFIER);
  EXPECT_EQ(tokens[3].text, "users");
}

TEST_F(LexerTest, ComplexOperators) {
  std::string query = "WHERE a != b AND c <= d;";
  Lexer lexer(query);
  std::vector<Token> tokens = lexer.tokenize();

  std::vector<Token> expected_tokens = {
	  {TokenType::WHERE, "WHERE"},
	  {TokenType::IDENTIFIER, "a"},
	  {TokenType::NE, "!="},
	  {TokenType::IDENTIFIER, "b"},
	  {TokenType::AND, "AND"},
	  {TokenType::IDENTIFIER, "c"},
	  {TokenType::LTE, "<="},
	  {TokenType::IDENTIFIER, "d"},
	  {TokenType::SEMICOLON, ";"},
	  {TokenType::EOF_FILE, ""}
  };
  assert_tokens_equal(tokens, expected_tokens);
}
