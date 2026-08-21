#include <string>
#include <unordered_map>
#include "token.h"
#pragma once
namespace letty {
    inline const std::unordered_map<std::string, TokenType> keyword_map = {
            {"SELECT", TokenType::SELECT},
            {"FROM", TokenType::FROM},
            {"WHERE", TokenType::WHERE},
            {"INSERT", TokenType::INSERT},
            {"INTO", TokenType::INTO},
            {"VALUES", TokenType::VALUES},
            {"UPDATE", TokenType::UPDATE},
            {"SET", TokenType::SET},
            {"DELETE", TokenType::DELETE},
            {"CREATE", TokenType::CREATE},
            {"TABLE", TokenType::TABLE},
            {"INDEX", TokenType::INDEX},
            {"DROP", TokenType::DROP},
            {"IF", TokenType::IF},
            {"EXISTS", TokenType::EXISTS},
            {"PRIMARY", TokenType::PRIMARY},
            {"KEY", TokenType::KEY},

            {"INT", TokenType::INT},
            {"FLOAT", TokenType::FLOAT},
            {"VARCHAR", TokenType::VARCHAR},
            {"BOOL", TokenType::BOOL},
            {"DATE", TokenType::DATE},
            {"TIMESTAMP", TokenType::TIMESTAMP},
            {"JOIN", TokenType::JOIN},
            {"ON", TokenType::ON},
            {"GROUP", TokenType::GROUP},
            {"BY", TokenType::BY},
            {"HAVING", TokenType::HAVING},
            {"ORDER", TokenType::ORDER},
            {"ASC", TokenType::ASC},
            {"DESC", TokenType::DESC},

            {"AND", TokenType::AND},
            {"OR", TokenType::OR},
            {"NOT", TokenType::NOT},
            {"IS", TokenType::IS},
            {"NULL", TokenType::NULL_LITERAL},
            {"TRUE", TokenType::TRUE},
            {"FALSE", TokenType::FALSE},

            {"AS", TokenType::AS},
            {"LIMIT", TokenType::LIMIT},
            {"OFFSET", TokenType::OFFSET},
            {"COMPACT", TokenType::COMPACT}
    };
}