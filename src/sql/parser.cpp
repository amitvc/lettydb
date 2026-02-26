/**
 * @file parser.cpp
 * @brief Implementation of the SQL parser that builds Abstract Syntax Tree for each type SQL query command.
 * 
 * This file implements the Parser class which converts token streams into
 * Abstract Syntax Trees using recursive descent parsing. The parser handles
 * SELECT statements with proper operator precedence for expressions.
 */

#include "parser.h"
#include <sstream>


namespace letty {

    SQLDate parse_date_literal(const std::string& s) {
        SQLDate date;
        std::sscanf(s.c_str(), "%d-%d-%d", &date.year, &date.month, &date.day);
        return date;
    }

    SQLTimestamp parse_timestamp_literal(const std::string& s) {
        SQLTimestamp ts;
        std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &ts.year, &ts.month, &ts.day, &ts.hour, &ts.minute, &ts.second);
        return ts;
    }

    /**
     * @brief Main entry point for parsing the token stream
     * 
     * Examines the first token to determine the statement type and delegates
     * to the appropriate parsing method. Currently only SELECT statements are
     * fully implemented.
     * 
     * @return Root ASTNode representing the parsed SQL statement
     * @throws std::runtime_error for unsupported statement types
     */
    std::unique_ptr<ASTNode> Parser::parse() {

        std::unique_ptr<ASTNode> rootNode;

        switch (peek().type) {
            case TokenType::SELECT:
                return parse_select_node();
            case TokenType::INSERT:
			  	return parse_insert_node();
            case TokenType::DELETE:
                return parse_delete_node();
            case TokenType::UPDATE:
                return parse_update_node();
                break;
            case TokenType::DROP:
                return parse_drop_node();
            case TokenType::CREATE:
                return parse_create_node();
            default:
                throw std::runtime_error("Unsupported statement type: " + peek().text);
        }
        return nullptr; // Should not reach here

    }

    /**
     * @brief Parses a complete UPDATE statement.
     * 
     * Syntax:
     * UPDATE table_name SET col1 = val1, col2 = val2 [WHERE condition]
     */
    std::unique_ptr<ASTNode> Parser::parse_update_node() {
        auto rootNode = std::make_unique<UpdateStatementNode>();
        ensure(TokenType::UPDATE, "Expected 'UPDATE' keyword");
        
        auto tableToken = ensure(TokenType::IDENTIFIER, "Expected table name");
        rootNode->table_name = std::make_unique<IdentifierNode>(tableToken.text);

        ensure(TokenType::SET, "Expected 'SET' keyword");

        do {
            if (match(TokenType::COMMA)) {
                advance();
            }

            UpdateStatementNode::UpdateSet updateSet;
            auto colToken = ensure(TokenType::IDENTIFIER, "Expected column name");
            updateSet.column = std::make_unique<IdentifierNode>(colToken.text);

            ensure(TokenType::EQ, "Expected '=' after column name");

            updateSet.value = parse_logical_expression();
            rootNode->updates.push_back(std::move(updateSet));

        } while(match(TokenType::COMMA));

        if (match(TokenType::WHERE)) {
            advance();
            rootNode->where_clause = parse_logical_expression();
        }

        return rootNode;
    }

    /**
     * @brief Parses a complete SELECT statement with all clauses
     * 
     * Handles the full SELECT statement syntax:
     * SELECT { * | column_list } FROM table_ref [JOIN table_ref ON condition] [WHERE condition]
     * 
     * @return SelectStatementNode containing all parsed components
     */
    std::unique_ptr<ASTNode> Parser::parse_select_node() {
        auto rootNode = std::make_unique<SelectStatementNode>();
        advance(); // Advance past the first token

        // The second token is either * or list of columns.
        if (match(TokenType::STAR)) {
            advance(); // move to next token
            rootNode->is_select_all = true;
        } else {
            rootNode->columns = parse_columns_collection();
        }

        ensure(TokenType::FROM, "Expected identifier From.");

        rootNode->from_clause = parse_from_table_ref();

        // We will support only single join in the first implementation
        if (match(TokenType::JOIN)) {
            SelectStatementNode::JoinClause join_clause;
            advance();
            join_clause.table = parse_from_table_ref();
            ensure(TokenType::ON, "Expected 'ON' keyword for JOIN clause.");
            join_clause.on_condition = parse_logical_expression();
            rootNode->join_clause.push_back(std::move(join_clause));
        }

        // Parse optional WHERE clause
        if (match(TokenType::WHERE)) {
            advance(); // Consume WHERE token
            rootNode->where_clause = parse_logical_expression();
        }

        if (match(TokenType::GROUP)) {
            advance();
            ensure(TokenType::BY, "Expected 'By' keyword after GROUP. Instead found "+ tokens[pos].text);
            rootNode->group_by = parse_group_by_clause();
        }
        return rootNode;
    }

	std::unique_ptr<ASTNode> Parser::parse_insert_node() {
	    auto rootNode = std::make_unique<InsertStatementNode>();
	  	ensure(TokenType::INSERT,  "Expected 'INSERT' keyword.");
	  	ensure(TokenType::INTO, "Expected INTO keyword ");
	  	const Token& tableNameToken = ensure(TokenType::IDENTIFIER, "Expected Identifier for table name");
	  	rootNode->tableName = std::make_unique<IdentifierNode>(tableNameToken.text);

		// If we find column names
		if (match(TokenType::LPAREN)) {
		  advance();
		  rootNode->columnNames = parse_identifier_list();
		  ensure(TokenType::RPAREN, "Expecting )");
		} else if (match(TokenType::IDENTIFIER)) {
		  // Error: identifier found without opening parenthesis
		  throw std::runtime_error("Expected '(' before column list or VALUES keyword");
		}

		if (match(TokenType::VALUES)) {
			this->advance();
			do {
			  if (match(TokenType::COMMA)) {
				advance();
			  }
			  rootNode->values.push_back(parse_value_list());
			} while (match(TokenType::COMMA));
		}
		return rootNode;
	}

std::vector<std::unique_ptr<LiteralNode>> Parser::parse_value_list() {
		ensure(TokenType::LPAREN, "Expected ( ");
		std::vector<std::unique_ptr<LiteralNode>> values;
		
		do {
			if (match(TokenType::COMMA)) {
				advance();
			}
			switch (peek().type) {
				case TokenType::INT_LITERAL:
					values.push_back(std::make_unique<LiteralNode>(std::stoll(peek().text)));
					break;
				case TokenType::FLOAT_LITERAL:
					values.push_back(std::make_unique<LiteralNode>(std::stod(peek().text)));
					break;
				case TokenType::DATE_LITERAL:
					values.push_back(std::make_unique<LiteralNode>(parse_date_literal(peek().text)));
					break;
				case TokenType::TIMESTAMP_LITERAL:
					values.push_back(std::make_unique<LiteralNode>(parse_timestamp_literal(peek().text)));
					break;
				case TokenType::STRING_LITERAL:
					values.push_back(std::make_unique<LiteralNode>(peek().text));
					break;
				case TokenType::TRUE:
					values.push_back(std::make_unique<LiteralNode>(true));
					break;
				case TokenType::FALSE:
					values.push_back(std::make_unique<LiteralNode>(false));
					break;
			}
			advance();
		} while (match(TokenType::COMMA));
		
		ensure(TokenType::RPAREN, "Expected )");
		return values;
	}

std::unique_ptr<ASTNode> Parser::parse_create_node() {
        ensure(TokenType::CREATE,  "Expected 'CREATE' keyword.");
        if (match(TokenType::TABLE)) {
            return parse_create_table_node();
        } else if (match(TokenType::INDEX)) {
            return parse_create_index_node();
        }
        throw std::runtime_error("Expected TABLE or INDEX after CREATE");
    }

	std::unique_ptr<ASTNode> Parser::parse_create_table_node() {
	  auto rootNode = std::make_unique<CreateTableStatementNode>();
	  ensure(TokenType::TABLE, "Expected token Table ");
	  auto tableName = ensure(TokenType::IDENTIFIER, "Expected table name");
	  rootNode->table_name = std::make_unique<IdentifierNode>(tableName.text);

	  ensure(TokenType::LPAREN, "Expected token ( ");

	  // Parse column definitions and constraints
	  do {

		if (match(TokenType::COMMA)) {
		  advance();
		}

		if (match(TokenType::IDENTIFIER)) {
		  auto columnDef = std::make_unique<CreateTableStatementNode::ColumnDefinition>();

		  // Parse column name
		  auto columnName = ensure(TokenType::IDENTIFIER, "Expected column name");
		  columnDef->name = std::make_unique<IdentifierNode>(columnName.text);

		  if (match(TokenType::INT)) {
			columnDef->type = TokenType::INT;
			advance();
		  } else if (match(TokenType::BOOL)) {
			columnDef->type = TokenType::BOOL;
			advance();
		  } else if (match(TokenType::FLOAT)) {
			columnDef->type = TokenType::FLOAT;
			advance();
		  } else if (match(TokenType::DATE)) {
			columnDef->type = TokenType::DATE;
			advance();
		  } else if (match(TokenType::TIMESTAMP)) {
			columnDef->type = TokenType::TIMESTAMP;
			advance();
		  } else if (match(TokenType::VARCHAR)) {
			columnDef->type = TokenType::VARCHAR;
			advance();
			if (match(TokenType::LPAREN)) {
			  advance(); // consume '('
			  auto sizeToken = ensure(TokenType::INT_LITERAL, "Expected size for VARCHAR");
			  columnDef->size = std::stoi(sizeToken.text);
			  ensure(TokenType::RPAREN, "Expected ')' after VARCHAR size");
			}
		  } else {
			throw std::runtime_error("Unexpected Column type specified. Found " + peek().text);
		  }

		  if (match(TokenType::PRIMARY)) {
			advance();
			ensure(TokenType::KEY, "Expected token KEY");
			rootNode->primary_key_columns.push_back(std::make_unique<IdentifierNode>(columnName.text));
		  }
		  rootNode->columns.push_back(std::move(columnDef));
		}

		if (match(TokenType::PRIMARY)) {
		  advance();
		  ensure(TokenType::KEY, "Expected token KEY");
		  ensure(TokenType::LPAREN, "Expected token (");
		  do {
			if (match(TokenType::COMMA)) {
			  advance();
			}
			if (match(TokenType::IDENTIFIER)) {
			  rootNode->primary_key_columns.push_back(std::make_unique<IdentifierNode>(tokens[pos].text));
			  advance();
			}
		  } while (match(TokenType::COMMA));
		  ensure(TokenType::RPAREN, "Expected ')' after primary key columns");
		  continue;

		}
	  } while (match(TokenType::COMMA));

	  ensure(TokenType::RPAREN, "Expected token ) ");

	  return rootNode;
	}

    std::unique_ptr<ASTNode> Parser::parse_create_index_node() {
        auto rootNode = std::make_unique<CreateIndexStatementNode>();
        ensure(TokenType::INDEX, "Expected 'INDEX' keyword");
        
        auto indexToken = ensure(TokenType::IDENTIFIER, "Expected index name");
        rootNode->index_name = std::make_unique<IdentifierNode>(indexToken.text);

        ensure(TokenType::ON, "Expected 'ON' keyword");

        auto tableToken = ensure(TokenType::IDENTIFIER, "Expected table name");
        rootNode->table_name = std::make_unique<IdentifierNode>(tableToken.text);

        ensure(TokenType::LPAREN, "Expected '(' before column list");
        
        rootNode->columns = parse_identifier_list();

        ensure(TokenType::RPAREN, "Expected ')' after column list");

        return rootNode;
    }


    /**
     * @brief Parses a complete DELETE statement.
     * Example: DELETE FROM table_name [WHERE condition]
     */
    std::unique_ptr<ASTNode> Parser::parse_delete_node() {
        auto rootNode = std::make_unique<DeleteStatementNode>();
        
        ensure(TokenType::DELETE, "Expected 'DELETE' keyword");
        ensure(TokenType::FROM, "Expected 'FROM' keyword");
        
        const auto& tableToken = ensure(TokenType::IDENTIFIER, "Expected table name");
        rootNode->table_name = std::make_unique<IdentifierNode>(tableToken.text);

        if (match(TokenType::WHERE)) {
            advance();
            rootNode->where_clause = parse_logical_expression();
        }

        return rootNode;
    }


/**
     * @brief Parses a complete DROP statement.
     * Handles the full DROP statement syntax:
     * DROP IF EXISTS table list
     * @return DropTableStatementNode containing all parsed components
     */
    std::unique_ptr<ASTNode> Parser::parse_drop_node() {
        auto rootNode = std::make_unique<DropTableStatementNode>();
        advance(); // Advance past the drop token
        // Confirm drop is followed by TABLE
        ensure(TokenType::TABLE, "Expected 'TABLE' keyword after DROP. Instead found " + peek().text);
        if (match(TokenType::IF)) {
            advance();
            ensure(TokenType::EXISTS, "Expected 'Exists' keyword after IF.");
            rootNode->if_exists = true;
            rootNode->table_names = parse_identifier_list();
        } else {
            rootNode-> table_names = parse_identifier_list();
        }
        return rootNode;
    }

    std::vector<std::unique_ptr<IdentifierNode>> Parser::parse_identifier_list() {
        std::vector<std::unique_ptr<IdentifierNode>> identifiers;
        do {
            if (peek().type == TokenType::COMMA) {
                advance();
            }
            identifiers.push_back(
                    std::make_unique<IdentifierNode>(ensure(TokenType::IDENTIFIER, "Expected table name.").text)
            );
        } while (match(TokenType::COMMA));
        return identifiers;
    }


    std::unique_ptr<SelectStatementNode::GroupByClause> Parser::parse_group_by_clause() {
        auto clause = std::make_unique<SelectStatementNode::GroupByClause>();
        clause->expressions = parse_expression_list();
        if (match(TokenType::HAVING)) {
            advance();
            clause->having_clause = parse_logical_expression();
        }
        return clause;
    }

    std::vector<std::unique_ptr<ExpressionNode>> Parser::parse_expression_list() {
        std::vector<std::unique_ptr<ExpressionNode>> expressions;
        expressions.push_back(parse_logical_expression());
        
        while (match(TokenType::COMMA)) {
            advance(); // consume comma
            expressions.push_back(parse_logical_expression());
        }
        return expressions;
    }

    /**
     * @brief Checks whether the current token matches the specified type
     * 
     * This is a non-consuming peek operation that checks if the current token
     * matches the given type without advancing the parser position.
     * 
     * @param type TokenType to check against current token
     * @return true if current token matches type and we're not at end, false otherwise
     */
    bool Parser::match(TokenType type) {
        return !(is_at_end() || peek().type != type);
    }

    /**
     * @brief Parses a comma-separated list of column expressions with optional aliases
     * 
     * Handles various column formats:
     * - Simple identifiers: column_name
     * - Qualified identifiers: table.column_name
     * - Aliases: column_name AS alias_name or column_name alias_name
     * 
     * @return Vector of SelectColumn structures containing expressions and aliases
     */
    std::vector<SelectStatementNode::SelectColumn> Parser::parse_columns_collection() {
        // Select col, col2, col2
        // Select t.col as xx, t.col2,
        std::vector<SelectStatementNode::SelectColumn> columns;
        do {
            if (peek().type == TokenType::COMMA) {
                advance();
            }

            SelectStatementNode::SelectColumn curr_column;
            curr_column.expression = extract_column();

            if (match(TokenType::AS)) {
                advance();
                curr_column.alias = ensure(TokenType::IDENTIFIER, "Expected alias name.").text;
            }
            columns.push_back(std::move(curr_column));
        } while (match(TokenType::COMMA));
        return columns;
    }

    /**
     * @brief Validates and consumes a token of the expected type
     * 
     * This method enforces that the current token matches the expected type.
     * If it matches, the token is consumed and returned. If not, a descriptive
     * error is thrown.
     * 
     * @param type The expected TokenType
     * @param message Custom error message to include in exception
     * @return Reference to the consumed token
     * @throws std::runtime_error if token type doesn't match expectation
     */
    const Token &Parser::ensure(TokenType type, const std::string &message) {
        if (peek().type == type) return advance();
        throw std::runtime_error(message + " Got token with text: " + peek().text);
    }

    /**
     * @brief Parses column references including qualified identifiers
     * 
     * Handles both simple identifiers (column_name) and qualified identifiers
     * (table.column_name). Creates appropriate AST nodes for each case.
     * 
     * @return IdentifierNode for simple names or QualifiedIdentifierNode for table.column
     * @throws std::runtime_error if current token is not an identifier
     */
    std::unique_ptr<ExpressionNode> Parser::extract_column() {
        if (!match(TokenType::IDENTIFIER)) {
            throw std::runtime_error("Expected identifier instead found " + peek().text);
        } else {
            std::string name = advance().text;
            if (match(TokenType::DOT)) {
                auto qualifier = std::make_unique<IdentifierNode>(name);
                advance();
                auto member_name = ensure(TokenType::IDENTIFIER, "Expected column name after '.'").text;
                auto member = std::make_unique<IdentifierNode>(member_name);
                return std::make_unique<QualifiedIdentifierNode>(std::move(qualifier), std::move(member));
            } else {
                return std::make_unique<IdentifierNode>(name);
            }
        }
    }

    /**
     * @brief Parses a table reference with optional alias in FROM or JOIN clauses
     * 
     * Handles various table reference formats:
     * - Simple table name: table_name
     * - Explicit alias: table_name AS alias_name
     * - Implicit alias: table_name alias_name
     * 
     * @return TableReference structure with table name and optional alias
     */
    std::unique_ptr<SelectStatementNode::TableReference> Parser::parse_from_table_ref() {
        auto table_ref = std::make_unique<SelectStatementNode::TableReference>();
        table_ref->name = std::make_unique<IdentifierNode>(ensure(TokenType::IDENTIFIER, "Expected table name.").text);


        if (match(TokenType::AS)) {
            advance(); // Consume the AS token
            table_ref->alias = ensure(TokenType::IDENTIFIER, "Expected alias for table.").text;
        } else if (peek().type == TokenType::IDENTIFIER) {
            table_ref->alias = advance().text;
        }
        return table_ref;
    }

    /**
     * @brief Entry point for expression parsing with full operator precedence
     * 
     * This method serves as the top-level entry point for parsing expressions.
     * It delegates to parse_or_expression() which implements the lowest precedence
     * level in the operator precedence hierarchy.
     * 
     * Operator precedence (highest to lowest):
     * 1. Comparison operators (=, !=, <, >, <=, >=)
     * 2. AND
     * 3. OR
     * 
     * @return ExpressionNode representing the complete parsed expression
     */
    std::unique_ptr<ExpressionNode> Parser::parse_logical_expression() {
        auto left = parse_and_expression();

        while (match(TokenType::OR)) {
            advance();
            std::string op = tokens[pos - 1].text;
            auto right = parse_and_expression();
            left = std::make_unique<BinaryOperationNode>(std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<ExpressionNode> Parser::parse_and_expression() {
        auto left = parse_relational_expression();

        while (match(TokenType::AND)) {
            advance();
            std::string op = tokens[pos - 1].text;
            auto right = parse_relational_expression();
            left = std::make_unique<BinaryOperationNode>(std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<ExpressionNode> Parser::parse_relational_expression() {
        auto left = parse_value_or_identifier();

        while (peek().type == TokenType::EQ || peek().type == TokenType::NE ||
               peek().type == TokenType::LT || peek().type == TokenType::LTE ||
               peek().type == TokenType::GT || peek().type == TokenType::GTE) {
            std::string op = advance().text;
            auto right = parse_value_or_identifier();
            left = std::make_unique<BinaryOperationNode>(std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<ExpressionNode> Parser::parse_value_or_identifier() {
        if (match(TokenType::INT_LITERAL)) {
            advance();
            int64_t val = std::stoll(tokens[pos - 1].text);
            return std::make_unique<LiteralNode>(val);
        }
        if (match(TokenType::FLOAT_LITERAL)) {
            advance();
            double val = std::stod(tokens[pos - 1].text);
            return std::make_unique<LiteralNode>(val);
        }
        if (match(TokenType::DATE_LITERAL)) {
            advance();
            return std::make_unique<LiteralNode>(parse_date_literal(tokens[pos - 1].text));
        }
        if (match(TokenType::TIMESTAMP_LITERAL)) {
            advance();
            return std::make_unique<LiteralNode>(parse_timestamp_literal(tokens[pos - 1].text));
        }
        if (match(TokenType::STRING_LITERAL)) {
            advance();
            return std::make_unique<LiteralNode>(tokens[pos - 1].text);
        }

        if (match(TokenType::IDENTIFIER)) {
            advance();
            std::string name = tokens[pos - 1].text;
            if (match(TokenType::DOT)) {
                advance();
                auto qualifier = std::make_unique<IdentifierNode>(name);
                auto member_name = ensure(TokenType::IDENTIFIER, "Expected column name after '.'").text;
                auto member = std::make_unique<IdentifierNode>(member_name);
                return std::make_unique<QualifiedIdentifierNode>(std::move(qualifier), std::move(member));
            }
            return std::make_unique<IdentifierNode>(name);
        }

        if (match(TokenType::LPAREN)) {
            advance();
            auto expr = parse_logical_expression();
            ensure(TokenType::RPAREN, "Expected ')' after expression.");
            return expr;
        }

        throw std::runtime_error("Unexpected token in expression: " + peek().text);
    }


    /**
     * @brief Consumes the current token and advances the parser position
     * 
     * This method moves the parser to the next token in the stream and returns
     * a reference to the token that was consumed. It includes bounds checking
     * to prevent advancing past the end of the token stream.
     * 
     * @return Reference to the consumed token
     * @throws std::out_of_range if attempting to advance past end of tokens
     */
    Token &Parser::advance() {
        if (!is_at_end()) {
            return tokens[pos++];
        }
        throw std::out_of_range("Cannot advance past the end of tokens.");
    }
}

