#pragma once
#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <replxx.hxx>

namespace letty {

class DatabaseEngine;

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

    /**
     * This CLI is temporary CLI to help me test locally. It will be replaced with a more robust CLI to interact with DB
     * @param db_path
     */
    explicit LettyCli(const std::string& db_path = "letty.db", bool force_simple = false);
    

    ~LettyCli();

    void Run();

private:

    void ProcessCommand(const std::string& input, bool quiet = false);


    void RunSimpleInteractive();


    std::string Prompt();

    void PrintHelp();
    
    void PrintResultTable(const std::vector<std::string>& column_names,
                          const std::vector<class Tuple>& rows);

    std::unique_ptr<DatabaseEngine> db_engine_;

    std::string db_path_;
    std::string history_path_;
    bool force_simple_ = false;
    replxx::Replxx rx_;
};

}
