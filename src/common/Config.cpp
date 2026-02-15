#include "common/Config.h"
#include "common/PathUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace autolife {

namespace {
std::string trimCopy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string normalizeStrategyName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    name = trimCopy(name);

    // Backward compatibility alias
    if (name == "grid") {
        return "grid_trading";
    }
    return name;
}

std::string readEnvVar(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr || len == 0) {
        if (value != nullptr) {
            free(value);
        }
        return "";
    }
    std::string out = trimCopy(value);
    free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? trimCopy(value) : "";
#endif
}
}

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

void Config::load(const std::string& path) {
    try {
        std::filesystem::path config_path;
        if (std::filesystem::path(path).is_absolute()) {
            config_path = path;
        } else {
            config_path = utils::PathUtils::resolveRelativePath(path);
        }

        std::cout << "설정 파일 경로: " << config_path << std::endl;

        if (!std::filesystem::exists(config_path)) {
            std::cout << "경고: 설정 파일을 찾을 수 없습니다: " << config_path << std::endl;
            std::cout << "기본값을 사용합니다." << std::endl;
            return;
        }

        std::ifstream file(config_path);
        if (!file.is_open()) {
            std::cout << "경고: 설정 파일을 열 수 없습니다." << std::endl;
            return;
        }

        nlohmann::json j;
        file >> j;

        if (j.contains("api")) {
            const std::string file_access_key = trimCopy(j["api"].value("access_key", ""));
            const std::string file_secret_key = trimCopy(j["api"].value("secret_key", ""));
            if (!file_access_key.empty() || !file_secret_key.empty()) {
                std::cout << "경고: config api 키 값은 무시됩니다. 환경 변수(UPBIT_ACCESS_KEY/UPBIT_SECRET_KEY)를 사용하세요."
                          << std::endl;
            }
        }

        access_key_ = readEnvVar("UPBIT_ACCESS_KEY");
        secret_key_ = readEnvVar("UPBIT_SECRET_KEY");
        if (access_key_.empty() || secret_key_.empty()) {
            std::cout << "경고: UPBIT_ACCESS_KEY 또는 UPBIT_SECRET_KEY 환경 변수가 비어 있습니다." << std::endl;
        }

        if (j.contains("trading")) {
            auto& t = j["trading"];

            initial_capital_ = t.value("initial_capital", 50000.0);
            max_drawdown_ = t.value("max_drawdown", 0.15);
            position_size_ratio_ = t.value("position_size_ratio", 0.01);
            log_level_ = t.value("log_level", "info");

            std::string mode_str = t.value("mode", "PAPER");
            engine_config_.mode = (mode_str == "LIVE") ? engine::TradingMode::LIVE : engine::TradingMode::PAPER;
            engine_config_.dry_run = t.value("dry_run", false);
            engine_config_.initial_capital = initial_capital_;

            engine_config_.scan_interval_seconds = t.value("scan_interval_seconds", 60);
            engine_config_.min_volume_krw = t.value("min_volume_krw", 1000000000LL);

            engine_config_.max_positions = t.value("max_positions", 10);
            engine_config_.max_daily_trades = t.value("max_daily_trades", 50);
            engine_config_.max_drawdown = max_drawdown_;

            engine_config_.max_daily_loss_krw = t.value("max_daily_loss_krw", 50000.0);
            engine_config_.max_order_krw = t.value("max_order_krw", 500000.0);
            engine_config_.min_order_krw = t.value("min_order_krw", 5000.0);
            engine_config_.small_account_tier1_capital_krw = t.value("small_account_tier1_capital_krw", 60000.0);
            engine_config_.small_account_tier2_capital_krw = t.value("small_account_tier2_capital_krw", 100000.0);
            engine_config_.small_account_tier1_max_order_pct = t.value("small_account_tier1_max_order_pct", 0.20);
            engine_config_.small_account_tier2_max_order_pct = t.value("small_account_tier2_max_order_pct", 0.15);
            engine_config_.order_fee_reserve_pct = t.value("order_fee_reserve_pct", 0.001);
            engine_config_.max_new_orders_per_scan = t.value("max_new_orders_per_scan", 2);
            engine_config_.max_exposure_pct = t.value("max_exposure_pct", 0.85);
            engine_config_.max_daily_loss_pct = t.value("max_daily_loss_pct", 0.05);
            engine_config_.risk_per_trade_pct = t.value("risk_per_trade_pct", 0.005);
            engine_config_.max_slippage_pct = t.value("max_slippage_pct", 0.003);
            engine_config_.min_expected_edge_pct = t.value("min_expected_edge_pct", 0.0010);
            engine_config_.min_reward_risk = t.value("min_reward_risk", 1.20);
            engine_config_.min_rr_weak_signal = t.value("min_rr_weak_signal", 1.80);
            engine_config_.min_rr_strong_signal = t.value("min_rr_strong_signal", 1.25);
            engine_config_.min_strategy_trades_for_ev = t.value("min_strategy_trades_for_ev", 30);
            engine_config_.min_strategy_expectancy_krw = t.value("min_strategy_expectancy_krw", 0.0);
            engine_config_.min_strategy_profit_factor = t.value("min_strategy_profit_factor", 1.00);
            engine_config_.avoid_high_volatility = t.value("avoid_high_volatility", true);
            engine_config_.avoid_trending_down = t.value("avoid_trending_down", true);
            engine_config_.enable_core_plane_bridge = t.value("enable_core_plane_bridge", false);
            engine_config_.enable_core_policy_plane = t.value("enable_core_policy_plane", false);
            engine_config_.enable_core_risk_plane = t.value("enable_core_risk_plane", false);
            engine_config_.enable_core_execution_plane = t.value("enable_core_execution_plane", false);
            engine_config_.hostility_ewma_alpha = t.value("hostility_ewma_alpha", 0.14);
            engine_config_.hostility_hostile_threshold = t.value("hostility_hostile_threshold", 0.62);
            engine_config_.hostility_severe_threshold = t.value("hostility_severe_threshold", 0.82);
            engine_config_.hostility_extreme_threshold = t.value("hostility_extreme_threshold", 0.88);
            engine_config_.hostility_pause_scans = t.value("hostility_pause_scans", 4);
            engine_config_.hostility_pause_scans_extreme = t.value("hostility_pause_scans_extreme", 6);
            engine_config_.hostility_pause_recent_sample_min = t.value("hostility_pause_recent_sample_min", 10);
            engine_config_.hostility_pause_recent_expectancy_krw = t.value("hostility_pause_recent_expectancy_krw", 0.0);
            engine_config_.hostility_pause_recent_win_rate = t.value("hostility_pause_recent_win_rate", 0.40);
            engine_config_.backtest_hostility_pause_candles = t.value("backtest_hostility_pause_candles", 36);
            engine_config_.backtest_hostility_pause_candles_extreme = t.value("backtest_hostility_pause_candles_extreme", 60);

            if (t.contains("enabled_strategies")) {
                engine_config_.enabled_strategies = t["enabled_strategies"].get<std::vector<std::string>>();
                for (auto& strategy_name : engine_config_.enabled_strategies) {
                    strategy_name = normalizeStrategyName(strategy_name);
                }
            }

            fee_rate_ = t.value("fee_rate", 0.0005);
            min_order_krw_ = t.value("min_order_krw", 5000.0);
            max_slippage_pct_ = t.value("max_slippage_pct", 0.003);
            risk_per_trade_pct_ = t.value("risk_per_trade_pct", 0.01);
        }

        if (j.contains("strategies") && j["strategies"].contains("scalping")) {
            auto& s = j["strategies"]["scalping"];
            scalping_config_.max_daily_trades = s.value("max_daily_trades", 15);
            scalping_config_.max_hourly_trades = s.value("max_hourly_trades", 5);
            scalping_config_.max_consecutive_losses = s.value("max_consecutive_losses", 5);
            scalping_config_.rsi_lower = s.value("rsi_lower", 20.0);
            scalping_config_.rsi_upper = s.value("rsi_upper", 75.0);
            scalping_config_.volume_z_score_threshold = s.value("volume_z_score_threshold", 1.15);
            scalping_config_.base_take_profit = s.value("base_take_profit", 0.02);
            scalping_config_.base_stop_loss = s.value("base_stop_loss", 0.01);
            scalping_config_.min_risk_reward_ratio = s.value("min_risk_reward_ratio", 1.5);
            scalping_config_.min_signal_strength = s.value("min_signal_strength", 0.65);
        }

        if (j.contains("strategies") && j["strategies"].contains("momentum")) {
            auto& s = j["strategies"]["momentum"];
            momentum_config_.max_daily_trades = s.value("max_daily_trades", 12);
            momentum_config_.max_hourly_trades = s.value("max_hourly_trades", 4);
            momentum_config_.max_consecutive_losses = s.value("max_consecutive_losses", 4);
            momentum_config_.min_liquidity_score = s.value("min_liquidity_score", 50.0);
            momentum_config_.min_signal_strength = s.value("min_signal_strength", 0.40);
            momentum_config_.min_signal_interval_sec = s.value("min_signal_interval_sec", 300);
            momentum_config_.min_risk_reward_ratio = s.value("min_risk_reward_ratio", 2.0);
            momentum_config_.min_expected_sharpe = s.value("min_expected_sharpe", 1.0);
        }

        if (j.contains("strategies") && j["strategies"].contains("breakout")) {
            auto& s = j["strategies"]["breakout"];
            breakout_config_.max_daily_trades = s.value("max_daily_trades", 10);
            breakout_config_.max_hourly_trades = s.value("max_hourly_trades", 3);
            breakout_config_.max_consecutive_losses = s.value("max_consecutive_losses", 4);
            breakout_config_.min_liquidity_score = s.value("min_liquidity_score", 50.0);
            breakout_config_.min_signal_strength = s.value("min_signal_strength", 0.40);
            breakout_config_.min_signal_interval_sec = s.value("min_signal_interval_sec", 720);
            breakout_config_.min_risk_reward_ratio = s.value("min_risk_reward_ratio", 1.5);
        }

        if (j.contains("strategies") && j["strategies"].contains("mean_reversion")) {
            auto& s = j["strategies"]["mean_reversion"];
            mean_reversion_config_.max_daily_trades = s.value("max_daily_trades", 12);
            mean_reversion_config_.max_hourly_trades = s.value("max_hourly_trades", 4);
            mean_reversion_config_.max_consecutive_losses = s.value("max_consecutive_losses", 4);
            mean_reversion_config_.min_liquidity_score = s.value("min_liquidity_score", 50.0);
            mean_reversion_config_.min_signal_strength = s.value("min_signal_strength", 0.40);
            mean_reversion_config_.min_signal_interval_sec = s.value("min_signal_interval_sec", 600);
            mean_reversion_config_.min_reversion_probability = s.value("min_reversion_probability", 0.70);
        }

        if (j.contains("strategies") && j["strategies"].contains("grid_trading")) {
            auto& s = j["strategies"]["grid_trading"];
            grid_trading_config_.max_daily_trades = s.value("max_daily_trades", 15);
            grid_trading_config_.max_hourly_trades = s.value("max_hourly_trades", 5);
            grid_trading_config_.max_consecutive_losses = s.value("max_consecutive_losses", 3);
            grid_trading_config_.min_liquidity_score = s.value("min_liquidity_score", 60.0);
            grid_trading_config_.min_signal_strength = s.value("min_signal_strength", 0.40);
            grid_trading_config_.min_signal_interval_sec = s.value("min_signal_interval_sec", 900);
            grid_trading_config_.max_grid_capital_pct = s.value("max_grid_capital_pct", 0.30);
        }

        std::cout << "설정 파일 로드 완료" << std::endl;
        std::cout << "Config Loaded: Fee=" << fee_rate_ << ", MinOrder=" << min_order_krw_ << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "설정 로드 오류: " << e.what() << std::endl;
    }
}

} // namespace autolife
