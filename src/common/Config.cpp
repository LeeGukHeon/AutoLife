#include "common/Config.h"
#include "common/PathUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>

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
    if (name == "foundation") {
        return "foundation_adaptive";
    }
    if (name == "foundation_adaptive_strategy") {
        return "foundation_adaptive";
    }
    return name;
}

std::unordered_map<std::string, std::string> parseDotEnvFile(const std::filesystem::path& path) {
    std::unordered_map<std::string, std::string> out;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return out;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trimCopy(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) {
            continue;
        }

        std::string key = trimCopy(line.substr(0, eq_pos));
        std::string value = trimCopy(line.substr(eq_pos + 1));
        if (key.empty()) {
            continue;
        }
        if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
            value.erase(value.begin());
        }
        if (!value.empty() && (value.back() == '"' || value.back() == '\'')) {
            value.pop_back();
        }

        out[key] = trimCopy(value);
    }

    return out;
}

const std::unordered_map<std::string, std::string>& getDotEnvCache() {
    static std::unordered_map<std::string, std::string> cache;
    static bool loaded = false;
    if (loaded) {
        return cache;
    }
    loaded = true;

    std::vector<std::filesystem::path> candidates;
    candidates.push_back(utils::PathUtils::resolveRelativePath(".env"));
    candidates.push_back(utils::PathUtils::resolveRelativePath("../../.env"));
    candidates.push_back(utils::PathUtils::resolveRelativePath("../../../.env"));
    candidates.push_back(std::filesystem::current_path() / ".env");

    for (const auto& candidate : candidates) {
        if (!std::filesystem::exists(candidate)) {
            continue;
        }
        cache = parseDotEnvFile(candidate);
        if (!cache.empty()) {
            std::cout << ".env loaded from: " << candidate << std::endl;
            break;
        }
    }

    return cache;
}

std::string readEnvVar(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) == 0 && value != nullptr && len > 0) {
        std::string out = trimCopy(value);
        free(value);
        if (!out.empty()) {
            return out;
        }
    } else if (value != nullptr) {
        free(value);
    }
#else
    const char* value = std::getenv(name);
    if (value != nullptr) {
        const std::string out = trimCopy(value);
        if (!out.empty()) {
            return out;
        }
    }
#endif

    const auto& dotenv = getDotEnvCache();
    auto it = dotenv.find(name);
    if (it == dotenv.end()) {
        return "";
    }
    return trimCopy(it->second);
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
            std::vector<std::filesystem::path> dotenv_candidates;
            dotenv_candidates.push_back(config_path.parent_path().parent_path().parent_path() / ".env");
            dotenv_candidates.push_back(std::filesystem::current_path() / ".env");

            for (const auto& dotenv_path : dotenv_candidates) {
                if (!std::filesystem::exists(dotenv_path)) {
                    continue;
                }
                const auto env_map = parseDotEnvFile(dotenv_path);
                if (access_key_.empty()) {
                    auto it = env_map.find("UPBIT_ACCESS_KEY");
                    if (it != env_map.end()) {
                        access_key_ = trimCopy(it->second);
                    }
                }
                if (secret_key_.empty()) {
                    auto it = env_map.find("UPBIT_SECRET_KEY");
                    if (it != env_map.end()) {
                        secret_key_ = trimCopy(it->second);
                    }
                }
                if (!access_key_.empty() && !secret_key_.empty()) {
                    std::cout << ".env fallback loaded from: " << dotenv_path << std::endl;
                    break;
                }
            }
        }
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
            engine_config_.allow_live_orders = t.value("allow_live_orders", false);
            engine_config_.enable_live_mtf_dataset_capture =
                t.value("enable_live_mtf_dataset_capture", true);
            engine_config_.live_mtf_dataset_capture_interval_seconds =
                std::max(30, t.value("live_mtf_dataset_capture_interval_seconds", 300));
            engine_config_.live_mtf_dataset_capture_output_dir = trimCopy(
                t.value("live_mtf_dataset_capture_output_dir", "data/backtest_real_live")
            );
            if (engine_config_.live_mtf_dataset_capture_output_dir.empty()) {
                engine_config_.live_mtf_dataset_capture_output_dir = "data/backtest_real_live";
            }
            if (t.contains("live_mtf_dataset_capture_timeframes") &&
                t["live_mtf_dataset_capture_timeframes"].is_array()) {
                std::vector<std::string> tf_list;
                for (const auto& tf : t["live_mtf_dataset_capture_timeframes"]) {
                    if (!tf.is_string()) {
                        continue;
                    }
                    auto normalized = trimCopy(tf.get<std::string>());
                    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (!normalized.empty()) {
                        tf_list.push_back(std::move(normalized));
                    }
                }
                if (!tf_list.empty()) {
                    engine_config_.live_mtf_dataset_capture_timeframes = std::move(tf_list);
                }
            }
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
            engine_config_.enable_strategy_ev_pre_cat_soften_non_severe =
                t.value("enable_strategy_ev_pre_cat_soften_non_severe", false);
            engine_config_.enable_strategy_ev_pre_cat_relaxed_recovery_evidence =
                t.value("enable_strategy_ev_pre_cat_relaxed_recovery_evidence", false);
            engine_config_.enable_strategy_ev_pre_cat_relaxed_recovery_full_history_anchor =
                t.value("enable_strategy_ev_pre_cat_relaxed_recovery_full_history_anchor", false);
            engine_config_.enable_strategy_ev_pre_cat_recovery_evidence_hysteresis =
                t.value("enable_strategy_ev_pre_cat_recovery_evidence_hysteresis", false);
            engine_config_.strategy_ev_pre_cat_recovery_evidence_hysteresis_hold_steps =
                t.value("strategy_ev_pre_cat_recovery_evidence_hysteresis_hold_steps", 12);
            engine_config_.strategy_ev_pre_cat_recovery_evidence_hysteresis_min_trades =
                t.value("strategy_ev_pre_cat_recovery_evidence_hysteresis_min_trades", 8);
            engine_config_.enable_strategy_ev_pre_cat_recovery_quality_hysteresis_relief =
                t.value("enable_strategy_ev_pre_cat_recovery_quality_hysteresis_relief", false);
            engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_strength =
                t.value("strategy_ev_pre_cat_recovery_quality_hysteresis_min_strength", 0.67);
            engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_expected_value =
                t.value("strategy_ev_pre_cat_recovery_quality_hysteresis_min_expected_value", 0.00060);
            engine_config_.strategy_ev_pre_cat_recovery_quality_hysteresis_min_liquidity =
                t.value("strategy_ev_pre_cat_recovery_quality_hysteresis_min_liquidity", 56.0);
            engine_config_.strategy_ev_pre_cat_relaxed_recovery_min_trades =
                t.value("strategy_ev_pre_cat_relaxed_recovery_min_trades", 8);
            engine_config_.strategy_ev_pre_cat_relaxed_recovery_expectancy_gap_krw =
                t.value("strategy_ev_pre_cat_relaxed_recovery_expectancy_gap_krw", 2.0);
            engine_config_.strategy_ev_pre_cat_relaxed_recovery_profit_factor_gap =
                t.value("strategy_ev_pre_cat_relaxed_recovery_profit_factor_gap", 0.12);
            engine_config_.strategy_ev_pre_cat_relaxed_recovery_min_win_rate =
                t.value("strategy_ev_pre_cat_relaxed_recovery_min_win_rate", 0.48);
            engine_config_.enable_strategy_ev_pre_cat_recovery_evidence_bridge =
                t.value("enable_strategy_ev_pre_cat_recovery_evidence_bridge", false);
            engine_config_.enable_strategy_ev_pre_cat_recovery_evidence_bridge_surrogate =
                t.value("enable_strategy_ev_pre_cat_recovery_evidence_bridge_surrogate", false);
            engine_config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_strategy_trades =
                t.value("strategy_ev_pre_cat_recovery_evidence_bridge_max_strategy_trades", 24);
            engine_config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_full_history_pressure =
                t.value("strategy_ev_pre_cat_recovery_evidence_bridge_max_full_history_pressure", 0.96);
            engine_config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_severe_axis_count =
                t.value("strategy_ev_pre_cat_recovery_evidence_bridge_max_severe_axis_count", 2);
            engine_config_.strategy_ev_pre_cat_recovery_evidence_bridge_max_activations_per_key =
                t.value("strategy_ev_pre_cat_recovery_evidence_bridge_max_activations_per_key", 1);
            engine_config_.enable_strategy_ev_pre_cat_pressure_rebound_relief =
                t.value("enable_strategy_ev_pre_cat_pressure_rebound_relief", false);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_strategy_trades =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_max_strategy_trades", 28);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_strength =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_min_strength", 0.74);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_expected_value =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_min_expected_value", 0.00110);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_liquidity =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_min_liquidity", 60.0);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_reward_risk =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_min_reward_risk", 1.25);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_min_full_history_pressure =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_min_full_history_pressure", 0.95);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_recent_history_pressure =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_max_recent_history_pressure", 0.78);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_regime_history_pressure =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_max_regime_history_pressure", 0.80);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_severe_axis_count =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_max_severe_axis_count", 2);
            engine_config_.strategy_ev_pre_cat_pressure_rebound_relief_max_activations_per_key =
                t.value("strategy_ev_pre_cat_pressure_rebound_relief_max_activations_per_key", 1);
            engine_config_.enable_strategy_ev_pre_cat_negative_history_quarantine =
                t.value("enable_strategy_ev_pre_cat_negative_history_quarantine", false);
            engine_config_.strategy_ev_pre_cat_negative_history_quarantine_hold_steps =
                t.value("strategy_ev_pre_cat_negative_history_quarantine_hold_steps", 6);
            engine_config_.strategy_ev_pre_cat_negative_history_quarantine_min_full_history_pressure =
                t.value("strategy_ev_pre_cat_negative_history_quarantine_min_full_history_pressure", 0.95);
            engine_config_.strategy_ev_pre_cat_negative_history_quarantine_max_history_pf =
                t.value("strategy_ev_pre_cat_negative_history_quarantine_max_history_pf", 0.45);
            engine_config_.strategy_ev_pre_cat_negative_history_quarantine_max_history_expectancy_krw =
                t.value("strategy_ev_pre_cat_negative_history_quarantine_max_history_expectancy_krw", -10.0);
            engine_config_.enable_strategy_ev_pre_cat_relaxed_severe_gate =
                t.value("enable_strategy_ev_pre_cat_relaxed_severe_gate", false);
            engine_config_.strategy_ev_pre_cat_relaxed_severe_min_trades =
                t.value("strategy_ev_pre_cat_relaxed_severe_min_trades", 18);
            engine_config_.strategy_ev_pre_cat_relaxed_severe_pressure_threshold =
                t.value("strategy_ev_pre_cat_relaxed_severe_pressure_threshold", 0.78);
            engine_config_.enable_strategy_ev_pre_cat_composite_severe_model =
                t.value("enable_strategy_ev_pre_cat_composite_severe_model", false);
            engine_config_.strategy_ev_pre_cat_composite_pressure_threshold =
                t.value("strategy_ev_pre_cat_composite_pressure_threshold", 0.84);
            engine_config_.strategy_ev_pre_cat_composite_min_critical_signals =
                t.value("strategy_ev_pre_cat_composite_min_critical_signals", 2);
            engine_config_.enable_strategy_ev_pre_cat_sync_guard =
                t.value("enable_strategy_ev_pre_cat_sync_guard", false);
            engine_config_.enable_strategy_ev_pre_cat_contextual_severe_downgrade =
                t.value("enable_strategy_ev_pre_cat_contextual_severe_downgrade", false);
            engine_config_.strategy_ev_pre_cat_contextual_severe_max_pressure =
                t.value("strategy_ev_pre_cat_contextual_severe_max_pressure", 0.90);
            engine_config_.strategy_ev_pre_cat_contextual_severe_max_axis_count =
                t.value("strategy_ev_pre_cat_contextual_severe_max_axis_count", 2);
            engine_config_.enable_strategy_ev_pre_cat_no_soft_quality_relief =
                t.value("enable_strategy_ev_pre_cat_no_soft_quality_relief", false);
            engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_max_strategy_trades =
                t.value("strategy_ev_pre_cat_no_soft_quality_relief_max_strategy_trades", 20);
            engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_min_strength =
                t.value("strategy_ev_pre_cat_no_soft_quality_relief_min_strength", 0.72);
            engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_min_expected_value =
                t.value("strategy_ev_pre_cat_no_soft_quality_relief_min_expected_value", 0.00100);
            engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_min_liquidity =
                t.value("strategy_ev_pre_cat_no_soft_quality_relief_min_liquidity", 58.0);
            engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_min_reward_risk =
                t.value("strategy_ev_pre_cat_no_soft_quality_relief_min_reward_risk", 1.30);
            engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_max_full_history_pressure =
                t.value("strategy_ev_pre_cat_no_soft_quality_relief_max_full_history_pressure", 1.00);
            engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_max_severe_axis_count =
                t.value("strategy_ev_pre_cat_no_soft_quality_relief_max_severe_axis_count", 2);
            engine_config_.strategy_ev_pre_cat_no_soft_quality_relief_max_activations_per_key =
                t.value("strategy_ev_pre_cat_no_soft_quality_relief_max_activations_per_key", 1);
            engine_config_.enable_strategy_ev_pre_cat_candidate_rr_failsafe =
                t.value("enable_strategy_ev_pre_cat_candidate_rr_failsafe", false);
            engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_strategy_trades =
                t.value("strategy_ev_pre_cat_candidate_rr_failsafe_max_strategy_trades", 18);
            engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_strength =
                t.value("strategy_ev_pre_cat_candidate_rr_failsafe_min_strength", 0.73);
            engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_expected_value =
                t.value("strategy_ev_pre_cat_candidate_rr_failsafe_min_expected_value", 0.00095);
            engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_liquidity =
                t.value("strategy_ev_pre_cat_candidate_rr_failsafe_min_liquidity", 59.0);
            engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_min_reward_risk =
                t.value("strategy_ev_pre_cat_candidate_rr_failsafe_min_reward_risk", 1.30);
            engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_full_history_pressure =
                t.value("strategy_ev_pre_cat_candidate_rr_failsafe_max_full_history_pressure", 0.96);
            engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_severe_axis_count =
                t.value("strategy_ev_pre_cat_candidate_rr_failsafe_max_severe_axis_count", 2);
            engine_config_.strategy_ev_pre_cat_candidate_rr_failsafe_max_activations_per_key =
                t.value("strategy_ev_pre_cat_candidate_rr_failsafe_max_activations_per_key", 1);
            engine_config_.strategy_ev_pre_cat_unsynced_override_min_strength =
                t.value("strategy_ev_pre_cat_unsynced_override_min_strength", 0.74);
            engine_config_.strategy_ev_pre_cat_unsynced_override_min_expected_value =
                t.value("strategy_ev_pre_cat_unsynced_override_min_expected_value", 0.00085);
            engine_config_.strategy_ev_pre_cat_unsynced_override_min_liquidity =
                t.value("strategy_ev_pre_cat_unsynced_override_min_liquidity", 60.0);
            engine_config_.strategy_ev_pre_cat_unsynced_override_min_reward_risk =
                t.value("strategy_ev_pre_cat_unsynced_override_min_reward_risk", 1.28);
            engine_config_.strategy_ev_pre_cat_unsynced_override_max_full_history_pressure =
                t.value("strategy_ev_pre_cat_unsynced_override_max_full_history_pressure", 1.00);
            engine_config_.strategy_ev_pre_cat_unsynced_override_max_severe_axis_count =
                t.value("strategy_ev_pre_cat_unsynced_override_max_severe_axis_count", 4);
            engine_config_.avoid_high_volatility = t.value("avoid_high_volatility", true);
            engine_config_.avoid_trending_down = t.value("avoid_trending_down", true);
            engine_config_.enable_core_plane_bridge = t.value("enable_core_plane_bridge", true);
            engine_config_.enable_core_policy_plane = t.value("enable_core_policy_plane", true);
            engine_config_.enable_core_risk_plane = t.value("enable_core_risk_plane", true);
            engine_config_.enable_core_execution_plane = t.value("enable_core_execution_plane", true);
            engine_config_.use_strategy_alpha_head_mode = t.value("use_strategy_alpha_head_mode", false);
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
            engine_config_.enable_entry_quality_adaptive_relief =
                t.value("enable_entry_quality_adaptive_relief", true);
            engine_config_.entry_quality_adaptive_relief_rr_max_gap =
                t.value("entry_quality_adaptive_relief_rr_max_gap", 0.08);
            engine_config_.entry_quality_adaptive_relief_edge_max_gap =
                t.value("entry_quality_adaptive_relief_edge_max_gap", 0.00012);
            engine_config_.entry_quality_adaptive_relief_min_signal_strength =
                t.value("entry_quality_adaptive_relief_min_signal_strength", 0.68);
            engine_config_.entry_quality_adaptive_relief_min_expected_value =
                t.value("entry_quality_adaptive_relief_min_expected_value", 0.00035);
            engine_config_.entry_quality_adaptive_relief_min_liquidity_score =
                t.value("entry_quality_adaptive_relief_min_liquidity_score", 58.0);
            engine_config_.entry_quality_adaptive_relief_min_strategy_trades =
                t.value("entry_quality_adaptive_relief_min_strategy_trades", 16);
            engine_config_.entry_quality_adaptive_relief_min_strategy_win_rate =
                t.value("entry_quality_adaptive_relief_min_strategy_win_rate", 0.47);
            engine_config_.entry_quality_adaptive_relief_min_strategy_profit_factor =
                t.value("entry_quality_adaptive_relief_min_strategy_profit_factor", 0.98);
            engine_config_.entry_quality_adaptive_relief_block_high_stress_regime =
                t.value("entry_quality_adaptive_relief_block_high_stress_regime", true);
            engine_config_.entry_quality_adaptive_relief_position_scale =
                t.value("entry_quality_adaptive_relief_position_scale", 0.80);
            engine_config_.second_stage_history_safety_severe_scale =
                t.value("second_stage_history_safety_severe_scale", 1.00);
            engine_config_.enable_second_stage_history_safety_severe_relief =
                t.value("enable_second_stage_history_safety_severe_relief", true);
            engine_config_.second_stage_history_safety_relief_max_scale =
                t.value("second_stage_history_safety_relief_max_scale", 0.45);
            engine_config_.second_stage_history_safety_relief_min_strategy_trades =
                t.value("second_stage_history_safety_relief_min_strategy_trades", 12);
            engine_config_.second_stage_history_safety_relief_min_signal_strength =
                t.value("second_stage_history_safety_relief_min_signal_strength", 0.68);
            engine_config_.second_stage_history_safety_relief_min_expected_value =
                t.value("second_stage_history_safety_relief_min_expected_value", 0.00045);
            engine_config_.second_stage_history_safety_relief_min_liquidity_score =
                t.value("second_stage_history_safety_relief_min_liquidity_score", 58.0);
            engine_config_.second_stage_history_safety_relief_block_hostile_regime =
                t.value("second_stage_history_safety_relief_block_hostile_regime", false);
            engine_config_.enable_two_head_entry_second_stage_aggregation =
                t.value("enable_two_head_entry_second_stage_aggregation", true);
            engine_config_.two_head_entry_quality_weight =
                t.value("two_head_entry_quality_weight", 0.55);
            engine_config_.two_head_second_stage_weight =
                t.value("two_head_second_stage_weight", 0.45);
            engine_config_.two_head_min_entry_quality_score =
                t.value("two_head_min_entry_quality_score", 0.90);
            engine_config_.two_head_min_second_stage_score =
                t.value("two_head_min_second_stage_score", 0.88);
            engine_config_.two_head_min_aggregate_score =
                t.value("two_head_min_aggregate_score", 0.98);
            engine_config_.two_head_aggregation_block_high_stress_regime =
                t.value("two_head_aggregation_block_high_stress_regime", false);
            engine_config_.two_head_aggregation_min_strategy_trades =
                t.value("two_head_aggregation_min_strategy_trades", 8);
            engine_config_.enable_two_head_rr_margin_near_miss_floor_relax =
                t.value("enable_two_head_rr_margin_near_miss_floor_relax", false);
            engine_config_.two_head_rr_margin_near_miss_second_stage_floor_relax =
                t.value("two_head_rr_margin_near_miss_second_stage_floor_relax", 0.06);
            engine_config_.two_head_rr_margin_near_miss_aggregate_floor_relax =
                t.value("two_head_rr_margin_near_miss_aggregate_floor_relax", 0.03);
            engine_config_.enable_two_head_rr_margin_near_miss_adaptive_floor_relax =
                t.value("enable_two_head_rr_margin_near_miss_adaptive_floor_relax", false);
            engine_config_.two_head_rr_margin_near_miss_adaptive_floor_relax_min_activation =
                t.value("two_head_rr_margin_near_miss_adaptive_floor_relax_min_activation", 0.45);
            engine_config_.two_head_rr_margin_near_miss_adaptive_floor_relax_max_second_stage =
                t.value("two_head_rr_margin_near_miss_adaptive_floor_relax_max_second_stage", 0.08);
            engine_config_.two_head_rr_margin_near_miss_adaptive_floor_relax_max_aggregate =
                t.value("two_head_rr_margin_near_miss_adaptive_floor_relax_max_aggregate", 0.04);
            engine_config_.two_head_rr_margin_near_miss_adaptive_floor_relax_quality_weight =
                t.value("two_head_rr_margin_near_miss_adaptive_floor_relax_quality_weight", 0.55);
            engine_config_.two_head_rr_margin_near_miss_adaptive_floor_relax_gap_weight =
                t.value("two_head_rr_margin_near_miss_adaptive_floor_relax_gap_weight", 0.45);
            engine_config_.enable_two_head_rr_margin_near_miss_surplus_compensation =
                t.value("enable_two_head_rr_margin_near_miss_surplus_compensation", false);
            engine_config_.two_head_rr_margin_near_miss_surplus_min_entry_surplus =
                t.value("two_head_rr_margin_near_miss_surplus_min_entry_surplus", 0.05);
            engine_config_.two_head_rr_margin_near_miss_surplus_min_edge_score =
                t.value("two_head_rr_margin_near_miss_surplus_min_edge_score", 1.03);
            engine_config_.two_head_rr_margin_near_miss_surplus_max_second_stage_deficit =
                t.value("two_head_rr_margin_near_miss_surplus_max_second_stage_deficit", 0.05);
            engine_config_.two_head_rr_margin_near_miss_surplus_max_aggregate_deficit =
                t.value("two_head_rr_margin_near_miss_surplus_max_aggregate_deficit", 0.04);
            engine_config_.two_head_rr_margin_near_miss_surplus_entry_weight =
                t.value("two_head_rr_margin_near_miss_surplus_entry_weight", 0.35);
            engine_config_.two_head_rr_margin_near_miss_surplus_max_aggregate_bonus =
                t.value("two_head_rr_margin_near_miss_surplus_max_aggregate_bonus", 0.05);
            engine_config_.enable_second_stage_rr_margin_near_miss_relief =
                t.value("enable_second_stage_rr_margin_near_miss_relief", true);
            engine_config_.second_stage_rr_margin_near_miss_max_gap =
                t.value("second_stage_rr_margin_near_miss_max_gap", 0.02);
            engine_config_.second_stage_rr_margin_near_miss_min_signal_strength =
                t.value("second_stage_rr_margin_near_miss_min_signal_strength", 0.70);
            engine_config_.second_stage_rr_margin_near_miss_min_expected_value =
                t.value("second_stage_rr_margin_near_miss_min_expected_value", 0.00055);
            engine_config_.second_stage_rr_margin_near_miss_min_liquidity_score =
                t.value("second_stage_rr_margin_near_miss_min_liquidity_score", 60.0);
            engine_config_.second_stage_rr_margin_near_miss_min_strategy_trades =
                t.value("second_stage_rr_margin_near_miss_min_strategy_trades", 12);
            engine_config_.second_stage_rr_margin_near_miss_block_high_stress_regime =
                t.value("second_stage_rr_margin_near_miss_block_high_stress_regime", true);
            engine_config_.second_stage_rr_margin_near_miss_score_boost =
                t.value("second_stage_rr_margin_near_miss_score_boost", 0.08);
            engine_config_.enable_second_stage_rr_margin_soft_score =
                t.value("enable_second_stage_rr_margin_soft_score", false);
            engine_config_.second_stage_rr_margin_soft_score_max_gap =
                t.value("second_stage_rr_margin_soft_score_max_gap", 0.02);
            engine_config_.second_stage_rr_margin_soft_score_floor =
                t.value("second_stage_rr_margin_soft_score_floor", 0.70);
            engine_config_.second_stage_rr_margin_soft_score_gap_tightness_weight =
                t.value("second_stage_rr_margin_soft_score_gap_tightness_weight", 0.20);
            engine_config_.enable_second_stage_rr_margin_near_miss_head_score_floor =
                t.value("enable_second_stage_rr_margin_near_miss_head_score_floor", false);
            engine_config_.second_stage_rr_margin_near_miss_head_score_floor_base =
                t.value("second_stage_rr_margin_near_miss_head_score_floor_base", 0.0);
            engine_config_.second_stage_rr_margin_near_miss_head_score_floor_quality_weight =
                t.value("second_stage_rr_margin_near_miss_head_score_floor_quality_weight", 0.0);
            engine_config_.second_stage_rr_margin_near_miss_head_score_floor_gap_weight =
                t.value("second_stage_rr_margin_near_miss_head_score_floor_gap_weight", 0.0);
            engine_config_.second_stage_rr_margin_near_miss_head_score_floor_max =
                t.value("second_stage_rr_margin_near_miss_head_score_floor_max", 1.0);

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
