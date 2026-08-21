#pragma once

#include <variant>
#include <string>
#include <vector>
#include <memory>
#include "token.h"

namespace letty {

    struct SQLDate {
        int year, month, day;
    };

    struct SQLTimestamp {
        int year, month, day, hour, minute, second;
    };

/**
 * @class ASTNode
 * @brief The base class for all nodes in the Abstract Syntax Tree.
 *
 * This class provides a common interface for all parts of a parsed SQL query.
 * The virtual destructor is crucial for ensuring that derived-class objects
 * are properly destroyed when deleted through a base-class pointer.
 */
    class ASTNode {
    public:
        virtual ~ASTNode() = default;
    };

/**
 * @class ExpressionNode
 * @brief Base class for all expression nodes in the AST.
 *
 * An expression is anything that can be evaluated to a value, such as a literal,
 * a column name, or a binary operation like 'price > 100'.
 */
  class ExpressionNode : public ASTNode {
  };

/**
 * @class LiteralNode
 * @brief Represents a literal value, like a number, string, bool, date ot timestamp using std::variant for type safety.
 */
    class LiteralNode : public ExpressionNode {
    public:
        template<typename T>
        explicit LiteralNode(T val) : value(std::move(val)) {}

        std::variant<std::monostate, int64_t, double, std::string, bool, SQLDate, SQLTimestamp> value;

    };

/**
 * @class IdentifierNode
 * @brief Represents an identifier, such as a table or column name.
 */
    class IdentifierNode : public ExpressionNode {
    public:
        std::string name;

        explicit IdentifierNode(std::string name) : name(std::move(name)) {}
    };

/**
* @class BinaryOperationNode
* @brief Represents an operation with two operands, like 'a + b' or 'c > d'.
*/
    class BinaryOperationNode : public ExpressionNode {
    public:
        std::unique_ptr<ExpressionNode> left;
        std::string op; // e.g., "=", ">", "AND"
        std::unique_ptr<ExpressionNode> right;

        BinaryOperationNode(std::unique_ptr<ExpressionNode> left, std::string op, std::unique_ptr<ExpressionNode> right)
                : left(std::move(left)), op(std::move(op)), right(std::move(right)) {}
    };

/**
 * @class QualifiedIdentifierNode
 * @brief Represents a qualified name, such as 'table.column' or 'alias.column'.
 */
    class QualifiedIdentifierNode : public ExpressionNode {
    public:
        std::unique_ptr<IdentifierNode> qualifier; // The left part (e.g., 'table' or 'alias')
        std::unique_ptr<IdentifierNode> name;      // The right part (e.g., 'column')

        QualifiedIdentifierNode(std::unique_ptr<IdentifierNode> qualifier, std::unique_ptr<IdentifierNode> name)
                : qualifier(std::move(qualifier)), name(std::move(name)) {}
    };

    /**
     * @class IsNullExpressionNode
     * @brief Represents an IS [NOT] NULL expression, such as 'col IS NULL' or 'col IS NOT NULL'.
     */
    class IsNullExpressionNode : public ExpressionNode {
    public:
        std::unique_ptr<ExpressionNode> expression;
        bool is_not; // true for IS NOT NULL, false for IS NULL

        explicit IsNullExpressionNode(std::unique_ptr<ExpressionNode> expression, bool is_not = false)
                : expression(std::move(expression)), is_not(is_not) {}
    };

    /**
     * @class SelectStatementNode
     * @brief Represents a full SELECT statement. This is a top-level AST node.
     */
    class SelectStatementNode final : public ASTNode {
    public:
        // Represents a single column in the SELECT list, which can have an optional alias.
        struct SelectColumn {
            std::unique_ptr<ExpressionNode> expression;
            std::string alias; // Empty if no alias
        };

        // Represents a single table reference, e.g., "users" or "users u"
        struct TableReference {
            std::unique_ptr<IdentifierNode> name;
            std::string alias;
        };

        // Represents a JOIN clause, e.g., "JOIN products p ON u.id = p.user_id"
        struct JoinClause {
            std::unique_ptr<TableReference> table;
            std::unique_ptr<ExpressionNode> on_condition;
        };

        struct OrderByClause {
            std::unique_ptr<ExpressionNode> expression;
            bool is_ascending = true;
        };

        struct GroupByClause {
            std::vector<std::unique_ptr<ExpressionNode>> expressions;
            std::unique_ptr<ExpressionNode> having_clause;  // Optional HAVING
        };


        bool is_select_all = false;
        std::vector<SelectColumn> columns;
        std::unique_ptr<TableReference> from_clause;
        std::vector<JoinClause> join_clause;
        std::unique_ptr<ExpressionNode> where_clause; // Can be nullptr if no WHERE clause
        // GROUP BY
        std::unique_ptr<GroupByClause> group_by;

        // ORDER BY
        std::vector<OrderByClause> order_by;
    };

	/**
	 * @class DropTableStatementNode
	 * @brief Represents Drop table statement AST node.
	 */
    class DropTableStatementNode final : public ASTNode {
    public:
        bool if_exists = false;
        // This holds the table names
        std::vector<std::unique_ptr<IdentifierNode>> table_names;
    };

	/**
	 * @class  InsertStatementNode
	 * @brief Represents Insert  statement AST node.
	 */
	class InsertStatementNode final : public ASTNode {
	 public:
	  	std::unique_ptr<IdentifierNode> tableName;
		// This is optional
		std::vector<std::unique_ptr<IdentifierNode>> columnNames;

		// Keep list -> list [values]
		std::vector<std::vector<std::unique_ptr<LiteralNode>>> values;
	};

    /**
     * @class DeleteStatementNode
     * @brief Represents a DELETE statement.
     */
    class DeleteStatementNode final : public ASTNode {
    public:
        std::unique_ptr<IdentifierNode> table_name;
        std::unique_ptr<ExpressionNode> where_clause; // Optional
    };

    /**
     * @class UpdateStatementNode
     * @brief Represents an UPDATE statement.
     */
    class UpdateStatementNode final : public ASTNode {
    public:
        struct UpdateSet {
            std::unique_ptr<IdentifierNode> column;
            std::unique_ptr<ExpressionNode> value;
        };

        std::unique_ptr<IdentifierNode> table_name;
        std::vector<UpdateSet> updates;
        std::unique_ptr<ExpressionNode> where_clause; // Optional
    };

    /**
     * @class CompactStatementNode
     * @brief Represents a COMPACT TABLE statement.
     */
    class CompactStatementNode final : public ASTNode {
    public:
        std::string table_name;
        explicit CompactStatementNode(std::string table_name) : table_name(std::move(table_name)) {}
    };

    /**
     * @class CreateTableStatementNode
     * @brief Represents a full CREATE TABLE statement. This is a top-level AST node.
     *
     */
    class CreateTableStatementNode final : public ASTNode {
    public:

        /**
         * @struct ColumnDefinition
         * @brief Represents the definition of a single column in a table.
         */
        struct ColumnDefinition {
            std::unique_ptr<IdentifierNode> column_name;
            TokenType data_type; // e.g., TokenType::INT, TokenType::VARCHAR or TokenType::BOOL
            int size = 0; // For VARCHAR data type.
            bool nullable = true; // Default true: column is nullable unless NOT NULL is specified.
        };

        std::vector<std::unique_ptr<ColumnDefinition>> columns;
        std::unique_ptr<IdentifierNode> table_name;
        std::vector<std::unique_ptr<IdentifierNode>> primary_key_columns; // Supports multi-column primary key.
    };

    /**
     * @class CreateIndexStatementNode
     * @brief Represents a CREATE INDEX statement.
     */
    class CreateIndexStatementNode final : public ASTNode {
    public:
        std::unique_ptr<IdentifierNode> index_name;
        std::unique_ptr<IdentifierNode> table_name;
        std::vector<std::unique_ptr<IdentifierNode>> columns;
    };
}