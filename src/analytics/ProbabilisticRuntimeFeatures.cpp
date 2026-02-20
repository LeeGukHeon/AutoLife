#include "analytics/ProbabilisticRuntimeFeatures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_map>

namespace autolife {
namespace analytics {

namespace {

constexpr std::array<const char*, 36> kFeatureColumns = {
    "ret_1m",
    "ret_5m",
    "ret_20m",
    "ema_gap_12_26",
    "rsi_14",
    "atr_pct_14",
    "bb_width_20",
    "vol_ratio_20",
    "notional_ratio_20",
    "ctx_5m_age_min",
    "ctx_5m_ret_3",
    "ctx_5m_ret_12",
    "ctx_5m_ema_gap_20",
    "ctx_5m_rsi_14",
    "ctx_5m_atr_pct_14",
    "ctx_15m_age_min",
    "ctx_15m_ret_3",
    "ctx_15m_ret_12",
    "ctx_15m_ema_gap_20",
    "ctx_15m_rsi_14",
    "ctx_15m_atr_pct_14",
    "ctx_60m_age_min",
    "ctx_60m_ret_3",
    "ctx_60m_ret_12",
    "ctx_60m_ema_gap_20",
    "ctx_60m_rsi_14",
    "ctx_60m_atr_pct_14",
    "ctx_240m_age_min",
    "ctx_240m_ret_3",
    "ctx_240m_ret_12",
    "ctx_240m_ema_gap_20",
    "ctx_240m_rsi_14",
    "ctx_240m_atr_pct_14",
    "regime_trend_60_sign",
    "regime_trend_240_sign",
    "regime_vol_60_atr_pct"
};

long long normalizeTimestampMs(long long ts) {
    if (ts > 0 && ts < 1000000000000LL) {
        return ts * 1000LL;
    }
    return ts;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

double safeRet(const std::vector<double>& values, std::size_t idx_now, std::size_t idx_prev) {
    if (idx_now >= values.size() || idx_prev >= values.size()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double base = values[idx_prev];
    if (!(std::isfinite(base) && base > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double cur = values[idx_now];
    if (!std::isfinite(cur)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return (cur / base) - 1.0;
}

int sign3(double value) {
    if (!std::isfinite(value)) {
        return 0;
    }
    if (value > 0.0) {
        return 1;
    }
    if (value < 0.0) {
        return -1;
    }
    return 0;
}

std::vector<double> computeEmaPythonStyle(const std::vector<double>& values, int period) {
    std::vector<double> out(values.size(), std::numeric_limits<double>::quiet_NaN());
    if (values.empty()) {
        return out;
    }
    const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
    double ema = values.front();
    out[0] = ema;
    for (std::size_t i = 1; i < values.size(); ++i) {
        ema = (alpha * values[i]) + ((1.0 - alpha) * ema);
        out[i] = ema;
    }
    return out;
}

std::vector<double> computeRsiPythonStyle(const std::vector<double>& values, int period) {
    std::vector<double> out(values.size(), std::numeric_limits<double>::quiet_NaN());
    if (values.size() <= static_cast<std::size_t>(period)) {
        return out;
    }

    std::vector<double> gains(values.size(), 0.0);
    std::vector<double> losses(values.size(), 0.0);
    for (std::size_t i = 1; i < values.size(); ++i) {
        const double delta = values[i] - values[i - 1];
        gains[i] = std::max(0.0, delta);
        losses[i] = std::max(0.0, -delta);
    }

    double avg_gain = 0.0;
    double avg_loss = 0.0;
    for (int i = 1; i <= period; ++i) {
        avg_gain += gains[static_cast<std::size_t>(i)];
        avg_loss += losses[static_cast<std::size_t>(i)];
    }
    avg_gain /= static_cast<double>(period);
    avg_loss /= static_cast<double>(period);

    const std::size_t start = static_cast<std::size_t>(period);
    if (avg_loss == 0.0) {
        out[start] = 100.0;
    } else {
        const double rs = avg_gain / avg_loss;
        out[start] = 100.0 - (100.0 / (1.0 + rs));
    }

    for (std::size_t i = start + 1; i < values.size(); ++i) {
        avg_gain = ((avg_gain * static_cast<double>(period - 1)) + gains[i]) / static_cast<double>(period);
        avg_loss = ((avg_loss * static_cast<double>(period - 1)) + losses[i]) / static_cast<double>(period);
        if (avg_loss == 0.0) {
            out[i] = 100.0;
        } else {
            const double rs = avg_gain / avg_loss;
            out[i] = 100.0 - (100.0 / (1.0 + rs));
        }
    }
    return out;
}

std::vector<double> computeAtrPythonStyle(
    const std::vector<double>& highs,
    const std::vector<double>& lows,
    const std::vector<double>& closes,
    int period
) {
    std::vector<double> out(closes.size(), std::numeric_limits<double>::quiet_NaN());
    if (closes.size() <= static_cast<std::size_t>(period)) {
        return out;
    }

    std::vector<double> tr(closes.size(), 0.0);
    for (std::size_t i = 1; i < closes.size(); ++i) {
        const double h = highs[i];
        const double l = lows[i];
        const double prev_c = closes[i - 1];
        tr[i] = std::max({h - l, std::abs(h - prev_c), std::abs(l - prev_c)});
    }

    double atr = 0.0;
    for (int i = 1; i <= period; ++i) {
        atr += tr[static_cast<std::size_t>(i)];
    }
    atr /= static_cast<double>(period);
    out[static_cast<std::size_t>(period)] = atr;

    for (std::size_t i = static_cast<std::size_t>(period + 1); i < closes.size(); ++i) {
        atr = ((atr * static_cast<double>(period - 1)) + tr[i]) / static_cast<double>(period);
        out[i] = atr;
    }
    return out;
}

bool findLastIndexAtOrBefore(const std::vector<Candle>& candles, long long anchor_ts_ms, std::size_t& out_idx) {
    if (candles.empty()) {
        return false;
    }
    std::size_t lo = 0;
    std::size_t hi = candles.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo) / 2);
        const long long ts = normalizeTimestampMs(candles[mid].timestamp);
        if (ts <= anchor_ts_ms) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return false;
    }
    out_idx = lo - 1;
    return true;
}

double transformFeature(const std::string& key, double value, bool& ok) {
    ok = true;
    if (!std::isfinite(value)) {
        ok = false;
        return std::numeric_limits<double>::quiet_NaN();
    }

    double x = value;
    if (key == "rsi_14" || endsWith(key, "_rsi_14")) {
        x = (value - 50.0) / 50.0;
    } else if (endsWith(key, "_age_min")) {
        x = std::clamp(value, 0.0, 240.0) / 240.0;
    } else if (key == "vol_ratio_20" || key == "notional_ratio_20") {
        x = std::log(std::max(value, 1e-9));
    } else if (endsWith(key, "_sign")) {
        x = value;
    } else if (startsWith(key, "ret_") ||
               key.find("_ret_") != std::string::npos ||
               key.find("atr_pct") != std::string::npos ||
               key.find("bb_width") != std::string::npos ||
               key.find("gap_") != std::string::npos ||
               key.find("_gap_") != std::string::npos) {
        x = value * 100.0;
    }

    if (!std::isfinite(x)) {
        ok = false;
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::clamp(x, -8.0, 8.0);
}

void setError(std::string* error_message, const std::string& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

} // namespace

bool buildProbabilisticTransformedFeatures(
    const strategy::Signal& signal,
    const CoinMetrics& metrics,
    std::vector<double>& out_transformed_features,
    std::string* error_message
) {
    out_transformed_features.clear();

    const auto tf_1m_it = metrics.candles_by_tf.find("1m");
    if (tf_1m_it == metrics.candles_by_tf.end() || tf_1m_it->second.empty()) {
        setError(error_message, "missing_anchor_1m_candles");
        return false;
    }
    const auto& anchor_candles = tf_1m_it->second;
    if (anchor_candles.size() < 40) {
        setError(error_message, "insufficient_anchor_1m_candles");
        return false;
    }

    const std::size_t i = anchor_candles.size() - 1;
    const Candle& anchor = anchor_candles[i];
    const long long anchor_ts_ms = normalizeTimestampMs(anchor.timestamp);
    const double close_now = anchor.close;
    const double vol_now = anchor.volume;
    if (!(std::isfinite(close_now) && close_now > 0.0 && std::isfinite(vol_now) && vol_now >= 0.0)) {
        setError(error_message, "invalid_anchor_price_or_volume");
        return false;
    }

    std::vector<double> close(anchor_candles.size(), 0.0);
    std::vector<double> high(anchor_candles.size(), 0.0);
    std::vector<double> low(anchor_candles.size(), 0.0);
    std::vector<double> volume(anchor_candles.size(), 0.0);
    for (std::size_t idx = 0; idx < anchor_candles.size(); ++idx) {
        close[idx] = anchor_candles[idx].close;
        high[idx] = anchor_candles[idx].high;
        low[idx] = anchor_candles[idx].low;
        volume[idx] = anchor_candles[idx].volume;
    }

    const auto ema12 = computeEmaPythonStyle(close, 12);
    const auto ema26 = computeEmaPythonStyle(close, 26);
    const auto rsi14 = computeRsiPythonStyle(close, 14);
    const auto atr14 = computeAtrPythonStyle(high, low, close, 14);
    if (!(std::isfinite(ema12[i]) && std::isfinite(ema26[i]) &&
          std::isfinite(rsi14[i]) && std::isfinite(atr14[i]))) {
        setError(error_message, "invalid_anchor_indicators");
        return false;
    }

    if (i < 19) {
        setError(error_message, "insufficient_anchor_rolling_window");
        return false;
    }
    double sum_close_20 = 0.0;
    double sum_close_sq_20 = 0.0;
    double sum_volume_20 = 0.0;
    double sum_notional_20 = 0.0;
    for (std::size_t idx = i - 19; idx <= i; ++idx) {
        const double c = close[idx];
        const double v = volume[idx];
        if (!(std::isfinite(c) && c > 0.0 && std::isfinite(v) && v >= 0.0)) {
            setError(error_message, "invalid_anchor_rolling_values");
            return false;
        }
        sum_close_20 += c;
        sum_close_sq_20 += (c * c);
        sum_volume_20 += v;
        sum_notional_20 += (c * v);
    }
    const double mean_close_20 = sum_close_20 / 20.0;
    const double var_close_20 = std::max(0.0, (sum_close_sq_20 / 20.0) - (mean_close_20 * mean_close_20));
    const double std_close_20 = std::sqrt(var_close_20);
    const double bb_width_20 = (close_now > 0.0) ? ((4.0 * std_close_20) / close_now) : std::numeric_limits<double>::quiet_NaN();
    const double mean_volume_20 = sum_volume_20 / 20.0;
    const double mean_notional_20 = sum_notional_20 / 20.0;
    const double notional_now = close_now * vol_now;
    const double vol_ratio_20 = (mean_volume_20 > 0.0) ? (vol_now / mean_volume_20) : std::numeric_limits<double>::quiet_NaN();
    const double notional_ratio_20 = (mean_notional_20 > 0.0) ? (notional_now / mean_notional_20) : std::numeric_limits<double>::quiet_NaN();

    struct Ctx {
        double age_min = std::numeric_limits<double>::quiet_NaN();
        double ret_3 = std::numeric_limits<double>::quiet_NaN();
        double ret_12 = std::numeric_limits<double>::quiet_NaN();
        double ema_gap_20 = std::numeric_limits<double>::quiet_NaN();
        double rsi_14 = std::numeric_limits<double>::quiet_NaN();
        double atr_pct_14 = std::numeric_limits<double>::quiet_NaN();
    };

    const std::array<std::pair<int, const char*>, 4> tf_map = {{
        {5, "5m"},
        {15, "15m"},
        {60, "1h"},
        {240, "4h"},
    }};

    std::unordered_map<int, Ctx> ctx_values;
    for (const auto& [tf_minutes, tf_key] : tf_map) {
        const auto tf_it = metrics.candles_by_tf.find(tf_key);
        if (tf_it == metrics.candles_by_tf.end() || tf_it->second.size() < 25) {
            setError(error_message, "missing_or_insufficient_context_" + std::to_string(tf_minutes) + "m");
            return false;
        }

        const auto& tf_candles = tf_it->second;
        std::size_t ptr = 0;
        if (!findLastIndexAtOrBefore(tf_candles, anchor_ts_ms, ptr)) {
            setError(error_message, "context_alignment_failed_" + std::to_string(tf_minutes) + "m");
            return false;
        }
        if (ptr < 20) {
            setError(error_message, "context_warmup_failed_" + std::to_string(tf_minutes) + "m");
            return false;
        }

        std::vector<double> tf_close(tf_candles.size(), 0.0);
        std::vector<double> tf_high(tf_candles.size(), 0.0);
        std::vector<double> tf_low(tf_candles.size(), 0.0);
        for (std::size_t idx = 0; idx < tf_candles.size(); ++idx) {
            tf_close[idx] = tf_candles[idx].close;
            tf_high[idx] = tf_candles[idx].high;
            tf_low[idx] = tf_candles[idx].low;
        }
        const auto tf_ema20 = computeEmaPythonStyle(tf_close, 20);
        const auto tf_rsi14 = computeRsiPythonStyle(tf_close, 14);
        const auto tf_atr14 = computeAtrPythonStyle(tf_high, tf_low, tf_close, 14);

        const double close_tf = tf_close[ptr];
        const double ema20_tf = tf_ema20[ptr];
        const double rsi_tf = tf_rsi14[ptr];
        const double atr_tf = tf_atr14[ptr];
        if (!(std::isfinite(close_tf) && close_tf > 0.0 &&
              std::isfinite(ema20_tf) && std::isfinite(rsi_tf) && std::isfinite(atr_tf))) {
            setError(error_message, "invalid_context_indicator_" + std::to_string(tf_minutes) + "m");
            return false;
        }

        Ctx ctx;
        const long long ts_tf_ms = normalizeTimestampMs(tf_candles[ptr].timestamp);
        ctx.age_min = std::max(0.0, (static_cast<double>(anchor_ts_ms) - static_cast<double>(ts_tf_ms)) / 60000.0);
        ctx.ret_3 = safeRet(tf_close, ptr, ptr - 3);
        ctx.ret_12 = safeRet(tf_close, ptr, ptr - 12);
        ctx.ema_gap_20 = (close_tf - ema20_tf) / close_tf;
        ctx.rsi_14 = rsi_tf;
        ctx.atr_pct_14 = atr_tf / close_tf;
        ctx_values[tf_minutes] = ctx;
    }

    const auto& ctx5 = ctx_values[5];
    const auto& ctx15 = ctx_values[15];
    const auto& ctx60 = ctx_values[60];
    const auto& ctx240 = ctx_values[240];

    std::unordered_map<std::string, double> raw;
    raw["ret_1m"] = safeRet(close, i, i - 1);
    raw["ret_5m"] = safeRet(close, i, i - 5);
    raw["ret_20m"] = safeRet(close, i, i - 20);
    raw["ema_gap_12_26"] = (ema12[i] - ema26[i]) / close_now;
    raw["rsi_14"] = rsi14[i];
    raw["atr_pct_14"] = atr14[i] / close_now;
    raw["bb_width_20"] = bb_width_20;
    raw["vol_ratio_20"] = vol_ratio_20;
    raw["notional_ratio_20"] = notional_ratio_20;

    raw["ctx_5m_age_min"] = ctx5.age_min;
    raw["ctx_5m_ret_3"] = ctx5.ret_3;
    raw["ctx_5m_ret_12"] = ctx5.ret_12;
    raw["ctx_5m_ema_gap_20"] = ctx5.ema_gap_20;
    raw["ctx_5m_rsi_14"] = ctx5.rsi_14;
    raw["ctx_5m_atr_pct_14"] = ctx5.atr_pct_14;

    raw["ctx_15m_age_min"] = ctx15.age_min;
    raw["ctx_15m_ret_3"] = ctx15.ret_3;
    raw["ctx_15m_ret_12"] = ctx15.ret_12;
    raw["ctx_15m_ema_gap_20"] = ctx15.ema_gap_20;
    raw["ctx_15m_rsi_14"] = ctx15.rsi_14;
    raw["ctx_15m_atr_pct_14"] = ctx15.atr_pct_14;

    raw["ctx_60m_age_min"] = ctx60.age_min;
    raw["ctx_60m_ret_3"] = ctx60.ret_3;
    raw["ctx_60m_ret_12"] = ctx60.ret_12;
    raw["ctx_60m_ema_gap_20"] = ctx60.ema_gap_20;
    raw["ctx_60m_rsi_14"] = ctx60.rsi_14;
    raw["ctx_60m_atr_pct_14"] = ctx60.atr_pct_14;

    raw["ctx_240m_age_min"] = ctx240.age_min;
    raw["ctx_240m_ret_3"] = ctx240.ret_3;
    raw["ctx_240m_ret_12"] = ctx240.ret_12;
    raw["ctx_240m_ema_gap_20"] = ctx240.ema_gap_20;
    raw["ctx_240m_rsi_14"] = ctx240.rsi_14;
    raw["ctx_240m_atr_pct_14"] = ctx240.atr_pct_14;

    raw["regime_trend_60_sign"] = static_cast<double>(sign3(ctx60.ret_12));
    raw["regime_trend_240_sign"] = static_cast<double>(sign3(ctx240.ret_12));
    raw["regime_vol_60_atr_pct"] = ctx60.atr_pct_14;

    out_transformed_features.reserve(kFeatureColumns.size());
    for (const char* key_raw : kFeatureColumns) {
        const std::string key(key_raw);
        const auto it = raw.find(key);
        if (it == raw.end()) {
            setError(error_message, "missing_feature_" + key);
            out_transformed_features.clear();
            return false;
        }
        bool ok = false;
        const double x = transformFeature(key, it->second, ok);
        if (!ok || !std::isfinite(x)) {
            setError(error_message, "invalid_feature_" + key);
            out_transformed_features.clear();
            return false;
        }
        out_transformed_features.push_back(x);
    }

    (void)signal;
    return true;
}

} // namespace analytics
} // namespace autolife

