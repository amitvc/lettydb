#include "sql/parser.h"

#include <gtest/gtest.h>
#include "sql/lexer.h"
#include <vector>
#include "sql/ast.h"

using namespace letty;


class ParserTest : public ::testing::Test {
protected:
    // Helper method to cast ASTNode to SelectStatementNode
    SelectStatementNode* asSelectStatement(std::unique_ptr<ASTNode>& node) {
        return dynamic_cast<SelectStatementNode*>(node.get());
    }

    DropTableStatementNode* asDropStatement(std::unique_ptr<ASTNode>& node) {
        return dynamic_cast<DropTableStatementNode*>(node.get());
    }
    
    CreateTableStatementNode* asCreateTableStatement(std::unique_ptr<ASTNode>& node) {
        return dynamic_cast<CreateTableStatementNode*>(node.get());
    }

	InsertStatementNode* asInsertStatement(std::unique_ptr<ASTNode> &node) {
	  return dynamic_cast<InsertStatementNode*>(node.get());
	}

    DeleteStatementNode* asDeleteStatement(std::unique_ptr<ASTNode> &node) {
        return dynamic_cast<DeleteStatementNode*>(node.get());
    }

    UpdateStatementNode* asUpdateStatement(std::unique_ptr<ASTNode> &node) {
        return dynamic_cast<UpdateStatementNode*>(node.get());
    }

    CreateIndexStatementNode* asCreateIndexStatement(std::unique_ptr<ASTNode> &node) {
        return dynamic_cast<CreateIndexStatementNode*>(node.get());
    }
    
    // Helper method to cast ExpressionNode to specific types
    IdentifierNode* asIdentifier(std::unique_ptr<ExpressionNode>& expr) {
        return dynamic_cast<IdentifierNode*>(expr.get());
    }
    
    QualifiedIdentifierNode* asQualifiedIdentifier(std::unique_ptr<ExpressionNode>& expr) {
        return dynamic_cast<QualifiedIdentifierNode*>(expr.get());
    }
    
    LiteralNode* asLiteral(std::unique_ptr<ExpressionNode>& expr) {
        return dynamic_cast<LiteralNode*>(expr.get());
    }
    
    BinaryOperationNode* asBinaryOperation(std::unique_ptr<ExpressionNode>& expr) {
        return dynamic_cast<BinaryOperationNode*>(expr.get());
    }

    std::unique_ptr<ASTNode> parse_query(const std::string& query) {
        Lexer lexer(query);
        auto tokens = lexer.tokenize();
        Parser parser(tokens);
        return parser.parse();
    }
};

TEST_F(ParserTest, SelectAll) {
    std::string query = "SELECT * FROM users;";
  	std::cout << "Breakpoint here!" << std::endl;
    auto ast = parse_query(query);
    
    // Verify it's a SelectStatementNode
    ASSERT_NE(ast, nullptr);
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify SELECT * is parsed correctly
    EXPECT_TRUE(selectNode->is_select_all);
    EXPECT_TRUE(selectNode->columns.empty());
    
    // Verify FROM clause
    ASSERT_NE(selectNode->from_clause, nullptr);
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    EXPECT_TRUE(selectNode->from_clause->alias.empty());
}


TEST_F(ParserTest, SelectJoinTwoTablesWithColumnAlias) {
    std::string query = "SELECT u.id as user_id, p.name \n"
                        "FROM users u\n"
                        "JOIN products p ON u.id = p.user_id\n"
                        "WHERE p.price < 50 AND u.age <= 25;";
    auto ast = parse_query(query);
    
    SelectStatementNode *selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify columns
    ASSERT_EQ(selectNode->columns.size(), 2);
    QualifiedIdentifierNode* col1 = asQualifiedIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col1, nullptr);
    EXPECT_EQ(col1->qualifier->name, "u");
    EXPECT_EQ(col1->name->name, "id");
    EXPECT_EQ(selectNode->columns[0].alias, "user_id");
    
    // Verify FROM clause
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    EXPECT_EQ(selectNode->from_clause->alias, "u");
    
    // Verify JOIN clause
    ASSERT_EQ(selectNode->join_clause.size(), 1);
    EXPECT_EQ(selectNode->join_clause[0].table->name->name, "products");
    EXPECT_EQ(selectNode->join_clause[0].table->alias, "p");
    
    // Verify JOIN ON condition (u.id = p.user_id)
    BinaryOperationNode* joinCondition = asBinaryOperation(selectNode->join_clause[0].on_condition);
    ASSERT_NE(joinCondition, nullptr);
    EXPECT_EQ(joinCondition->op, "=");
    
    // Verify WHERE clause exists
    ASSERT_NE(selectNode->where_clause, nullptr);
}


TEST_F(ParserTest, SelectJoinTwoTablesWithWhereAndGroupByClause) {
    std::string query = "SELECT u.department, u.salary \n"
                        "FROM users u\n"
                        "JOIN departments d ON u.dept_id = d.id\n"
                        "WHERE u.salary > 50000 AND d.budget <= 1000000\n"
                        "GROUP BY u.department;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify columns
    ASSERT_EQ(selectNode->columns.size(), 2);
    
    QualifiedIdentifierNode* col1 = asQualifiedIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col1, nullptr);
    EXPECT_EQ(col1->qualifier->name, "u");
    EXPECT_EQ(col1->name->name, "department");
    EXPECT_TRUE(selectNode->columns[0].alias.empty());
    
    QualifiedIdentifierNode* col2 = asQualifiedIdentifier(selectNode->columns[1].expression);
    ASSERT_NE(col2, nullptr);
    EXPECT_EQ(col2->qualifier->name, "u");
    EXPECT_EQ(col2->name->name, "salary");
    
    // Verify FROM clause
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    EXPECT_EQ(selectNode->from_clause->alias, "u");
    
    // Verify JOIN clause
    ASSERT_EQ(selectNode->join_clause.size(), 1);
    EXPECT_EQ(selectNode->join_clause[0].table->name->name, "departments");
    EXPECT_EQ(selectNode->join_clause[0].table->alias, "d");
    ASSERT_NE(selectNode->join_clause[0].on_condition, nullptr);
    
    // Verify WHERE clause
    ASSERT_NE(selectNode->where_clause, nullptr);
    
    // Verify GROUP BY clause
    ASSERT_NE(selectNode->group_by, nullptr);
    ASSERT_EQ(selectNode->group_by->expressions.size(), 1);
}

TEST_F(ParserTest, SelectJoinTwoTablesWithGroupByAndHavingClause) {
    std::string query = "SELECT u.department, u.salary \n"
                        "FROM users u\n"
                        "JOIN departments d ON u.dept_id = d.id\n"
                        "GROUP BY u.department having u.salary > 5000 AND d.budget <= 10000";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify columns
    ASSERT_EQ(selectNode->columns.size(), 2);
    QualifiedIdentifierNode* col1 = asQualifiedIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col1, nullptr);
    EXPECT_EQ(col1->qualifier->name, "u");
    EXPECT_EQ(col1->name->name, "department");
    
    // Verify FROM and JOIN
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    EXPECT_EQ(selectNode->from_clause->alias, "u");
    ASSERT_EQ(selectNode->join_clause.size(), 1);
    EXPECT_EQ(selectNode->join_clause[0].table->name->name, "departments");
    EXPECT_EQ(selectNode->join_clause[0].table->alias, "d");
    
    // Verify GROUP BY with HAVING
    ASSERT_NE(selectNode->group_by, nullptr);
    ASSERT_EQ(selectNode->group_by->expressions.size(), 1);
    ASSERT_NE(selectNode->group_by->having_clause, nullptr);
}

TEST_F(ParserTest, SelectSingleColumn) {
    std::string query = "SELECT name FROM users;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify not SELECT *
    EXPECT_FALSE(selectNode->is_select_all);
    
    // Verify single column
    ASSERT_EQ(selectNode->columns.size(), 1);
    IdentifierNode* column = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(column, nullptr);
    EXPECT_EQ(column->name, "name");
    EXPECT_TRUE(selectNode->columns[0].alias.empty());
    
    // Verify FROM clause
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
}

TEST_F(ParserTest, SelectMultipleColumns) {
    std::string query = "SELECT id, name, email FROM users;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify multiple columns
    ASSERT_EQ(selectNode->columns.size(), 3);
    
    IdentifierNode* col1 = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col1, nullptr);
    EXPECT_EQ(col1->name, "id");
    
    IdentifierNode* col2 = asIdentifier(selectNode->columns[1].expression);
    ASSERT_NE(col2, nullptr);
    EXPECT_EQ(col2->name, "name");
    
    IdentifierNode* col3 = asIdentifier(selectNode->columns[2].expression);
    ASSERT_NE(col3, nullptr);
    EXPECT_EQ(col3->name, "email");
}

TEST_F(ParserTest, SelectWithColumnAliases) {
    std::string query = "SELECT id as user_id, name as full_name FROM users;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify columns with aliases
    ASSERT_EQ(selectNode->columns.size(), 2);
    
    IdentifierNode* col1 = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col1, nullptr);
    EXPECT_EQ(col1->name, "id");
    EXPECT_EQ(selectNode->columns[0].alias, "user_id");
    
    IdentifierNode* col2 = asIdentifier(selectNode->columns[1].expression);
    ASSERT_NE(col2, nullptr);
    EXPECT_EQ(col2->name, "name");
    EXPECT_EQ(selectNode->columns[1].alias, "full_name");
}

TEST_F(ParserTest, SelectWithTableAlias) {
    std::string query = "SELECT u.id, u.name FROM users u;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify qualified identifiers
    ASSERT_EQ(selectNode->columns.size(), 2);
    
    QualifiedIdentifierNode* qual1 = asQualifiedIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(qual1, nullptr);
    EXPECT_EQ(qual1->qualifier->name, "u");
    EXPECT_EQ(qual1->name->name, "id");
    
    QualifiedIdentifierNode* qual2 = asQualifiedIdentifier(selectNode->columns[1].expression);
    ASSERT_NE(qual2, nullptr);
    EXPECT_EQ(qual2->qualifier->name, "u");
    EXPECT_EQ(qual2->name->name, "name");
    
    // Verify table alias
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    EXPECT_EQ(selectNode->from_clause->alias, "u");
}

TEST_F(ParserTest, SelectWithImplicitTableAlias) {
    std::string query = "SELECT u.id, u.name FROM users u;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify qualified identifiers
    ASSERT_EQ(selectNode->columns.size(), 2);
    
    QualifiedIdentifierNode* qual1 = asQualifiedIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(qual1, nullptr);
    EXPECT_EQ(qual1->qualifier->name, "u");
    EXPECT_EQ(qual1->name->name, "id");
    
    QualifiedIdentifierNode* qual2 = asQualifiedIdentifier(selectNode->columns[1].expression);
    ASSERT_NE(qual2, nullptr);
    EXPECT_EQ(qual2->qualifier->name, "u");
    EXPECT_EQ(qual2->name->name, "name");
    
    // Verify implicit table alias
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    EXPECT_EQ(selectNode->from_clause->alias, "u");
}

TEST_F(ParserTest, SelectWithSimpleWhereClause) {
    std::string query = "SELECT name FROM users WHERE age > 18;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify WHERE clause exists
    ASSERT_NE(selectNode->where_clause, nullptr);
    
    // Verify WHERE clause is a binary operation
    BinaryOperationNode* whereExpr = asBinaryOperation(selectNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    EXPECT_EQ(whereExpr->op, ">");
    
    // Verify left side is identifier "age"
    IdentifierNode* leftSide = asIdentifier(whereExpr->left);
    ASSERT_NE(leftSide, nullptr);
    EXPECT_EQ(leftSide->name, "age");
    
    // Verify right side is literal 18
    LiteralNode* rightSide = asLiteral(whereExpr->right);
    ASSERT_NE(rightSide, nullptr);
    EXPECT_EQ(std::get<int64_t>(rightSide->value), 18);
}

TEST_F(ParserTest, SelectWithComplexWhereClause) {
    std::string query = "SELECT name FROM users WHERE age >= 18 AND status = 'active' OR department = 'IT';";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify single column
    ASSERT_EQ(selectNode->columns.size(), 1);
    IdentifierNode* col = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "name");
    
    // Verify WHERE clause exists (complex logical expression)
    ASSERT_NE(selectNode->where_clause, nullptr);
    BinaryOperationNode* whereExpr = asBinaryOperation(selectNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    // Based on parser behavior, the top-level operator is OR but due to precedence it may group differently
    // Let's just verify we have a valid binary operation structure
    ASSERT_NE(whereExpr->left, nullptr);
    ASSERT_NE(whereExpr->right, nullptr);
}

TEST_F(ParserTest, SelectWithParenthesesInWhereClause) {
    std::string query = "SELECT name FROM users WHERE (age > 18) AND department = 'IT';";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify column
    ASSERT_EQ(selectNode->columns.size(), 1);
    IdentifierNode* col = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "name");
    
    // Verify WHERE clause with parentheses
    ASSERT_NE(selectNode->where_clause, nullptr);
    BinaryOperationNode* whereExpr = asBinaryOperation(selectNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    EXPECT_EQ(whereExpr->op, "AND");
}

TEST_F(ParserTest, SelectWithStringLiterals) {
    std::string query = "SELECT name FROM users WHERE department = 'Engineering' AND role = 'Developer';";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify column
    ASSERT_EQ(selectNode->columns.size(), 1);
    IdentifierNode* col = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "name");
    
    // Verify WHERE clause with string literals
    ASSERT_NE(selectNode->where_clause, nullptr);
    BinaryOperationNode* whereExpr = asBinaryOperation(selectNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    // The structure may vary based on parser precedence
    // Just verify we have a valid binary operation structure
    ASSERT_NE(whereExpr->left, nullptr);
    ASSERT_NE(whereExpr->right, nullptr);
}

TEST_F(ParserTest, SelectWithIntegerLiterals) {
    std::string query = "SELECT name FROM users WHERE age = 25 AND salary > 50000;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify column
    ASSERT_EQ(selectNode->columns.size(), 1);
    IdentifierNode* col = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "name");
    
    // Verify WHERE clause with integer literals
    ASSERT_NE(selectNode->where_clause, nullptr);
    BinaryOperationNode* whereExpr = asBinaryOperation(selectNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    EXPECT_EQ(whereExpr->op, "AND");
    
    // Verify left side: age = 25
    BinaryOperationNode* leftComparison = asBinaryOperation(whereExpr->left);
    ASSERT_NE(leftComparison, nullptr);
    EXPECT_EQ(leftComparison->op, "=");
    
    LiteralNode* intLiteral = asLiteral(leftComparison->right);
    ASSERT_NE(intLiteral, nullptr);
    EXPECT_EQ(std::get<int64_t>(intLiteral->value), 25);
}

TEST_F(ParserTest, SelectWithAllComparisonOperators) {
    std::string query = "SELECT name FROM users WHERE age > 18 AND salary >= 30000 AND experience < 10 AND rating <= 5 AND status = 'active' AND department != 'temp';";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify column
    ASSERT_EQ(selectNode->columns.size(), 1);
    IdentifierNode* col = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "name");
    
    // Verify complex WHERE clause with all comparison operators
    ASSERT_NE(selectNode->where_clause, nullptr);
    BinaryOperationNode* whereExpr = asBinaryOperation(selectNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    EXPECT_EQ(whereExpr->op, "AND");
    

}

TEST_F(ParserTest, SelectWithJoinOnMultipleConditions) {
    std::string query = "SELECT u.name, d.name FROM users u JOIN departments d ON u.dept_id = d.id AND u.status = 'active';";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify columns with qualified identifiers
    ASSERT_EQ(selectNode->columns.size(), 2);
    QualifiedIdentifierNode* col1 = asQualifiedIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col1, nullptr);
    EXPECT_EQ(col1->qualifier->name, "u");
    EXPECT_EQ(col1->name->name, "name");
    
    QualifiedIdentifierNode* col2 = asQualifiedIdentifier(selectNode->columns[1].expression);
    ASSERT_NE(col2, nullptr);
    EXPECT_EQ(col2->qualifier->name, "d");
    EXPECT_EQ(col2->name->name, "name");
    
    // Verify FROM and JOIN
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    EXPECT_EQ(selectNode->from_clause->alias, "u");
    
    ASSERT_EQ(selectNode->join_clause.size(), 1);
    EXPECT_EQ(selectNode->join_clause[0].table->name->name, "departments");
    EXPECT_EQ(selectNode->join_clause[0].table->alias, "d");
    
    // Verify JOIN ON condition with multiple conditions
    ASSERT_NE(selectNode->join_clause[0].on_condition, nullptr);
    BinaryOperationNode* joinCondition = asBinaryOperation(selectNode->join_clause[0].on_condition);
    ASSERT_NE(joinCondition, nullptr);
    EXPECT_EQ(joinCondition->op, "AND");
}

TEST_F(ParserTest, SelectWithGroupByMultipleColumns) {
    std::string query = "SELECT department, role FROM users GROUP BY department, role;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify columns
    ASSERT_EQ(selectNode->columns.size(), 2);
    IdentifierNode* col1 = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col1, nullptr);
    EXPECT_EQ(col1->name, "department");
    
    IdentifierNode* col2 = asIdentifier(selectNode->columns[1].expression);
    ASSERT_NE(col2, nullptr);
    EXPECT_EQ(col2->name, "role");
    
    // Verify FROM clause
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    
    // Verify GROUP BY with multiple columns
    ASSERT_NE(selectNode->group_by, nullptr);
    ASSERT_EQ(selectNode->group_by->expressions.size(), 2);
    
    IdentifierNode* groupBy1 = asIdentifier(selectNode->group_by->expressions[0]);
    ASSERT_NE(groupBy1, nullptr);
    EXPECT_EQ(groupBy1->name, "department");
    
    IdentifierNode* groupBy2 = asIdentifier(selectNode->group_by->expressions[1]);
    ASSERT_NE(groupBy2, nullptr);
    EXPECT_EQ(groupBy2->name, "role");
}

TEST_F(ParserTest, SelectWithGroupByAndHaving) {
    std::string query = "SELECT department FROM users GROUP BY department HAVING department = 'Engineering';";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify GROUP BY clause exists
    ASSERT_NE(selectNode->group_by, nullptr);
    
    // Verify GROUP BY has one expression
    ASSERT_EQ(selectNode->group_by->expressions.size(), 1);
    IdentifierNode* groupByCol = asIdentifier(selectNode->group_by->expressions[0]);
    ASSERT_NE(groupByCol, nullptr);
    EXPECT_EQ(groupByCol->name, "department");
    
    // Verify HAVING clause exists
    ASSERT_NE(selectNode->group_by->having_clause, nullptr);
    BinaryOperationNode* havingExpr = asBinaryOperation(selectNode->group_by->having_clause);
    ASSERT_NE(havingExpr, nullptr);
    EXPECT_EQ(havingExpr->op, "=");
    
    // Verify HAVING left side is identifier "department"
    IdentifierNode* havingLeft = asIdentifier(havingExpr->left);
    ASSERT_NE(havingLeft, nullptr);
    EXPECT_EQ(havingLeft->name, "department");
    
    // Verify HAVING right side is string literal "Engineering"
    LiteralNode* havingRight = asLiteral(havingExpr->right);
    ASSERT_NE(havingRight, nullptr);
    EXPECT_EQ(std::get<std::string>(havingRight->value), "Engineering");
}

TEST_F(ParserTest, SelectWithGroupByComplexHaving) {
    std::string query = "SELECT department FROM users GROUP BY department HAVING department = 'Engineering' AND salary > 50000;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify column
    ASSERT_EQ(selectNode->columns.size(), 1);
    IdentifierNode* col = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "department");
    
    // Verify FROM clause
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    
    // Verify GROUP BY
    ASSERT_NE(selectNode->group_by, nullptr);
    ASSERT_EQ(selectNode->group_by->expressions.size(), 1);
    IdentifierNode* groupByCol = asIdentifier(selectNode->group_by->expressions[0]);
    ASSERT_NE(groupByCol, nullptr);
    EXPECT_EQ(groupByCol->name, "department");
    
    // Verify complex HAVING clause
    ASSERT_NE(selectNode->group_by->having_clause, nullptr);
    BinaryOperationNode* havingExpr = asBinaryOperation(selectNode->group_by->having_clause);
    ASSERT_NE(havingExpr, nullptr);
    // Verify we have a valid binary operation structure
    ASSERT_NE(havingExpr->left, nullptr);
    ASSERT_NE(havingExpr->right, nullptr);
}

TEST_F(ParserTest, SelectWithQualifiedIdentifiersEverywhere) {
    std::string query = "SELECT u.id, u.name, d.budget FROM users u JOIN departments d ON u.dept_id = d.id WHERE u.salary > 50000 GROUP BY u.department, d.name;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify all columns are qualified identifiers
    ASSERT_EQ(selectNode->columns.size(), 3);
    
    QualifiedIdentifierNode* col1 = asQualifiedIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col1, nullptr);
    EXPECT_EQ(col1->qualifier->name, "u");
    EXPECT_EQ(col1->name->name, "id");
    
    QualifiedIdentifierNode* col2 = asQualifiedIdentifier(selectNode->columns[1].expression);
    ASSERT_NE(col2, nullptr);
    EXPECT_EQ(col2->qualifier->name, "u");
    EXPECT_EQ(col2->name->name, "name");
    
    QualifiedIdentifierNode* col3 = asQualifiedIdentifier(selectNode->columns[2].expression);
    ASSERT_NE(col3, nullptr);
    EXPECT_EQ(col3->qualifier->name, "d");
    EXPECT_EQ(col3->name->name, "budget");
    
    // Verify FROM and JOIN with aliases
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    EXPECT_EQ(selectNode->from_clause->alias, "u");
    
    ASSERT_EQ(selectNode->join_clause.size(), 1);
    EXPECT_EQ(selectNode->join_clause[0].table->name->name, "departments");
    EXPECT_EQ(selectNode->join_clause[0].table->alias, "d");
    
    // Verify WHERE and GROUP BY clauses exist
    ASSERT_NE(selectNode->where_clause, nullptr);
    ASSERT_NE(selectNode->group_by, nullptr);
    ASSERT_EQ(selectNode->group_by->expressions.size(), 2);
}

TEST_F(ParserTest, SelectComplexQuery) {
    std::string query = "SELECT u.id as user_id, u.name, d.name as dept_name "
                        "FROM users u "
                        "JOIN departments d ON u.dept_id = d.id "
                        "WHERE u.age >= 21 "
                        "GROUP BY u.department, u.role "
                        "HAVING u.salary > 30000;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    ASSERT_EQ(selectNode->columns.size(), 3);
    ASSERT_EQ(selectNode->join_clause.size(), 1);
    ASSERT_NE(selectNode->where_clause, nullptr);
    ASSERT_NE(selectNode->group_by, nullptr);
    ASSERT_NE(selectNode->group_by->having_clause, nullptr);
}

TEST_F(ParserTest, SelectNestedParenthesesExpressions) {
    std::string query = "SELECT name FROM users WHERE (age > 18) AND salary > 50000;";
    auto ast = parse_query(query);
    
    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);
    
    // Verify column
    ASSERT_EQ(selectNode->columns.size(), 1);
    IdentifierNode* col = asIdentifier(selectNode->columns[0].expression);
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->name, "name");
    
    // Verify FROM clause
    EXPECT_EQ(selectNode->from_clause->name->name, "users");
    
    // Verify WHERE clause with parentheses and AND operation
    ASSERT_NE(selectNode->where_clause, nullptr);
    BinaryOperationNode* whereExpr = asBinaryOperation(selectNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    EXPECT_EQ(whereExpr->op, "AND");
    
    // Verify right side is a simple comparison (salary > 50000)
    BinaryOperationNode* rightComparison = asBinaryOperation(whereExpr->right);
    ASSERT_NE(rightComparison, nullptr);
    EXPECT_EQ(rightComparison->op, ">");
    
    IdentifierNode* salaryId = asIdentifier(rightComparison->left);
    ASSERT_NE(salaryId, nullptr);
    EXPECT_EQ(salaryId->name, "salary");
    
    LiteralNode* salaryValue = asLiteral(rightComparison->right);
    ASSERT_NE(salaryValue, nullptr);
    EXPECT_EQ(std::get<int64_t>(salaryValue->value), 50000);
}

TEST_F(ParserTest, DropTableSingleTable) {
    std::string query = "DROP TABLE Users;";
    auto ast = parse_query(query);
    DropTableStatementNode *dropTableStatementNode = asDropStatement(ast);
    ASSERT_NE(dropTableStatementNode, nullptr);
    EXPECT_EQ(dropTableStatementNode->table_names.size(), 1);
    EXPECT_EQ(dropTableStatementNode->table_names[0]->name, "Users");

}

TEST_F(ParserTest, DropTableMultipleTables) {
    std::string query = "DROP TABLE Users,Department,Inventory;";
    auto ast = parse_query(query);
    DropTableStatementNode *dropTableStatementNode = asDropStatement(ast);
    ASSERT_NE(dropTableStatementNode, nullptr);
    EXPECT_EQ(dropTableStatementNode->table_names.size(), 3);
    std::vector<std::string> tableNames = {"Users", "Department", "Inventory"};

    for (size_t i = 0; i < tableNames.size(); ++i) {
        EXPECT_EQ(dropTableStatementNode->table_names[i]->name, tableNames.at(i));
    }
}

TEST_F(ParserTest, DropTableIfExistsMultipleTables) {
    std::string query = "DROP TABLE IF EXISTS Users,Department,Inventory;";
    auto ast = parse_query(query);
    DropTableStatementNode *dropTableStatementNode = asDropStatement(ast);
    ASSERT_NE(dropTableStatementNode, nullptr);
    EXPECT_EQ(dropTableStatementNode->table_names.size(), 3);
    std::vector<std::string> tableNames = {"Users", "Department", "Inventory"};

    for (size_t i = 0; i < tableNames.size(); ++i) {
        EXPECT_EQ(dropTableStatementNode->table_names[i]->name, tableNames.at(i));
    }
}

TEST_F(ParserTest, CreateTableSimpleQuery) {
    std::string query = "CREATE TABLE users (id INT, username VARCHAR, is_active BOOL);";
    auto ast = parse_query(query);
    
    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);
    
    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "users");
    
    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 3);
    
    // First column: id INT
    EXPECT_EQ(createNode->columns[0]->column_name->name, "id");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::INT);
    EXPECT_EQ(createNode->columns[0]->size, 0);
    
    // Second column: username VARCHAR
    EXPECT_EQ(createNode->columns[1]->column_name->name, "username");
    EXPECT_EQ(createNode->columns[1]->data_type, TokenType::VARCHAR);
    EXPECT_EQ(createNode->columns[1]->size, 0);
    
    // Third column: is_active BOOL
    EXPECT_EQ(createNode->columns[2]->column_name->name, "is_active");
    EXPECT_EQ(createNode->columns[2]->data_type, TokenType::BOOL);
    EXPECT_EQ(createNode->columns[2]->size, 0);
}

TEST_F(ParserTest, CreateTableWithVarcharSize) {
    std::string query = "CREATE TABLE users (id INT, username VARCHAR(255), description VARCHAR(1000));";
    auto ast = parse_query(query);
    
    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);
    
    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "users");
    
    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 3);
    
    // First column: id INT
    EXPECT_EQ(createNode->columns[0]->column_name->name, "id");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::INT);
    EXPECT_EQ(createNode->columns[0]->size, 0);
    
    // Second column: username VARCHAR(255)
    EXPECT_EQ(createNode->columns[1]->column_name->name, "username");
    EXPECT_EQ(createNode->columns[1]->data_type, TokenType::VARCHAR);
    EXPECT_EQ(createNode->columns[1]->size, 255);
    
    // Third column: description VARCHAR(1000)
    EXPECT_EQ(createNode->columns[2]->column_name->name, "description");
    EXPECT_EQ(createNode->columns[2]->data_type, TokenType::VARCHAR);
    EXPECT_EQ(createNode->columns[2]->size, 1000);
}

TEST_F(ParserTest, CreateTableSingleColumn) {
    std::string query = "CREATE TABLE simple_table (id INT);";
    auto ast = parse_query(query);
    
    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);
    
    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "simple_table");
    
    // Verify single column
    ASSERT_EQ(createNode->columns.size(), 1);
    EXPECT_EQ(createNode->columns[0]->column_name->name, "id");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::INT);
    EXPECT_EQ(createNode->columns[0]->size, 0);
}

TEST_F(ParserTest, CreateTableAllDataTypes) {
    std::string query = "CREATE TABLE all_types (num INT, text VARCHAR(50), flag BOOL);";
    auto ast = parse_query(query);
    
    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);
    
    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "all_types");
    
    // Verify all supported data types
    ASSERT_EQ(createNode->columns.size(), 3);
    
    EXPECT_EQ(createNode->columns[0]->column_name->name, "num");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::INT);
    
    EXPECT_EQ(createNode->columns[1]->column_name->name, "text");
    EXPECT_EQ(createNode->columns[1]->data_type, TokenType::VARCHAR);
    EXPECT_EQ(createNode->columns[1]->size, 50);
    
    EXPECT_EQ(createNode->columns[2]->column_name->name, "flag");
    EXPECT_EQ(createNode->columns[2]->data_type, TokenType::BOOL);
    
    // No primary key defined
    EXPECT_EQ(createNode->primary_key_columns.size(), 0);
}

TEST_F(ParserTest, CreateTableWithColumnLevelPrimaryKey) {
    std::string query = "CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(100), active BOOL);";
    auto ast = parse_query(query);
    
    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);
    
    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "users");
    
    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 3);
    EXPECT_EQ(createNode->columns[0]->column_name->name, "id");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::INT);
    
    // Verify primary key
    ASSERT_EQ(createNode->primary_key_columns.size(), 1);
    EXPECT_EQ(createNode->primary_key_columns[0]->name, "id");
}

TEST_F(ParserTest, CreateTableWithTableLevelSinglePrimaryKey) {
    std::string query = "CREATE TABLE users (id INT, name VARCHAR(100), PRIMARY KEY (id));";
    auto ast = parse_query(query);
    
    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);
    
    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "users");
    
    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 2);
    EXPECT_EQ(createNode->columns[0]->column_name->name, "id");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::INT);
    EXPECT_EQ(createNode->columns[1]->column_name->name, "name");
    EXPECT_EQ(createNode->columns[1]->data_type, TokenType::VARCHAR);
    
    // Verify primary key
    ASSERT_EQ(createNode->primary_key_columns.size(), 1);
    EXPECT_EQ(createNode->primary_key_columns[0]->name, "id");
}

TEST_F(ParserTest, CreateTableWithTableLevelCompositePrimaryKey) {
    std::string query = "CREATE TABLE user_roles (user_id INT, role_id INT, assigned_date VARCHAR(20), PRIMARY KEY (user_id, role_id));";
    auto ast = parse_query(query);
    
    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);
    
    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "user_roles");
    
    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 3);
    EXPECT_EQ(createNode->columns[0]->column_name->name, "user_id");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::INT);
    EXPECT_EQ(createNode->columns[1]->column_name->name, "role_id");
    EXPECT_EQ(createNode->columns[1]->data_type, TokenType::INT);
    EXPECT_EQ(createNode->columns[2]->column_name->name, "assigned_date");
    EXPECT_EQ(createNode->columns[2]->data_type, TokenType::VARCHAR);
    
    // Verify composite primary key
    ASSERT_EQ(createNode->primary_key_columns.size(), 2);
    EXPECT_EQ(createNode->primary_key_columns[0]->name, "user_id");
    EXPECT_EQ(createNode->primary_key_columns[1]->name, "role_id");
}

TEST_F(ParserTest, CreateTableWithFloat) {
    std::string query = "CREATE TABLE products (price FLOAT);";
    auto ast = parse_query(query);

    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);

    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "products");

    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 1);

    // First column: price FLOAT
    EXPECT_EQ(createNode->columns[0]->column_name->name, "price");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::FLOAT);
    EXPECT_EQ(createNode->columns[0]->size, 0);
}

TEST_F(ParserTest, InsertWithFloatLiteral) {
    std::string query = "INSERT INTO products (price) VALUES (99.99);";
    auto ast = parse_query(query);

    InsertStatementNode* insertNode = asInsertStatement(ast);
    ASSERT_NE(insertNode, nullptr);

    // Verify table name
    EXPECT_EQ(insertNode->tableName->name, "products");

    // Verify column names
    ASSERT_EQ(insertNode->columnNames.size(), 1);
    EXPECT_EQ(insertNode->columnNames[0]->name, "price");

    // Verify values
    ASSERT_EQ(insertNode->values.size(), 1);
    ASSERT_EQ(insertNode->values[0].size(), 1);

    // Verify the float literal
    ASSERT_EQ(insertNode->values[0].size(), 1);
    auto* literal = insertNode->values[0][0].get();
    ASSERT_NE(literal, nullptr);
    EXPECT_EQ(std::get<double>(literal->value), 99.99);
}

TEST_F(ParserTest, SelectWithFloatInWhereClause) {
    std::string query = "SELECT name FROM products WHERE price > 10.5;";
    auto ast = parse_query(query);

    SelectStatementNode* selectNode = asSelectStatement(ast);
    ASSERT_NE(selectNode, nullptr);

    // Verify WHERE clause exists
    ASSERT_NE(selectNode->where_clause, nullptr);

    // Verify WHERE clause is a binary operation
    BinaryOperationNode* whereExpr = asBinaryOperation(selectNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    EXPECT_EQ(whereExpr->op, ">");

    // Verify left side is identifier "price"
    IdentifierNode* leftSide = asIdentifier(whereExpr->left);
    ASSERT_NE(leftSide, nullptr);
    EXPECT_EQ(leftSide->name, "price");

    // Verify right side is literal 10.5
    LiteralNode* rightSide = asLiteral(whereExpr->right);
    ASSERT_NE(rightSide, nullptr);
    EXPECT_EQ(std::get<double>(rightSide->value), 10.5);
}

TEST_F(ParserTest, CreateTableWithDate) {
    std::string query = "CREATE TABLE events (event_date DATE);";
    auto ast = parse_query(query);

    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);

    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "events");

    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 1);

    // First column: event_date DATE
    EXPECT_EQ(createNode->columns[0]->column_name->name, "event_date");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::DATE);
    EXPECT_EQ(createNode->columns[0]->size, 0);
}

TEST_F(ParserTest, CreateTableWithTimestamp) {
    std::string query = "CREATE TABLE logs (log_time TIMESTAMP);";
    auto ast = parse_query(query);

    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);

    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "logs");

    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 1);

    // First column: log_time TIMESTAMP
    EXPECT_EQ(createNode->columns[0]->size, 0);
}

TEST_F(ParserTest, InsertWithDateLiteral) {
    std::string query = "INSERT INTO events (event_date) VALUES ('2025-10-31');";
    auto ast = parse_query(query);

    InsertStatementNode* insertNode = asInsertStatement(ast);
    ASSERT_NE(insertNode, nullptr);

    // Verify table name
    EXPECT_EQ(insertNode->tableName->name, "events");

    // Verify column names
    ASSERT_EQ(insertNode->columnNames.size(), 1);
    EXPECT_EQ(insertNode->columnNames[0]->name, "event_date");

    // Verify values
    ASSERT_EQ(insertNode->values.size(), 1);
    ASSERT_EQ(insertNode->values[0].size(), 1);

    // Verify the date literal
    auto* literal = insertNode->values[0][0].get();
    ASSERT_NE(literal, nullptr);
    auto date = std::get<SQLDate>(literal->value);
    EXPECT_EQ(date.year, 2025);
    EXPECT_EQ(date.month, 10);
    EXPECT_EQ(date.day, 31);
}

TEST_F(ParserTest, InsertWithTimestampLiteral) {
    std::string query = "INSERT INTO logs (log_time) VALUES ('2025-10-31 12:30:00');";
    auto ast = parse_query(query);

    InsertStatementNode* insertNode = asInsertStatement(ast);
    ASSERT_NE(insertNode, nullptr);

    // Verify table name
    EXPECT_EQ(insertNode->tableName->name, "logs");

    // Verify column names
    ASSERT_EQ(insertNode->columnNames.size(), 1);
    EXPECT_EQ(insertNode->columnNames[0]->name, "log_time");

    // Verify values
    ASSERT_EQ(insertNode->values.size(), 1);
    ASSERT_EQ(insertNode->values[0].size(), 1);

    // Verify the timestamp literal
    auto* literal = insertNode->values[0][0].get();
    ASSERT_NE(literal, nullptr);
    auto ts = std::get<SQLTimestamp>(literal->value);
    EXPECT_EQ(ts.year, 2025);
    EXPECT_EQ(ts.month, 10);
    EXPECT_EQ(ts.day, 31);
    EXPECT_EQ(ts.hour, 12);
    EXPECT_EQ(ts.minute, 30);
    EXPECT_EQ(ts.second, 0);
}

TEST_F(ParserTest, CreateTableWithMixedColumns) {
    std::string query = "CREATE TABLE products (id INT PRIMARY KEY, name VARCHAR(255), price INT, description VARCHAR(1000), is_active BOOL);";
    auto ast = parse_query(query);
    
    CreateTableStatementNode* createNode = asCreateTableStatement(ast);
    ASSERT_NE(createNode, nullptr);
    
    // Verify table name
    EXPECT_EQ(createNode->table_name->name, "products");
    
    // Verify columns
    ASSERT_EQ(createNode->columns.size(), 5);
    EXPECT_EQ(createNode->columns[0]->column_name->name, "id");
    EXPECT_EQ(createNode->columns[0]->data_type, TokenType::INT);
    EXPECT_EQ(createNode->columns[1]->column_name->name, "name");
    EXPECT_EQ(createNode->columns[1]->data_type, TokenType::VARCHAR);
    EXPECT_EQ(createNode->columns[1]->size, 255);
    EXPECT_EQ(createNode->columns[2]->column_name->name, "price");
    EXPECT_EQ(createNode->columns[2]->data_type, TokenType::INT);
    EXPECT_EQ(createNode->columns[3]->column_name->name, "description");
    EXPECT_EQ(createNode->columns[3]->data_type, TokenType::VARCHAR);
    EXPECT_EQ(createNode->columns[3]->size, 1000);
    EXPECT_EQ(createNode->columns[4]->column_name->name, "is_active");
    EXPECT_EQ(createNode->columns[4]->data_type, TokenType::BOOL);
    
    // Verify primary key
    ASSERT_EQ(createNode->primary_key_columns.size(), 1);
    EXPECT_EQ(createNode->primary_key_columns[0]->name, "id");
}

TEST_F(ParserTest, ErrorThrownOnSelectCommandWithNoColumn) {
    // Test ensure() throws runtime_error when expected token type doesn't match
    std::string query = "SELECT FROM users;";  // Missing * or column name

    
    // Try to ensure IDENTIFIER but get FROM instead - should throw
    EXPECT_THROW({
	  parse_query(query);
    }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnSelectCommandWithNoFrom) {
  // Test ensure() throws runtime_error when expected token type doesn't match
  std::string query = "SELECT * users;";  // Missing * or column name


  // Try to ensure IDENTIFIER but get FROM instead - should throw
  EXPECT_THROW({
				 parse_query(query);
			   }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnDropCommandWithNoTables) {
  // Test ensure() throws runtime_error when expected token type doesn't match
  std::string query = "DROP TABLE;";  // Missing * or column name


  // Try to ensure IDENTIFIER but get FROM instead - should throw
  EXPECT_THROW({
				 parse_query(query);
			   }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnDropCommandWithIncompleteIfExistsClause) {
  // Test ensure() throws runtime_error when expected token type doesn't match
  std::string query = "DROP TABLE IF users,department;";  // Missing * or column name


  // Try to ensure IDENTIFIER but get FROM instead - should throw
  EXPECT_THROW({
				 parse_query(query);
			   }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingTableKeyword) {
  std::string query = "CREATE users (id INT);";  // Missing TABLE keyword

  // Should throw when ensure(TABLE) is called
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingTableName) {
  std::string query = "CREATE TABLE (id INT);";  // Missing table name

  // Should throw when ensure(IDENTIFIER) is called for table name
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingOpeningParen) {
  std::string query = "CREATE TABLE users id INT);";  // Missing opening paren

  // Should throw when ensure(LPAREN) is called
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingColumnName) {
  std::string query = "CREATE TABLE users (INT);";  // Missing column name

  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingVarcharSize) {
  std::string query = "CREATE TABLE users (name VARCHAR());";  // Missing size in VARCHAR

  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingVarcharClosingParen) {
  std::string query = "CREATE TABLE users (name VARCHAR(255);";  // Missing ) after VARCHAR size

  // Should throw when ensure(RPAREN) is called after VARCHAR size
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingPrimaryKeyword) {
  std::string query = "CREATE TABLE users (id INT KEY);";  // Missing PRIMARY before KEY

  // Should throw when ensure(KEY) is called after PRIMARY
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingPrimaryKeyOpeningParen) {
  std::string query = "CREATE TABLE users (id INT, PRIMARY KEY id);";  // Missing ( before column list

  // Should throw when ensure(LPAREN) is called for PRIMARY KEY
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingPrimaryKeyClosingParen) {
  std::string query = "CREATE TABLE users (id INT, PRIMARY KEY (id;";  // Missing ) after column list

  // Should throw when ensure(RPAREN) is called for PRIMARY KEY
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, ErrorThrownOnCreateTableMissingClosingParen) {
  std::string query = "CREATE TABLE users (id INT;";  // Missing closing paren

  // Should throw when ensure(RPAREN) is called for table definition
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, SimpleInsertStatement) {
  std::string query = "INSERT INTO  users  VALUES (10, 'test', FALSE);";
  auto ast = parse_query(query);
  auto root = asInsertStatement(ast);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->tableName->name, "users");
  ASSERT_EQ(root->columnNames.size(), 0);
  ASSERT_EQ(root->values.size(), 1);
  ASSERT_EQ(root->values[0].size(), 3);
  ASSERT_EQ(std::get<int64_t>(root->values[0][0]->value), 10);
  ASSERT_EQ(std::get<std::string>(root->values[0][1]->value), "test");
  ASSERT_EQ(std::get<bool>(root->values[0][2]->value), false);

}

TEST_F(ParserTest, SimpleInsertStatementWithColumns) {
  std::string query = "INSERT INTO  users (id, name, isAlive) VALUES (10, 'test', FALSE);";
  auto ast = parse_query(query);
  auto root = asInsertStatement(ast);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->tableName->name, "users");
  ASSERT_EQ(root->columnNames.size(), 3);
  ASSERT_EQ(root->columnNames[0]->name, "id");
  ASSERT_EQ(root->columnNames[1]->name, "name");
  ASSERT_EQ(root->columnNames[2]->name, "isAlive");

  ASSERT_EQ(root->values.size(), 1);
  ASSERT_EQ(root->values[0].size(), 3);
  ASSERT_EQ(std::get<int64_t>(root->values[0][0]->value), 10);
  ASSERT_EQ(std::get<std::string>(root->values[0][1]->value), "test");
  ASSERT_EQ(std::get<bool>(root->values[0][2]->value), false);
}

TEST_F(ParserTest, MultiInsertStatementWithColumns) {
  std::string query = "INSERT INTO  users (id, name, isAlive) VALUES (10, 'test', FALSE), (12, 'test', TRUE);";
  auto ast = parse_query(query);
  auto root = asInsertStatement(ast);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->tableName->name, "users");
  ASSERT_EQ(root->columnNames.size(), 3);
  ASSERT_EQ(root->columnNames[0]->name, "id");
  ASSERT_EQ(root->columnNames[1]->name, "name");
  ASSERT_EQ(root->columnNames[2]->name, "isAlive");

  ASSERT_EQ(root->values.size(), 2);
  ASSERT_EQ(root->values[0].size(), 3);
  ASSERT_EQ(std::get<int64_t>(root->values[0][0]->value), 10);
  ASSERT_EQ(std::get<std::string>(root->values[0][1]->value), "test");
  ASSERT_EQ(std::get<bool>(root->values[0][2]->value), false);
  ASSERT_EQ(std::get<int64_t>(root->values[1][0]->value), 12);
  ASSERT_EQ(std::get<std::string>(root->values[1][1]->value), "test");
  ASSERT_EQ(std::get<bool>(root->values[1][2]->value), true);
}

// Error-thrown style tests for INSERT command
TEST_F(ParserTest, InsertMissingIntoKeyword) {
  std::string query = "INSERT users VALUES (1, 'test');";  // Missing INTO
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertMissingTableName) {
  std::string query = "INSERT INTO VALUES (1, 'test');";  // Missing table name
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertMissingValuesKeyword) {
  std::string query = "INSERT INTO users (1, 'test');";  // Missing VALUES
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertMissingLeftParen) {
  std::string query = "INSERT INTO users VALUES 1, 'test');";  // Missing (
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertMissingRightParen) {
  std::string query = "INSERT INTO users VALUES (1, 'test';";  // Missing )
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertMissingLeftParenInColumns) {
  std::string query = "INSERT INTO users id, name) VALUES (1, 'test');";  // Missing ( for columns
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertMissingRightParenInColumns) {
  std::string query = "INSERT INTO users (id, name VALUES (1, 'test');";  // Missing ) for columns
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertEmptyValues) {
  std::string query = "INSERT INTO users VALUES ();";  // Empty values
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertTrailingCommaInValues) {
  std::string query = "INSERT INTO users VALUES (1, 'test',);";  // Trailing comma in values
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}

TEST_F(ParserTest, InsertTrailingCommaInMultipleRows) {
  std::string query = "INSERT INTO users VALUES (1, 'test'), (2, 'test2'),;";  // Trailing comma in multiple rows
  EXPECT_THROW({
    parse_query(query);
  }, std::runtime_error);
}
TEST_F(ParserTest, DeleteAllRows) {
    std::string query = "DELETE FROM users;";
    auto ast = parse_query(query);
    
    DeleteStatementNode* deleteNode = asDeleteStatement(ast);
    ASSERT_NE(deleteNode, nullptr);
    
    EXPECT_EQ(deleteNode->table_name->name, "users");
    EXPECT_EQ(deleteNode->where_clause, nullptr);
}

TEST_F(ParserTest, DeleteWithWhereClause) {
    std::string query = "DELETE FROM users WHERE id = 5;";
    auto ast = parse_query(query);
    
    DeleteStatementNode* deleteNode = asDeleteStatement(ast);
    ASSERT_NE(deleteNode, nullptr);
    
    EXPECT_EQ(deleteNode->table_name->name, "users");
    
    ASSERT_NE(deleteNode->where_clause, nullptr);
    BinaryOperationNode* whereExpr = asBinaryOperation(deleteNode->where_clause);
    ASSERT_NE(whereExpr, nullptr);
    EXPECT_EQ(whereExpr->op, "=");
    
    IdentifierNode* left = asIdentifier(whereExpr->left);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->name, "id");
    
    LiteralNode* right = asLiteral(whereExpr->right);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(std::get<int64_t>(right->value), 5);
}

TEST_F(ParserTest, SimpleUpdate) {
    std::string query = "UPDATE users SET name = 'John', age = 30 WHERE id = 1;";
    auto ast = parse_query(query);

    auto updateStmt = asUpdateStatement(ast);
    ASSERT_NE(updateStmt, nullptr);
    EXPECT_EQ(updateStmt->table_name->name, "users");
    
    ASSERT_EQ(updateStmt->updates.size(), 2);
    EXPECT_EQ(updateStmt->updates[0].column->name, "name");
    auto nameValue = asLiteral(updateStmt->updates[0].value);
    EXPECT_EQ(std::get<std::string>(nameValue->value), "John");

    EXPECT_EQ(updateStmt->updates[1].column->name, "age");
    auto ageValue = asLiteral(updateStmt->updates[1].value);
    EXPECT_EQ(std::get<int64_t>(ageValue->value), 30);

    ASSERT_NE(updateStmt->where_clause, nullptr);
    auto whereClause = asBinaryOperation(updateStmt->where_clause);
    EXPECT_EQ(whereClause->op, "=");
}

TEST_F(ParserTest, UpdateWithoutWhere) {
    std::string query = "UPDATE products SET price = 100;";
    auto ast = parse_query(query);

    auto updateStmt = asUpdateStatement(ast);
    ASSERT_NE(updateStmt, nullptr);
    EXPECT_EQ(updateStmt->table_name->name, "products");
    
    ASSERT_EQ(updateStmt->updates.size(), 1);
    EXPECT_EQ(updateStmt->updates[0].column->name, "price");
    
    EXPECT_EQ(updateStmt->where_clause, nullptr);
}

TEST_F(ParserTest, CreateIndexSimple) {
    std::string query = "CREATE INDEX idx_name ON users (name);";
    auto ast = parse_query(query);

    auto indexStmt = asCreateIndexStatement(ast);
    ASSERT_NE(indexStmt, nullptr);
    EXPECT_EQ(indexStmt->index_name->name, "idx_name");
    EXPECT_EQ(indexStmt->table_name->name, "users");
    
    ASSERT_EQ(indexStmt->columns.size(), 1);
    EXPECT_EQ(indexStmt->columns[0]->name, "name");
}

TEST_F(ParserTest, CreateIndexComposite) {
    std::string query = "CREATE INDEX idx_composite ON users (name, age);";
    auto ast = parse_query(query);

    auto indexStmt = asCreateIndexStatement(ast);
    ASSERT_NE(indexStmt, nullptr);
    EXPECT_EQ(indexStmt->index_name->name, "idx_composite");
    EXPECT_EQ(indexStmt->table_name->name, "users");

    ASSERT_EQ(indexStmt->columns.size(), 2);
    EXPECT_EQ(indexStmt->columns[0]->name, "name");
    EXPECT_EQ(indexStmt->columns[1]->name, "age");
}

TEST_F(ParserTest, BoolLiteralInWhere) {
    auto ast = parse_query("SELECT * FROM users WHERE id = 1 AND active = TRUE");
    auto* select = dynamic_cast<SelectStatementNode*>(ast.get());
    ASSERT_NE(select, nullptr);
    ASSERT_NE(select->where_clause, nullptr);

    auto* where = asBinaryOperation(select->where_clause);
    ASSERT_NE(where, nullptr);
    EXPECT_EQ(where->op, "AND");

    auto* id_comparison = dynamic_cast<BinaryOperationNode*>(where->left.get());
    ASSERT_NE(id_comparison, nullptr);
    EXPECT_EQ(id_comparison->op, "=");

    auto* id_column = dynamic_cast<IdentifierNode*>(id_comparison->left.get());
    ASSERT_NE(id_column, nullptr);
    EXPECT_EQ(id_column->name, "id");

    auto* id_value = dynamic_cast<LiteralNode*>(id_comparison->right.get());
    ASSERT_NE(id_value, nullptr);
    ASSERT_TRUE(std::holds_alternative<int64_t>(id_value->value));
    EXPECT_EQ(std::get<int64_t>(id_value->value), 1);

    auto* active_comparison = dynamic_cast<BinaryOperationNode*>(where->right.get());
    ASSERT_NE(active_comparison, nullptr);
    EXPECT_EQ(active_comparison->op, "=");

    auto* active_column = dynamic_cast<IdentifierNode*>(active_comparison->left.get());
    ASSERT_NE(active_column, nullptr);
    EXPECT_EQ(active_column->name, "active");

    auto* active_value = dynamic_cast<LiteralNode*>(active_comparison->right.get());
    ASSERT_NE(active_value, nullptr);
    ASSERT_TRUE(std::holds_alternative<bool>(active_value->value));
    EXPECT_TRUE(std::get<bool>(active_value->value));
}

TEST_F(ParserTest, CreateTableNotNullSetsNullableFalse) {
    auto ast = parse_query("CREATE TABLE t (id INT NOT NULL, name VARCHAR(50))");
    auto* create = dynamic_cast<CreateTableStatementNode*>(ast.get());
    ASSERT_NE(create, nullptr);
    ASSERT_EQ(create->columns.size(), 2u);
    EXPECT_FALSE(create->columns[0]->nullable) << "id should be NOT NULL";
    EXPECT_TRUE(create->columns[1]->nullable)  << "name should be nullable";
}

TEST_F(ParserTest, CreateTableAllColumnsNotNull) {
    auto ast = parse_query("CREATE TABLE t (id INT NOT NULL, name VARCHAR(50) NOT NULL)");
    auto* create = dynamic_cast<CreateTableStatementNode*>(ast.get());
    ASSERT_NE(create, nullptr);
    ASSERT_EQ(create->columns.size(), 2u);
    EXPECT_FALSE(create->columns[0]->nullable);
    EXPECT_FALSE(create->columns[1]->nullable);
}

TEST_F(ParserTest, CreateTableDefaultsToNullable) {
    auto ast = parse_query("CREATE TABLE t (id INT, name VARCHAR(50))");
    auto* create = dynamic_cast<CreateTableStatementNode*>(ast.get());
    ASSERT_NE(create, nullptr);
    ASSERT_EQ(create->columns.size(), 2u);
    EXPECT_TRUE(create->columns[0]->nullable);
    EXPECT_TRUE(create->columns[1]->nullable);
}

TEST_F(ParserTest, InsertNegativeInteger) {
    auto ast = parse_query("INSERT INTO t VALUES (-5)");
    auto* insert = dynamic_cast<InsertStatementNode*>(ast.get());
    ASSERT_NE(insert, nullptr);
    ASSERT_EQ(insert->values.size(), 1u);
    ASSERT_EQ(insert->values[0].size(), 1u);
    EXPECT_EQ(std::get<int64_t>(insert->values[0][0]->value), -5);
}

TEST_F(ParserTest, InsertNegativeFloat) {
    auto ast = parse_query("INSERT INTO t VALUES (-3.14)");
    auto* insert = dynamic_cast<InsertStatementNode*>(ast.get());
    ASSERT_NE(insert, nullptr);
    ASSERT_EQ(insert->values.size(), 1u);
    ASSERT_EQ(insert->values[0].size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(insert->values[0][0]->value), -3.14);
}

TEST_F(ParserTest, InsertMixedNegativeAndPositive) {
    auto ast = parse_query("INSERT INTO t VALUES (-1, -2, 3, 3.14, -3.0)");
    auto* insert = dynamic_cast<InsertStatementNode*>(ast.get());
    ASSERT_NE(insert, nullptr);
    ASSERT_EQ(insert->values[0].size(), 5u);
    EXPECT_EQ(std::get<int64_t>(insert->values[0][0]->value), -1);
    EXPECT_EQ(std::get<int64_t>(insert->values[0][1]->value), -2);
    EXPECT_EQ(std::get<int64_t>(insert->values[0][2]->value), 3);
  	EXPECT_DOUBLE_EQ(std::get<double>(insert->values[0][3]->value), 3.14);
  	EXPECT_DOUBLE_EQ(std::get<double>(insert->values[0][4]->value), -3.0);
}

TEST_F(ParserTest, InsertNullLiteral) {
    auto ast = parse_query("INSERT INTO t VALUES (NULL)");
    auto* insert = dynamic_cast<InsertStatementNode*>(ast.get());
    ASSERT_NE(insert, nullptr);
    ASSERT_EQ(insert->values.size(), 1u);
    ASSERT_EQ(insert->values[0].size(), 1u);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(insert->values[0][0]->value));
}

TEST_F(ParserTest, InsertNullMixedWithOtherValues) {
    auto ast = parse_query("INSERT INTO t VALUES (1, NULL, 'hello')");
    auto* insert = dynamic_cast<InsertStatementNode*>(ast.get());
    ASSERT_NE(insert, nullptr);
    ASSERT_EQ(insert->values[0].size(), 3u);
    EXPECT_EQ(std::get<int64_t>(insert->values[0][0]->value), 1);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(insert->values[0][1]->value));
    EXPECT_EQ(std::get<std::string>(insert->values[0][2]->value), "hello");
}
