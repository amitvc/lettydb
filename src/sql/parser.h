#pragma once
#include <string>
#include "lexer.h"
#include "token.h"
#include "ast.h"

namespace letty {

    /**
     * @class Parser
     * @brief SQL parser that builds Abstract Syntax Trees from token streams
     * 
     * The Parser class implements a recursive descent parser that converts
     * a sequence of tokens (produced by the Lexer) into an Abstract Syntax Tree (AST).
     * It supports parsing SELECT statements with JOIN, WHERE, and expression clauses.
     * 
     * @par Supported SQL Statements:
     * - SELECT with column lists or SELECT *
     * - FROM clause with table references and aliases
     * - JOIN clause with ON conditions
     * - WHERE clause with complex boolean expressions
     * 
     * @par Expression Parsing:
     * Uses operator precedence parsing for logical expressions:
     * - OR expressions (lowest precedence)
     * - AND expressions (higher precedence)
     * - Comparison expressions (=, !=, <, >, <=, >=)
     * 
     * @par Error Handling:
     * - Throws std::runtime_error for syntax errors
     * - Provides descriptive error messages with context
     * - Uses ensure() method for expected token validation
     * 
     * @par Usage Example:
     * @code
     * std::vector<Token> tokens = lexer.tokenize();
     * Parser parser(std::move(tokens));
     * auto ast = parser.parse();
     * @endcode
     */
    class Parser {
        public:
            /**
             * @brief Constructs a Parser with a vector of tokens
             * @param tokens Vector of tokens to parse (typically from Lexer::tokenize())
             */
            explicit Parser(std::vector<Token> tokens) : tokens(std::move(tokens)), pos(0) {
            }

            /**
             * @brief Parses the token stream and returns the root AST node
             * @return Unique pointer to the root ASTNode representing the parsed statement
             * @throws std::runtime_error for unsupported statement types or syntax errors
             */
            std::unique_ptr<ASTNode> parse();

        private:
            std::vector<Token> tokens;  ///< Token stream to parse
            int pos;                   ///< Current position in the token stream

            /**
             * @brief Consumes and returns the current token, advancing the position
             * @return Reference to the consumed token
             * @throws std::out_of_range if attempting to advance past end of tokens
             */
            Token& advance();

            /**
             * @brief Returns the current token without consuming it
             * @return Reference to the current token
             */
            inline Token& peek() {
                return tokens[pos];
            }

            /**
             * @brief Checks if we've reached the end of the token stream
             * @return true if at end of tokens, false otherwise
             */
            inline bool is_at_end() {
                return pos >= tokens.size();
            }

            /**
             * @brief Validates that the current token matches expected type and consumes it
             * @param type Expected TokenType
             * @param message Error message if token doesn't match
             * @return Reference to the consumed token
             * @throws std::runtime_error if token type doesn't match
             */
            const Token& ensure(TokenType type, const std::string& message);
            
            /**
             * @brief Parses column references (identifiers or qualified identifiers)
             * @return ExpressionNode representing the column (IdentifierNode or QualifiedIdentifierNode)
             */
            std::unique_ptr<ExpressionNode> extract_column();

            // Operations for constructing Select AST Node
            /**
             * @brief Parses a complete SELECT statement
             * @return SelectStatementNode with all parsed clauses
             */
            std::unique_ptr<ASTNode> parse_select_node();
            
            /**
             * @brief Parses the column list in a SELECT statement
             * @return Vector of SelectColumn structures with expressions and optional aliases
             */
            std::vector<SelectStatementNode::SelectColumn> parse_columns_collection();
            
            /**
             * @brief Parses a table reference with optional alias
             * @return TableReference structure with table name and optional alias
             */
            std::unique_ptr<SelectStatementNode::TableReference> parse_from_table_ref();


            std::unique_ptr<SelectStatementNode::GroupByClause> parse_group_by_clause();

            std::vector<std::unique_ptr<IdentifierNode>> parse_identifier_list();

            /**
             * @brief Entry point for expression parsing with full operator precedence
             * @return ExpressionNode representing the parsed expression
             */
            std::unique_ptr<ExpressionNode> parse_logical_expression();
            std::unique_ptr<ExpressionNode> parse_and_expression();
            std::unique_ptr<ExpressionNode> parse_value_or_identifier();
            std::unique_ptr<ExpressionNode> parse_relational_expression();
            std::vector<std::unique_ptr<ExpressionNode>> parse_expression_list();

            /**
             * @brief Parses a comma-separated list of literal values enclosed in parentheses
             * @return Vector of LiteralNode pointers representing the parsed values
             */
            std::vector<std::unique_ptr<LiteralNode>> parse_value_list();

            // Future statement parsers (not yet implemented)
            std::unique_ptr<ASTNode> parse_insert_node();
            std::unique_ptr<ASTNode> parse_delete_node();   ///< TODO: Parse DELETE statements
            std::unique_ptr<ASTNode> parse_update_node();   ///< TODO: Parse UPDATE statements
            std::unique_ptr<ASTNode> parse_create_node();
            std::unique_ptr<ASTNode> parse_create_table_node();
            std::unique_ptr<ASTNode> parse_create_index_node();
            std::unique_ptr<ASTNode> parse_drop_node();

            /**
             * @brief Checks if current token matches the given type without consuming it
             * @param type TokenType to match against
             * @return true if current token matches type and we're not at end, false otherwise
             */
            bool match(TokenType type);

    };
}


