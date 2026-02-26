#include "LettyCli.h"
#include "database_engine.h"
#include "catalog/catalog_manager.h"
#include "monitoring/storage_inspector.h"
#include "storage/tuple.h"
#include "sql/executor.h"
#include "sql/lexer.h"
#include "sql/parser.h"
#include "common/logger.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unistd.h>  // for isatty()
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>
#include <tabulate/table.hpp>

namespace letty {

static std::string val_to_str(const Value& val) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "NULL";
        else if constexpr (std::is_same_v<T, int32_t>)   return fmt::format("{}", arg);
        else if constexpr (std::is_same_v<T, double>)    return fmt::format("{:.2f}", arg);
        else if constexpr (std::is_same_v<T, std::string>) return arg;
        else if constexpr (std::is_same_v<T, bool>)      return arg ? "true" : "false";
        else return "?";
    }, val);
}

static void apply_unicode_borders(tabulate::Table& t) {
    t.format()
      .corner_top_left("┌").corner_top_right("┐")
      .corner_bottom_left("└").corner_bottom_right("┘")
      .border_top("─").border_bottom("─")
      .border_left("│").border_right("│")
      .column_separator("│");
    if (t.size() > 0) {
        t[0].format()
          .border_bottom("─")
          .corner_bottom_left("├")
          .corner_bottom_right("┤");
    }
}


/**
 * This CLI is temporary CLI to help me test locally. It will be replaced with a more robust CLI to interact with DB
 * @param db_path
 */
LettyCli::LettyCli(const std::string& db_path, bool force_simple)
    : db_path_(db_path), force_simple_(force_simple) {

    db_engine_ = std::make_unique<DatabaseEngine>(db_path);

    // Initialize replxx line editor
    history_path_ = db_path_ + ".history";
    rx_.install_window_change_handler();
    rx_.set_max_history_size(1000);
    rx_.history_load(history_path_);

    // SQL keyword completions
    rx_.set_completion_callback([](const std::string& input, int& context_len) {
        static const std::vector<std::string> keywords = {
            "SELECT", "INSERT", "INTO", "INSPECT", "SUMMARY", "GAM", "PAGE", "VALUES", "CREATE", "TABLE",
            "DROP", "DELETE", "UPDATE", "SET", "FROM", "WHERE",
            "AND", "OR", "NOT", "NULL", "INT", "VARCHAR", "FLOAT",
            "BOOL", "PRIMARY", "KEY", "exit", "quit", "help"
        };
        replxx::Replxx::completions_t completions;

        int word_start = static_cast<int>(input.size());
        while (word_start > 0 && !std::isspace(input[word_start - 1]))
            --word_start;
        std::string prefix = input.substr(word_start);
        context_len = static_cast<int>(prefix.size());

        std::string upper_prefix = prefix;
        std::transform(upper_prefix.begin(), upper_prefix.end(), upper_prefix.begin(), ::toupper);

        for (const auto& kw : keywords) {
            if (!upper_prefix.empty() && kw.substr(0, upper_prefix.size()) == upper_prefix)
                completions.emplace_back(kw);
        }
        return completions;
    });
}

LettyCli::~LettyCli() {
    rx_.history_save(history_path_);
}

void LettyCli::Run() {
    if (force_simple_ || !isatty(STDIN_FILENO)) {
        RunSimpleInteractive();
        return;
    }

    fmt::print("╔══════════════════════════════════════════╗\n");
    fmt::print("║       Welcome to LettyDB CLI!            ║\n");
    fmt::print("║  Type SQL commands or 'exit' to quit     ║\n");
    fmt::print("║  Database: {:<30}║\n", db_path_);
    fmt::print("╚══════════════════════════════════════════╝\n\n");

    while (true) {
        const char* raw_input = rx_.input(Prompt());
        if (raw_input == nullptr) {
            fmt::print("\nGoodbye!\n");
            break;
        }

        std::string input(raw_input);
        auto start = input.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        auto end = input.find_last_not_of(" \t\r\n");
        input = input.substr(start, end - start + 1);
        if (input.empty()) continue;

        rx_.history_add(input);

        std::string lower_input = input;
        std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(), ::tolower);

        if (lower_input == "exit" || lower_input == "quit" || lower_input == "\\q") {
            fmt::print("Goodbye!\n");
            break;
        }
        if (lower_input == "help" || lower_input == "\\h") {
            PrintHelp();
            continue;
        }

        ProcessCommand(input, false);
    }
}

void LettyCli::RunSimpleInteractive() {
    fmt::print("╔══════════════════════════════════════════╗\n");
    fmt::print("║       Welcome to LettyDB CLI (IDE)!      ║\n");
    fmt::print("║  Type SQL commands or 'exit' to quit     ║\n");
    fmt::print("║  Database: {:<30}║\n", db_path_);
    fmt::print("╚══════════════════════════════════════════╝\n\n");

    std::string line;
    std::cout << Prompt() << std::flush;

    while (std::getline(std::cin, line)) {
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) { std::cout << Prompt() << std::flush; continue; }
        auto end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        if (line.empty()) { std::cout << Prompt() << std::flush; continue; }

        std::string lower_input = line;
        std::transform(lower_input.begin(), lower_input.end(), lower_input.begin(), ::tolower);

        if (lower_input == "exit" || lower_input == "quit" || lower_input == "\\q") {
            fmt::print("Goodbye!\n");
            break;
        }
        if (lower_input == "help" || lower_input == "\\h") {
            PrintHelp();
            std::cout << Prompt() << std::flush;
            continue;
        }

        ProcessCommand(line, false);
        std::cout << Prompt() << std::flush;
    }
}

// Methods removed as per user request to disable batch mode logic

std::string LettyCli::Prompt() {
    return "letty> ";
}

void LettyCli::PrintHelp() {
    fmt::print("\nAvailable commands:\n");
    fmt::print("  CREATE TABLE name (col1 INT, col2 VARCHAR(50), ...)\n");
    fmt::print("  INSERT INTO name VALUES (val1, val2, ...)\n");
    fmt::print("  SELECT * FROM name\n");
    fmt::print("  INSPECT <SUMMARY|GAM|PAGE|TABLE> [ARGS]\n");
    fmt::print("  exit, quit, \\q  - Exit the CLI\n");
    fmt::print("  help, \\h        - Show this help\n\n");
}

void LettyCli::ProcessCommand(const std::string& input, bool quiet) {
    if (input.rfind("INSPECT", 0) == 0 || input.rfind("inspect", 0) == 0) {
        std::string cmd = input;
        std::vector<std::string> parts;
        size_t pos = 0;
        while ((pos = cmd.find(' ')) != std::string::npos) {
            parts.push_back(cmd.substr(0, pos));
            cmd.erase(0, pos + 1);
            while (!cmd.empty() && cmd[0] == ' ') cmd.erase(0, 1);
        }
        parts.push_back(cmd);

        if (parts.size() < 2) {
            fmt::print(stderr, "Usage: INSPECT <SUMMARY|GAM|PAGE|TABLE> [ARGS]\n");
            return;
        }

        std::string type = parts[1];
        std::transform(type.begin(), type.end(), type.begin(), ::toupper);

        try {
            if (type == "SUMMARY") {
                auto json = nlohmann::json::parse(db_engine_->get_inspector().get_summary());

                int    total_pages   = json["total_pages"].get<int>();
                int    alloc_extents = json["allocated_extents"].get<int>();
                int    file_extents  = json["extent_manager"]["file_extents"].get<int>();
                int    gam_capacity  = json["extent_manager"]["total_extents"].get<int>();
                double pct_full      = json["percent_full"].get<double>();
                int    pool_size     = json["bpm"]["pool_size"].get<int>();
                double hit_ratio     = json["bpm"]["hit_ratio"].get<double>() * 100.0;
                long   hits          = json["bpm"]["hits"].get<long>();
                long   misses        = json["bpm"]["misses"].get<long>();
                long   evictions     = json["bpm"]["evictions"].get<long>();
                long   dirty_evict   = json["bpm"]["dirty_evictions"].get<long>();

                fmt::print("\n┌─────────────────────────────────┐\n");
                fmt::print("│        Database Summary         │\n");
                fmt::print("└─────────────────────────────────┘\n");
                fmt::print("  File Size   : {} pages ({} KB)\n", total_pages, total_pages * 4);

                fmt::print("\n  Extents\n");
                fmt::print("  ├─ In file   : {}\n", file_extents);
                fmt::print("  ├─ Allocated : {}\n", alloc_extents);
                fmt::print("  ├─ Free      : {}\n", file_extents - alloc_extents);
                fmt::print("  ├─ Used      : {:.1f}%\n", pct_full);
                fmt::print("  └─ GAM cap   : {} extents\n", gam_capacity);

                fmt::print("\n  Buffer Pool ({} pages)\n", pool_size);
                fmt::print("  ├─ Hit ratio : {:.1f}%\n", hit_ratio);
                fmt::print("  ├─ Hits      : {}\n", hits);
                fmt::print("  ├─ Misses    : {}\n", misses);
                fmt::print("  ├─ Evictions : {}\n", evictions);
                fmt::print("  └─ Dirty evict: {}\n\n", dirty_evict);

            } else if (type == "GAM") {
                auto json = nlohmann::json::parse(db_engine_->get_inspector().get_gam());

                fmt::print("\n┌─────────────────────────────────┐\n");
                fmt::print("│   Global Allocation Map (GAM)   │\n");
                fmt::print("└─────────────────────────────────┘\n");
                fmt::print("  Legend: █ = allocated   ░ = free\n\n");

                for (const auto& gam_node : json) {
                    const auto& alloc = gam_node["allocation"];
                    int total   = static_cast<int>(alloc.size());
                    int n_alloc = 0;
                    for (const auto& bit : alloc)
                        if (bit.get<int>() == 1) ++n_alloc;

                    fmt::print("  GAM page {}  ({} extents, {} allocated)\n",
                               gam_node["page_id"].get<int>(), total, n_alloc);

                    // Ruler: label every 8 extents
                    fmt::print("  ");
                    for (int i = 0; i < total; ++i) {
                        if (i % 8 == 0) {
                            std::string lbl = fmt::format("{}", i);
                            fmt::print("{}", lbl);
                            int pad = 8 - static_cast<int>(lbl.size());
                            if (pad > 0 && (i + pad) < total)
                                fmt::print("{}", std::string(pad, ' '));
                        }
                    }
                    fmt::print("\n  ");
                    for (int i = 0; i < total; ++i)
                        fmt::print("{}", alloc[i].get<int>() == 1 ? "█" : "░");
                    fmt::print("\n\n");
                }

            } else if (type == "PAGE") {
                if (parts.size() < 3) {
                    fmt::print(stderr, "Usage: INSPECT PAGE <page_id> [owner_table]\n");
                    return;
                }
                page_id_t pid    = std::stoi(parts[2]);
                std::string owner = parts.size() > 3 ? parts[3] : "";

                auto json = nlohmann::json::parse(db_engine_->get_inspector().get_page_detail(pid, owner));

                fmt::print("\n┌─────────────────────────────────┐\n");
                fmt::print("│       Page {:<21}│\n", pid);
                fmt::print("└─────────────────────────────────┘\n");

                if (json.contains("error")) {
                    fmt::print("  Error: {}\n\n", json["error"].get<std::string>());
                } else {
                    if (json.contains("header")) {
                        auto h = json["header"];
                        fmt::print("  Slots          : {}\n", h["num_slots"].get<int>());
                        fmt::print("  Free space ptr : {}\n", h["free_space_ptr"].get<int>());
                        fmt::print("  LSN            : {}\n", h["lsn"].get<int>());
                    }

                    if (json.contains("decoded_tuples") && !json["decoded_tuples"].empty()) {
                        tabulate::Table t;
                        t.add_row({"Slot", "Offset", "Values"});
                        for (const auto& tup : json["decoded_tuples"]) {
                            std::string vals_str;
                            const auto& vals = tup["values"];
                            for (size_t i = 0; i < vals.size(); ++i) {
                                if (i > 0) vals_str += ", ";
                                if (vals[i].is_string())
                                    vals_str += "\"" + vals[i].get<std::string>() + "\"";
                                else if (vals[i].is_null())
                                    vals_str += "NULL";
                                else
                                    vals_str += vals[i].dump();
                            }
                            t.add_row({
                                fmt::format("{}", tup["slot"].get<int>()),
                                fmt::format("{}", tup["offset"].get<int>()),
                                vals_str
                            });
                        }
                        apply_unicode_borders(t);
                        t.column(0).format().font_align(tabulate::FontAlign::right);
                        t.column(1).format().font_align(tabulate::FontAlign::right);
                        fmt::print("\n");
                        std::cout << t << "\n";

                    } else if (json.contains("slots") && !json["slots"].empty()) {
                        tabulate::Table t;
                        t.add_row({"Slot", "Offset", "Length"});
                        for (const auto& s : json["slots"]) {
                            t.add_row({
                                fmt::format("{}", s["index"].get<int>()),
                                fmt::format("{}", s["offset"].get<int>()),
                                fmt::format("{}", s["length"].get<int>())
                            });
                        }
                        apply_unicode_borders(t);
                        t.column(0).format().font_align(tabulate::FontAlign::right);
                        t.column(1).format().font_align(tabulate::FontAlign::right);
                        t.column(2).format().font_align(tabulate::FontAlign::right);
                        fmt::print("\n  Slots (no schema — pass owner table to decode tuples):\n");
                        std::cout << t << "\n";
                    }

                    // Hex dump: 4 lines × 16 bytes with offset prefix
                    if (json.contains("hex")) {
                        fmt::print("\n  Hex dump (first 64 bytes):\n");
                        const std::string& hex = json["hex"].get<std::string>();
                        std::istringstream iss(hex);
                        std::string line;
                        int line_no = 0;
                        while (line_no < 4 && std::getline(iss, line))
                            fmt::print("  {:04x}  {}\n", line_no++ * 16, line);
                    }
                }
                fmt::print("\n");

            } else if (type == "TABLE") {
                if (parts.size() < 3) {
                    fmt::print(stderr, "Usage: INSPECT TABLE <name>\n");
                    return;
                }
                std::string table = parts[2];
                auto json = nlohmann::json::parse(db_engine_->get_inspector().get_iam_chain(table));

                fmt::print("\n┌─────────────────────────────────┐\n");
                fmt::print("│  IAM Chain: {:<20}│\n", table);
                fmt::print("└─────────────────────────────────┘\n");

                if (json.is_object() || json.empty()) {
                    fmt::print("  Table not found or no IAM chain.\n\n");
                } else {
                    int total_iam_pages  = 0;
                    int total_extents    = 0;
                    int total_data_pages = 0;

                    for (const auto& node : json) {
                        ++total_iam_pages;
                        fmt::print("  IAM page {}\n", node["page_id"].get<int>());

                        if (node.contains("extents") && !node["extents"].empty()) {
                            tabulate::Table t;
                            t.add_row({"Extent", "Pages", "Size"});
                            for (const auto& ext : node["extents"]) {
                                int eid        = ext.get<int>();
                                int page_start = eid * EXTENT_SIZE;
                                int page_end   = page_start + EXTENT_SIZE - 1;
                                ++total_extents;
                                total_data_pages += EXTENT_SIZE;
                                t.add_row({
                                    fmt::format("{}", eid),
                                    fmt::format("{} \u2013 {}", page_start, page_end),
                                    fmt::format("{} pages", EXTENT_SIZE)
                                });
                            }
                            apply_unicode_borders(t);
                            t.column(0).format().font_align(tabulate::FontAlign::right);
                            std::cout << t << "\n";
                        }
                    }
                    fmt::print("  Total: {} IAM page(s), {} extent(s), {} data pages\n\n",
                               total_iam_pages, total_extents, total_data_pages);
                }

            } else {
                fmt::print(stderr, "Unknown inspection type: {}\n", type);
            }

        } catch (const std::exception& e) {
            fmt::print(stderr, "Inspection failed: {}\n", e.what());
        }
        return;
    }

    try {
        Lexer lexer(input);
        auto tokens = lexer.tokenize();

        Parser parser(std::move(tokens));
        auto ast = parser.parse();

        if (!ast) {
            fmt::print(stderr, "Error: Failed to parse SQL statement\n");
            return;
        }

        ExecutionResult result = db_engine_->get_executor().execute(ast.get());
        if (!result.success) {
            fmt::print(stderr, "Error: {}\n", result.error_message);
            return;
        }

        if (!result.rows.empty() || !result.column_names.empty()) {
            if (!quiet) {
                std::string table_name;
                if (auto* select = dynamic_cast<SelectStatementNode*>(ast.get())) {
                    if (select->from_clause && select->from_clause->name)
                        table_name = select->from_clause->name->name;
                }
                auto table_meta = table_name.empty() ? nullptr : db_engine_->get_catalog().get_table(table_name);
                PrintResultTable(result.column_names, result.rows,
                                 table_meta ? &table_meta->schema : nullptr);
                fmt::print("{} row(s) returned\n", result.rows.size());
            }
        } else if (!quiet && result.affected_rows > 0) {
            fmt::print("OK, {} row(s) affected\n", result.affected_rows);
        } else if (!quiet) {
            fmt::print("OK\n");
        }

    } catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
    }
}

void LettyCli::PrintResultTable(const std::vector<std::string>& column_names,
                                 const std::vector<Tuple>& rows,
                                 const Schema* schema) {
    if (column_names.empty()) return;

    tabulate::Table t;

    tabulate::Table::Row_t header;
    for (const auto& col : column_names)
        header.push_back(col);
    t.add_row(header);

    for (const auto& row : rows) {
        tabulate::Table::Row_t data_row;
        for (size_t i = 0; i < column_names.size(); ++i)
            data_row.push_back(i < row.size() ? val_to_str(row.get_value(i)) : "");
        t.add_row(data_row);
    }

    t[0].format().font_style({tabulate::FontStyle::bold});
    apply_unicode_borders(t);
    std::cout << t << "\n";
}

} // namespace letty
