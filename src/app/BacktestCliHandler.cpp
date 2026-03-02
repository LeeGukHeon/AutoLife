#include "app/BacktestCliHandler.h"

#include "app/BacktestReportFormatter.h"
#include "common/Logger.h"
#include "common/PathUtils.h"
#include "runtime/BacktestRuntime.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace autolife::app {

namespace {

using BacktestResult = backtest::BacktestEngine::Result;

static std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

struct CompanionCheckResult {
    bool applicable = false;
    std::vector<std::string> missing_tokens;
    std::vector<std::string> found_tokens;
};

static CompanionCheckResult checkHigherTfCompanions(const std::string& csv_path) {
    CompanionCheckResult out;
    std::filesystem::path primary(csv_path);
    if (!std::filesystem::exists(primary) || !primary.has_parent_path()) {
        return out;
    }

    const std::string stem_lower = toLowerCopy(primary.stem().string());
    const std::string prefix = "upbit_";
    const std::string pivot = "_1m_";
    if (!startsWith(stem_lower, prefix)) {
        return out;
    }

    const size_t market_begin = prefix.size();
    const size_t market_end = stem_lower.find(pivot, market_begin);
    if (market_end == std::string::npos || market_end <= market_begin) {
        return out;
    }
    out.applicable = true;

    const std::string market_token = stem_lower.substr(market_begin, market_end - market_begin);
    const std::filesystem::path parent = primary.parent_path();

    auto hasCompanion = [&](const std::string& token) {
        const std::string expected_prefix = "upbit_" + market_token + "_" + token + "_";
        for (const auto& entry : std::filesystem::directory_iterator(parent)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (toLowerCopy(entry.path().extension().string()) != ".csv") {
                continue;
            }
            const std::string candidate_stem = toLowerCopy(entry.path().stem().string());
            if (startsWith(candidate_stem, expected_prefix)) {
                return true;
            }
        }
        return false;
    };

    for (const auto& token : {"5m", "15m", "60m", "240m"}) {
        if (hasCompanion(token)) {
            out.found_tokens.push_back(token);
        } else {
            out.missing_tokens.push_back(token);
        }
    }

    return out;
}

static void printCompanionRequirementError(const std::string& csv_path, const CompanionCheckResult& check) {
    std::cout << "실거래 동등 MTF 모드 검증 실패: " << csv_path << "\n";
    if (!check.applicable) {
        std::cout << "  파일명 규칙이 맞지 않습니다. 예: upbit_KRW_BTC_1m_12000.csv\n";
        std::cout << "  companion(5m/15m/60m/240m) 자동 매칭이 가능한 1m 파일을 지정하세요.\n";
        return;
    }

    if (!check.missing_tokens.empty()) {
        std::cout << "  누락된 companion TF: ";
        for (size_t i = 0; i < check.missing_tokens.size(); ++i) {
            if (i > 0) {
                std::cout << ", ";
            }
            std::cout << check.missing_tokens[i];
        }
        std::cout << "\n";
        std::cout << "  같은 폴더에 upbit_<market>_5m_*.csv / 15m / 60m / 240m 파일이 필요합니다.\n";
    }
}

constexpr double kStructuralMinOrderKrw = 5000.0;
constexpr double kStructuralFeeRatePerLeg = 0.0005;         // 0.05% each side
constexpr double kStructuralSlippageAlpha = 0.5;
constexpr double kStructuralSlippageCapBps = 30.0;
constexpr double kStructuralLabelCostBps = 12.0;

struct StructuralMarketAgg {
    int trade_count = 0;
    double price_move_pnl_sum_krw = 0.0;
    double fee_sum_krw = 0.0;
    double slippage_sum_krw = 0.0;
    double structural_pnl_after_cost_sum_krw = 0.0;
    int forced_full_exit_count = 0;
    int partial_disabled_count = 0;
    int dust_leftover_count = 0;
};

static std::string normalizeStructuralExitReason(const std::string& raw_reason) {
    const std::string reason = toLowerCopy(raw_reason);
    if (reason == "stoploss" || reason == "stop_loss") {
        return "stop_loss";
    }
    if (reason == "takeprofit1" || reason == "partial_take_profit" || reason == "partial_tp") {
        return "partial_tp";
    }
    if (reason == "takeprofit2" || reason == "take_profit" || reason == "tp_full") {
        return "tp_full";
    }
    if (reason == "takeprofitfullduetominorder" || reason == "take_profit_full_due_to_min_order") {
        return "tp_full_due_to_min_order";
    }
    if (reason == "strategy_exit") {
        return "strategy_exit";
    }
    if (reason == "backtesteod") {
        return "time_exit";
    }
    if (reason == "timeexit" || reason == "time_exit") {
        return "time_exit";
    }
    return "other";
}

static double toLiquidityNorm01(double liquidity_score) {
    if (!std::isfinite(liquidity_score)) {
        return 0.5;
    }
    if (liquidity_score > 1.0) {
        return std::clamp(liquidity_score / 100.0, 0.0, 1.0);
    }
    return std::clamp(liquidity_score, 0.0, 1.0);
}

static double estimateSpreadPctForStructuralEv(const BacktestResult::TradeHistorySample& trade) {
    const double liq = toLiquidityNorm01(trade.liquidity_score);
    const double vol = std::clamp(
        std::isfinite(trade.volatility) ? std::abs(trade.volatility) : 0.0,
        0.0,
        1.0
    );
    const double spread_pct =
        0.00020 + ((1.0 - liq) * 0.00090) + (vol * 0.00020);
    return std::clamp(spread_pct, 0.00005, 0.00600);
}

static double spreadToSlippageBps(double spread_pct) {
    const double raw_bps = std::max(0.0, spread_pct) * 10000.0 * kStructuralSlippageAlpha;
    return std::min(raw_bps, kStructuralSlippageCapBps);
}

static void writeExitPolicyDumpArtifact(
    const BacktestResult& result,
    const std::filesystem::path& run_dir
) {
    std::filesystem::create_directories(run_dir);
    const std::filesystem::path path = run_dir / "exit_policy_dump.json";
    nlohmann::json payload;
    payload["exit_policy_v1"] = {
        {"enabled", result.post_entry_risk_telemetry.exit_policy_v1_enabled},
        {"partial_tp_enabled", result.post_entry_risk_telemetry.exit_policy_partial_tp_enabled},
        {"breakeven_enabled", result.post_entry_risk_telemetry.exit_policy_breakeven_enabled},
        {"trailing_enabled", result.post_entry_risk_telemetry.exit_policy_trailing_enabled},
        {"tp_full_enabled", result.post_entry_risk_telemetry.exit_policy_tp_full_enabled},
        {"stop_loss_enabled", result.post_entry_risk_telemetry.exit_policy_stop_loss_enabled},
        {"time_exit_enabled", result.post_entry_risk_telemetry.exit_policy_time_exit_enabled},
        {"time_exit_max_holding_minutes",
         result.post_entry_risk_telemetry.exit_policy_time_exit_max_holding_minutes},
        {"rr_full", result.post_entry_risk_telemetry.exit_policy_rr_full},
    };
    payload["upbit_constraints"] = {
        {"min_order_krw", kStructuralMinOrderKrw},
        {"fee_rate_roundtrip_default", kStructuralFeeRatePerLeg * 2.0},
        {"slippage_model_v1", {
            {"alpha", kStructuralSlippageAlpha},
            {"cap_bps", kStructuralSlippageCapBps},
            {"formula", "min(spread_pct*10000*alpha, cap_bps)"},
        }},
    };
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }
    out << payload.dump(2) << "\n";
}

static double quantileFromSorted(const std::vector<double>& sorted_values, double q) {
    if (sorted_values.empty()) {
        return 0.0;
    }
    const double qv = std::clamp(q, 0.0, 1.0);
    if (sorted_values.size() == 1) {
        return sorted_values.front();
    }
    const double pos = qv * static_cast<double>(sorted_values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(pos));
    if (lo == hi) {
        return sorted_values[lo];
    }
    const double weight = pos - static_cast<double>(lo);
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * weight;
}

static nlohmann::json loadRunProvenanceJson(const std::filesystem::path& run_dir) {
    const std::filesystem::path path = run_dir / "run_provenance.json";
    if (!std::filesystem::exists(path)) {
        return nlohmann::json::object();
    }
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            return nlohmann::json::object();
        }
        nlohmann::json payload;
        in >> payload;
        if (!payload.is_object()) {
            return nlohmann::json::object();
        }
        return payload;
    } catch (...) {
        return nlohmann::json::object();
    }
}

static nlohmann::json writeStructuralEvArtifacts(
    const BacktestResult& result,
    const std::filesystem::path& run_dir
) {
    std::filesystem::create_directories(run_dir);
    const std::filesystem::path rows_path = run_dir / "structural_ev_trade_rows.jsonl";
    const std::filesystem::path breakdown_path = run_dir / "structural_ev_breakdown.json";
    const std::filesystem::path summary_csv_path = run_dir / "structural_ev_summary.csv";
    const nlohmann::json run_provenance = loadRunProvenanceJson(run_dir);

    std::ofstream rows_out(rows_path, std::ios::binary | std::ios::trunc);
    std::map<std::string, StructuralMarketAgg> market_aggs;
    std::map<std::string, int> exit_reason_counts;
    std::map<std::string, int> min_order_effect_counts{
        {"NONE", 0},
        {"FORCED_FULL_EXIT", 0},
        {"PARTIAL_DISABLED", 0},
        {"DUST_LEFTOVER", 0},
    };

    double price_move_pnl_sum_krw = 0.0;
    double fee_sum_krw = 0.0;
    double slippage_sum_krw = 0.0;
    double structural_pnl_after_cost_sum_krw = 0.0;
    std::vector<double> predicted_structural_ev_bps_values;
    int row_count = 0;

    for (const auto& trade : result.trade_history_samples) {
        if (!(trade.quantity > 0.0) || !(trade.entry_price > 0.0) || !(trade.exit_price > 0.0)) {
            continue;
        }

        const std::string market = trade.market.empty() ? "UNKNOWN" : trade.market;
        const std::string exit_reason = normalizeStructuralExitReason(trade.exit_reason);
        const double entry_notional_krw = std::max(0.0, trade.entry_price * trade.quantity);
        const double exit_notional_krw = std::max(0.0, trade.exit_price * trade.quantity);
        const double notional_krw = std::max(entry_notional_krw, exit_notional_krw);

        const double price_move_pnl_krw = (trade.exit_price - trade.entry_price) * trade.quantity;
        const double fee_krw = (trade.fee_paid_krw > 0.0)
            ? trade.fee_paid_krw
            : ((entry_notional_krw + exit_notional_krw) * kStructuralFeeRatePerLeg);

        const double spread_pct_entry_est = estimateSpreadPctForStructuralEv(trade);
        const double spread_pct_exit_est = estimateSpreadPctForStructuralEv(trade);
        const double slippage_entry_bps = spreadToSlippageBps(spread_pct_entry_est);
        const double slippage_exit_bps = spreadToSlippageBps(spread_pct_exit_est);
        const double slippage_krw =
            (entry_notional_krw * (slippage_entry_bps / 10000.0)) +
            (exit_notional_krw * (slippage_exit_bps / 10000.0));

        const double structural_pnl_after_cost_krw = price_move_pnl_krw - fee_krw - slippage_krw;

        std::string min_order_effect_tag = "NONE";
        if (exit_reason == "tp_full_due_to_min_order") {
            min_order_effect_tag = "FORCED_FULL_EXIT";
        } else if (exit_reason == "tp_full" && entry_notional_krw < (2.0 * kStructuralMinOrderKrw)) {
            min_order_effect_tag = "PARTIAL_DISABLED";
        } else if (exit_notional_krw > 0.0 && exit_notional_krw < kStructuralMinOrderKrw) {
            min_order_effect_tag = "DUST_LEFTOVER";
        }

        const double p_cal = std::clamp(
            std::isfinite(trade.probabilistic_h5_calibrated) ? trade.probabilistic_h5_calibrated : 0.5,
            0.0,
            1.0
        );
        const double tp_pct = std::max(0.0, trade.initial_take_profit_distance_pct);
        const double sl_pct = std::max(0.0, trade.initial_stop_loss_distance_pct);
        const double slippage_bps = slippage_entry_bps + slippage_exit_bps;
        const double cost_bps = (kStructuralFeeRatePerLeg * 2.0 * 10000.0) + slippage_bps;
        const double predicted_structural_ev_bps =
            (tp_pct > 0.0 && sl_pct > 0.0)
                ? ((10000.0 * ((p_cal * tp_pct) - ((1.0 - p_cal) * sl_pct))) - cost_bps)
                : std::numeric_limits<double>::quiet_NaN();

        nlohmann::json row;
        row["event_type"] = (exit_reason == "partial_tp") ? "PARTIAL" : "EXIT";
        row["exit_reason"] = exit_reason;
        row["market"] = market;
        row["ts_entry"] = trade.entry_time;
        row["ts_exit"] = trade.exit_time;
        row["holding_sec"] = std::max(0.0, trade.holding_minutes * 60.0);
        row["entry_price"] = trade.entry_price;
        row["exit_price"] = trade.exit_price;
        row["qty"] = trade.quantity;
        row["notional_krw"] = notional_krw;
        row["price_move_pnl_krw"] = price_move_pnl_krw;
        row["fee_cost_krw"] = fee_krw;
        row["slippage_cost_krw"] = slippage_krw;
        row["structural_pnl_after_cost_krw"] = structural_pnl_after_cost_krw;
        row["realized_profit_loss_krw"] = trade.profit_loss_krw;
        row["realized_profit_loss_pct"] = trade.profit_loss_pct;
        row["min_order_effect_tag"] = min_order_effect_tag;
        row["p_calibrated"] = p_cal;
        row["tp_pct"] = tp_pct;
        row["sl_pct"] = sl_pct;
        row["spread_pct_entry_estimate"] = spread_pct_entry_est;
        row["spread_pct_exit_estimate"] = spread_pct_exit_est;
        row["fee_bps_roundtrip"] = (kStructuralFeeRatePerLeg * 2.0 * 10000.0);
        row["slippage_bps_estimate"] = slippage_bps;
        row["label_cost_bps"] = kStructuralLabelCostBps;
        row["predicted_structural_ev_bps"] = predicted_structural_ev_bps;
        row["backend_effective"] = result.entry_funnel.gate_vnext_backend_effective;
        row["backend_request"] = result.entry_funnel.gate_vnext_backend_request;
        row["topk_effective"] = result.entry_funnel.quality_topk_effective;
        row["run_config_path"] = run_provenance.value("config_path", std::string{});
        row["run_bundle_path"] = run_provenance.value("bundle_path", std::string{});
        row["run_lgbm_model_sha256"] = run_provenance.value("lgbm_model_sha256", std::string{});

        if (rows_out.is_open()) {
            rows_out << row.dump() << "\n";
        }

        auto& agg = market_aggs[market];
        agg.trade_count += 1;
        agg.price_move_pnl_sum_krw += price_move_pnl_krw;
        agg.fee_sum_krw += fee_krw;
        agg.slippage_sum_krw += slippage_krw;
        agg.structural_pnl_after_cost_sum_krw += structural_pnl_after_cost_krw;
        if (min_order_effect_tag == "FORCED_FULL_EXIT") {
            agg.forced_full_exit_count += 1;
        } else if (min_order_effect_tag == "PARTIAL_DISABLED") {
            agg.partial_disabled_count += 1;
        } else if (min_order_effect_tag == "DUST_LEFTOVER") {
            agg.dust_leftover_count += 1;
        }

        exit_reason_counts[exit_reason] += 1;
        min_order_effect_counts[min_order_effect_tag] += 1;
        price_move_pnl_sum_krw += price_move_pnl_krw;
        fee_sum_krw += fee_krw;
        slippage_sum_krw += slippage_krw;
        structural_pnl_after_cost_sum_krw += structural_pnl_after_cost_krw;
        if (std::isfinite(predicted_structural_ev_bps)) {
            predicted_structural_ev_bps_values.push_back(predicted_structural_ev_bps);
        }
        row_count += 1;
    }
    if (rows_out.is_open()) {
        rows_out.flush();
    }

    std::vector<std::pair<std::string, StructuralMarketAgg>> sorted_markets(
        market_aggs.begin(),
        market_aggs.end()
    );
    std::sort(
        sorted_markets.begin(),
        sorted_markets.end(),
        [](const auto& lhs, const auto& rhs) {
            if (lhs.second.structural_pnl_after_cost_sum_krw !=
                rhs.second.structural_pnl_after_cost_sum_krw) {
                return lhs.second.structural_pnl_after_cost_sum_krw <
                       rhs.second.structural_pnl_after_cost_sum_krw;
            }
            return lhs.first < rhs.first;
        }
    );

    std::sort(predicted_structural_ev_bps_values.begin(), predicted_structural_ev_bps_values.end());
    const nlohmann::json predicted_ev_summary = {
        {"count", static_cast<int>(predicted_structural_ev_bps_values.size())},
        {"min", predicted_structural_ev_bps_values.empty()
                    ? 0.0
                    : quantileFromSorted(predicted_structural_ev_bps_values, 0.0)},
        {"median", predicted_structural_ev_bps_values.empty()
                       ? 0.0
                       : quantileFromSorted(predicted_structural_ev_bps_values, 0.5)},
        {"max", predicted_structural_ev_bps_values.empty()
                    ? 0.0
                    : quantileFromSorted(predicted_structural_ev_bps_values, 1.0)},
    };

    nlohmann::json top_loss_markets = nlohmann::json::array();
    for (std::size_t i = 0; i < sorted_markets.size() && i < 3; ++i) {
        const auto& item = sorted_markets[i];
        top_loss_markets.push_back(
            {
                {"market", item.first},
                {"trade_count", item.second.trade_count},
                {"structural_pnl_after_cost_sum_krw", item.second.structural_pnl_after_cost_sum_krw},
                {"fee_sum_krw", item.second.fee_sum_krw},
                {"slippage_sum_krw", item.second.slippage_sum_krw},
            }
        );
    }

    nlohmann::json per_exit_reason = nlohmann::json::array();
    for (const auto& [reason, count] : exit_reason_counts) {
        per_exit_reason.push_back({{"exit_reason", reason}, {"count", count}});
    }
    std::sort(
        per_exit_reason.begin(),
        per_exit_reason.end(),
        [](const nlohmann::json& lhs, const nlohmann::json& rhs) {
            if (lhs.value("count", 0) != rhs.value("count", 0)) {
                return lhs.value("count", 0) > rhs.value("count", 0);
            }
            return lhs.value("exit_reason", std::string{}) < rhs.value("exit_reason", std::string{});
        }
    );

    nlohmann::json breakdown;
    breakdown["run_provenance"] = run_provenance;
    breakdown["backend_effective"] = result.entry_funnel.gate_vnext_backend_effective;
    breakdown["backend_request"] = result.entry_funnel.gate_vnext_backend_request;
    breakdown["topk_effective"] = result.entry_funnel.quality_topk_effective;
    breakdown["trade_rows_count"] = row_count;
    breakdown["component_aggregates"] = {
        {"price_move_pnl_sum_krw", price_move_pnl_sum_krw},
        {"fee_sum_krw", fee_sum_krw},
        {"slippage_sum_krw", slippage_sum_krw},
        {"structural_pnl_after_cost_sum_krw", structural_pnl_after_cost_sum_krw},
    };
    breakdown["slippage_model_v1"] = {
        {"alpha", kStructuralSlippageAlpha},
        {"cap_bps", kStructuralSlippageCapBps},
        {"formula", "min(spread_pct*10000*alpha, cap_bps)"},
    };
    breakdown["predicted_structural_ev_bps_summary"] = predicted_ev_summary;
    breakdown["min_order_effect_counts"] = min_order_effect_counts;
    breakdown["exit_reason_breakdown"] = per_exit_reason;
    breakdown["top_loss_markets"] = top_loss_markets;

    nlohmann::json per_market_rows = nlohmann::json::array();
    for (const auto& [market, agg] : sorted_markets) {
        per_market_rows.push_back(
            {
                {"market", market},
                {"trade_count", agg.trade_count},
                {"price_move_pnl_sum_krw", agg.price_move_pnl_sum_krw},
                {"fee_sum_krw", agg.fee_sum_krw},
                {"slippage_sum_krw", agg.slippage_sum_krw},
                {"structural_pnl_after_cost_sum_krw", agg.structural_pnl_after_cost_sum_krw},
                {"forced_full_exit_count", agg.forced_full_exit_count},
                {"partial_disabled_count", agg.partial_disabled_count},
                {"dust_leftover_count", agg.dust_leftover_count},
            }
        );
    }
    breakdown["per_market"] = per_market_rows;

    {
        std::ofstream out(breakdown_path, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out << breakdown.dump(2) << "\n";
        }
    }

    {
        std::ofstream out(summary_csv_path, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out << "market,trade_count,price_move_pnl_sum_krw,fee_sum_krw,slippage_sum_krw,structural_pnl_after_cost_sum_krw,forced_full_exit_count,partial_disabled_count,dust_leftover_count\n";
            out << "TOTAL,"
                << row_count << ","
                << price_move_pnl_sum_krw << ","
                << fee_sum_krw << ","
                << slippage_sum_krw << ","
                << structural_pnl_after_cost_sum_krw << ","
                << min_order_effect_counts["FORCED_FULL_EXIT"] << ","
                << min_order_effect_counts["PARTIAL_DISABLED"] << ","
                << min_order_effect_counts["DUST_LEFTOVER"] << "\n";
            for (const auto& [market, agg] : sorted_markets) {
                out << market << ","
                    << agg.trade_count << ","
                    << agg.price_move_pnl_sum_krw << ","
                    << agg.fee_sum_krw << ","
                    << agg.slippage_sum_krw << ","
                    << agg.structural_pnl_after_cost_sum_krw << ","
                    << agg.forced_full_exit_count << ","
                    << agg.partial_disabled_count << ","
                    << agg.dust_leftover_count << "\n";
            }
        }
    }

    return breakdown;
}

nlohmann::json buildBacktestResultJson(const BacktestResult& result) {
    nlohmann::json j;
    j["final_balance"] = result.final_balance;
    j["total_profit"] = result.total_profit;
    j["max_drawdown"] = result.max_drawdown;
    j["total_trades"] = result.total_trades;
    j["winning_trades"] = result.winning_trades;
    j["losing_trades"] = result.losing_trades;
    j["win_rate"] = result.win_rate;
    j["avg_win_krw"] = result.avg_win_krw;
    j["avg_loss_krw"] = result.avg_loss_krw;
    j["profit_factor"] = result.profit_factor;
    j["expectancy_krw"] = result.expectancy_krw;
    j["avg_holding_minutes"] = result.avg_holding_minutes;
    j["avg_fee_krw"] = result.avg_fee_krw;
    j["intrabar_stop_tp_collision_count"] = result.intrabar_stop_tp_collision_count;
    j["exit_reason_counts"] = result.exit_reason_counts;
    j["entry_rejection_reason_counts"] = result.entry_rejection_reason_counts;
    j["no_signal_pattern_counts"] = result.no_signal_pattern_counts;
    j["entry_quality_edge_gap_buckets"] = result.entry_quality_edge_gap_buckets;
    j["intrabar_collision_by_strategy"] = result.intrabar_collision_by_strategy;
    j["strategy_collect_exception_count"] = result.strategy_collect_exception_count;
    const int partial_ratio_samples = std::max(0, result.post_entry_risk_telemetry.adaptive_partial_ratio_samples);
    const double partial_ratio_avg = (partial_ratio_samples > 0)
        ? (result.post_entry_risk_telemetry.adaptive_partial_ratio_sum / static_cast<double>(partial_ratio_samples))
        : 0.0;
    j["post_entry_risk_telemetry"] = {
        {"exit_policy_v1_enabled",
         result.post_entry_risk_telemetry.exit_policy_v1_enabled},
        {"exit_policy_partial_tp_enabled",
         result.post_entry_risk_telemetry.exit_policy_partial_tp_enabled},
        {"exit_policy_breakeven_enabled",
         result.post_entry_risk_telemetry.exit_policy_breakeven_enabled},
        {"exit_policy_trailing_enabled",
         result.post_entry_risk_telemetry.exit_policy_trailing_enabled},
        {"exit_policy_tp_full_enabled",
         result.post_entry_risk_telemetry.exit_policy_tp_full_enabled},
        {"exit_policy_stop_loss_enabled",
         result.post_entry_risk_telemetry.exit_policy_stop_loss_enabled},
        {"exit_policy_time_exit_enabled",
         result.post_entry_risk_telemetry.exit_policy_time_exit_enabled},
        {"exit_policy_time_exit_max_holding_minutes",
         result.post_entry_risk_telemetry.exit_policy_time_exit_max_holding_minutes},
        {"exit_policy_rr_full",
         result.post_entry_risk_telemetry.exit_policy_rr_full},
        {"adaptive_stop_updates", result.post_entry_risk_telemetry.adaptive_stop_updates},
        {"adaptive_tp_recalibration_updates", result.post_entry_risk_telemetry.adaptive_tp_recalibration_updates},
        {"adaptive_partial_ratio_samples", partial_ratio_samples},
        {"adaptive_partial_ratio_avg", partial_ratio_avg},
        {"be_move_attempt_count", result.post_entry_risk_telemetry.be_move_attempt_count},
        {"be_move_applied_count", result.post_entry_risk_telemetry.be_move_applied_count},
        {"be_move_skipped_due_to_delay_count",
         result.post_entry_risk_telemetry.be_move_skipped_due_to_delay_count},
        {"stop_loss_after_partial_tp_count",
         result.post_entry_risk_telemetry.stop_loss_after_partial_tp_count},
        {"stop_loss_before_partial_tp_count",
         result.post_entry_risk_telemetry.stop_loss_before_partial_tp_count},
        {"be_after_partial_tp_delay_sec",
         result.post_entry_risk_telemetry.be_after_partial_tp_delay_sec},
        {"stop_loss_trigger_count",
         result.post_entry_risk_telemetry.stop_loss_trigger_count},
        {"stop_loss_pnl_sum_krw",
         result.post_entry_risk_telemetry.stop_loss_pnl_sum_krw},
        {"stop_loss_avg_pnl_krw",
         result.post_entry_risk_telemetry.stop_loss_avg_pnl_krw},
        {"partial_tp_exit_count",
         result.post_entry_risk_telemetry.partial_tp_exit_count},
        {"take_profit_full_count",
         result.post_entry_risk_telemetry.take_profit_full_count},
        {"take_profit_full_pnl_sum_krw",
         result.post_entry_risk_telemetry.take_profit_full_pnl_sum_krw},
        {"take_profit_full_avg_pnl_krw",
         result.post_entry_risk_telemetry.take_profit_full_avg_pnl_krw},
        {"tp_hit_rate",
         result.post_entry_risk_telemetry.tp_hit_rate},
        {"trending_trade_count",
         result.post_entry_risk_telemetry.trending_trade_count},
        {"trending_holding_minutes_sum",
         result.post_entry_risk_telemetry.trending_holding_minutes_sum},
        {"avg_holding_minutes_trending",
         result.post_entry_risk_telemetry.avg_holding_minutes_trending},
        {"stop_loss_distance_samples_trending",
         result.post_entry_risk_telemetry.stop_loss_distance_samples_trending},
        {"stop_loss_distance_sum_pct_trending",
         result.post_entry_risk_telemetry.stop_loss_distance_sum_pct_trending},
        {"avg_stop_loss_distance_pct_trending",
         result.post_entry_risk_telemetry.avg_stop_loss_distance_pct_trending},
        {"take_profit_distance_samples_trending",
         result.post_entry_risk_telemetry.take_profit_distance_samples_trending},
        {"take_profit_distance_sum_pct_trending",
         result.post_entry_risk_telemetry.take_profit_distance_sum_pct_trending},
        {"avg_take_profit_distance_pct_trending",
         result.post_entry_risk_telemetry.avg_take_profit_distance_pct_trending},
        {"adaptive_partial_ratio_histogram", {
            {"0.35_0.44", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[0]},
            {"0.45_0.54", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[1]},
            {"0.55_0.64", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[2]},
            {"0.65_0.74", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[3]},
            {"0.75_0.80", result.post_entry_risk_telemetry.adaptive_partial_ratio_histogram[4]}
        }}
    };
    nlohmann::json correlation_near_cap_rows = nlohmann::json::array();
    for (const auto& row : result.phase4_portfolio_diagnostics.correlation_near_cap_candidates) {
        correlation_near_cap_rows.push_back({
            {"market", row.market},
            {"cluster", row.cluster},
            {"exposure_current", row.exposure_current},
            {"cluster_cap_value", row.cluster_cap_value},
            {"candidate_position_size", row.candidate_position_size},
            {"projected_exposure", row.projected_exposure},
            {"headroom_before", row.headroom_before},
            {"rejected_by_cluster_cap", row.rejected_by_cluster_cap}
        });
    }
    nlohmann::json correlation_penalty_score_rows = nlohmann::json::array();
    for (const auto& row : result.phase4_portfolio_diagnostics.correlation_penalty_score_samples) {
        correlation_penalty_score_rows.push_back({
            {"market", row.market},
            {"cluster", row.cluster},
            {"score_before_penalty", row.score_before_penalty},
            {"penalty", row.penalty},
            {"score_after_penalty", row.score_after_penalty},
            {"rejected_by_penalty", row.rejected_by_penalty}
        });
    }
    nlohmann::json cluster_cap_debug_rows = nlohmann::json::array();
    for (const auto& row : result.phase4_portfolio_diagnostics.cluster_cap_debug_trace_samples) {
        cluster_cap_debug_rows.push_back({
            {"market", row.market},
            {"cluster", row.cluster},
            {"cluster_exposure_before", row.cluster_exposure_before},
            {"candidate_notional_fraction", row.candidate_notional_fraction},
            {"cluster_cap_value", row.cluster_cap_value},
            {"would_exceed", row.would_exceed},
            {"after_accept_cluster_exposure", row.after_accept_cluster_exposure}
        });
    }
    j["phase4_portfolio_diagnostics"] = {
        {"enabled", result.phase4_portfolio_diagnostics.enabled},
        {"phase4_portfolio_allocator_enabled",
         result.phase4_portfolio_diagnostics.phase4_portfolio_allocator_enabled},
        {"phase4_correlation_control_enabled",
         result.phase4_portfolio_diagnostics.phase4_correlation_control_enabled},
        {"phase4_risk_budget_enabled",
         result.phase4_portfolio_diagnostics.phase4_risk_budget_enabled},
        {"phase4_drawdown_governor_enabled",
         result.phase4_portfolio_diagnostics.phase4_drawdown_governor_enabled},
        {"phase4_execution_aware_sizing_enabled",
         result.phase4_portfolio_diagnostics.phase4_execution_aware_sizing_enabled},
        {"phase4_portfolio_diagnostics_enabled",
         result.phase4_portfolio_diagnostics.phase4_portfolio_diagnostics_enabled},
        {"allocator_top_k", result.phase4_portfolio_diagnostics.allocator_top_k},
        {"allocator_min_score", result.phase4_portfolio_diagnostics.allocator_min_score},
        {"risk_budget_per_market_cap",
         result.phase4_portfolio_diagnostics.risk_budget_per_market_cap},
        {"risk_budget_gross_cap", result.phase4_portfolio_diagnostics.risk_budget_gross_cap},
        {"risk_budget_cap", result.phase4_portfolio_diagnostics.risk_budget_cap},
        {"risk_budget_regime_multipliers",
         result.phase4_portfolio_diagnostics.risk_budget_regime_multipliers},
        {"regime_budget_multiplier_applied_count",
         result.phase4_portfolio_diagnostics.regime_budget_multiplier_applied_count},
        {"regime_budget_multiplier_count_by_regime",
         result.phase4_portfolio_diagnostics.regime_budget_multiplier_count_by_regime},
        {"regime_budget_multiplier_sum_by_regime",
         result.phase4_portfolio_diagnostics.regime_budget_multiplier_sum_by_regime},
        {"regime_budget_multiplier_avg_by_regime",
         result.phase4_portfolio_diagnostics.regime_budget_multiplier_avg_by_regime},
        {"drawdown_current", result.phase4_portfolio_diagnostics.drawdown_current},
        {"drawdown_budget_multiplier",
         result.phase4_portfolio_diagnostics.drawdown_budget_multiplier},
        {"correlation_default_cluster_cap",
         result.phase4_portfolio_diagnostics.correlation_default_cluster_cap},
        {"correlation_market_cluster_count",
         result.phase4_portfolio_diagnostics.correlation_market_cluster_count},
        {"execution_liquidity_low_threshold",
         result.phase4_portfolio_diagnostics.execution_liquidity_low_threshold},
        {"execution_liquidity_mid_threshold",
         result.phase4_portfolio_diagnostics.execution_liquidity_mid_threshold},
        {"execution_min_position_size",
         result.phase4_portfolio_diagnostics.execution_min_position_size},
        {"candidates_total", result.phase4_portfolio_diagnostics.candidates_total},
        {"selected_total", result.phase4_portfolio_diagnostics.selected_total},
        {"rejected_by_budget", result.phase4_portfolio_diagnostics.rejected_by_budget},
        {"rejected_by_cluster_cap", result.phase4_portfolio_diagnostics.rejected_by_cluster_cap},
        {"rejected_by_correlation_penalty",
         result.phase4_portfolio_diagnostics.rejected_by_correlation_penalty},
        {"cluster_cap_skips_count", result.phase4_portfolio_diagnostics.cluster_cap_skips_count},
        {"cluster_cap_would_exceed_count",
         result.phase4_portfolio_diagnostics.cluster_cap_would_exceed_count},
        {"cluster_exposure_update_count",
         result.phase4_portfolio_diagnostics.cluster_exposure_update_count},
        {"rejected_by_execution_cap", result.phase4_portfolio_diagnostics.rejected_by_execution_cap},
        {"rejected_by_drawdown_governor",
         result.phase4_portfolio_diagnostics.rejected_by_drawdown_governor},
        {"candidate_snapshot_total", result.phase4_portfolio_diagnostics.candidate_snapshot_total},
        {"candidate_snapshot_sampled", result.phase4_portfolio_diagnostics.candidate_snapshot_sampled},
        {"selection_rate", result.phase4_portfolio_diagnostics.selection_rate},
        {"correlation_constraint_apply_stage",
         result.phase4_portfolio_diagnostics.correlation_constraint_apply_stage},
        {"correlation_constraint_unit",
         result.phase4_portfolio_diagnostics.correlation_constraint_unit},
        {"correlation_cluster_eval_count",
         result.phase4_portfolio_diagnostics.correlation_cluster_eval_count},
        {"correlation_cluster_near_cap_count",
         result.phase4_portfolio_diagnostics.correlation_cluster_near_cap_count},
        {"correlation_penalty_applied_count",
         result.phase4_portfolio_diagnostics.correlation_penalty_applied_count},
        {"correlation_penalty_avg",
         result.phase4_portfolio_diagnostics.correlation_penalty_avg},
        {"correlation_penalty_max",
         result.phase4_portfolio_diagnostics.correlation_penalty_max},
        {"correlation_cluster_exposure_current",
         result.phase4_portfolio_diagnostics.correlation_cluster_exposure_current},
        {"correlation_cluster_cap_values",
         result.phase4_portfolio_diagnostics.correlation_cluster_cap_values},
        {"correlation_near_cap_candidates", correlation_near_cap_rows},
        {"correlation_penalty_score_samples", correlation_penalty_score_rows},
        {"cluster_cap_debug_trace_samples", cluster_cap_debug_rows}
    };
    j["strategyless_exit_diagnostics"] = {
        {"position_checks", result.strategyless_position_checks},
        {"runtime_archetype_checks", result.strategyless_runtime_archetype_checks},
        {"risk_exit_signals", result.strategyless_risk_exit_signals},
        {"current_stop_hits", result.strategyless_current_stop_hits},
        {"current_tp1_hits", result.strategyless_current_tp1_hits},
        {"current_tp2_hits", result.strategyless_current_tp2_hits}
    };

    nlohmann::json strategy_exit_trigger_samples = nlohmann::json::array();
    for (const auto& sample : result.entry_funnel.strategy_exit_trigger_samples) {
        strategy_exit_trigger_samples.push_back({
            {"ts_ms", sample.ts_ms},
            {"market", sample.market},
            {"regime", sample.regime},
            {"unrealized_pnl_at_trigger", sample.unrealized_pnl_at_trigger},
            {"holding_time_seconds", sample.holding_time_seconds},
            {"reason_code", sample.reason_code}
        });
    }
    nlohmann::json strategy_exit_clamp_samples = nlohmann::json::array();
    for (const auto& sample : result.entry_funnel.strategy_exit_clamp_samples) {
        strategy_exit_clamp_samples.push_back({
            {"ts_ms", sample.ts_ms},
            {"market", sample.market},
            {"regime", sample.regime},
            {"stop_loss_price", sample.stop_loss_price},
            {"exit_price_before_clamp", sample.exit_price_before_clamp},
            {"exit_price_after_clamp", sample.exit_price_after_clamp},
            {"pnl_before_clamp", sample.pnl_before_clamp},
            {"pnl_after_clamp", sample.pnl_after_clamp},
            {"reason_code", sample.reason_code}
        });
    }
    j["entry_funnel"] = {
        {"entry_rounds", result.entry_funnel.entry_rounds},
        {"skipped_due_to_open_position", result.entry_funnel.skipped_due_to_open_position},
        {"no_signal_generated", result.entry_funnel.no_signal_generated},
        {"filtered_out_by_policy", result.entry_funnel.filtered_out_by_policy},
        {"gate_system_version_effective", result.entry_funnel.gate_system_version_effective},
        {"quality_topk_effective", result.entry_funnel.quality_topk_effective},
        {"gate_vnext_backend_provenance",
         {
             {"backend_request", result.entry_funnel.gate_vnext_backend_request},
             {"backend_effective", result.entry_funnel.gate_vnext_backend_effective},
             {"lgbm_model_sha256", result.entry_funnel.gate_vnext_lgbm_model_sha256},
         }},
        {"stage_funnel_vnext",
         {
             {"s0_snapshots_valid", result.entry_funnel.gate_vnext_s0_snapshots_valid},
             {"s1_selected_topk", result.entry_funnel.gate_vnext_s1_selected_topk},
             {"s2_sized_count", result.entry_funnel.gate_vnext_s2_sized_count},
             {"s3_exec_gate_pass", result.entry_funnel.gate_vnext_s3_exec_gate_pass},
             {"s4_submitted", result.entry_funnel.gate_vnext_s4_submitted},
             {"s5_filled", result.entry_funnel.gate_vnext_s5_filled},
             {"drop_ev_negative_count", result.entry_funnel.gate_vnext_drop_ev_negative_count},
             {"ev_negative_size_zero_count", result.entry_funnel.gate_vnext_ev_negative_size_zero_count},
             {"ev_positive_size_gt_zero_count", result.entry_funnel.gate_vnext_ev_positive_size_gt_zero_count},
             {"expected_value_from_prob_min_bps",
              result.entry_funnel.gate_vnext_expected_value_from_prob_min_bps},
             {"expected_value_from_prob_median_bps",
              result.entry_funnel.gate_vnext_expected_value_from_prob_median_bps},
             {"expected_value_from_prob_max_bps",
              result.entry_funnel.gate_vnext_expected_value_from_prob_max_bps},
             {"p_cal_min", result.entry_funnel.gate_vnext_p_cal_min},
             {"p_cal_median", result.entry_funnel.gate_vnext_p_cal_median},
             {"p_cal_max", result.entry_funnel.gate_vnext_p_cal_max},
             {"tp_pct_min", result.entry_funnel.gate_vnext_tp_pct_min},
             {"tp_pct_median", result.entry_funnel.gate_vnext_tp_pct_median},
             {"tp_pct_max", result.entry_funnel.gate_vnext_tp_pct_max},
             {"sl_pct_min", result.entry_funnel.gate_vnext_sl_pct_min},
             {"sl_pct_median", result.entry_funnel.gate_vnext_sl_pct_median},
             {"sl_pct_max", result.entry_funnel.gate_vnext_sl_pct_max},
             {"ev_in_min_bps", result.entry_funnel.gate_vnext_ev_in_min_bps},
             {"ev_in_median_bps", result.entry_funnel.gate_vnext_ev_in_median_bps},
             {"ev_in_max_bps", result.entry_funnel.gate_vnext_ev_in_max_bps},
             {"ev_for_size_min_bps", result.entry_funnel.gate_vnext_ev_for_size_min_bps},
             {"ev_for_size_median_bps", result.entry_funnel.gate_vnext_ev_for_size_median_bps},
             {"ev_for_size_max_bps", result.entry_funnel.gate_vnext_ev_for_size_max_bps},
             {"size_fraction_min", result.entry_funnel.gate_vnext_size_fraction_min},
             {"size_fraction_median", result.entry_funnel.gate_vnext_size_fraction_median},
             {"size_fraction_max", result.entry_funnel.gate_vnext_size_fraction_max},
             {"scan_rounds", result.entry_funnel.gate_vnext_scan_rounds}
         }},
        {"reject_expected_edge_negative_count",
         result.entry_funnel.reject_expected_edge_negative_count},
        {"reject_regime_entry_disabled_count",
         result.entry_funnel.reject_regime_entry_disabled_count},
        {"reject_regime_entry_disabled_by_regime",
         result.entry_funnel.reject_regime_entry_disabled_by_regime},
        {"regime_entry_disable_enabled",
         result.entry_funnel.regime_entry_disable_enabled},
        {"regime_entry_disable",
         result.entry_funnel.regime_entry_disable},
        {"strategy_exit_mode_effective", result.entry_funnel.strategy_exit_mode_effective},
        {"strategy_exit_triggered_count", result.entry_funnel.strategy_exit_triggered_count},
        {"strategy_exit_would_trigger_count", result.entry_funnel.strategy_exit_triggered_count},
        {"strategy_exit_observe_only_suppressed_count",
         result.entry_funnel.strategy_exit_observe_only_suppressed_count},
        {"strategy_exit_executed_count", result.entry_funnel.strategy_exit_executed_count},
        {"strategy_exit_clamp_applied_count", result.entry_funnel.strategy_exit_clamp_applied_count},
        {"strategy_exit_triggered_by_market",
         result.entry_funnel.strategy_exit_triggered_by_market},
        {"strategy_exit_triggered_by_regime",
         result.entry_funnel.strategy_exit_triggered_by_regime},
        {"strategy_exit_trigger_samples", strategy_exit_trigger_samples},
        {"strategy_exit_would_trigger_samples", strategy_exit_trigger_samples},
        {"strategy_exit_clamp_samples", strategy_exit_clamp_samples},
        {"no_best_signal", result.entry_funnel.no_best_signal},
        {"blocked_pattern_gate", result.entry_funnel.blocked_pattern_gate},
        {"blocked_rr_rebalance", result.entry_funnel.blocked_rr_rebalance},
        {"blocked_risk_gate", result.entry_funnel.blocked_risk_gate},
        {"blocked_risk_gate_strategy_ev", result.entry_funnel.blocked_risk_gate_strategy_ev},
        {"blocked_risk_gate_strategy_ev_severe_threshold", result.entry_funnel.blocked_risk_gate_strategy_ev_severe_threshold},
        {"blocked_risk_gate_strategy_ev_catastrophic_history", result.entry_funnel.blocked_risk_gate_strategy_ev_catastrophic_history},
        {"blocked_risk_gate_strategy_ev_loss_asymmetry", result.entry_funnel.blocked_risk_gate_strategy_ev_loss_asymmetry},
        {"blocked_risk_gate_strategy_ev_unknown", result.entry_funnel.blocked_risk_gate_strategy_ev_unknown},
        {"blocked_risk_gate_regime", result.entry_funnel.blocked_risk_gate_regime},
        {"blocked_risk_gate_entry_quality_invalid_levels", result.entry_funnel.blocked_risk_gate_entry_quality_invalid_levels},
        {"blocked_risk_gate_other", result.entry_funnel.blocked_risk_gate_other},
        {"blocked_risk_manager", result.entry_funnel.blocked_risk_manager},
        {"blocked_min_order_or_capital", result.entry_funnel.blocked_min_order_or_capital},
        {"blocked_order_sizing", result.entry_funnel.blocked_order_sizing},
        {"entries_executed", result.entry_funnel.entries_executed}
    };
    j["ranging_shadow"] = {
        {"shadow_count_total", result.ranging_shadow.shadow_count_total},
        {"shadow_count_by_regime", result.ranging_shadow.shadow_count_by_regime},
        {"shadow_count_by_market", result.ranging_shadow.shadow_count_by_market},
        {"shadow_would_pass_execution_guard_count",
         result.ranging_shadow.shadow_would_pass_execution_guard_count},
        {"shadow_edge_neg_count", result.ranging_shadow.shadow_edge_neg_count},
        {"shadow_edge_pos_count", result.ranging_shadow.shadow_edge_pos_count}
    };
    j["shadow_funnel"] = {
        {"rounds", result.shadow_funnel.rounds},
        {"primary_generated_signals", result.shadow_funnel.primary_generated_signals},
        {"primary_after_policy_filter", result.shadow_funnel.primary_after_policy_filter},
        {"shadow_after_policy_filter", result.shadow_funnel.shadow_after_policy_filter},
        {"primary_best_signal_available", result.shadow_funnel.primary_best_signal_available},
        {"shadow_best_signal_available", result.shadow_funnel.shadow_best_signal_available},
        {"supply_improved_rounds", result.shadow_funnel.supply_improved_rounds},
        {"policy_supply_lift_sum", result.shadow_funnel.policy_supply_lift_sum},
        {"avg_policy_supply_lift", result.shadow_funnel.avg_policy_supply_lift}
    };

    j["strategy_signal_funnel"] = nlohmann::json::array();
    for (const auto& sf : result.strategy_signal_funnel) {
        j["strategy_signal_funnel"].push_back({
            {"strategy_name", sf.strategy_name},
            {"generated_signals", sf.generated_signals},
            {"selected_best", sf.selected_best},
            {"blocked_by_risk_manager", sf.blocked_by_risk_manager},
            {"entries_executed", sf.entries_executed}
        });
    }
    j["strategy_collection_summaries"] = nlohmann::json::array();
    for (const auto& sc : result.strategy_collection_summaries) {
        j["strategy_collection_summaries"].push_back({
            {"strategy_name", sc.strategy_name},
            {"skipped_disabled", sc.skipped_disabled},
            {"no_signal", sc.no_signal},
            {"generated", sc.generated}
        });
    }

    j["strategy_summaries"] = nlohmann::json::array();
    for (const auto& s : result.strategy_summaries) {
        j["strategy_summaries"].push_back({
            {"strategy_name", s.strategy_name},
            {"total_trades", s.total_trades},
            {"winning_trades", s.winning_trades},
            {"losing_trades", s.losing_trades},
            {"win_rate", s.win_rate},
            {"total_profit", s.total_profit},
            {"avg_win_krw", s.avg_win_krw},
            {"avg_loss_krw", s.avg_loss_krw},
            {"profit_factor", s.profit_factor}
        });
    }

    j["pattern_summaries"] = nlohmann::json::array();
    for (const auto& p : result.pattern_summaries) {
        j["pattern_summaries"].push_back({
            {"strategy_name", p.strategy_name},
            {"entry_archetype", p.entry_archetype},
            {"regime", p.regime},
            {"volatility_bucket", p.volatility_bucket},
            {"liquidity_bucket", p.liquidity_bucket},
            {"strength_bucket", p.strength_bucket},
            {"expected_value_bucket", p.expected_value_bucket},
            {"reward_risk_bucket", p.reward_risk_bucket},
            {"total_trades", p.total_trades},
            {"winning_trades", p.winning_trades},
            {"losing_trades", p.losing_trades},
            {"win_rate", p.win_rate},
            {"total_profit", p.total_profit},
            {"avg_profit_krw", p.avg_profit_krw},
            {"profit_factor", p.profit_factor}
        });
    }

    j["trade_history_samples"] = nlohmann::json::array();
    for (const auto& t : result.trade_history_samples) {
        j["trade_history_samples"].push_back({
            {"market", t.market},
            {"strategy_name", t.strategy_name},
            {"entry_archetype", t.entry_archetype},
            {"regime", t.regime},
            {"exit_reason", t.exit_reason},
            {"entry_time", t.entry_time},
            {"exit_time", t.exit_time},
            {"entry_price", t.entry_price},
            {"exit_price", t.exit_price},
            {"quantity", t.quantity},
            {"initial_stop_loss_distance_pct", t.initial_stop_loss_distance_pct},
            {"initial_take_profit_distance_pct", t.initial_take_profit_distance_pct},
            {"holding_minutes", t.holding_minutes},
            {"profit_loss_krw", t.profit_loss_krw},
            {"profit_loss_pct", t.profit_loss_pct},
            {"fee_paid_krw", t.fee_paid_krw},
            {"signal_filter", t.signal_filter},
            {"signal_strength", t.signal_strength},
            {"liquidity_score", t.liquidity_score},
            {"volatility", t.volatility},
            {"expected_value", t.expected_value},
            {"reward_risk_ratio", t.reward_risk_ratio},
            {"probabilistic_runtime_applied", t.probabilistic_runtime_applied},
            {"probabilistic_h5_calibrated", t.probabilistic_h5_calibrated},
            {"probabilistic_h5_margin", t.probabilistic_h5_margin}
        });
    }

    j["phase4_candidate_snapshot_samples"] = nlohmann::json::array();
    for (const auto& s : result.phase4_candidate_snapshot_samples) {
        j["phase4_candidate_snapshot_samples"].push_back({
            {"market", s.market},
            {"decision_time", s.decision_time},
            {"strategy_name", s.strategy_name},
            {"regime", s.regime},
            {"volatility_bucket", s.volatility_bucket},
            {"liquidity_bucket", s.liquidity_bucket},
            {"expected_edge_after_cost_pct", s.expected_edge_after_cost_pct},
            {"expected_edge_tail_after_cost_pct", s.expected_edge_tail_after_cost_pct},
            {"expected_edge_calibrated_raw_bps", s.expected_edge_calibrated_raw_bps},
            {"expected_edge_calibrated_corrected_bps", s.expected_edge_calibrated_corrected_bps},
            {"margin", s.margin},
            {"implied_win", s.implied_win},
            {"prob_confidence_raw", s.prob_confidence_raw},
            {"prob_confidence", s.prob_confidence},
            {"prob_model_backend", s.prob_model_backend},
            {"lgbm_ev_affine_enabled", s.lgbm_ev_affine_enabled},
            {"lgbm_ev_affine_applied", s.lgbm_ev_affine_applied},
            {"lgbm_ev_affine_scale", s.lgbm_ev_affine_scale},
            {"lgbm_ev_affine_shift", s.lgbm_ev_affine_shift},
            {"ev_confidence", s.ev_confidence},
            {"signal_strength", s.signal_strength},
            {"liquidity_score", s.liquidity_score},
            {"volatility", s.volatility},
            {"entry_cost_pct", s.entry_cost_pct},
            {"exit_cost_pct", s.exit_cost_pct},
            {"tail_cost_pct", s.tail_cost_pct},
            {"cost_mode", s.cost_mode},
            {"edge_regressor_used", s.edge_regressor_used},
            {"selected", s.selected},
            {"has_open_position", s.has_open_position},
            {"open_position_qty", s.open_position_qty},
            {"open_position_unrealized_pnl", s.open_position_unrealized_pnl},
            {"open_position_time_in_position_minutes", s.open_position_time_in_position_minutes}
        });
    }

    return j;
}

}  // namespace

bool tryRunCliBacktest(int argc, char* argv[], Config& config, int& out_exit_code) {
    if (!(argc > 1 && std::string(argv[1]) == "--backtest" && argc > 2)) {
        return false;
    }

    bool json_mode = false;
    std::vector<std::string> cli_enabled_strategies;
    double cli_initial_capital = -1.0;
    bool cli_require_higher_tf_companions = false;
    bool cli_shadow_policy_only = false;

    auto trim_copy = [](std::string s) {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return std::string();
        }
        const auto last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    };
    auto normalize_strategy_name = [&](std::string s) {
        s = trim_copy(s);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "foundation" ||
            s == "foundation_adaptive_strategy" ||
            s == "foundation adaptive strategy") {
            return std::string("foundation_adaptive");
        }
        return s;
    };

    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json") {
            json_mode = true;
            continue;
        }
        if (arg == "--require-higher-tf-companions") {
            cli_require_higher_tf_companions = true;
            continue;
        }
        if (arg == "--shadow-policy-only") {
            cli_shadow_policy_only = true;
            continue;
        }
        if (arg == "--strategies" && i + 1 < argc) {
            std::string csv = argv[++i];
            size_t start = 0;
            while (start <= csv.size()) {
                const size_t comma = csv.find(',', start);
                std::string token = (comma == std::string::npos)
                    ? csv.substr(start)
                    : csv.substr(start, comma - start);
                token = normalize_strategy_name(token);
                if (!token.empty()) {
                    cli_enabled_strategies.push_back(token);
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            continue;
        }
        if (arg == "--initial-capital" && i + 1 < argc) {
            try {
                cli_initial_capital = std::stod(argv[++i]);
            } catch (...) {
                std::cerr << "Invalid --initial-capital value. Ignored.\n";
            }
        }
    }

    if (cli_initial_capital > 0.0) {
        config.setInitialCapital(cli_initial_capital);
    }
    if (!cli_enabled_strategies.empty()) {
        config.setEnabledStrategies(cli_enabled_strategies);
    }
    if (cli_shadow_policy_only) {
        config.setBacktestShadowPolicyOnly(true);
    }

    std::cout << "백테스트 모드(CLI) 실행\n";
    const std::string cli_backtest_path = argv[2];
    if (!std::filesystem::exists(cli_backtest_path)) {
        std::cerr << "백테스트 파일을 찾을 수 없습니다: " << cli_backtest_path << "\n";
        out_exit_code = 1;
        return true;
    }
    if (cli_require_higher_tf_companions) {
        const auto check = checkHigherTfCompanions(cli_backtest_path);
        if (!check.applicable || !check.missing_tokens.empty()) {
            printCompanionRequirementError(cli_backtest_path, check);
            out_exit_code = 1;
            return true;
        }
    }
    LOG_INFO("Starting Backtest Mode with file: {}", cli_backtest_path);

    backtest::BacktestEngine bt_engine;
    bt_engine.init(config);
    bt_engine.loadData(cli_backtest_path);
    bt_engine.run();

    auto result = bt_engine.getResult();
    const std::filesystem::path run_dir = autolife::utils::PathUtils::getRunDir();
    const nlohmann::json structural_ev_breakdown = writeStructuralEvArtifacts(result, run_dir);
    writeExitPolicyDumpArtifact(result, run_dir);
    if (json_mode) {
        auto payload = buildBacktestResultJson(result);
        payload["structural_ev"] = structural_ev_breakdown;
        std::cout << payload.dump() << "\n";
        out_exit_code = 0;
        return true;
    }

    BacktestSummaryOptions summary_options;
    summary_options.include_extended_metrics = true;
    printBacktestResultSummary(result, summary_options, std::cout);
    out_exit_code = 0;
    return true;
}

}  // namespace autolife::app
