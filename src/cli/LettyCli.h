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
 * @brief Interactive command-line interface for the Letty database. This is temporary CLI we are using until the project
 * is complete.
 * 
 * The LettyCli class provides a command-line interface that allows
 * users to interact with the Letty database. It handles user input,
 * processes SQL commands via the Executor, and displays results.
 * 
 * @par Usage Example:
 * @code
 * LettyCli cli;
 * cli.Run();
 * @endcode
 */
class LettyCli {
public:

    explicit LettyCli(const std::string& db_path = "letty.db", bool force_simple = false);
    

    ~LettyCli();

    void Run();

private:

    void ProcessCommand(const std::string& input, bool quiet = false);


    void RunSimpleInteractive();


    std::string Prompt();

    void PrintHelp();
    
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
    bool force_simple_ = false;
    replxx::Replxx rx_;
};

}
