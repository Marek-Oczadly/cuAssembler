#pragma once

#include <string>

namespace CuAsm {

// Log levels, mirroring python logging module conventions used by the original CuAsmLogger.
enum class LogLevel : int {
    Debug = 0,
    Info = 15,
    Procedure = 25,
    Warning = 30,
    Error = 40,
};

class CuAsmLogger {
public:
    static void initLogger(const std::string& logFile = "",
                            int fileLevel = static_cast<int>(LogLevel::Info),
                            int stdoutLevel = static_cast<int>(LogLevel::Procedure));

    static void logDebug(const std::string& message);
    static void logInfo(const std::string& message);
    static void logEntry(const std::string& message);
    static void logProcedure(const std::string& message);
    static void logWarning(const std::string& message);
    static void logError(const std::string& message);
};

} // namespace CuAsm
