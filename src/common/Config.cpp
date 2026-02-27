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
#include <nlohmann/json.hpp>

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
            engine_config_.live_paper_use_fixed_initial_capital =
                t.value("live_paper_use_fixed_initial_capital", false);
            engine_config_.live_paper_fixed_initial_capital_krw = std::max(
                0.0,
                t.value("live_paper_fixed_initial_capital_krw", initial_capital_)
            );
            engine_config_.use_confirmed_candle_only_for_signals =
                t.value("use_confirmed_candle_only_for_signals", true);
            engine_config_.enable_realtime_entry_veto =
                t.value("enable_realtime_entry_veto", true);
            engine_config_.realtime_entry_veto_tracking_window_seconds = std::max(
                10,
                t.value("realtime_entry_veto_tracking_window_seconds", 90)
            );
            engine_config_.realtime_entry_veto_max_drop_pct = std::clamp(
                t.value("realtime_entry_veto_max_drop_pct", 0.0035),
                0.0002,
                0.05
            );
            engine_config_.realtime_entry_veto_max_spread_pct = std::clamp(
                t.value("realtime_entry_veto_max_spread_pct", 0.0030),
                0.0001,
                0.05
            );
            engine_config_.realtime_entry_veto_min_orderbook_imbalance = std::clamp(
                t.value("realtime_entry_veto_min_orderbook_imbalance", -0.35),
                -1.0,
                1.0
            );
            engine_config_.execution_guard_veto_quality_strength_center = std::clamp(
                t.value("execution_guard_veto_quality_strength_center", 0.60),
                0.0,
                1.0
            );
            engine_config_.execution_guard_veto_quality_strength_scale = std::clamp(
                t.value("execution_guard_veto_quality_strength_scale", 0.25),
                0.001,
                10.0
            );
            engine_config_.execution_guard_veto_quality_liquidity_center = std::clamp(
                t.value("execution_guard_veto_quality_liquidity_center", 58.0),
                0.0,
                10000.0
            );
            engine_config_.execution_guard_veto_quality_liquidity_scale = std::clamp(
                t.value("execution_guard_veto_quality_liquidity_scale", 20.0),
                0.001,
                10000.0
            );
            engine_config_.execution_guard_veto_quality_strength_weight = std::clamp(
                t.value("execution_guard_veto_quality_strength_weight", 0.55),
                0.0,
                1.0
            );
            engine_config_.execution_guard_veto_quality_liquidity_weight = std::clamp(
                t.value("execution_guard_veto_quality_liquidity_weight", 0.45),
                0.0,
                1.0
            );
            engine_config_.execution_guard_veto_hostile_tighten_add = std::clamp(
                t.value("execution_guard_veto_hostile_tighten_add", 0.12),
                -1.0,
                1.0
            );
            engine_config_.execution_guard_veto_uptrend_relief_strength_threshold = std::clamp(
                t.value("execution_guard_veto_uptrend_relief_strength_threshold", 0.75),
                0.0,
                1.0
            );
            engine_config_.execution_guard_veto_uptrend_relief_liquidity_threshold = std::clamp(
                t.value("execution_guard_veto_uptrend_relief_liquidity_threshold", 65.0),
                0.0,
                10000.0
            );
            engine_config_.execution_guard_veto_uptrend_relief_add = std::clamp(
                t.value("execution_guard_veto_uptrend_relief_add", 0.08),
                -1.0,
                1.0
            );
            engine_config_.execution_guard_veto_max_drop_tighten_scale = std::clamp(
                t.value("execution_guard_veto_max_drop_tighten_scale", 0.35),
                0.0,
                10.0
            );
            engine_config_.execution_guard_veto_max_drop_relief_scale = std::clamp(
                t.value("execution_guard_veto_max_drop_relief_scale", 0.15),
                0.0,
                10.0
            );
            engine_config_.execution_guard_veto_max_spread_tighten_scale = std::clamp(
                t.value("execution_guard_veto_max_spread_tighten_scale", 0.40),
                0.0,
                10.0
            );
            engine_config_.execution_guard_veto_max_spread_relief_scale = std::clamp(
                t.value("execution_guard_veto_max_spread_relief_scale", 0.18),
                0.0,
                10.0
            );
            engine_config_.execution_guard_veto_min_imbalance_tighten_scale = std::clamp(
                t.value("execution_guard_veto_min_imbalance_tighten_scale", 0.22),
                0.0,
                10.0
            );
            engine_config_.execution_guard_veto_min_imbalance_relief_scale = std::clamp(
                t.value("execution_guard_veto_min_imbalance_relief_scale", 0.12),
                0.0,
                10.0
            );
            engine_config_.execution_guard_veto_max_drop_clamp_min = std::clamp(
                t.value("execution_guard_veto_max_drop_clamp_min", 0.0004),
                0.0,
                1.0
            );
            engine_config_.execution_guard_veto_max_drop_clamp_max = std::clamp(
                t.value("execution_guard_veto_max_drop_clamp_max", 0.0200),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_veto_max_drop_clamp_max <
                engine_config_.execution_guard_veto_max_drop_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_veto_max_drop_clamp_min,
                    engine_config_.execution_guard_veto_max_drop_clamp_max
                );
            }
            engine_config_.execution_guard_veto_max_spread_clamp_min = std::clamp(
                t.value("execution_guard_veto_max_spread_clamp_min", 0.0002),
                0.0,
                1.0
            );
            engine_config_.execution_guard_veto_max_spread_clamp_max = std::clamp(
                t.value("execution_guard_veto_max_spread_clamp_max", 0.0150),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_veto_max_spread_clamp_max <
                engine_config_.execution_guard_veto_max_spread_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_veto_max_spread_clamp_min,
                    engine_config_.execution_guard_veto_max_spread_clamp_max
                );
            }
            engine_config_.execution_guard_veto_min_imbalance_clamp_min = std::clamp(
                t.value("execution_guard_veto_min_imbalance_clamp_min", -0.90),
                -1.0,
                1.0
            );
            engine_config_.execution_guard_veto_min_imbalance_clamp_max = std::clamp(
                t.value("execution_guard_veto_min_imbalance_clamp_max", -0.05),
                -1.0,
                1.0
            );
            if (engine_config_.execution_guard_veto_min_imbalance_clamp_max <
                engine_config_.execution_guard_veto_min_imbalance_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_veto_min_imbalance_clamp_min,
                    engine_config_.execution_guard_veto_min_imbalance_clamp_max
                );
            }
            engine_config_.execution_guard_live_scan_volume_quantile_median = std::clamp(
                t.value("execution_guard_live_scan_volume_quantile_median", 0.50),
                0.0,
                1.0
            );
            engine_config_.execution_guard_live_scan_volume_quantile_p70 = std::clamp(
                t.value("execution_guard_live_scan_volume_quantile_p70", 0.70),
                0.0,
                1.0
            );
            engine_config_.execution_guard_live_scan_base_spread_multiplier = std::clamp(
                t.value("execution_guard_live_scan_base_spread_multiplier", 1.25),
                0.0,
                10.0
            );
            engine_config_.execution_guard_live_scan_base_spread_clamp_min = std::clamp(
                t.value("execution_guard_live_scan_base_spread_clamp_min", 0.0005),
                0.0,
                1.0
            );
            engine_config_.execution_guard_live_scan_base_spread_clamp_max = std::clamp(
                t.value("execution_guard_live_scan_base_spread_clamp_max", 0.01),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_live_scan_base_spread_clamp_max <
                engine_config_.execution_guard_live_scan_base_spread_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_live_scan_base_spread_clamp_min,
                    engine_config_.execution_guard_live_scan_base_spread_clamp_max
                );
            }
            engine_config_.execution_guard_live_scan_base_floor_krw = std::clamp(
                t.value("execution_guard_live_scan_base_floor_krw", 50000000.0),
                0.0,
                1.0e15
            );
            engine_config_.execution_guard_live_scan_base_floor_min_order_multiplier = std::clamp(
                t.value("execution_guard_live_scan_base_floor_min_order_multiplier", 2000.0),
                0.0,
                1.0e9
            );
            engine_config_.execution_guard_live_scan_volume_tighten_base = std::clamp(
                t.value("execution_guard_live_scan_volume_tighten_base", 0.80),
                0.0,
                10.0
            );
            engine_config_.execution_guard_live_scan_volume_tighten_scale = std::clamp(
                t.value("execution_guard_live_scan_volume_tighten_scale", 0.45),
                0.0,
                10.0
            );
            engine_config_.execution_guard_live_scan_universe_anchor_base = std::clamp(
                t.value("execution_guard_live_scan_universe_anchor_base", 0.85),
                0.0,
                10.0
            );
            engine_config_.execution_guard_live_scan_universe_anchor_tighten_scale = std::clamp(
                t.value("execution_guard_live_scan_universe_anchor_tighten_scale", 0.20),
                0.0,
                10.0
            );
            engine_config_.execution_guard_live_scan_dynamic_ceiling_base_volume_multiplier = std::clamp(
                t.value("execution_guard_live_scan_dynamic_ceiling_base_volume_multiplier", 2.0),
                0.0,
                1000.0
            );
            engine_config_.execution_guard_live_scan_dynamic_ceiling_p70_multiplier = std::clamp(
                t.value("execution_guard_live_scan_dynamic_ceiling_p70_multiplier", 1.40),
                0.0,
                1000.0
            );
            engine_config_.execution_guard_live_scan_spread_tighten_scale = std::clamp(
                t.value("execution_guard_live_scan_spread_tighten_scale", 0.30),
                0.0,
                10.0
            );
            engine_config_.execution_guard_live_scan_final_spread_clamp_min = std::clamp(
                t.value("execution_guard_live_scan_final_spread_clamp_min", 0.0003),
                0.0,
                1.0
            );
            engine_config_.execution_guard_live_scan_final_spread_clamp_max = std::clamp(
                t.value("execution_guard_live_scan_final_spread_clamp_max", 0.01),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_live_scan_final_spread_clamp_max <
                engine_config_.execution_guard_live_scan_final_spread_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_live_scan_final_spread_clamp_min,
                    engine_config_.execution_guard_live_scan_final_spread_clamp_max
                );
            }
            engine_config_.execution_guard_live_scan_ask_notional_base_multiplier = std::clamp(
                t.value("execution_guard_live_scan_ask_notional_base_multiplier", 5.0),
                0.0,
                1000.0
            );
            engine_config_.execution_guard_live_scan_ask_notional_tighten_scale = std::clamp(
                t.value("execution_guard_live_scan_ask_notional_tighten_scale", 2.0),
                0.0,
                1000.0
            );
            engine_config_.execution_guard_live_scan_ask_notional_min_multiplier = std::clamp(
                t.value("execution_guard_live_scan_ask_notional_min_multiplier", 3.0),
                0.0,
                1000.0
            );
            engine_config_.execution_guard_live_scan_ask_notional_max_multiplier = std::clamp(
                t.value("execution_guard_live_scan_ask_notional_max_multiplier", 12.0),
                0.0,
                1000.0
            );
            if (engine_config_.execution_guard_live_scan_ask_notional_max_multiplier <
                engine_config_.execution_guard_live_scan_ask_notional_min_multiplier) {
                std::swap(
                    engine_config_.execution_guard_live_scan_ask_notional_min_multiplier,
                    engine_config_.execution_guard_live_scan_ask_notional_max_multiplier
                );
            }
            engine_config_.execution_guard_slippage_buy_hostile_tighten_add = std::clamp(
                t.value("execution_guard_slippage_buy_hostile_tighten_add", 0.15),
                -1.0,
                1.0
            );
            engine_config_.execution_guard_slippage_buy_low_liquidity_threshold = std::clamp(
                t.value("execution_guard_slippage_buy_low_liquidity_threshold", 55.0),
                0.0,
                10000.0
            );
            engine_config_.execution_guard_slippage_buy_low_liquidity_scale = std::clamp(
                t.value("execution_guard_slippage_buy_low_liquidity_scale", 25.0),
                0.001,
                10000.0
            );
            engine_config_.execution_guard_slippage_buy_low_liquidity_tighten_cap = std::clamp(
                t.value("execution_guard_slippage_buy_low_liquidity_tighten_cap", 0.25),
                0.0,
                10.0
            );
            engine_config_.execution_guard_slippage_buy_regime_quality_relief_add = std::clamp(
                t.value("execution_guard_slippage_buy_regime_quality_relief_add", 0.10),
                -1.0,
                1.0
            );
            engine_config_.execution_guard_slippage_buy_quality_strength_center = std::clamp(
                t.value("execution_guard_slippage_buy_quality_strength_center", 0.65),
                0.0,
                1.0
            );
            engine_config_.execution_guard_slippage_buy_quality_strength_scale = std::clamp(
                t.value("execution_guard_slippage_buy_quality_strength_scale", 0.25),
                0.001,
                10.0
            );
            engine_config_.execution_guard_slippage_buy_quality_strength_weight = std::clamp(
                t.value("execution_guard_slippage_buy_quality_strength_weight", 0.20),
                0.0,
                10.0
            );
            engine_config_.execution_guard_slippage_buy_quality_liquidity_center = std::clamp(
                t.value("execution_guard_slippage_buy_quality_liquidity_center", 60.0),
                0.0,
                10000.0
            );
            engine_config_.execution_guard_slippage_buy_quality_liquidity_scale = std::clamp(
                t.value("execution_guard_slippage_buy_quality_liquidity_scale", 20.0),
                0.001,
                10000.0
            );
            engine_config_.execution_guard_slippage_buy_quality_liquidity_weight = std::clamp(
                t.value("execution_guard_slippage_buy_quality_liquidity_weight", 0.15),
                0.0,
                10.0
            );
            engine_config_.execution_guard_slippage_buy_quality_ev_center = std::clamp(
                t.value("execution_guard_slippage_buy_quality_ev_center", 0.0004),
                -1.0,
                1.0
            );
            engine_config_.execution_guard_slippage_buy_quality_ev_scale = std::clamp(
                t.value("execution_guard_slippage_buy_quality_ev_scale", 0.0010),
                1e-9,
                10.0
            );
            engine_config_.execution_guard_slippage_buy_quality_ev_weight = std::clamp(
                t.value("execution_guard_slippage_buy_quality_ev_weight", 0.12),
                0.0,
                10.0
            );
            engine_config_.execution_guard_slippage_buy_quality_tighten_dampen = std::clamp(
                t.value("execution_guard_slippage_buy_quality_tighten_dampen", 0.60),
                0.0,
                10.0
            );
            engine_config_.execution_guard_slippage_buy_tighten_scale = std::clamp(
                t.value("execution_guard_slippage_buy_tighten_scale", 0.35),
                0.0,
                10.0
            );
            engine_config_.execution_guard_slippage_buy_relief_scale = std::clamp(
                t.value("execution_guard_slippage_buy_relief_scale", 0.18),
                0.0,
                10.0
            );
            engine_config_.execution_guard_slippage_sell_relax_tighten_scale = std::clamp(
                t.value("execution_guard_slippage_sell_relax_tighten_scale", 0.15),
                -10.0,
                10.0
            );
            engine_config_.execution_guard_slippage_sell_relax_urgent_add = std::clamp(
                t.value("execution_guard_slippage_sell_relax_urgent_add", 0.45),
                -10.0,
                10.0
            );
            engine_config_.execution_guard_slippage_sell_relax_low_liquidity_threshold = std::clamp(
                t.value("execution_guard_slippage_sell_relax_low_liquidity_threshold", 50.0),
                0.0,
                10000.0
            );
            engine_config_.execution_guard_slippage_sell_relax_low_liquidity_add = std::clamp(
                t.value("execution_guard_slippage_sell_relax_low_liquidity_add", 0.10),
                -10.0,
                10.0
            );
            engine_config_.execution_guard_slippage_sell_relax_uptrend_liquidity_threshold = std::clamp(
                t.value("execution_guard_slippage_sell_relax_uptrend_liquidity_threshold", 65.0),
                0.0,
                10000.0
            );
            engine_config_.execution_guard_slippage_sell_relax_uptrend_relief = std::clamp(
                t.value("execution_guard_slippage_sell_relax_uptrend_relief", 0.05),
                -10.0,
                10.0
            );
            engine_config_.execution_guard_slippage_sell_relax_clamp_min = std::clamp(
                t.value("execution_guard_slippage_sell_relax_clamp_min", -0.15),
                -10.0,
                10.0
            );
            engine_config_.execution_guard_slippage_sell_relax_clamp_max = std::clamp(
                t.value("execution_guard_slippage_sell_relax_clamp_max", 1.20),
                -10.0,
                10.0
            );
            if (engine_config_.execution_guard_slippage_sell_relax_clamp_max <
                engine_config_.execution_guard_slippage_sell_relax_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_slippage_sell_relax_clamp_min,
                    engine_config_.execution_guard_slippage_sell_relax_clamp_max
                );
            }
            engine_config_.execution_guard_slippage_sell_urgent_cap = std::clamp(
                t.value("execution_guard_slippage_sell_urgent_cap", 0.03),
                0.0,
                1.0
            );
            engine_config_.execution_guard_slippage_sell_nonurgent_cap = std::clamp(
                t.value("execution_guard_slippage_sell_nonurgent_cap", 0.02),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_slippage_sell_urgent_cap <
                engine_config_.execution_guard_slippage_sell_nonurgent_cap) {
                std::swap(
                    engine_config_.execution_guard_slippage_sell_nonurgent_cap,
                    engine_config_.execution_guard_slippage_sell_urgent_cap
                );
            }
            engine_config_.execution_guard_slippage_base_clamp_min = std::clamp(
                t.value("execution_guard_slippage_base_clamp_min", 0.0002),
                0.0,
                1.0
            );
            engine_config_.execution_guard_slippage_base_clamp_max = std::clamp(
                t.value("execution_guard_slippage_base_clamp_max", 0.05),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_slippage_base_clamp_max <
                engine_config_.execution_guard_slippage_base_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_slippage_base_clamp_min,
                    engine_config_.execution_guard_slippage_base_clamp_max
                );
            }
            engine_config_.execution_guard_slippage_buy_clamp_min = std::clamp(
                t.value("execution_guard_slippage_buy_clamp_min", 0.0003),
                0.0,
                1.0
            );
            engine_config_.execution_guard_slippage_buy_clamp_max = std::clamp(
                t.value("execution_guard_slippage_buy_clamp_max", 0.012),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_slippage_buy_clamp_max <
                engine_config_.execution_guard_slippage_buy_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_slippage_buy_clamp_min,
                    engine_config_.execution_guard_slippage_buy_clamp_max
                );
            }
            engine_config_.execution_guard_slippage_sell_clamp_min = std::clamp(
                t.value("execution_guard_slippage_sell_clamp_min", 0.0003),
                0.0,
                1.0
            );
            engine_config_.execution_guard_slippage_guard_clamp_min = std::clamp(
                t.value("execution_guard_slippage_guard_clamp_min", 0.0005),
                0.0,
                1.0
            );
            engine_config_.execution_guard_slippage_guard_buy_clamp_max = std::clamp(
                t.value("execution_guard_slippage_guard_buy_clamp_max", 0.02),
                0.0,
                1.0
            );
            engine_config_.execution_guard_slippage_guard_sell_clamp_max = std::clamp(
                t.value("execution_guard_slippage_guard_sell_clamp_max", 0.03),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_slippage_guard_sell_clamp_max <
                engine_config_.execution_guard_slippage_guard_buy_clamp_max) {
                std::swap(
                    engine_config_.execution_guard_slippage_guard_buy_clamp_max,
                    engine_config_.execution_guard_slippage_guard_sell_clamp_max
                );
            }
            engine_config_.execution_guard_slippage_guard_multiplier = std::clamp(
                t.value("execution_guard_slippage_guard_multiplier", 1.5),
                0.0,
                10.0
            );
            engine_config_.execution_guard_tighten_severe_gap_min = std::clamp(
                t.value("execution_guard_tighten_severe_gap_min", 0.01),
                0.0,
                1.0
            );
            engine_config_.execution_guard_tighten_severe_clamp_min = std::clamp(
                t.value("execution_guard_tighten_severe_clamp_min", 0.01),
                0.0,
                1.0
            );
            engine_config_.execution_guard_tighten_severe_clamp_max = std::clamp(
                t.value("execution_guard_tighten_severe_clamp_max", 1.0),
                0.0,
                1.0
            );
            if (engine_config_.execution_guard_tighten_severe_clamp_max <
                engine_config_.execution_guard_tighten_severe_clamp_min) {
                std::swap(
                    engine_config_.execution_guard_tighten_severe_clamp_min,
                    engine_config_.execution_guard_tighten_severe_clamp_max
                );
            }
            engine_config_.enable_live_cache_warmup =
                t.value("enable_live_cache_warmup", true);
            engine_config_.live_cache_warmup_min_scans = std::max(
                1,
                t.value("live_cache_warmup_min_scans", 5)
            );
            engine_config_.live_cache_warmup_min_ready_ratio = std::clamp(
                t.value("live_cache_warmup_min_ready_ratio", 0.75),
                0.0,
                1.0
            );
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
            engine_config_.enable_probabilistic_runtime_model =
                t.value("enable_probabilistic_runtime_model", true);
            engine_config_.probabilistic_runtime_bundle_path = trimCopy(
                t.value("probabilistic_runtime_bundle_path", "config/model/probabilistic_runtime_bundle_v2.json")
            );
            if (engine_config_.probabilistic_runtime_bundle_path.empty()) {
                engine_config_.probabilistic_runtime_bundle_path = "config/model/probabilistic_runtime_bundle_v2.json";
            }
            engine_config_.probabilistic_runtime_hard_gate =
                t.value("probabilistic_runtime_hard_gate", false);
            engine_config_.probabilistic_runtime_hard_gate_margin = std::clamp(
                t.value("probabilistic_runtime_hard_gate_margin", -0.08),
                -0.50,
                0.20
            );
            engine_config_.probabilistic_runtime_score_weight = std::clamp(
                t.value("probabilistic_runtime_score_weight", 0.12),
                0.0,
                1.0
            );
            engine_config_.probabilistic_runtime_expected_edge_weight = std::clamp(
                t.value("probabilistic_runtime_expected_edge_weight", 0.00030),
                0.0,
                0.010
            );
            engine_config_.probabilistic_runtime_primary_mode =
                t.value("probabilistic_runtime_primary_mode", true);
            engine_config_.probabilistic_runtime_scan_prefilter_enabled =
                t.value("probabilistic_runtime_scan_prefilter_enabled", true);
            engine_config_.probabilistic_runtime_scan_prefilter_margin = std::clamp(
                t.value("probabilistic_runtime_scan_prefilter_margin", -0.10),
                -0.30,
                0.10
            );
            engine_config_.probabilistic_runtime_strength_blend = std::clamp(
                t.value("probabilistic_runtime_strength_blend", 0.45),
                0.0,
                1.0
            );
            engine_config_.probabilistic_uncertainty_ensemble_enabled =
                t.value("probabilistic_uncertainty_ensemble_enabled", false);
            engine_config_.probabilistic_uncertainty_size_mode = trimCopy(
                t.value("probabilistic_uncertainty_size_mode", "linear")
            );
            std::transform(
                engine_config_.probabilistic_uncertainty_size_mode.begin(),
                engine_config_.probabilistic_uncertainty_size_mode.end(),
                engine_config_.probabilistic_uncertainty_size_mode.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
            );
            if (engine_config_.probabilistic_uncertainty_size_mode != "linear" &&
                engine_config_.probabilistic_uncertainty_size_mode != "exp") {
                engine_config_.probabilistic_uncertainty_size_mode = "linear";
            }
            engine_config_.probabilistic_uncertainty_u_max = std::clamp(
                t.value("probabilistic_uncertainty_u_max", 0.06),
                1e-6,
                1.0
            );
            engine_config_.probabilistic_uncertainty_exp_k = std::clamp(
                t.value("probabilistic_uncertainty_exp_k", 8.0),
                0.0,
                100.0
            );
            engine_config_.probabilistic_uncertainty_min_scale = std::clamp(
                t.value("probabilistic_uncertainty_min_scale", 0.10),
                0.01,
                1.0
            );
            engine_config_.probabilistic_uncertainty_skip_when_high =
                t.value("probabilistic_uncertainty_skip_when_high", false);
            engine_config_.probabilistic_uncertainty_skip_u = std::clamp(
                t.value("probabilistic_uncertainty_skip_u", 0.12),
                engine_config_.probabilistic_uncertainty_u_max,
                1.0
            );
            engine_config_.probabilistic_regime_spec_enabled =
                t.value("probabilistic_regime_spec_enabled", false);
            engine_config_.probabilistic_regime_volatility_window = std::clamp(
                t.value("probabilistic_regime_volatility_window", 48),
                10,
                720
            );
            engine_config_.probabilistic_regime_drawdown_window = std::clamp(
                t.value("probabilistic_regime_drawdown_window", 36),
                5,
                720
            );
            engine_config_.probabilistic_regime_volatile_zscore = std::clamp(
                t.value("probabilistic_regime_volatile_zscore", 1.20),
                0.0,
                20.0
            );
            engine_config_.probabilistic_regime_hostile_zscore = std::clamp(
                t.value("probabilistic_regime_hostile_zscore", 2.00),
                engine_config_.probabilistic_regime_volatile_zscore,
                30.0
            );
            engine_config_.probabilistic_regime_volatile_drawdown_speed_bps = std::clamp(
                t.value("probabilistic_regime_volatile_drawdown_speed_bps", 3.0),
                0.0,
                1000.0
            );
            engine_config_.probabilistic_regime_hostile_drawdown_speed_bps = std::clamp(
                t.value("probabilistic_regime_hostile_drawdown_speed_bps", 8.0),
                engine_config_.probabilistic_regime_volatile_drawdown_speed_bps,
                1000.0
            );
            engine_config_.probabilistic_regime_enable_btc_correlation_shock =
                t.value("probabilistic_regime_enable_btc_correlation_shock", false);
            engine_config_.probabilistic_regime_correlation_window = std::clamp(
                t.value("probabilistic_regime_correlation_window", 48),
                10,
                720
            );
            engine_config_.probabilistic_regime_correlation_shock_threshold = std::clamp(
                t.value("probabilistic_regime_correlation_shock_threshold", 1.20),
                0.0,
                2.0
            );
            engine_config_.probabilistic_regime_hostile_block_new_entries =
                t.value("probabilistic_regime_hostile_block_new_entries", true);
            engine_config_.probabilistic_regime_volatile_threshold_add = std::clamp(
                t.value("probabilistic_regime_volatile_threshold_add", 0.010),
                0.0,
                0.40
            );
            engine_config_.probabilistic_regime_hostile_threshold_add = std::clamp(
                t.value("probabilistic_regime_hostile_threshold_add", 0.030),
                engine_config_.probabilistic_regime_volatile_threshold_add,
                0.60
            );
            engine_config_.probabilistic_regime_volatile_size_multiplier = std::clamp(
                t.value("probabilistic_regime_volatile_size_multiplier", 0.50),
                0.05,
                1.0
            );
            engine_config_.probabilistic_regime_hostile_size_multiplier = std::clamp(
                t.value("probabilistic_regime_hostile_size_multiplier", 0.20),
                0.01,
                engine_config_.probabilistic_regime_volatile_size_multiplier
            );
            engine_config_.probabilistic_primary_min_h5_calibrated = std::clamp(
                t.value("probabilistic_primary_min_h5_calibrated", 0.48),
                0.0,
                1.0
            );
            engine_config_.probabilistic_primary_min_h5_margin = std::clamp(
                t.value("probabilistic_primary_min_h5_margin", -0.03),
                -0.50,
                0.20
            );
            engine_config_.probabilistic_primary_min_liquidity_score = std::clamp(
                t.value("probabilistic_primary_min_liquidity_score", 42.0),
                0.0,
                100.0
            );
            engine_config_.probabilistic_primary_min_signal_strength = std::clamp(
                t.value("probabilistic_primary_min_signal_strength", 0.34),
                0.0,
                1.0
            );
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
            engine_config_.entry_capacity_default_stop_guard_pct = std::clamp(
                t.value("entry_capacity_default_stop_guard_pct", 0.03),
                0.0,
                1.0
            );
            engine_config_.entry_capacity_stop_guard_min_pct = std::clamp(
                t.value("entry_capacity_stop_guard_min_pct", 0.03),
                0.0,
                1.0
            );
            engine_config_.entry_capacity_stop_guard_max_pct = std::clamp(
                t.value("entry_capacity_stop_guard_max_pct", 0.20),
                0.0,
                1.0
            );
            if (engine_config_.entry_capacity_stop_guard_max_pct <
                engine_config_.entry_capacity_stop_guard_min_pct) {
                std::swap(
                    engine_config_.entry_capacity_stop_guard_min_pct,
                    engine_config_.entry_capacity_stop_guard_max_pct
                );
            }
            engine_config_.entry_capacity_exit_retention_floor = std::clamp(
                t.value("entry_capacity_exit_retention_floor", 0.50),
                0.01,
                0.99
            );
            engine_config_.entry_capacity_slippage_guard_multiplier = std::clamp(
                t.value("entry_capacity_slippage_guard_multiplier", 1.50),
                0.0,
                10.0
            );
            engine_config_.max_exposure_pct = t.value("max_exposure_pct", 0.85);
            engine_config_.max_daily_loss_pct = t.value("max_daily_loss_pct", 0.05);
            engine_config_.risk_per_trade_pct = t.value("risk_per_trade_pct", 0.005);
            engine_config_.max_slippage_pct = t.value("max_slippage_pct", 0.003);
            engine_config_.foundation_exit_stop_loss_pct = std::clamp(
                t.value("foundation_exit_stop_loss_pct", 0.006),
                0.0,
                1.0
            );
            engine_config_.foundation_exit_take_profit_pct = std::clamp(
                t.value("foundation_exit_take_profit_pct", 0.018),
                0.0,
                5.0
            );
            engine_config_.foundation_exit_time_limit_hours = std::clamp(
                t.value("foundation_exit_time_limit_hours", 6.0),
                0.0,
                168.0
            );
            engine_config_.foundation_exit_time_limit_min_profit_pct = std::clamp(
                t.value("foundation_exit_time_limit_min_profit_pct", 0.001),
                -1.0,
                1.0
            );
            engine_config_.foundation_position_risk_budget_pct = std::clamp(
                t.value("foundation_position_risk_budget_pct", 0.0065),
                0.0,
                1.0
            );
            engine_config_.foundation_position_notional_cap_pct = std::clamp(
                t.value("foundation_position_notional_cap_pct", 0.22),
                0.0,
                1.0
            );
            engine_config_.foundation_position_liq_mult_lt_50 = std::clamp(
                t.value("foundation_position_liq_mult_lt_50", 0.60),
                0.0,
                10.0
            );
            engine_config_.foundation_position_liq_mult_lt_55 = std::clamp(
                t.value("foundation_position_liq_mult_lt_55", 0.75),
                0.0,
                10.0
            );
            engine_config_.foundation_position_vol_mult_gt_4 = std::clamp(
                t.value("foundation_position_vol_mult_gt_4", 0.85),
                0.0,
                10.0
            );
            engine_config_.foundation_position_spread_threshold_pct = std::clamp(
                t.value("foundation_position_spread_threshold_pct", 0.0025),
                0.0,
                1.0
            );
            engine_config_.foundation_position_spread_mult = std::clamp(
                t.value("foundation_position_spread_mult", 0.80),
                0.0,
                10.0
            );
            engine_config_.foundation_position_min_notional_pct_low_liq = std::clamp(
                t.value("foundation_position_min_notional_pct_low_liq", 0.02),
                0.0,
                1.0
            );
            engine_config_.foundation_position_min_notional_pct_default = std::clamp(
                t.value("foundation_position_min_notional_pct_default", 0.03),
                0.0,
                1.0
            );
            engine_config_.foundation_position_output_min_pct = std::clamp(
                t.value("foundation_position_output_min_pct", 0.02),
                0.0,
                1.0
            );
            engine_config_.foundation_position_output_max_pct = std::clamp(
                t.value("foundation_position_output_max_pct", 0.22),
                0.0,
                1.0
            );
            if (engine_config_.foundation_position_output_max_pct <
                engine_config_.foundation_position_output_min_pct) {
                std::swap(
                    engine_config_.foundation_position_output_min_pct,
                    engine_config_.foundation_position_output_max_pct
                );
            }
            engine_config_.foundation_position_mult_thin_liquidity_adaptive = std::clamp(
                t.value("foundation_position_mult_thin_liquidity_adaptive", 0.48),
                0.0,
                10.0
            );
            engine_config_.foundation_position_mult_downtrend_low_flow_rebound = std::clamp(
                t.value("foundation_position_mult_downtrend_low_flow_rebound", 0.32),
                0.0,
                10.0
            );
            engine_config_.foundation_position_mult_ranging_low_flow = std::clamp(
                t.value("foundation_position_mult_ranging_low_flow", 0.50),
                0.0,
                10.0
            );
            engine_config_.foundation_position_mult_thin_low_vol_uptrend = std::clamp(
                t.value("foundation_position_mult_thin_low_vol_uptrend", 0.55),
                0.0,
                10.0
            );
            engine_config_.foundation_position_mult_thin_low_vol = std::clamp(
                t.value("foundation_position_mult_thin_low_vol", 0.62),
                0.0,
                10.0
            );
            engine_config_.foundation_risk_mult_uptrend = std::clamp(
                t.value("foundation_risk_mult_uptrend", 1.35),
                0.0,
                10.0
            );
            engine_config_.foundation_risk_floor_uptrend = std::clamp(
                t.value("foundation_risk_floor_uptrend", 0.0040),
                0.0,
                1.0
            );
            engine_config_.foundation_risk_cap_uptrend = std::clamp(
                t.value("foundation_risk_cap_uptrend", 0.0180),
                0.0,
                1.0
            );
            if (engine_config_.foundation_risk_cap_uptrend <
                engine_config_.foundation_risk_floor_uptrend) {
                std::swap(
                    engine_config_.foundation_risk_floor_uptrend,
                    engine_config_.foundation_risk_cap_uptrend
                );
            }
            engine_config_.foundation_risk_mult_ranging = std::clamp(
                t.value("foundation_risk_mult_ranging", 1.15),
                0.0,
                10.0
            );
            engine_config_.foundation_risk_floor_ranging = std::clamp(
                t.value("foundation_risk_floor_ranging", 0.0035),
                0.0,
                1.0
            );
            engine_config_.foundation_risk_cap_ranging = std::clamp(
                t.value("foundation_risk_cap_ranging", 0.0140),
                0.0,
                1.0
            );
            if (engine_config_.foundation_risk_cap_ranging <
                engine_config_.foundation_risk_floor_ranging) {
                std::swap(
                    engine_config_.foundation_risk_floor_ranging,
                    engine_config_.foundation_risk_cap_ranging
                );
            }
            engine_config_.foundation_risk_mult_hostile = std::clamp(
                t.value("foundation_risk_mult_hostile", 0.90),
                0.0,
                10.0
            );
            engine_config_.foundation_risk_floor_hostile = std::clamp(
                t.value("foundation_risk_floor_hostile", 0.0030),
                0.0,
                1.0
            );
            engine_config_.foundation_risk_cap_hostile = std::clamp(
                t.value("foundation_risk_cap_hostile", 0.0100),
                0.0,
                1.0
            );
            if (engine_config_.foundation_risk_cap_hostile <
                engine_config_.foundation_risk_floor_hostile) {
                std::swap(
                    engine_config_.foundation_risk_floor_hostile,
                    engine_config_.foundation_risk_cap_hostile
                );
            }
            engine_config_.foundation_risk_mult_unknown = std::clamp(
                t.value("foundation_risk_mult_unknown", 1.00),
                0.0,
                10.0
            );
            engine_config_.foundation_risk_floor_unknown = std::clamp(
                t.value("foundation_risk_floor_unknown", 0.0038),
                0.0,
                1.0
            );
            engine_config_.foundation_risk_cap_unknown = std::clamp(
                t.value("foundation_risk_cap_unknown", 0.0140),
                0.0,
                1.0
            );
            if (engine_config_.foundation_risk_cap_unknown <
                engine_config_.foundation_risk_floor_unknown) {
                std::swap(
                    engine_config_.foundation_risk_floor_unknown,
                    engine_config_.foundation_risk_cap_unknown
                );
            }
            engine_config_.foundation_reward_risk_uptrend = std::clamp(
                t.value("foundation_reward_risk_uptrend", 1.90),
                0.0,
                20.0
            );
            engine_config_.foundation_reward_risk_ranging = std::clamp(
                t.value("foundation_reward_risk_ranging", 1.45),
                0.0,
                20.0
            );
            engine_config_.foundation_reward_risk_hostile = std::clamp(
                t.value("foundation_reward_risk_hostile", 1.15),
                0.0,
                20.0
            );
            engine_config_.foundation_reward_risk_unknown = std::clamp(
                t.value("foundation_reward_risk_unknown", 1.35),
                0.0,
                20.0
            );
            engine_config_.foundation_take_profit_1_rr_multiplier = std::clamp(
                t.value("foundation_take_profit_1_rr_multiplier", 0.55),
                0.0,
                20.0
            );
            engine_config_.foundation_breakeven_rr_multiplier = std::clamp(
                t.value("foundation_breakeven_rr_multiplier", 0.80),
                0.0,
                20.0
            );
            engine_config_.foundation_trailing_rr_multiplier = std::clamp(
                t.value("foundation_trailing_rr_multiplier", 1.20),
                0.0,
                20.0
            );
            engine_config_.foundation_strong_buy_strength_threshold = std::clamp(
                t.value("foundation_strong_buy_strength_threshold", 0.74),
                0.0,
                1.0
            );
            engine_config_.foundation_implied_win_base = std::clamp(
                t.value("foundation_implied_win_base", 0.47),
                0.0,
                1.0
            );
            engine_config_.foundation_implied_win_strength_scale = std::clamp(
                t.value("foundation_implied_win_strength_scale", 0.18),
                -10.0,
                10.0
            );
            engine_config_.foundation_implied_win_preclamp_min = std::clamp(
                t.value("foundation_implied_win_preclamp_min", 0.44),
                0.0,
                1.0
            );
            engine_config_.foundation_implied_win_preclamp_max = std::clamp(
                t.value("foundation_implied_win_preclamp_max", 0.66),
                0.0,
                1.0
            );
            if (engine_config_.foundation_implied_win_preclamp_max <
                engine_config_.foundation_implied_win_preclamp_min) {
                std::swap(
                    engine_config_.foundation_implied_win_preclamp_min,
                    engine_config_.foundation_implied_win_preclamp_max
                );
            }
            engine_config_.foundation_implied_win_hostile_uptrend_penalty = std::clamp(
                t.value("foundation_implied_win_hostile_uptrend_penalty", 0.08),
                -1.0,
                1.0
            );
            engine_config_.foundation_implied_win_hostile_min = std::clamp(
                t.value("foundation_implied_win_hostile_min", 0.36),
                0.0,
                1.0
            );
            engine_config_.foundation_implied_win_hostile_max = std::clamp(
                t.value("foundation_implied_win_hostile_max", 0.58),
                0.0,
                1.0
            );
            if (engine_config_.foundation_implied_win_hostile_max <
                engine_config_.foundation_implied_win_hostile_min) {
                std::swap(
                    engine_config_.foundation_implied_win_hostile_min,
                    engine_config_.foundation_implied_win_hostile_max
                );
            }
            engine_config_.foundation_implied_win_mtf_scale = std::clamp(
                t.value("foundation_implied_win_mtf_scale", 0.22),
                -10.0,
                10.0
            );
            engine_config_.foundation_implied_win_mtf_add_min = std::clamp(
                t.value("foundation_implied_win_mtf_add_min", -0.05),
                -1.0,
                1.0
            );
            engine_config_.foundation_implied_win_mtf_add_max = std::clamp(
                t.value("foundation_implied_win_mtf_add_max", 0.06),
                -1.0,
                1.0
            );
            if (engine_config_.foundation_implied_win_mtf_add_max <
                engine_config_.foundation_implied_win_mtf_add_min) {
                std::swap(
                    engine_config_.foundation_implied_win_mtf_add_min,
                    engine_config_.foundation_implied_win_mtf_add_max
                );
            }
            engine_config_.foundation_implied_win_final_min = std::clamp(
                t.value("foundation_implied_win_final_min", 0.34),
                0.0,
                1.0
            );
            engine_config_.foundation_implied_win_final_max = std::clamp(
                t.value("foundation_implied_win_final_max", 0.72),
                0.0,
                1.0
            );
            if (engine_config_.foundation_implied_win_final_max <
                engine_config_.foundation_implied_win_final_min) {
                std::swap(
                    engine_config_.foundation_implied_win_final_min,
                    engine_config_.foundation_implied_win_final_max
                );
            }
            engine_config_.foundation_signal_filter_base = std::clamp(
                t.value("foundation_signal_filter_base", 0.52),
                0.0,
                1.0
            );
            engine_config_.foundation_signal_filter_strength_scale = std::clamp(
                t.value("foundation_signal_filter_strength_scale", 0.30),
                -10.0,
                10.0
            );
            engine_config_.foundation_signal_filter_min = std::clamp(
                t.value("foundation_signal_filter_min", 0.55),
                0.0,
                1.0
            );
            engine_config_.foundation_signal_filter_max = std::clamp(
                t.value("foundation_signal_filter_max", 0.85),
                0.0,
                1.0
            );
            if (engine_config_.foundation_signal_filter_max <
                engine_config_.foundation_signal_filter_min) {
                std::swap(
                    engine_config_.foundation_signal_filter_min,
                    engine_config_.foundation_signal_filter_max
                );
            }
            engine_config_.foundation_signal_filter_hostile_uptrend_add = std::clamp(
                t.value("foundation_signal_filter_hostile_uptrend_add", 0.03),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_filter_hostile_uptrend_max = std::clamp(
                t.value("foundation_signal_filter_hostile_uptrend_max", 0.88),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_min_bars_5m = std::clamp(
                t.value("foundation_mtf_min_bars_5m", 24),
                1,
                10000
            );
            engine_config_.foundation_mtf_min_bars_15m = std::clamp(
                t.value("foundation_mtf_min_bars_15m", 20),
                1,
                10000
            );
            engine_config_.foundation_mtf_min_bars_1h = std::clamp(
                t.value("foundation_mtf_min_bars_1h", 16),
                1,
                10000
            );
            engine_config_.foundation_mtf_momentum_5m_lookback = std::clamp(
                t.value("foundation_mtf_momentum_5m_lookback", 6),
                1,
                1000
            );
            engine_config_.foundation_mtf_momentum_15m_lookback = std::clamp(
                t.value("foundation_mtf_momentum_15m_lookback", 4),
                1,
                1000
            );
            engine_config_.foundation_mtf_momentum_1h_lookback = std::clamp(
                t.value("foundation_mtf_momentum_1h_lookback", 3),
                1,
                1000
            );
            engine_config_.foundation_mtf_ema_fast_15m = std::clamp(
                t.value("foundation_mtf_ema_fast_15m", 8),
                1,
                1000
            );
            engine_config_.foundation_mtf_ema_slow_15m = std::clamp(
                t.value("foundation_mtf_ema_slow_15m", 21),
                engine_config_.foundation_mtf_ema_fast_15m + 1,
                2000
            );
            engine_config_.foundation_mtf_ema_fast_1h = std::clamp(
                t.value("foundation_mtf_ema_fast_1h", 5),
                1,
                1000
            );
            engine_config_.foundation_mtf_ema_slow_1h = std::clamp(
                t.value("foundation_mtf_ema_slow_1h", 13),
                engine_config_.foundation_mtf_ema_fast_1h + 1,
                2000
            );
            engine_config_.foundation_mtf_rsi_period = std::clamp(
                t.value("foundation_mtf_rsi_period", 14),
                2,
                200
            );
            engine_config_.foundation_mtf_score_base = std::clamp(
                t.value("foundation_mtf_score_base", 0.50),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_score_momentum_5m_weight = std::clamp(
                t.value("foundation_mtf_score_momentum_5m_weight", 14.0),
                -200.0,
                200.0
            );
            engine_config_.foundation_mtf_score_momentum_5m_clip = std::clamp(
                t.value("foundation_mtf_score_momentum_5m_clip", 0.08),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_score_momentum_15m_weight = std::clamp(
                t.value("foundation_mtf_score_momentum_15m_weight", 18.0),
                -200.0,
                200.0
            );
            engine_config_.foundation_mtf_score_momentum_15m_clip = std::clamp(
                t.value("foundation_mtf_score_momentum_15m_clip", 0.10),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_score_momentum_1h_weight = std::clamp(
                t.value("foundation_mtf_score_momentum_1h_weight", 12.0),
                -200.0,
                200.0
            );
            engine_config_.foundation_mtf_score_momentum_1h_clip = std::clamp(
                t.value("foundation_mtf_score_momentum_1h_clip", 0.08),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_score_ema_gap_15m_weight = std::clamp(
                t.value("foundation_mtf_score_ema_gap_15m_weight", 16.0),
                -200.0,
                200.0
            );
            engine_config_.foundation_mtf_score_ema_gap_15m_clip = std::clamp(
                t.value("foundation_mtf_score_ema_gap_15m_clip", 0.08),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_score_ema_gap_1h_weight = std::clamp(
                t.value("foundation_mtf_score_ema_gap_1h_weight", 10.0),
                -200.0,
                200.0
            );
            engine_config_.foundation_mtf_score_ema_gap_1h_clip = std::clamp(
                t.value("foundation_mtf_score_ema_gap_1h_clip", 0.06),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_score_rsi_15m_center = std::clamp(
                t.value("foundation_mtf_score_rsi_15m_center", 56.0),
                0.0,
                100.0
            );
            engine_config_.foundation_mtf_score_rsi_15m_divisor = std::clamp(
                t.value("foundation_mtf_score_rsi_15m_divisor", 180.0),
                1e-9,
                1000000.0
            );
            engine_config_.foundation_mtf_score_rsi_15m_clip = std::clamp(
                t.value("foundation_mtf_score_rsi_15m_clip", 0.05),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_score_rsi_5m_center = std::clamp(
                t.value("foundation_mtf_score_rsi_5m_center", 54.0),
                0.0,
                100.0
            );
            engine_config_.foundation_mtf_score_rsi_5m_divisor = std::clamp(
                t.value("foundation_mtf_score_rsi_5m_divisor", 220.0),
                1e-9,
                1000000.0
            );
            engine_config_.foundation_mtf_score_rsi_5m_clip = std::clamp(
                t.value("foundation_mtf_score_rsi_5m_clip", 0.04),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_uptrend_min_momentum_15m = std::clamp(
                t.value("foundation_mtf_uptrend_min_momentum_15m", -0.0020),
                -1.0,
                1.0
            );
            engine_config_.foundation_mtf_uptrend_min_ema_gap_15m = std::clamp(
                t.value("foundation_mtf_uptrend_min_ema_gap_15m", -0.0014),
                -1.0,
                1.0
            );
            engine_config_.foundation_mtf_uptrend_min_momentum_1h = std::clamp(
                t.value("foundation_mtf_uptrend_min_momentum_1h", -0.0050),
                -1.0,
                1.0
            );
            engine_config_.foundation_mtf_uptrend_max_rsi_15m = std::clamp(
                t.value("foundation_mtf_uptrend_max_rsi_15m", 71.0),
                0.0,
                100.0
            );
            engine_config_.foundation_mtf_uptrend_min_score = std::clamp(
                t.value("foundation_mtf_uptrend_min_score", 0.43),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_ranging_max_abs_momentum_15m = std::clamp(
                t.value("foundation_mtf_ranging_max_abs_momentum_15m", 0.0240),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_ranging_max_rsi_15m = std::clamp(
                t.value("foundation_mtf_ranging_max_rsi_15m", 66.0),
                0.0,
                100.0
            );
            engine_config_.foundation_mtf_ranging_min_score = std::clamp(
                t.value("foundation_mtf_ranging_min_score", 0.37),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_hostile_min_momentum_1h = std::clamp(
                t.value("foundation_mtf_hostile_min_momentum_1h", -0.0100),
                -1.0,
                1.0
            );
            engine_config_.foundation_mtf_hostile_min_momentum_15m = std::clamp(
                t.value("foundation_mtf_hostile_min_momentum_15m", -0.0070),
                -1.0,
                1.0
            );
            engine_config_.foundation_mtf_hostile_max_rsi_5m = std::clamp(
                t.value("foundation_mtf_hostile_max_rsi_5m", 50.0),
                0.0,
                100.0
            );
            engine_config_.foundation_mtf_hostile_min_score = std::clamp(
                t.value("foundation_mtf_hostile_min_score", 0.50),
                0.0,
                1.0
            );
            engine_config_.foundation_mtf_unknown_min_score = std::clamp(
                t.value("foundation_mtf_unknown_min_score", 0.47),
                0.0,
                1.0
            );
            engine_config_.foundation_entry_base_liquidity_min = std::clamp(
                t.value("foundation_entry_base_liquidity_min", 42.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_base_volume_surge_min = std::clamp(
                t.value("foundation_entry_base_volume_surge_min", 0.68),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_spread_guard_max = std::clamp(
                t.value("foundation_entry_spread_guard_max", 0.0042),
                0.0,
                1.0
            );
            engine_config_.foundation_entry_adaptive_liquidity_floor_default = std::clamp(
                t.value("foundation_entry_adaptive_liquidity_floor_default", 32.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_adaptive_liquidity_floor_ranging = std::clamp(
                t.value("foundation_entry_adaptive_liquidity_floor_ranging", 20.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_adaptive_liquidity_floor_uptrend = std::clamp(
                t.value("foundation_entry_adaptive_liquidity_floor_uptrend", 30.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_adaptive_liquidity_floor_unknown = std::clamp(
                t.value("foundation_entry_adaptive_liquidity_floor_unknown", 24.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_adaptive_volume_floor_default = std::clamp(
                t.value("foundation_entry_adaptive_volume_floor_default", 0.45),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_adaptive_volume_floor_ranging = std::clamp(
                t.value("foundation_entry_adaptive_volume_floor_ranging", 0.20),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_adaptive_volume_floor_uptrend = std::clamp(
                t.value("foundation_entry_adaptive_volume_floor_uptrend", 0.34),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_adaptive_volume_floor_unknown = std::clamp(
                t.value("foundation_entry_adaptive_volume_floor_unknown", 0.28),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_adaptive_thin_volatility_max = std::clamp(
                t.value("foundation_entry_adaptive_thin_volatility_max", 3.6),
                0.0,
                1000.0
            );
            engine_config_.foundation_entry_adaptive_thin_imbalance_min = std::clamp(
                t.value("foundation_entry_adaptive_thin_imbalance_min", -0.26),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_adaptive_thin_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_adaptive_thin_buy_pressure_ratio_min", 0.74),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_narrow_relief_spread_max = std::clamp(
                t.value("foundation_entry_narrow_relief_spread_max", 0.0018),
                0.0,
                1.0
            );
            engine_config_.foundation_entry_narrow_relief_volatility_max = std::clamp(
                t.value("foundation_entry_narrow_relief_volatility_max", 2.2),
                0.0,
                1000.0
            );
            engine_config_.foundation_entry_narrow_relief_liquidity_min = std::clamp(
                t.value("foundation_entry_narrow_relief_liquidity_min", 34.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_narrow_relief_volume_surge_min = std::clamp(
                t.value("foundation_entry_narrow_relief_volume_surge_min", 0.58),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_narrow_relief_imbalance_min = std::clamp(
                t.value("foundation_entry_narrow_relief_imbalance_min", -0.14),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_narrow_relief_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_narrow_relief_buy_pressure_ratio_min", 0.85),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_enable_low_liq_relaxed_path = t.value(
                "foundation_entry_enable_low_liq_relaxed_path",
                true
            );
            engine_config_.foundation_entry_low_liq_relaxed_liquidity_min = std::clamp(
                t.value("foundation_entry_low_liq_relaxed_liquidity_min", 28.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_low_liq_relaxed_liquidity_max = std::clamp(
                t.value("foundation_entry_low_liq_relaxed_liquidity_max", 45.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_low_liq_relaxed_liquidity_max <
                engine_config_.foundation_entry_low_liq_relaxed_liquidity_min) {
                std::swap(
                    engine_config_.foundation_entry_low_liq_relaxed_liquidity_min,
                    engine_config_.foundation_entry_low_liq_relaxed_liquidity_max
                );
            }
            engine_config_.foundation_entry_low_liq_relaxed_volume_surge_min = std::clamp(
                t.value("foundation_entry_low_liq_relaxed_volume_surge_min", 0.46),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_low_liq_relaxed_imbalance_min = std::clamp(
                t.value("foundation_entry_low_liq_relaxed_imbalance_min", -0.18),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_low_liq_relaxed_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_low_liq_relaxed_buy_pressure_ratio_min", 0.80),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_enabled = t.value(
                "foundation_entry_low_liq_uptrend_enabled",
                true
            );
            engine_config_.foundation_entry_low_liq_uptrend_liquidity_min = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_liquidity_min", 34.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_liquidity_max = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_liquidity_max", 48.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_low_liq_uptrend_liquidity_max <
                engine_config_.foundation_entry_low_liq_uptrend_liquidity_min) {
                std::swap(
                    engine_config_.foundation_entry_low_liq_uptrend_liquidity_min,
                    engine_config_.foundation_entry_low_liq_uptrend_liquidity_max
                );
            }
            engine_config_.foundation_entry_low_liq_uptrend_volume_surge_min = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_volume_surge_min", 0.62),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_volume_surge_max = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_volume_surge_max", 1.70),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_low_liq_uptrend_volume_surge_max <
                engine_config_.foundation_entry_low_liq_uptrend_volume_surge_min) {
                std::swap(
                    engine_config_.foundation_entry_low_liq_uptrend_volume_surge_min,
                    engine_config_.foundation_entry_low_liq_uptrend_volume_surge_max
                );
            }
            engine_config_.foundation_entry_low_liq_uptrend_imbalance_min = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_imbalance_min", -0.06),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_ret5_min = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_ret5_min", 0.0004),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_ret20_min = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_ret20_min", 0.0010),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_price_to_ema_fast_min", 0.9992),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_ema_fast_to_ema_slow_min = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_ema_fast_to_ema_slow_min", 0.9990),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_rsi_min = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_rsi_min", 48.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_low_liq_uptrend_rsi_max = std::clamp(
                t.value("foundation_entry_low_liq_uptrend_rsi_max", 68.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_low_liq_uptrend_rsi_max <
                engine_config_.foundation_entry_low_liq_uptrend_rsi_min) {
                std::swap(
                    engine_config_.foundation_entry_low_liq_uptrend_rsi_min,
                    engine_config_.foundation_entry_low_liq_uptrend_rsi_max
                );
            }
            engine_config_.foundation_entry_thin_liq_adaptive_enabled = t.value(
                "foundation_entry_thin_liq_adaptive_enabled",
                true
            );
            engine_config_.foundation_entry_thin_liq_adaptive_liquidity_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_liquidity_min", 28.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_liquidity_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_liquidity_max", 56.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_thin_liq_adaptive_liquidity_max <
                engine_config_.foundation_entry_thin_liq_adaptive_liquidity_min) {
                std::swap(
                    engine_config_.foundation_entry_thin_liq_adaptive_liquidity_min,
                    engine_config_.foundation_entry_thin_liq_adaptive_liquidity_max
                );
            }
            engine_config_.foundation_entry_thin_liq_adaptive_volume_surge_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_volume_surge_min", 0.52),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_volume_surge_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_volume_surge_max", 1.85),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_thin_liq_adaptive_volume_surge_max <
                engine_config_.foundation_entry_thin_liq_adaptive_volume_surge_min) {
                std::swap(
                    engine_config_.foundation_entry_thin_liq_adaptive_volume_surge_min,
                    engine_config_.foundation_entry_thin_liq_adaptive_volume_surge_max
                );
            }
            engine_config_.foundation_entry_thin_liq_adaptive_spread_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_spread_max", 0.0038),
                0.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_uptrend_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_uptrend_price_to_ema_fast_min", 0.9995),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_uptrend_ema_fast_to_ema_slow_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_uptrend_ema_fast_to_ema_slow_min", 0.9990),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_uptrend_rsi_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_uptrend_rsi_min", 44.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_uptrend_rsi_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_uptrend_rsi_max", 68.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_thin_liq_adaptive_uptrend_rsi_max <
                engine_config_.foundation_entry_thin_liq_adaptive_uptrend_rsi_min) {
                std::swap(
                    engine_config_.foundation_entry_thin_liq_adaptive_uptrend_rsi_min,
                    engine_config_.foundation_entry_thin_liq_adaptive_uptrend_rsi_max
                );
            }
            engine_config_.foundation_entry_thin_liq_adaptive_uptrend_ret8_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_uptrend_ret8_min", -0.0004),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_uptrend_ret20_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_uptrend_ret20_min", 0.0003),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_uptrend_imbalance_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_uptrend_imbalance_min", -0.08),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_ranging_price_to_bb_middle_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_ranging_price_to_bb_middle_max", 1.0030),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_ranging_rsi_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_ranging_rsi_min", 34.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_ranging_rsi_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_ranging_rsi_max", 50.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_thin_liq_adaptive_ranging_rsi_max <
                engine_config_.foundation_entry_thin_liq_adaptive_ranging_rsi_min) {
                std::swap(
                    engine_config_.foundation_entry_thin_liq_adaptive_ranging_rsi_min,
                    engine_config_.foundation_entry_thin_liq_adaptive_ranging_rsi_max
                );
            }
            engine_config_.foundation_entry_thin_liq_adaptive_ranging_ret3_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_ranging_ret3_max", 0.0018),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_ranging_ret20_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_ranging_ret20_min", -0.0015),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_ranging_imbalance_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_ranging_imbalance_min", -0.12),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_hostile_rsi_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_hostile_rsi_max", 35.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_hostile_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_hostile_price_to_ema_fast_min", 0.9985),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_hostile_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_hostile_buy_pressure_ratio_min", 0.95),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_hostile_ret3_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_hostile_ret3_min", -0.0010),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_hostile_ret8_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_hostile_ret8_min", -0.0020),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_unknown_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_unknown_price_to_ema_fast_min", 0.9990),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_unknown_rsi_max = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_unknown_rsi_max", 55.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_unknown_imbalance_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_unknown_imbalance_min", -0.08),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_thin_liq_adaptive_unknown_ret8_min = std::clamp(
                t.value("foundation_entry_thin_liq_adaptive_unknown_ret8_min", -0.0006),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_ranging_low_flow_enabled = t.value(
                "foundation_entry_ranging_low_flow_enabled",
                true
            );
            engine_config_.foundation_entry_ranging_low_flow_liquidity_min = std::clamp(
                t.value("foundation_entry_ranging_low_flow_liquidity_min", 22.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_ranging_low_flow_liquidity_max = std::clamp(
                t.value("foundation_entry_ranging_low_flow_liquidity_max", 55.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_ranging_low_flow_liquidity_max <
                engine_config_.foundation_entry_ranging_low_flow_liquidity_min) {
                std::swap(
                    engine_config_.foundation_entry_ranging_low_flow_liquidity_min,
                    engine_config_.foundation_entry_ranging_low_flow_liquidity_max
                );
            }
            engine_config_.foundation_entry_ranging_low_flow_volume_surge_min = std::clamp(
                t.value("foundation_entry_ranging_low_flow_volume_surge_min", 0.34),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_ranging_low_flow_volume_surge_max = std::clamp(
                t.value("foundation_entry_ranging_low_flow_volume_surge_max", 1.30),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_ranging_low_flow_volume_surge_max <
                engine_config_.foundation_entry_ranging_low_flow_volume_surge_min) {
                std::swap(
                    engine_config_.foundation_entry_ranging_low_flow_volume_surge_min,
                    engine_config_.foundation_entry_ranging_low_flow_volume_surge_max
                );
            }
            engine_config_.foundation_entry_ranging_low_flow_spread_max = std::clamp(
                t.value("foundation_entry_ranging_low_flow_spread_max", 0.0042),
                0.0,
                1.0
            );
            engine_config_.foundation_entry_ranging_low_flow_price_to_bb_middle_max = std::clamp(
                t.value("foundation_entry_ranging_low_flow_price_to_bb_middle_max", 1.0030),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_ranging_low_flow_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_ranging_low_flow_price_to_ema_fast_min", 0.9950),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_ranging_low_flow_rsi_min = std::clamp(
                t.value("foundation_entry_ranging_low_flow_rsi_min", 28.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_ranging_low_flow_rsi_max = std::clamp(
                t.value("foundation_entry_ranging_low_flow_rsi_max", 52.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_ranging_low_flow_rsi_max <
                engine_config_.foundation_entry_ranging_low_flow_rsi_min) {
                std::swap(
                    engine_config_.foundation_entry_ranging_low_flow_rsi_min,
                    engine_config_.foundation_entry_ranging_low_flow_rsi_max
                );
            }
            engine_config_.foundation_entry_ranging_low_flow_ret3_max = std::clamp(
                t.value("foundation_entry_ranging_low_flow_ret3_max", 0.0015),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_ranging_low_flow_ret8_min = std::clamp(
                t.value("foundation_entry_ranging_low_flow_ret8_min", -0.0034),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_ranging_low_flow_imbalance_min = std::clamp(
                t.value("foundation_entry_ranging_low_flow_imbalance_min", -0.14),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_ranging_low_flow_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_ranging_low_flow_buy_pressure_ratio_min", 0.82),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_downtrend_rebound_enabled = t.value(
                "foundation_entry_downtrend_rebound_enabled",
                true
            );
            engine_config_.foundation_entry_downtrend_rebound_liquidity_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_liquidity_min", 18.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_downtrend_rebound_liquidity_max = std::clamp(
                t.value("foundation_entry_downtrend_rebound_liquidity_max", 52.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_downtrend_rebound_liquidity_max <
                engine_config_.foundation_entry_downtrend_rebound_liquidity_min) {
                std::swap(
                    engine_config_.foundation_entry_downtrend_rebound_liquidity_min,
                    engine_config_.foundation_entry_downtrend_rebound_liquidity_max
                );
            }
            engine_config_.foundation_entry_downtrend_rebound_volume_surge_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_volume_surge_min", 0.18),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_downtrend_rebound_volume_surge_max = std::clamp(
                t.value("foundation_entry_downtrend_rebound_volume_surge_max", 1.40),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_downtrend_rebound_volume_surge_max <
                engine_config_.foundation_entry_downtrend_rebound_volume_surge_min) {
                std::swap(
                    engine_config_.foundation_entry_downtrend_rebound_volume_surge_min,
                    engine_config_.foundation_entry_downtrend_rebound_volume_surge_max
                );
            }
            engine_config_.foundation_entry_downtrend_rebound_spread_max = std::clamp(
                t.value("foundation_entry_downtrend_rebound_spread_max", 0.0032),
                0.0,
                1.0
            );
            engine_config_.foundation_entry_downtrend_rebound_imbalance_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_imbalance_min", -0.12),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_downtrend_rebound_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_buy_pressure_ratio_min", 0.86),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_downtrend_rebound_ret3_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_ret3_min", -0.0012),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_downtrend_rebound_ret8_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_ret8_min", -0.0060),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_downtrend_rebound_ret20_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_ret20_min", -0.0280),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_downtrend_rebound_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_price_to_ema_fast_min", 0.9970),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_downtrend_rebound_ema_fast_to_ema_slow_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_ema_fast_to_ema_slow_min", 0.9920),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_downtrend_rebound_rsi_min = std::clamp(
                t.value("foundation_entry_downtrend_rebound_rsi_min", 23.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_downtrend_rebound_rsi_max = std::clamp(
                t.value("foundation_entry_downtrend_rebound_rsi_max", 43.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_downtrend_rebound_rsi_max <
                engine_config_.foundation_entry_downtrend_rebound_rsi_min) {
                std::swap(
                    engine_config_.foundation_entry_downtrend_rebound_rsi_min,
                    engine_config_.foundation_entry_downtrend_rebound_rsi_max
                );
            }
            engine_config_.foundation_entry_uptrend_ret20_floor_liquidity_pivot = std::clamp(
                t.value("foundation_entry_uptrend_ret20_floor_liquidity_pivot", 60.0),
                0.0,
                1000.0
            );
            engine_config_.foundation_entry_uptrend_ret20_floor_high_liq = std::clamp(
                t.value("foundation_entry_uptrend_ret20_floor_high_liq", 0.0008),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_ret20_floor_default = std::clamp(
                t.value("foundation_entry_uptrend_ret20_floor_default", 0.0004),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_overextended_rsi_min = std::clamp(
                t.value("foundation_entry_uptrend_overextended_rsi_min", 68.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_uptrend_overextended_ret5_min = std::clamp(
                t.value("foundation_entry_uptrend_overextended_ret5_min", 0.0045),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_base_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_uptrend_base_price_to_ema_fast_min", 0.9985),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_uptrend_base_ema_fast_to_ema_slow_min = std::clamp(
                t.value("foundation_entry_uptrend_base_ema_fast_to_ema_slow_min", 0.9980),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_uptrend_base_rsi_min = std::clamp(
                t.value("foundation_entry_uptrend_base_rsi_min", 42.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_uptrend_base_rsi_max = std::clamp(
                t.value("foundation_entry_uptrend_base_rsi_max", 74.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_uptrend_base_rsi_max <
                engine_config_.foundation_entry_uptrend_base_rsi_min) {
                std::swap(
                    engine_config_.foundation_entry_uptrend_base_rsi_min,
                    engine_config_.foundation_entry_uptrend_base_rsi_max
                );
            }
            engine_config_.foundation_entry_uptrend_base_imbalance_min = std::clamp(
                t.value("foundation_entry_uptrend_base_imbalance_min", -0.16),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_base_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_uptrend_base_buy_pressure_ratio_min", 0.86),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_uptrend_base_ret5_min = std::clamp(
                t.value("foundation_entry_uptrend_base_ret5_min", -0.0016),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_base_ret20_offset = std::clamp(
                t.value("foundation_entry_uptrend_base_ret20_offset", -0.0008),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_relief_context_liquidity_max = std::clamp(
                t.value("foundation_entry_uptrend_relief_context_liquidity_max", 55.0),
                0.0,
                1000.0
            );
            engine_config_.foundation_entry_uptrend_relief_context_volatility_max = std::clamp(
                t.value("foundation_entry_uptrend_relief_context_volatility_max", 1.8),
                0.0,
                1000.0
            );
            engine_config_.foundation_entry_uptrend_relief_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_uptrend_relief_price_to_ema_fast_min", 0.9978),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_uptrend_relief_ema_fast_to_ema_slow_min = std::clamp(
                t.value("foundation_entry_uptrend_relief_ema_fast_to_ema_slow_min", 0.9972),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_uptrend_relief_rsi_min = std::clamp(
                t.value("foundation_entry_uptrend_relief_rsi_min", 40.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_uptrend_relief_rsi_max = std::clamp(
                t.value("foundation_entry_uptrend_relief_rsi_max", 75.0),
                0.0,
                100.0
            );
            if (engine_config_.foundation_entry_uptrend_relief_rsi_max <
                engine_config_.foundation_entry_uptrend_relief_rsi_min) {
                std::swap(
                    engine_config_.foundation_entry_uptrend_relief_rsi_min,
                    engine_config_.foundation_entry_uptrend_relief_rsi_max
                );
            }
            engine_config_.foundation_entry_uptrend_relief_imbalance_min = std::clamp(
                t.value("foundation_entry_uptrend_relief_imbalance_min", -0.20),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_relief_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_uptrend_relief_buy_pressure_ratio_min", 0.82),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_uptrend_relief_ret5_min = std::clamp(
                t.value("foundation_entry_uptrend_relief_ret5_min", -0.0022),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_relief_ret20_offset = std::clamp(
                t.value("foundation_entry_uptrend_relief_ret20_offset", -0.0014),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_exhaustion_context_liquidity_min = std::clamp(
                t.value("foundation_entry_uptrend_exhaustion_context_liquidity_min", 62.0),
                0.0,
                1000.0
            );
            engine_config_.foundation_entry_uptrend_exhaustion_context_volatility_max = std::clamp(
                t.value("foundation_entry_uptrend_exhaustion_context_volatility_max", 1.7),
                0.0,
                1000.0
            );
            engine_config_.foundation_entry_uptrend_exhaustion_rsi_min = std::clamp(
                t.value("foundation_entry_uptrend_exhaustion_rsi_min", 63.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_uptrend_exhaustion_ret3_min = std::clamp(
                t.value("foundation_entry_uptrend_exhaustion_ret3_min", 0.0038),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_exhaustion_ret5_min = std::clamp(
                t.value("foundation_entry_uptrend_exhaustion_ret5_min", 0.0055),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_exhaustion_ema_premium_min = std::clamp(
                t.value("foundation_entry_uptrend_exhaustion_ema_premium_min", 0.0045),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_thin_context_volume_surge_min = std::clamp(
                t.value("foundation_entry_uptrend_thin_context_volume_surge_min", 0.74),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_uptrend_thin_context_imbalance_min = std::clamp(
                t.value("foundation_entry_uptrend_thin_context_imbalance_min", -0.10),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_thin_context_rsi_max = std::clamp(
                t.value("foundation_entry_uptrend_thin_context_rsi_max", 72.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_uptrend_thin_context_ret5_min = std::clamp(
                t.value("foundation_entry_uptrend_thin_context_ret5_min", -0.0006),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_uptrend_thin_context_ret20_min = std::clamp(
                t.value("foundation_entry_uptrend_thin_context_ret20_min", 0.0004),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_ranging_structure_price_to_bb_middle_max = std::clamp(
                t.value("foundation_entry_ranging_structure_price_to_bb_middle_max", 1.0025),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_ranging_structure_rsi_max = std::clamp(
                t.value("foundation_entry_ranging_structure_rsi_max", 46.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_ranging_structure_imbalance_min = std::clamp(
                t.value("foundation_entry_ranging_structure_imbalance_min", -0.24),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_ranging_structure_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_ranging_structure_buy_pressure_ratio_min", 0.80),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_ranging_relief_price_to_bb_middle_max = std::clamp(
                t.value("foundation_entry_ranging_relief_price_to_bb_middle_max", 1.0035),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_ranging_relief_rsi_max = std::clamp(
                t.value("foundation_entry_ranging_relief_rsi_max", 47.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_ranging_relief_imbalance_min = std::clamp(
                t.value("foundation_entry_ranging_relief_imbalance_min", -0.20),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_ranging_relief_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_ranging_relief_buy_pressure_ratio_min", 0.82),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_hostile_bear_rebound_rsi_max = std::clamp(
                t.value("foundation_entry_hostile_bear_rebound_rsi_max", 32.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_hostile_bear_rebound_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_hostile_bear_rebound_price_to_ema_fast_min", 1.0),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_hostile_bear_rebound_buy_pressure_ratio_min = std::clamp(
                t.value("foundation_entry_hostile_bear_rebound_buy_pressure_ratio_min", 1.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_hostile_bear_rebound_liquidity_min = std::clamp(
                t.value("foundation_entry_hostile_bear_rebound_liquidity_min", 62.0),
                0.0,
                1000.0
            );
            engine_config_.foundation_entry_hostile_bear_rebound_volume_surge_min = std::clamp(
                t.value("foundation_entry_hostile_bear_rebound_volume_surge_min", 1.10),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_unknown_structure_price_to_ema_fast_min = std::clamp(
                t.value("foundation_entry_unknown_structure_price_to_ema_fast_min", 1.0),
                0.0,
                2.0
            );
            engine_config_.foundation_entry_unknown_structure_rsi_max = std::clamp(
                t.value("foundation_entry_unknown_structure_rsi_max", 52.0),
                0.0,
                100.0
            );
            engine_config_.foundation_entry_unknown_structure_imbalance_min = std::clamp(
                t.value("foundation_entry_unknown_structure_imbalance_min", -0.10),
                -1.0,
                1.0
            );
            engine_config_.foundation_entry_snapshot_min_bars = std::clamp(
                t.value("foundation_entry_snapshot_min_bars", 60),
                1,
                100000
            );
            engine_config_.foundation_entry_snapshot_ema_fast_period = std::clamp(
                t.value("foundation_entry_snapshot_ema_fast_period", 12),
                1,
                10000
            );
            engine_config_.foundation_entry_snapshot_ema_slow_period = std::clamp(
                t.value("foundation_entry_snapshot_ema_slow_period", 48),
                engine_config_.foundation_entry_snapshot_ema_fast_period + 1,
                100000
            );
            engine_config_.foundation_entry_snapshot_rsi_period = std::clamp(
                t.value("foundation_entry_snapshot_rsi_period", 14),
                2,
                10000
            );
            engine_config_.foundation_entry_snapshot_bb_period = std::clamp(
                t.value("foundation_entry_snapshot_bb_period", 20),
                2,
                10000
            );
            engine_config_.foundation_entry_snapshot_bb_stddev = std::clamp(
                t.value("foundation_entry_snapshot_bb_stddev", 2.0),
                1e-9,
                1000.0
            );
            engine_config_.foundation_signal_context_thin_low_vol_liquidity_max = std::clamp(
                t.value("foundation_signal_context_thin_low_vol_liquidity_max", 55.0),
                0.0,
                1000.0
            );
            engine_config_.foundation_signal_context_thin_low_vol_volatility_max = std::clamp(
                t.value("foundation_signal_context_thin_low_vol_volatility_max", 1.8),
                0.0,
                1000.0
            );
            engine_config_.foundation_signal_context_hostile_uptrend_liquidity_max = std::clamp(
                t.value("foundation_signal_context_hostile_uptrend_liquidity_max", 50.0),
                0.0,
                1000.0
            );
            engine_config_.foundation_signal_context_hostile_uptrend_volatility_max = std::clamp(
                t.value("foundation_signal_context_hostile_uptrend_volatility_max", 1.9),
                0.0,
                1000.0
            );
            engine_config_.foundation_signal_path_risk_mult_thin_liq_adaptive = std::clamp(
                t.value("foundation_signal_path_risk_mult_thin_liq_adaptive", 0.74),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_path_reward_risk_min_thin_liq_adaptive = std::clamp(
                t.value("foundation_signal_path_reward_risk_min_thin_liq_adaptive", 2.10),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_path_risk_mult_downtrend_rebound = std::clamp(
                t.value("foundation_signal_path_risk_mult_downtrend_rebound", 0.58),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_path_reward_risk_min_downtrend_rebound = std::clamp(
                t.value("foundation_signal_path_reward_risk_min_downtrend_rebound", 2.00),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_path_risk_mult_low_liq_relaxed = std::clamp(
                t.value("foundation_signal_path_risk_mult_low_liq_relaxed", 0.84),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_path_reward_risk_min_low_liq_relaxed = std::clamp(
                t.value("foundation_signal_path_reward_risk_min_low_liq_relaxed", 2.25),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_path_risk_mult_ranging_low_flow = std::clamp(
                t.value("foundation_signal_path_risk_mult_ranging_low_flow", 0.78),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_path_reward_risk_min_ranging_low_flow = std::clamp(
                t.value("foundation_signal_path_reward_risk_min_ranging_low_flow", 1.80),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_context_thin_low_vol_risk_mult = std::clamp(
                t.value("foundation_signal_context_thin_low_vol_risk_mult", 0.88),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_context_thin_low_vol_reward_min_uptrend = std::clamp(
                t.value("foundation_signal_context_thin_low_vol_reward_min_uptrend", 1.35),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_context_thin_low_vol_reward_max_uptrend = std::clamp(
                t.value("foundation_signal_context_thin_low_vol_reward_max_uptrend", 1.75),
                0.0,
                100.0
            );
            if (engine_config_.foundation_signal_context_thin_low_vol_reward_max_uptrend <
                engine_config_.foundation_signal_context_thin_low_vol_reward_min_uptrend) {
                std::swap(
                    engine_config_.foundation_signal_context_thin_low_vol_reward_min_uptrend,
                    engine_config_.foundation_signal_context_thin_low_vol_reward_max_uptrend
                );
            }
            engine_config_.foundation_signal_context_thin_low_vol_reward_min_other = std::clamp(
                t.value("foundation_signal_context_thin_low_vol_reward_min_other", 1.45),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_context_thin_low_vol_reward_max_other = std::clamp(
                t.value("foundation_signal_context_thin_low_vol_reward_max_other", 1.90),
                0.0,
                100.0
            );
            if (engine_config_.foundation_signal_context_thin_low_vol_reward_max_other <
                engine_config_.foundation_signal_context_thin_low_vol_reward_min_other) {
                std::swap(
                    engine_config_.foundation_signal_context_thin_low_vol_reward_min_other,
                    engine_config_.foundation_signal_context_thin_low_vol_reward_max_other
                );
            }
            engine_config_.foundation_signal_context_hostile_uptrend_risk_mult = std::clamp(
                t.value("foundation_signal_context_hostile_uptrend_risk_mult", 0.92),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_context_hostile_uptrend_reward_min = std::clamp(
                t.value("foundation_signal_context_hostile_uptrend_reward_min", 1.30),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_context_hostile_uptrend_reward_max = std::clamp(
                t.value("foundation_signal_context_hostile_uptrend_reward_max", 1.65),
                0.0,
                100.0
            );
            if (engine_config_.foundation_signal_context_hostile_uptrend_reward_max <
                engine_config_.foundation_signal_context_hostile_uptrend_reward_min) {
                std::swap(
                    engine_config_.foundation_signal_context_hostile_uptrend_reward_min,
                    engine_config_.foundation_signal_context_hostile_uptrend_reward_max
                );
            }
            engine_config_.foundation_signal_mtf_risk_scale = std::clamp(
                t.value("foundation_signal_mtf_risk_scale", 0.40),
                -100.0,
                100.0
            );
            engine_config_.foundation_signal_mtf_risk_scale_min = std::clamp(
                t.value("foundation_signal_mtf_risk_scale_min", 0.84),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_mtf_risk_scale_max = std::clamp(
                t.value("foundation_signal_mtf_risk_scale_max", 1.12),
                0.0,
                100.0
            );
            if (engine_config_.foundation_signal_mtf_risk_scale_max <
                engine_config_.foundation_signal_mtf_risk_scale_min) {
                std::swap(
                    engine_config_.foundation_signal_mtf_risk_scale_min,
                    engine_config_.foundation_signal_mtf_risk_scale_max
                );
            }
            engine_config_.foundation_signal_mtf_reward_add_scale = std::clamp(
                t.value("foundation_signal_mtf_reward_add_scale", 0.90),
                -100.0,
                100.0
            );
            engine_config_.foundation_signal_mtf_reward_add_min = std::clamp(
                t.value("foundation_signal_mtf_reward_add_min", -0.12),
                -100.0,
                100.0
            );
            engine_config_.foundation_signal_mtf_reward_add_max = std::clamp(
                t.value("foundation_signal_mtf_reward_add_max", 0.18),
                -100.0,
                100.0
            );
            if (engine_config_.foundation_signal_mtf_reward_add_max <
                engine_config_.foundation_signal_mtf_reward_add_min) {
                std::swap(
                    engine_config_.foundation_signal_mtf_reward_add_min,
                    engine_config_.foundation_signal_mtf_reward_add_max
                );
            }
            engine_config_.foundation_signal_risk_pct_min = std::clamp(
                t.value("foundation_signal_risk_pct_min", 0.0022),
                0.0,
                1.0
            );
            engine_config_.foundation_signal_risk_pct_max = std::clamp(
                t.value("foundation_signal_risk_pct_max", 0.0185),
                0.0,
                1.0
            );
            if (engine_config_.foundation_signal_risk_pct_max <
                engine_config_.foundation_signal_risk_pct_min) {
                std::swap(
                    engine_config_.foundation_signal_risk_pct_min,
                    engine_config_.foundation_signal_risk_pct_max
                );
            }
            engine_config_.foundation_signal_reward_risk_min = std::clamp(
                t.value("foundation_signal_reward_risk_min", 1.10),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_reward_risk_max = std::clamp(
                t.value("foundation_signal_reward_risk_max", 2.50),
                0.0,
                100.0
            );
            if (engine_config_.foundation_signal_reward_risk_max <
                engine_config_.foundation_signal_reward_risk_min) {
                std::swap(
                    engine_config_.foundation_signal_reward_risk_min,
                    engine_config_.foundation_signal_reward_risk_max
                );
            }
            engine_config_.foundation_signal_strength_base = std::clamp(
                t.value("foundation_signal_strength_base", 0.50),
                0.0,
                1.0
            );
            engine_config_.foundation_signal_strength_ema_gap_weight = std::clamp(
                t.value("foundation_signal_strength_ema_gap_weight", 12.0),
                -1000.0,
                1000.0
            );
            engine_config_.foundation_signal_strength_ema_gap_add_min = std::clamp(
                t.value("foundation_signal_strength_ema_gap_add_min", -0.10),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_ema_gap_add_max = std::clamp(
                t.value("foundation_signal_strength_ema_gap_add_max", 0.20),
                -1.0,
                1.0
            );
            if (engine_config_.foundation_signal_strength_ema_gap_add_max <
                engine_config_.foundation_signal_strength_ema_gap_add_min) {
                std::swap(
                    engine_config_.foundation_signal_strength_ema_gap_add_min,
                    engine_config_.foundation_signal_strength_ema_gap_add_max
                );
            }
            engine_config_.foundation_signal_strength_rsi_center = std::clamp(
                t.value("foundation_signal_strength_rsi_center", 55.0),
                0.0,
                100.0
            );
            engine_config_.foundation_signal_strength_rsi_divisor = std::clamp(
                t.value("foundation_signal_strength_rsi_divisor", 120.0),
                1e-6,
                100000.0
            );
            engine_config_.foundation_signal_strength_rsi_add_min = std::clamp(
                t.value("foundation_signal_strength_rsi_add_min", -0.08),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_rsi_add_max = std::clamp(
                t.value("foundation_signal_strength_rsi_add_max", 0.10),
                -1.0,
                1.0
            );
            if (engine_config_.foundation_signal_strength_rsi_add_max <
                engine_config_.foundation_signal_strength_rsi_add_min) {
                std::swap(
                    engine_config_.foundation_signal_strength_rsi_add_min,
                    engine_config_.foundation_signal_strength_rsi_add_max
                );
            }
            engine_config_.foundation_signal_strength_liquidity_center = std::clamp(
                t.value("foundation_signal_strength_liquidity_center", 50.0),
                0.0,
                10000.0
            );
            engine_config_.foundation_signal_strength_liquidity_divisor = std::clamp(
                t.value("foundation_signal_strength_liquidity_divisor", 350.0),
                1e-6,
                100000.0
            );
            engine_config_.foundation_signal_strength_liquidity_add_min = std::clamp(
                t.value("foundation_signal_strength_liquidity_add_min", -0.07),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_liquidity_add_max = std::clamp(
                t.value("foundation_signal_strength_liquidity_add_max", 0.12),
                -1.0,
                1.0
            );
            if (engine_config_.foundation_signal_strength_liquidity_add_max <
                engine_config_.foundation_signal_strength_liquidity_add_min) {
                std::swap(
                    engine_config_.foundation_signal_strength_liquidity_add_min,
                    engine_config_.foundation_signal_strength_liquidity_add_max
                );
            }
            engine_config_.foundation_signal_strength_uptrend_add = std::clamp(
                t.value("foundation_signal_strength_uptrend_add", 0.08),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_hostile_regime_add = std::clamp(
                t.value("foundation_signal_strength_hostile_regime_add", -0.06),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_path_thin_liq_add = std::clamp(
                t.value("foundation_signal_strength_path_thin_liq_add", 0.03),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_path_ranging_add = std::clamp(
                t.value("foundation_signal_strength_path_ranging_add", 0.02),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_path_low_liq_add = std::clamp(
                t.value("foundation_signal_strength_path_low_liq_add", 0.04),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_hostile_uptrend_add = std::clamp(
                t.value("foundation_signal_strength_hostile_uptrend_add", -0.03),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_mtf_scale = std::clamp(
                t.value("foundation_signal_strength_mtf_scale", 0.30),
                -1000.0,
                1000.0
            );
            engine_config_.foundation_signal_strength_mtf_add_min = std::clamp(
                t.value("foundation_signal_strength_mtf_add_min", -0.10),
                -1.0,
                1.0
            );
            engine_config_.foundation_signal_strength_mtf_add_max = std::clamp(
                t.value("foundation_signal_strength_mtf_add_max", 0.12),
                -1.0,
                1.0
            );
            if (engine_config_.foundation_signal_strength_mtf_add_max <
                engine_config_.foundation_signal_strength_mtf_add_min) {
                std::swap(
                    engine_config_.foundation_signal_strength_mtf_add_min,
                    engine_config_.foundation_signal_strength_mtf_add_max
                );
            }
            engine_config_.foundation_signal_strength_final_min = std::clamp(
                t.value("foundation_signal_strength_final_min", 0.35),
                0.0,
                1.0
            );
            engine_config_.foundation_signal_strength_final_max = std::clamp(
                t.value("foundation_signal_strength_final_max", 0.92),
                0.0,
                1.0
            );
            if (engine_config_.foundation_signal_strength_final_max <
                engine_config_.foundation_signal_strength_final_min) {
                std::swap(
                    engine_config_.foundation_signal_strength_final_min,
                    engine_config_.foundation_signal_strength_final_max
                );
            }
            engine_config_.min_expected_edge_pct = t.value("min_expected_edge_pct", 0.0010);
            engine_config_.min_reward_risk = t.value("min_reward_risk", 1.20);
            engine_config_.min_rr_weak_signal = t.value("min_rr_weak_signal", 1.80);
            engine_config_.min_rr_strong_signal = t.value("min_rr_strong_signal", 1.25);
            engine_config_.min_strategy_trades_for_ev = t.value("min_strategy_trades_for_ev", 30);
            engine_config_.min_strategy_expectancy_krw = t.value("min_strategy_expectancy_krw", 0.0);
            engine_config_.min_strategy_profit_factor = t.value("min_strategy_profit_factor", 1.00);
            engine_config_.avoid_high_volatility = t.value("avoid_high_volatility", true);
            engine_config_.avoid_trending_down = t.value("avoid_trending_down", true);
            engine_config_.enable_core_plane_bridge = t.value("enable_core_plane_bridge", true);
            engine_config_.enable_core_policy_plane = t.value("enable_core_policy_plane", true);
            engine_config_.enable_core_risk_plane = t.value("enable_core_risk_plane", true);
            engine_config_.enable_core_execution_plane = t.value("enable_core_execution_plane", true);
            engine_config_.hostility_ewma_alpha = t.value("hostility_ewma_alpha", 0.14);
            engine_config_.hostility_hostile_threshold = t.value("hostility_hostile_threshold", 0.62);
            engine_config_.hostility_severe_threshold = t.value("hostility_severe_threshold", 0.82);
            engine_config_.hostility_extreme_threshold = t.value("hostility_extreme_threshold", 0.88);
            engine_config_.hostility_score_regime_high_vol = std::clamp(
                t.value("hostility_score_regime_high_vol", 0.72),
                0.0,
                1.0
            );
            engine_config_.hostility_score_regime_trending_down = std::clamp(
                t.value("hostility_score_regime_trending_down", 0.62),
                0.0,
                1.0
            );
            engine_config_.hostility_score_regime_ranging = std::clamp(
                t.value("hostility_score_regime_ranging", 0.34),
                0.0,
                1.0
            );
            engine_config_.hostility_score_regime_trending_up = std::clamp(
                t.value("hostility_score_regime_trending_up", 0.12),
                0.0,
                1.0
            );
            engine_config_.hostility_score_regime_unknown = std::clamp(
                t.value("hostility_score_regime_unknown", 0.28),
                0.0,
                1.0
            );
            engine_config_.hostility_score_volatility_pivot = std::clamp(
                t.value("hostility_score_volatility_pivot", 1.8),
                0.0,
                20.0
            );
            engine_config_.hostility_score_volatility_divisor = std::clamp(
                t.value("hostility_score_volatility_divisor", 6.0),
                0.001,
                1000.0
            );
            engine_config_.hostility_score_volatility_cap = std::clamp(
                t.value("hostility_score_volatility_cap", 0.28),
                0.0,
                1.0
            );
            engine_config_.hostility_score_liquidity_pivot = std::clamp(
                t.value("hostility_score_liquidity_pivot", 55.0),
                0.0,
                1000.0
            );
            engine_config_.hostility_score_liquidity_divisor = std::clamp(
                t.value("hostility_score_liquidity_divisor", 90.0),
                0.001,
                1000.0
            );
            engine_config_.hostility_score_liquidity_cap = std::clamp(
                t.value("hostility_score_liquidity_cap", 0.20),
                0.0,
                1.0
            );
            engine_config_.hostility_score_spread_pct_pivot = std::clamp(
                t.value("hostility_score_spread_pct_pivot", 0.18),
                0.0,
                100.0
            );
            engine_config_.hostility_score_spread_pct_divisor = std::clamp(
                t.value("hostility_score_spread_pct_divisor", 0.40),
                0.001,
                1000.0
            );
            engine_config_.hostility_score_spread_pct_cap = std::clamp(
                t.value("hostility_score_spread_pct_cap", 0.18),
                0.0,
                1.0
            );
            engine_config_.hostility_scan_buy_limit_hostile_add = std::clamp(
                t.value("hostility_scan_buy_limit_hostile_add", 0.13),
                0.0,
                1.0
            );

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

        std::cout << "Config Loaded: Fee=" << fee_rate_ << ", MinOrder=" << min_order_krw_ << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "설정 로드 오류: " << e.what() << std::endl;
    }
}

} // namespace autolife
