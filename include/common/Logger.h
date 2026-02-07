#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <memory>
#include <string>

namespace autolife {

class Logger {
public:
    static Logger& getInstance();
    void initialize(const std::string& log_dir = "logs");
    
    // 가변 인자 템플릿 대신 직접 spdlog 사용
    template<typename... Args>
    void info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        if (main_logger_) {
            main_logger_->info(fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    void warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        if (main_logger_) {
            main_logger_->warn(fmt, std::forward<Args>(args)...);
        }
    }
    
    template<typename... Args>
    void error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
        if (main_logger_) {
            main_logger_->error(fmt, std::forward<Args>(args)...);
        }
    }
    
    void logTrade(const std::string& market, const std::string& side,
                  double price, double volume, double pnl);
    
private:
    Logger() = default;
    std::shared_ptr<spdlog::logger> main_logger_;
    std::shared_ptr<spdlog::logger> trade_logger_;
    bool initialized_ = false;
};

#define LOG_INFO(...) autolife::Logger::getInstance().info(__VA_ARGS__)
#define LOG_WARN(...) autolife::Logger::getInstance().warn(__VA_ARGS__)
#define LOG_ERROR(...) autolife::Logger::getInstance().error(__VA_ARGS__)

} // namespace autolife
