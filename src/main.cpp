// Main entry point for Letty CLI application
//

#include "cli/LettyCli.h"
#include "common/logger.h"
#include <iostream>

int main(int argc, char* argv[]) {
    try {
        // Parse arguments
        std::string db_path = "letty.db";
        bool force_simple = false;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--simple") {
                force_simple = true;
            } else {
                db_path = arg;
            }
        }

        // Initialize logging system
        letty::Logger::init("letty.log");

        LOG_INFO("=== Letty Started ===");
        LOG_INFO("Version: 1.0.0");
        LOG_INFO("Build: " __DATE__ " " __TIME__);
	  	LOG_INFO("File Path: " + db_path);


        {
            letty::LettyCli cli(db_path, force_simple);
            cli.Run();
        }

        LOG_INFO("=== Letty Shutting Down ===");

        // Cleanup logging — must happen after all components are destroyed
        letty::Logger::shutdown();

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
	
    
    return 0;
}