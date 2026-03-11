#include "utils.h"
#include <regex>

namespace letty {

    /**
     * @brief Checks if a given string matches the 'YYYY-MM-DD' date literal format.
     *
     * @param s The string to validate.
     * @return true if the string is a valid date literal, false otherwise.
     */
    bool is_date_literal(const std::string& s) {
        static const std::regex date_regex("\\d{4}-\\d{2}-\\d{2}");
        return std::regex_match(s, date_regex);
    }

    /**
     * @brief Checks if a given string matches the 'YYYY-MM-DD HH:MM:SS' timestamp literal format.
     *
     * @param s The string to validate.
     * @return true if the string is a valid timestamp literal, false otherwise.
     */
    bool is_timestamp_literal(const std::string& s) {
        static const std::regex timestamp_regex("\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}");
        return std::regex_match(s, timestamp_regex);
    }

}
