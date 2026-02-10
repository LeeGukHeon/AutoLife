#include "common/Logger.h"
#include "common/PathUtils.h"
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace autolife {

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::initialize(const std::string& log_dir) {
    if (initialized_) return;
    
    // 실행 파일 기준 로그 경로
    std::filesystem::path logs_path;
    if (std::filesystem::path(log_dir).is_absolute()) {
        logs_path = log_dir;
    } else {
        logs_path = utils::PathUtils::resolveRelativePath(log_dir);
    }
    
    std::filesystem::create_directories(logs_path);
    
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] %v");
        
        // 파일 싱크 - UTF-8 인코딩 지원
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logs_path.string() + "/autolife.log", 1024 * 1024 * 10, 3
        );
        
        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
        main_logger_ = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());
        main_logger_->set_level(spdlog::level::info);
        // UTF-8 인코딩 보장
        main_logger_->flush_on(spdlog::level::warn);
        spdlog::register_logger(main_logger_);
        
        trade_logger_ = spdlog::daily_logger_mt("trade", logs_path.string() + "/trades.log");
        trade_logger_->set_pattern("%v");
        
        initialized_ = true;
        main_logger_->info("Logger initialized");
        main_logger_->info("Log directory: {}", logs_path.string());
        
    } catch (const std::exception& ex) {
        throw std::runtime_error("Log init failed");
    }
}

void Logger::logTrade(const std::string& market, const std::string& side,
                      double price, double volume, double pnl) {
    if (trade_logger_) {
        std::ostringstream oss;
        oss << market << "," << side << "," 
            << std::fixed << std::setprecision(8) << price << ","
            << std::fixed << std::setprecision(8) << volume << ","
            << std::fixed << std::setprecision(2) << pnl;
        trade_logger_->info(oss.str());
    }
}

} // namespace autolife
