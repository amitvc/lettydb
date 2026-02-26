#pragma once
#include <string>
#include "iostream"

namespace letty {
    // Types of tokens we support
    enum class TokenType {
        // Basic types
        IDENTIFIER,
        INT_LITERAL,
        FLOAT_LITERAL,
        DATE_LITERAL,
        TIMESTAMP_LITERAL,
        STRING_LITERAL,
        BOOL_LITERAL,
        NULL_LITERAL,

        // Keywords
        SELECT, FROM, WHERE, INSERT, INTO, VALUES,
        UPDATE, SET, DELETE, CREATE, TABLE, INDEX, DROP,
        INT, FLOAT, VARCHAR, BOOL, DATE, TIMESTAMP, JOIN,
        ON, GROUP, BY, HAVING, ORDER, ASC, DESC,
        IF, EXISTS, PRIMARY, KEY,

        // Operators
        EQ, NE, GT, LT, GTE, LTE,
	  	PLUS, MINUS,
        AND, OR, NOT,
        IS, TRUE, FALSE,

        // Symbols
        STAR, COMMA, DOT,
        LPAREN, RPAREN, SINGLE_QUOTE,
        SEMICOLON,

        // Aliasing
        AS,

        // Optional extensions
        LIMIT, OFFSET,

        // End
        EOF_TOKEN,
        EOF_FILE,
        UNKNOWN
    };

    struct Token {
        TokenType type;
        std::string text;
        ~Token() = default;
    };
}