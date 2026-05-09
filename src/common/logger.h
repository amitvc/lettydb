#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <string>

namespace letty {

/**
 * @class Logger
 * @brief Centralized logging utility for Letty
 * 
 * Provides both console and file logging with configurable levels.
 * Uses spdlog for high-performance, thread-safe logging.
 * 
 * Features:
 * - Console output with colors
 * - Rotating file logs (5MB per file, 3 files retained)
 * - Thread-safe operations
 * - Configurable log levels
 * - Timestamp and thread ID formatting
 */
class Logger {
public:
    /**
     * @brief Initialize the logging system for tests (appends to log file)
     * @param log_file Path to log file
     * @param level Log level
     */
    static void init_for_tests(const std::string& log_file = "letty_test.log",
                              spdlog::level::level_enum level = spdlog::level::debug,
                              spdlog::level::level_enum console_level = spdlog::level::debug) {
        // Create console sink with colors (less verbose for tests)
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(console_level); // Configurable console level for tests
        
        // Create basic file sink that appends (not rotating, to capture all test logs)
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true); // true = append
        file_sink->set_level(spdlog::level::debug); // All debug+ to file
        
        // Create logger with both sinks
        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
        auto logger = std::make_shared<spdlog::logger>("letty", sinks.begin(), sinks.end());
        
        // Set format with PID for test identification: [timestamp] [PID] [thread_id] [level] message
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%P] [%t] [%^%l%$] %v");
        logger->set_level(level);
        
        // Register as default logger
        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);
        
        // Log startup message
        SPDLOG_INFO("Letty Test Logger initialized - Log file: {} (append mode)", log_file);
    }

    /**
     * @brief Initialize the logging system
     * @param log_file Path to log file (default: "letty.log")
     * @param level Log level (default: debug in Debug build, info in Release)
     */
    static void init(const std::string& log_file = "letty.log", 
                     spdlog::level::level_enum level = 
#ifdef DEBUG
                         spdlog::level::debug
#else
                         spdlog::level::info
#endif
    ) {
        // Create console sink with colors
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(level);
        
        // Create rotating file sink (5MB max, 3 files)
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, 1024 * 1024 * 5, 3);
        file_sink->set_level(spdlog::level::debug); // Always log debug to file
        
        // Create logger with both sinks
        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
        auto logger = std::make_shared<spdlog::logger>("letty", sinks.begin(), sinks.end());
        
        // Set format: [timestamp] [thread_id] [level] message
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%^%l%$] %v");
        logger->set_level(spdlog::level::debug);
        
        // Register as default logger
        spdlog::register_logger(logger);
        spdlog::set_default_logger(logger);
        
        // Log startup message
        SPDLOG_INFO("Letty Logger initialized - Log file: {}", log_file);
    }
    
    static void shutdown() {
        spdlog::shutdown();
    }
    
    static void set_level(spdlog::level::level_enum level) {
        spdlog::set_level(level);
    }
};

}

// Convenient logging macros
#define LOG_TRACE(...)    SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...)    SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)     SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)     SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...)    SPDLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) SPDLOG_CRITICAL(__VA_ARGS__)

// Component-specific logging macros for better organization
#define LOG_STORAGE_DEBUG(...)    LOG_DEBUG("[STORAGE] " __VA_ARGS__)
#define LOG_STORAGE_INFO(...)     LOG_INFO("[STORAGE] " __VA_ARGS__)
#define LOG_STORAGE_WARN(...)     LOG_WARN("[STORAGE] " __VA_ARGS__)
#define LOG_STORAGE_ERROR(...)    LOG_ERROR("[STORAGE] " __VA_ARGS__)

#define LOG_BPM_DEBUG(...)    	  LOG_STORAGE_DEBUG("[BPM] " __VA_ARGS__)
#define LOG_BPM_INFO(...)         LOG_STORAGE_INFO("[BPM] " __VA_ARGS__)
#define LOG_BPM_WARN(...)         LOG_STORAGE_WARN("[BPM] " __VA_ARGS__)
#define LOG_BPM_ERROR(...)        LOG_STORAGE_ERROR("[BPM] " __VA_ARGS__)

#define LOG_SQL_DEBUG(...)        LOG_DEBUG("[SQL] " __VA_ARGS__)
#define LOG_SQL_INFO(...)         LOG_INFO("[SQL] " __VA_ARGS__)
#define LOG_SQL_WARN(...)         LOG_WARN("[SQL] " __VA_ARGS__)
#define LOG_SQL_ERROR(...)        LOG_ERROR("[SQL] " __VA_ARGS__)

#define LOG_CATALOG_DEBUG(...)    LOG_DEBUG("[CATALOG] " __VA_ARGS__)
#define LOG_CATALOG_INFO(...)     LOG_INFO("[CATALOG] " __VA_ARGS__)
#define LOG_CATALOG_WARN(...)     LOG_WARN("[CATALOG] " __VA_ARGS__)
#define LOG_CATALOG_ERROR(...)    LOG_ERROR("[CATALOG] " __VA_ARGS__)