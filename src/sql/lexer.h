#pragma once

#include <utility>
#include <vector>
#include <string>
#include "token.h"

namespace letty {

    /**
     * @class Lexer
     * @brief SQL lexical analyzer that tokenizes SQL input strings
     * 
     * The Lexer class is responsible for converting a raw SQL string into a sequence
     * of tokens that can be processed by the Parser. It implements a finite state
     * machine approach to recognize SQL keywords, identifiers, literals, operators,
     * and punctuation.
     * 
     * @par Token Recognition:
     * - Keywords: SELECT, FROM, WHERE, JOIN, etc. See token.h enum TokenType for list of keywords we currently support
     * - Identifiers: table names, column names, aliases
     * - Literals: integers, strings (single-quoted)
     * - Operators: =, !=, <, >, <=, >=
     * - Punctuation: (, ), ,, ., ;, *
     * 
     * @par Error Handling:
     * - Unexpected characters generate UNKNOWN tokens with logging
     * - Graceful error recovery allows parsing to continue
     * - Missing string terminators are handled
     * 
     * @par Usage Example:
     * @code
     * Lexer lexer("SELECT * FROM users WHERE id = 1");
     * auto tokens = lexer.tokenize();
     * @endcode
     */
    class Lexer {
        public:
            /**
             * @brief Constructs a Lexer with the given SQL input string
             * @param input The SQL query string to tokenize
             */
            explicit Lexer(std::string input) : input_(std::move(input)), curr_pos(0) {}
            
            /**
             * @brief Extracts and returns the next token from the input stream
             * @return The next Token in the input, or EOF_FILE token when input is exhausted
             * @throws std::runtime_error for unrecoverable parsing errors
             */
			[[nodiscard]]
            Token next_token();
            
            /**
             * @brief Tokenizes the entire input string into a vector of tokens
             * @return Vector of all tokens including the final EOF_FILE token
             */
			[[nodiscard]]
            std::vector<Token> tokenize();
        private:
            std::string input_;
            size_t curr_pos;

            /**
             * @brief Looks at the current character without consuming it
             * @return Current character or '\0' if at end of input
             */
            char peek() const;
            
            /**
             * @brief Consumes and returns the current character, advancing position
             * @return Current character or '\0' if at end of input
             */
            char advance();
            
            /**
             * @brief Checks if we've reached the end of the input string
             * @return true if at end of input, false otherwise
             */
            bool has_ended() const;
            
            /**
             * @brief Creates a token of the specified type and advances position
             * @param type The TokenType for the new token
             * @param value The string value of the token
             * @return Newly created Token
             */
            Token make_token(TokenType type, const std::string &value);
            
            /**
             * @brief Parses comparison and equality operators (=, !=, <, >, <=, >=)
             * @return Token representing the parsed operator or UNKNOWN for unrecognized operators
             */
            Token make_operator();
            
            /**
             * @brief Parses SQL keywords and identifiers
             * 
             * Reads alphanumeric characters and underscores, then checks against
             * the keyword map to determine if it's a reserved keyword or identifier.
             * Case-insensitive keyword matching is performed.
             * 
             * @return Token with type KEYWORD or IDENTIFIER
             */
            Token make_key_or_identifier();
            
            /**
             * @brief Parses integer literals
             * @return Token of type INT_LITERAL with the numeric string value
             */
            Token make_numbers();
            
            /**
             * @brief Parses single-quoted string literals
             * 
             * Handles string literals enclosed in single quotes. Note: escape
             * sequences are not currently supported.
             * 
             * @return Token of type STRING_LITERAL with the string content (excluding quotes)
             */
            Token make_string();
            
            /**
             * @brief Handles unexpected characters encountered during tokenization
             * 
             * Creates an UNKNOWN token for the unexpected character and logs the
             * occurrence. This provides error recovery so parsing can continue.
             * 
             * @return Token of type UNKNOWN containing the unexpected character
             */
            Token handle_unexpected_character();
    };
}