#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace autolife {

class Config {
public:
    static Config& getInstance();
    void load(const std::string& config_path);
    
    std::string getAccessKey() const { return access_key_; }
    std::string getSecretKey() const { return secret_key_; }
    double getInitialCapital() const { return initial_capital_; }
    double getMaxDrawdown() const { return max_drawdown_; }
    double getPositionSizeRatio() const { return position_size_ratio_; }
    std::string getLogLevel() const { return log_level_; }
    
private:
    Config() = default;
    std::string access_key_;
    std::string secret_key_;
    double initial_capital_ = 1000000.0;
    double max_drawdown_ = 0.15;
    double position_size_ratio_ = 0.01;
    std::string log_level_ = "info";
};

} // namespace autolife
