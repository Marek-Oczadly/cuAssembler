#pragma once

#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace CuAsm {

/**
 * @brief Logging levels used by CuAsmLogger, mirroring the numeric levels of python's
 *        logging module plus CuAsmLogger's custom ENTRY/PROCEDURE/SUBROUTINE levels.
 */
enum class LogLevel : int {
    NotSet     = 0,
    Debug      = 10,
    Subroutine = 15,
    Info       = 20,
    Procedure  = 25,
    Warning    = 30,
    Entry      = 35,
    Error      = 40,
    Critical   = 50,
};

/**
 * @brief A logger private to CuAsm, mirroring CuAsm.CuAsmLogger.CuAsmLogger.
 *
 * A customized logging style is used to show progress (indentation + progress-oriented
 * levels), without affecting the logging of other modules. All state is static: several
 * named loggers may exist simultaneously, and setActiveLogger() switches which one
 * subsequent log calls target, mirroring the python class's design.
 */
class CuAsmLogger {
public:
    /**
     * @brief Builds the default log file path in the system temp dir, mirroring
     *        CuAsmLogger.getDefaultLoggerFile.
     * @param name Base name of the log file (without extension).
     * @return The default temp-dir log file path.
     */
    static std::string getDefaultLoggerFile(const std::string& name);

    /**
     * @brief Builds a unique temporary log file path in the system temp dir, mirroring
     *        CuAsmLogger.getTemporaryLoggerFile.
     * @param name Base name of the log file (without extension).
     * @return A log file path that does not currently exist.
     */
    static std::string getTemporaryLoggerFile(const std::string& name);

    /**
     * @brief Initializes a named logger with the given log file and levels, mirroring
     *        CuAsmLogger.initLogger.
     * @param logFile Empty for a default temporary log file (DEFAULT), or a specific path;
     *        ".log" is appended if not already present. Ignored if hasLogFile is false.
     * @param fileLevel Minimum level written to the log file.
     * @param stdoutLevel Minimum level written to stdout.
     * @param name Logger instance name; several loggers may exist simultaneously.
     * @param fileMaxBytes Max size of the logfile in bytes before it rolls over.
     * @param fileBackupCount Number of rolled-over backup files to keep.
     * @param hasLogFile Whether file logging is enabled at all, mirroring log_file=None.
     * @param hasStdout Whether stdout logging is enabled at all.
     */
    static void initLogger(const std::string& logFile = "",
                            int fileLevel = static_cast<int>(LogLevel::Info),
                            int stdoutLevel = static_cast<int>(LogLevel::Procedure),
                            const std::string& name = "cuasm",
                            std::size_t fileMaxBytes = std::size_t(1) << 30,
                            int fileBackupCount = 3,
                            bool hasLogFile = true,
                            bool hasStdout = true);

    /**
     * @brief Switches subsequent log calls to a previously-initialized named logger,
     *        mirroring CuAsmLogger.setActiveLogger.
     * @param name Name of the logger to activate.
     */
    static void setActiveLogger(const std::string& name);

    /**
     * @brief Gets the log file path of the currently active logger, mirroring
     *        CuAsmLogger.getCurrentLogFile.
     * @return The active logger's log file path, or an empty string if file logging is disabled.
     */
    static std::string getCurrentLogFile();

    /**
     * @brief Logs a DEBUG-level message.
     * @param msg Message to log.
     */
    static void logDebug(const std::string& msg);

    /**
     * @brief Logs an INFO-level message.
     * @param msg Message to log.
     */
    static void logInfo(const std::string& msg);

    /**
     * @brief Logs a WARNING-level message.
     * @param msg Message to log.
     */
    static void logWarning(const std::string& msg);

    /**
     * @brief Logs an ERROR-level message.
     * @param msg Message to log.
     */
    static void logError(const std::string& msg);

    /**
     * @brief Logs a CRITICAL-level message.
     * @param msg Message to log.
     */
    static void logCritical(const std::string& msg);

    /**
     * @brief Logs an ENTRY-level message (main entry of a module), indented by the
     *        current indent level.
     * @param msg Message to log.
     */
    static void logEntry(const std::string& msg);

    /**
     * @brief Logs a PROCEDURE-level message (procedure of some module), indented by the
     *        current indent level.
     * @param msg Message to log.
     */
    static void logProcedure(const std::string& msg);

    /**
     * @brief Logs a SUBROUTINE-level message (internal subroutine), indented by the
     *        current indent level.
     * @param msg Message to log.
     */
    static void logSubroutine(const std::string& msg);

    /**
     * @brief Logs a message with no level tag, at PROCEDURE level, indented by the
     *        current indent level, mirroring CuAsmLogger.logLiteral.
     * @param msg Message to log.
     */
    static void logLiteral(const std::string& msg);

    /**
     * @brief Logs a message at an arbitrary level with no added prefix, mirroring
     *        CuAsmLogger.log.
     * @param level Level to log at.
     * @param msg Message to log.
     */
    static void log(int level, const std::string& msg);

    /**
     * @brief Wraps a callable with entry/exit timing log messages, mirroring the
     *        @CuAsmLogger.logTimeIt python decorator. Since C++ has no reflective
     *        function names, the callable's name must be supplied explicitly.
     * @param funcName Name to report in the log messages.
     * @param func Callable to wrap.
     * @return A callable with the same call signature as func, logging around each call.
     */
    template <typename Func>
    static auto logTimeIt(std::string funcName, Func func) {
        return [funcName = std::move(funcName), func = std::move(func)](auto&&... args) mutable {
            logLiteral("Running " + funcName + "...");
            incIndent();
            const auto t0 = std::chrono::steady_clock::now();

            if constexpr (std::is_void_v<decltype(func(std::forward<decltype(args)>(args)...))>) {
                func(std::forward<decltype(args)>(args)...);
                const auto t1 = std::chrono::steady_clock::now();
                decIndent();
                logLiteral(formatTimeItMessage(funcName, t0, t1));
            } else {
                auto ret = func(std::forward<decltype(args)>(args)...);
                const auto t1 = std::chrono::steady_clock::now();
                decIndent();
                logLiteral(formatTimeItMessage(funcName, t0, t1));
                return ret;
            }
        };
    }

    /**
     * @brief Wraps a callable so calls to it happen at one deeper indent level, mirroring
     *        the @CuAsmLogger.logIndentIt python decorator.
     * @param func Callable to wrap.
     * @return A callable with the same call signature as func, indenting around each call.
     */
    template <typename Func>
    static auto logIndentIt(Func func) {
        return [func = std::move(func)](auto&&... args) mutable {
            incIndent();
            if constexpr (std::is_void_v<decltype(func(std::forward<decltype(args)>(args)...))>) {
                func(std::forward<decltype(args)>(args)...);
                decIndent();
            } else {
                auto ret = func(std::forward<decltype(args)>(args)...);
                decIndent();
                return ret;
            }
        };
    }

    /**
     * @brief Wraps a callable with a "running" trace message and one deeper indent level,
     *        mirroring the @CuAsmLogger.logTraceIt python decorator.
     * @param funcName Name to report in the log message.
     * @param func Callable to wrap.
     * @return A callable with the same call signature as func, tracing around each call.
     */
    template <typename Func>
    static auto logTraceIt(std::string funcName, Func func) {
        return [funcName = std::move(funcName), func = std::move(func)](auto&&... args) mutable {
            logLiteral("Running " + funcName + "...");
            incIndent();
            if constexpr (std::is_void_v<decltype(func(std::forward<decltype(args)>(args)...))>) {
                func(std::forward<decltype(args)>(args)...);
                decIndent();
            } else {
                auto ret = func(std::forward<decltype(args)>(args)...);
                decIndent();
                return ret;
            }
        };
    }

    /**
     * @brief Increases the log indent level by one.
     */
    static void incIndent();

    /**
     * @brief Decreases the log indent level by one, floored at zero.
     */
    static void decIndent();

    /**
     * @brief Resets the log indent level to a given value, floored at zero.
     * @param val New indent level.
     */
    static void resetIndent(int val = 0);

    /**
     * @brief Sets the overall level threshold of the currently active logger, mirroring
     *        CuAsmLogger.setLevel. Messages below this level are dropped before per-sink
     *        (file/stdout) level checks are made.
     * @param level New minimum level.
     */
    static void setLevel(int level);

    /**
     * @brief Raises the active logger's level to ERROR, silencing debug/info/procedure/
     *        subroutine/warning/entry output, mirroring CuAsmLogger.disable.
     */
    static void disable();

private:
    /**
     * @brief Per-name logger state: sinks, thresholds and rollover bookkeeping.
     */
    struct LoggerState {
        std::string name;
        std::string logFilePath;
        std::ofstream fileStream;
        std::size_t fileMaxBytes = 0;
        int fileBackupCount = 0;
        int fileLevel = static_cast<int>(LogLevel::Debug);
        int stdoutLevel = static_cast<int>(LogLevel::Procedure);
        int loggerLevel = static_cast<int>(LogLevel::Debug);
        bool hasLogFile = false;
        bool hasStdout = false;
    };

    /**
     * @brief Formats the "Func ... completed! Time=... secs." message used by logTimeIt.
     * @param funcName Name of the wrapped function.
     * @param t0 Start time.
     * @param t1 End time.
     * @return The formatted message.
     */
    static std::string formatTimeItMessage(const std::string& funcName,
                                            std::chrono::steady_clock::time_point t0,
                                            std::chrono::steady_clock::time_point t1);

    /**
     * @brief Formats the current local time as "YYYY-MM-DD HH:MM:SS,mmm", mirroring python
     *        logging's default asctime format.
     * @return The formatted timestamp.
     */
    static std::string currentTimestamp();

    /**
     * @brief Rolls the log file over: renames path.(N-1) to path.N down to path.1, then the
     *        active file to path.1, and reopens a fresh empty file, mirroring
     *        logging.handlers.RotatingFileHandler.doRollover.
     * @param state Logger state whose file should be rolled over.
     */
    static void rollOverFile(LoggerState& state);

    /**
     * @brief Writes a line to a logger's file, rolling over first if it would exceed
     *        fileMaxBytes.
     * @param state Logger state to write to.
     * @param line Fully formatted line (timestamp + message) to write.
     */
    static void writeToFile(LoggerState& state, const std::string& line);

    /**
     * @brief Dispatches a fully-prefixed message to the active logger's sinks, applying the
     *        logger-level, file-level and stdout-level gates.
     * @param level Level of the message.
     * @param msg Fully-prefixed message text (no timestamp).
     */
    static void doLog(int level, const std::string& msg);

    static inline std::map<std::string, std::shared_ptr<LoggerState>> s_loggerRepos;
    static inline std::shared_ptr<LoggerState> s_currLogger = [] {
        auto state = std::make_shared<LoggerState>();
        state->name = "cuasm";
        state->loggerLevel = static_cast<int>(LogLevel::Debug);
        state->stdoutLevel = static_cast<int>(LogLevel::Procedure);
        state->hasLogFile = false;
        state->hasStdout = true;
        return state;
    }();
    static inline int s_indentLevel = 0;
    static inline std::string s_indentString;
};

inline std::string CuAsmLogger::getDefaultLoggerFile(const std::string& name) {
    return (std::filesystem::temp_directory_path() / (name + ".log")).string();
}

inline std::string CuAsmLogger::getTemporaryLoggerFile(const std::string& name) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path();

    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(alphabet) - 2);

    for (;;) {
        const std::time_t now = std::time(nullptr);
        std::tm tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &now);
#else
        localtime_r(&now, &tmBuf);
#endif
        std::ostringstream tag;
        tag << std::put_time(&tmBuf, ".%m%d-%H%M%S.");

        std::string randTag;
        for (int i = 0; i < 8; ++i) {
            randTag += alphabet[dist(gen)];
        }

        const fs::path candidate = dir / (name + tag.str() + randTag + ".log");
        if (!fs::exists(candidate)) {
            return candidate.string();
        }
    }
}

inline void CuAsmLogger::initLogger(const std::string& logFile,
                                     int fileLevel,
                                     int stdoutLevel,
                                     const std::string& name,
                                     std::size_t fileMaxBytes,
                                     int fileBackupCount,
                                     bool hasLogFile,
                                     bool hasStdout) {
    auto state = std::make_shared<LoggerState>();
    state->name = name;
    state->loggerLevel = static_cast<int>(LogLevel::Debug);
    state->fileLevel = fileLevel;
    state->stdoutLevel = stdoutLevel;
    state->fileMaxBytes = fileMaxBytes;
    state->fileBackupCount = fileBackupCount;
    state->hasLogFile = hasLogFile;
    state->hasStdout = hasStdout;

    if (hasLogFile) {
        std::string fullLogFile;
        if (logFile.empty()) {
            fullLogFile = getTemporaryLoggerFile(name);
        } else if (logFile.size() >= 4 && logFile.compare(logFile.size() - 4, 4, ".log") == 0) {
            fullLogFile = logFile;
        } else {
            fullLogFile = logFile + ".log";
        }

        std::cout << "InitLogger(" << name << ") with logfile \"" << fullLogFile << "\"..." << std::endl;

        state->logFilePath = fullLogFile;
        const bool needsRollOver = std::filesystem::exists(fullLogFile);
        state->fileStream.open(fullLogFile, std::ios::out | std::ios::app);

        if (needsRollOver) {
            std::cout << "Logfile " << fullLogFile << " already exists! Rolling over..." << std::endl;
            rollOverFile(*state);
        }
    }

    s_loggerRepos[name] = state;
    s_currLogger = state;
}

inline void CuAsmLogger::setActiveLogger(const std::string& name) {
    const auto it = s_loggerRepos.find(name);
    if (it != s_loggerRepos.end()) {
        s_currLogger = it->second;
    } else {
        std::cout << "CuAsmLogger " << name << " does not exist! Keeping current logger..." << std::endl;
    }
}

inline std::string CuAsmLogger::getCurrentLogFile() {
    if (!s_currLogger || !s_currLogger->hasLogFile) {
        return "";
    }
    return s_currLogger->logFilePath;
}

inline void CuAsmLogger::doLog(int level, const std::string& msg) {
    if (!s_currLogger) {
        return;
    }
    LoggerState& state = *s_currLogger;
    if (level < state.loggerLevel) {
        return;
    }

    const std::string line = currentTimestamp() + " - " + msg;
    if (state.hasLogFile && level >= state.fileLevel) {
        writeToFile(state, line);
    }
    if (state.hasStdout && level >= state.stdoutLevel) {
        std::cout << line << std::endl;
    }
}

inline void CuAsmLogger::logDebug(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Debug), "   DEBUG - " + msg);
}

inline void CuAsmLogger::logInfo(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Info), "    INFO - " + msg);
}

inline void CuAsmLogger::logWarning(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Warning), " WARNING - " + msg);
}

inline void CuAsmLogger::logError(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Error), "   ERROR - " + msg);
}

inline void CuAsmLogger::logCritical(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Critical), "CRITICAL - " + msg);
}

inline void CuAsmLogger::logEntry(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Entry), "   ENTRY - " + s_indentString + msg);
}

inline void CuAsmLogger::logProcedure(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Procedure), "    PROC - " + s_indentString + msg);
}

inline void CuAsmLogger::logSubroutine(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Subroutine), "     SUB - " + s_indentString + msg);
}

inline void CuAsmLogger::logLiteral(const std::string& msg) {
    doLog(static_cast<int>(LogLevel::Procedure), "         - " + s_indentString + msg);
}

inline void CuAsmLogger::log(int level, const std::string& msg) {
    doLog(level, msg);
}

inline void CuAsmLogger::incIndent() {
    ++s_indentLevel;
    s_indentString = std::string(static_cast<std::size_t>(s_indentLevel) * 4, ' ');
}

inline void CuAsmLogger::decIndent() {
    --s_indentLevel;
    if (s_indentLevel < 0) {
        s_indentLevel = 0;
    }
    s_indentString = std::string(static_cast<std::size_t>(s_indentLevel) * 4, ' ');
}

inline void CuAsmLogger::resetIndent(int val) {
    if (val < 0) {
        val = 0;
    }
    s_indentLevel = val;
    s_indentString = std::string(static_cast<std::size_t>(s_indentLevel) * 4, ' ');
}

inline void CuAsmLogger::setLevel(int level) {
    if (s_currLogger) {
        s_currLogger->loggerLevel = level;
    }
}

inline void CuAsmLogger::disable() {
    setLevel(static_cast<int>(LogLevel::Error));
}

inline std::string CuAsmLogger::formatTimeItMessage(const std::string& funcName,
                                                      std::chrono::steady_clock::time_point t0,
                                                      std::chrono::steady_clock::time_point t1) {
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::ostringstream oss;
    oss << "Func " << funcName << " completed! Time=" << std::fixed << std::setprecision(4)
        << std::setw(8) << secs << " secs.";
    return oss.str();
}

inline std::string CuAsmLogger::currentTimestamp() {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const std::time_t nowTimeT = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &nowTimeT);
#else
    localtime_r(&nowTimeT, &tmBuf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M:%S") << ',' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

inline void CuAsmLogger::rollOverFile(LoggerState& state) {
    namespace fs = std::filesystem;

    if (state.fileStream.is_open()) {
        state.fileStream.close();
    }

    if (state.fileBackupCount > 0) {
        for (int i = state.fileBackupCount - 1; i >= 1; --i) {
            const std::string src = state.logFilePath + "." + std::to_string(i);
            const std::string dst = state.logFilePath + "." + std::to_string(i + 1);
            if (fs::exists(src)) {
                if (fs::exists(dst)) {
                    fs::remove(dst);
                }
                fs::rename(src, dst);
            }
        }

        const std::string dst1 = state.logFilePath + ".1";
        if (fs::exists(dst1)) {
            fs::remove(dst1);
        }
        if (fs::exists(state.logFilePath)) {
            fs::rename(state.logFilePath, dst1);
        }
    } else if (fs::exists(state.logFilePath)) {
        fs::remove(state.logFilePath);
    }

    state.fileStream.open(state.logFilePath, std::ios::out | std::ios::app);
}

inline void CuAsmLogger::writeToFile(LoggerState& state, const std::string& line) {
    if (!state.fileStream.is_open()) {
        return;
    }

    if (state.fileMaxBytes > 0) {
        state.fileStream.flush();
        const auto currentSize = static_cast<std::size_t>(std::filesystem::file_size(state.logFilePath));
        if (currentSize + line.size() + 1 > state.fileMaxBytes) {
            rollOverFile(state);
        }
    }

    state.fileStream << line << '\n';
    state.fileStream.flush();
}

} // namespace CuAsm
