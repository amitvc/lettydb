//
// Created by Amit Chavan on 6/6/25.
//

#pragma once
#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <replxx.hxx>

namespace letty {

// Forward declarations
class DiskManager;
class BufferPoolManager;
class ExtentManager;
class IamManager;
class CatalogManager;
class TableManager;
class Executor;

/**
 * @class LettyCli
 * @brief Interactive command-line interface for the Letty database
 * 
 * The LettyCli class provides a command-line interface that allows
 * users to interact with the Letty database. It handles user input,
 * processes SQL commands via the Executor, and displays results.
 * 
 * @par Usage Example:
 * @code
 * LettyCli cli;
 * cli.Run();  // Starts the interactive session
 * @endcode
 */
class LettyCli {
public:
    /**
     * @brief Constructs the CLI and initializes the database.
     * @param db_path Path to the database file (default: "letty.db")
     */
    explicit LettyCli(const std::string& db_path = "letty.db");
    
    /**
     * @brief Destructor - cleans up database resources
     */
    ~LettyCli();

    /**
     * @brief Starts the interactive CLI session
     */
    /**
     * @brief Starts the interactive CLI session
     */
    void Run();

private:
    /**
     * @brief Processes a single SQL command from user input
     * @param input The raw SQL command string entered by the user
     */
    void ProcessCommand(const std::string& input, bool quiet = false);

    /**
     * @brief Fallback interactive mode using std::getline for non-TTY (IDE) environments.
     */
    void RunSimpleInteractive();

    /**
     * @brief Generates and returns the command prompt string
     */
    std::string Prompt();

    /**
     * @brief Prints the help text listing available commands
     */
    void PrintHelp();
    
    /**
     * @brief Formats SELECT results as a table
     */
    void PrintResultTable(const std::vector<std::string>& column_names,
                          const std::vector<class Tuple>& rows,
                          const class Schema* schema);

    // Database components
    std::unique_ptr<DiskManager> disk_manager_;
    std::unique_ptr<BufferPoolManager> buffer_pool_;
    std::unique_ptr<ExtentManager> extent_manager_;
    std::unique_ptr<IamManager> iam_manager_;
    std::unique_ptr<CatalogManager> catalog_manager_;
    std::unique_ptr<TableManager> table_manager_;
    std::unique_ptr<Executor> executor_;

    // Monitoring tools
    std::unique_ptr<class StorageInspector> inspector_;

    std::string db_path_;
    std::string history_path_;
    replxx::Replxx rx_;
};

} // namespace letty
