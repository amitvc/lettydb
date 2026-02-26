/**
 * @file lexer.cpp
 * @brief Implementation of the SQL lexical analyzer
 * 
 * This file implements the Lexer class which converts SQL query strings
 * into sequences of tokens for parsing. The lexer uses a character-by-character
 * scanning approach with lookahead for multi-character operators.
 */

#include "lexer.h"
#include "token_type_utils.h"
#include "utils.h"
#include <regex>
namespace letty {

    /**
     * @brief Main tokenization method that extracts the next token from input
     * 
     * This method implements a finite state machine that:
     * 1. Skips whitespace characters
     * 2. Recognizes single-character tokens (punctuation, operators)
     * 3. Delegates to specialized methods for complex tokens
     * 4. Handles unexpected characters gracefully
     * 
     * @return Next token in the input stream or EOF_FILE when exhausted
     */
    Token Lexer::next_token() {
        while (!has_ended()) {
            char c = peek();
            switch (c) {
                case ' ':
                case '\t':
                case '\r':
                case '\n':
                    advance(); // Skip whitespace
                    continue;

                case '*':
                    return make_token(TokenType::STAR, "*");
                case '.':
                    return make_token(TokenType::DOT, ".");
                case ',' :
                    return make_token(TokenType::COMMA, ",");
                case '(' :
                    return make_token(TokenType::LPAREN, "(");
                case ')' :
                    return make_token(TokenType::RPAREN, ")");
                case ';' :
                    return make_token(TokenType::SEMICOLON, ";");
                case '\'' :
                    return make_string();

                case '=':
                case '>':
                case '<':
                case '!':
                    return make_operator();

			  case '+':
				return make_token(TokenType::PLUS, "+");
			  case '-':
				return make_token(TokenType::MINUS, "-");

            }

            if (isdigit(c)) {
                return make_numbers();
            }

            if (std::isalpha(c) || c == '_') {
                return make_key_or_identifier();
            }

            // Handle unexpected characters with error recovery
            return handle_unexpected_character();
        }
        return  {TokenType::EOF_FILE, ""};
    }

   /**
    * @brief Parses comparison and equality operators
    * 
    * Handles both single-character operators (=, <, >) and two-character
    * operators (!=, <=, >=). Uses lookahead to determine if a second
    * character should be consumed.
    * 
    * @return Token representing the parsed operator
    */
   Token Lexer::make_operator() {
        char first_char = advance(); // Consume the first character of the operator

        switch (first_char) {
            case '=':
                return {TokenType::EQ, "="};
            case '!':
                if (!has_ended() && peek() == '=') {
                    advance(); // Consume the '='
                    return {TokenType::NE, "!="};
                }
                break; // Fallthrough to UNKNOWN for a lone '!'
            case '<':
                if (!has_ended() && peek() == '=') {
                    advance(); // Consume the '='
                    return {TokenType::LTE, "<="};
                }
                return {TokenType::LT, "<"};
            case '>':
                if (!has_ended() && peek() == '=') {
                    advance(); // Consume the '='
                    return {TokenType::GTE, ">="};
                }
                return {TokenType::GT, ">"};
        }

        // If we get here, it's an operator we don't recognize.
        return {TokenType::UNKNOWN, std::string(1, first_char)};
    }


    /**
     * @brief Helper method to create a token and advance the cursor
     * 
     * @param type The TokenType for the new token
     * @param value The string representation of the token
     * @return Newly created Token with the specified type and value
     */
    Token Lexer::make_token(TokenType type, const std::string &value) {
        advance(); // Move cursor ahead
        return {type, value};
    }

    Token Lexer::make_string() {
        advance();
        // Capture start position and use substr instead of char-by-char append
        size_t start = curr_pos;
        while (!has_ended() && peek() != '\'') {
            advance();
        }
        std::string value(input_.substr(start, curr_pos - start));

		// Check if we hit the end of the file without finding a closing quote
		if (has_ended()) {
		  // Return an UNKNOWN token or throw an error.
		  // For now, let's return UNKNOWN so the parser can handle it or fail gracefully.
		  return {TokenType::UNKNOWN, "'" + value};
		}

		advance(); // Consume the closing '
        if (is_date_literal(value)) {
            return {TokenType::DATE_LITERAL, value};
        } else if (is_timestamp_literal(value)) {
            return {TokenType::TIMESTAMP_LITERAL, value};
        }
        return {TokenType::STRING_LITERAL, value};
    }

    /**
     * @brief Parses integer literal tokens
     * 
     * Collects consecutive digit characters to form an integer literal.
     * Currently only supports positive integers and floats - negative numbers are
     * handled as a unary minus operator followed by a positive integer.
     * 
     * @return Token of type INT_LITERAL, FLOAT_LITERAL with the numeric string
     */
    Token Lexer::make_numbers() {
        size_t start = curr_pos;
        bool is_float = false;
        while (!has_ended() && isdigit(peek())) {
            advance();
        }

        if (!has_ended() && peek() == '.') {
            if (curr_pos + 1 < input_.size() && isdigit(input_[curr_pos + 1])) {
                is_float = true;
                advance(); // consume the dot
                while (!has_ended() && isdigit(peek())) {
                    advance();
                }
            }
        }

        std::string number(input_.substr(start, curr_pos - start));
        return {is_float ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL, std::move(number)};
    }

    /**
     * @brief Parses SQL keywords and user-defined identifiers
     * 
     * Collects alphanumeric characters and underscores, then performs
     * case-insensitive lookup in the keyword map. If found in the keyword
     * map, returns the corresponding keyword token; otherwise returns an
     * IDENTIFIER token.
     * 
     * @return Token of appropriate keyword type or IDENTIFIER
     */
    Token Lexer::make_key_or_identifier() {
        // Use substr instead of char-by-char append
        size_t start = curr_pos;
        while (!has_ended() && (std::isalnum(peek()) || peek() == '_')) {
            advance();
        }
        std::string text(input_.substr(start, curr_pos - start));

        // Uppercase in-place for keyword lookup
        std::string upper_text = text;
        std::transform(upper_text.begin(), upper_text.end(), upper_text.begin(),
                       [](unsigned char c){ return std::toupper(c); });

        auto it = keyword_map().find(upper_text);
        if (it != keyword_map().end()) {
            return {it->second, std::move(text)};
        }

        return {TokenType::IDENTIFIER, std::move(text)};
    }

    char Lexer::advance() {
        if (has_ended()) return '\0';
        return input_[curr_pos++];
    }

    char Lexer::peek() const {
        if (!has_ended()) {
            return input_[curr_pos];
        }
        return '\0'; // return end of string char.
    }

    bool Lexer::has_ended() const {
        return curr_pos >= input_.size();
    }

    /**
     * @brief Tokenizes the entire input string into a vector
     * 
     * Repeatedly calls next_token() until EOF_FILE is encountered,
     * collecting all tokens into a vector. The EOF_FILE token is included
     * in the result.
     * 
     * @return Complete vector of tokens from the input string
     */
    std::vector<Token> Lexer::tokenize() {
        std::vector<Token> tokens;
        tokens.reserve(16);  // Typical INSERT has ~13 tokens
        Token token;
        do {
            token = next_token();
            tokens.push_back(std::move(token));
        } while (tokens.back().type != TokenType::EOF_FILE);
        return tokens;
    }

    /**
     * @brief Error recovery method for unexpected characters
     * 
     * When an unrecognized character is encountered, this method:
     * 1. Advances past the character
     * 2. Returns an UNKNOWN token containing the character
     * 
     * This allows parsing to continue despite errors.
     * 
     * @return UNKNOWN token containing the unexpected character
     */
    Token Lexer::handle_unexpected_character() {
        char unexpected_char = advance();
        
        // Create UNKNOWN token with the unexpected character
        std::string error_value(1, unexpected_char);

        return {TokenType::UNKNOWN, error_value};
    }
}
