#include "common/ProbabilisticRegimeSpec.h"

#include "engine/EngineConfig.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <numeric>

namespace autolife {
namespace common {
namespace probabilistic_regime {

namespace {

double safeClose(const Candle& candle) {
    const double close = candle.close;
    if (!std::isfinite(close) || close <= 0.0) {
        return 0.0;
    }
    return close;
}

std::vector<double> recentReturns(const std::vector<Candle>& candles, std::size_t max_bars) {
    std::vector<double> out;
    if (candles.size() < 2 || max_bars == 0) {
        return out;
    }
    const std::size_t bars = std::min(max_bars, candles.size() - 1);
    const std::size_t begin = candles.size() - bars - 1;
    out.reserve(bars);
    for (std::size_t i = begin + 1; i < candles.size(); ++i) {
        const double prev = safeClose(candles[i - 1]);
        const double cur = safeClose(candles[i]);
        if (prev <= 0.0 || cur <= 0.0) {
            continue;
        }
        out.push_back((cur / prev) - 1.0);
    }
    return out;
}

bool meanStd(const std::vector<double>& values, double* mean, double* stddev) {
    if (mean == nullptr || stddev == nullptr || values.empty()) {
        return false;
    }
    const double m = std::accumulate(values.begin(), values.end(), 0.0) /
                     static_cast<double>(values.size());
    double var = 0.0;
    for (double v : values) {
        const double d = v - m;
        var += d * d;
    }
    var /= static_cast<double>(values.size());
    *mean = m;
    *stddev = std::sqrt(std::max(0.0, var));
    return true;
}

double pearsonCorr(const std::vector<double>& x, const std::vector<double>& y) {
    if (x.size() != y.size() || x.size() < 3) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double mx = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
    const double my = std::accumulate(y.begin(), y.end(), 0.0) / static_cast<double>(y.size());

    double cov = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double dx = x[i] - mx;
        const double dy = y[i] - my;
        cov += dx * dy;
        vx += dx * dx;
        vy += dy * dy;
    }
    if (vx <= 1e-12 || vy <= 1e-12) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::clamp(cov / std::sqrt(vx * vy), -1.0, 1.0);
}

} // namespace

Snapshot analyze(
    const std::vector<Candle>& market_candles,
    const engine::EngineConfig& cfg,
    const std::vector<Candle>* btc_reference_candles
) {
    Snapshot out;
    out.enabled = cfg.probabilistic_regime_spec_enabled;
    if (!out.enabled) {
        return out;
    }

    const int vol_window = std::clamp(cfg.probabilistic_regime_volatility_window, 10, 720);
    std::vector<double> market_returns = recentReturns(
        market_candles,
        static_cast<std::size_t>(vol_window)
    );
    if (market_returns.size() >= 4) {
        std::vector<double> abs_returns;
        abs_returns.reserve(market_returns.size());
        for (double v : market_returns) {
            abs_returns.push_back(std::abs(v));
        }
        if (abs_returns.size() >= 3) {
            const double latest_abs_ret = abs_returns.back();
            abs_returns.pop_back();
            double mean_abs = 0.0;
            double std_abs = 0.0;
            if (meanStd(abs_returns, &mean_abs, &std_abs) && std_abs > 1e-12) {
                out.volatility_zscore = (latest_abs_ret - mean_abs) / std_abs;
                out.sufficient_data = true;
            }
        }
    }

    if (market_candles.size() >= 2) {
        const int dd_window = std::clamp(cfg.probabilistic_regime_drawdown_window, 5, 720);
        const int use_window = std::min<int>(
            dd_window,
            static_cast<int>(market_candles.size()) - 1
        );
        if (use_window > 0) {
            const std::size_t begin = market_candles.size() - static_cast<std::size_t>(use_window) - 1;
            double peak_close = 0.0;
            for (std::size_t i = begin; i < market_candles.size(); ++i) {
                peak_close = std::max(peak_close, safeClose(market_candles[i]));
            }
            const double current_close = safeClose(market_candles.back());
            if (peak_close > 0.0 && current_close > 0.0 && current_close < peak_close) {
                const double drawdown_bps = ((peak_close - current_close) / peak_close) * 10000.0;
                out.drawdown_speed_bps = drawdown_bps / static_cast<double>(use_window);
            }
            out.sufficient_data = true;
        }
    }

    if (cfg.probabilistic_regime_enable_btc_correlation_shock &&
        btc_reference_candles != nullptr &&
        !btc_reference_candles->empty()) {
        const int corr_window = std::clamp(cfg.probabilistic_regime_correlation_window, 10, 720);
        std::vector<double> btc_returns = recentReturns(
            *btc_reference_candles,
            static_cast<std::size_t>(corr_window)
        );
        const std::size_t common_n = std::min(market_returns.size(), btc_returns.size());
        if (common_n >= 6) {
            std::vector<double> x(
                market_returns.end() - static_cast<std::ptrdiff_t>(common_n),
                market_returns.end()
            );
            std::vector<double> y(
                btc_returns.end() - static_cast<std::ptrdiff_t>(common_n),
                btc_returns.end()
            );
            const double corr = pearsonCorr(x, y);
            if (std::isfinite(corr)) {
                out.btc_correlation = corr;
                out.correlation_shock = std::max(0.0, 1.0 - corr);
                const double threshold = std::clamp(
                    cfg.probabilistic_regime_correlation_shock_threshold,
                    0.0,
                    2.0
                );
                out.correlation_shock_triggered = out.correlation_shock >= threshold;
                out.sufficient_data = true;
            }
        }
    }

    const double volatile_z = std::max(0.0, cfg.probabilistic_regime_volatile_zscore);
    const double hostile_z = std::max(volatile_z, cfg.probabilistic_regime_hostile_zscore);
    const double volatile_dd = std::max(0.0, cfg.probabilistic_regime_volatile_drawdown_speed_bps);
    const double hostile_dd = std::max(volatile_dd, cfg.probabilistic_regime_hostile_drawdown_speed_bps);

    const bool hostile =
        (out.volatility_zscore >= hostile_z) ||
        (out.drawdown_speed_bps >= hostile_dd) ||
        out.correlation_shock_triggered;
    const bool volatile_state =
        (out.volatility_zscore >= volatile_z) ||
        (out.drawdown_speed_bps >= volatile_dd);

    if (hostile) {
        out.state = State::HOSTILE;
    } else if (volatile_state) {
        out.state = State::VOLATILE;
    } else {
        out.state = State::NORMAL;
    }
    return out;
}

double thresholdAdd(const engine::EngineConfig& cfg, State state) {
    if (!cfg.probabilistic_regime_spec_enabled) {
        return 0.0;
    }
    switch (state) {
    case State::VOLATILE:
        return std::clamp(cfg.probabilistic_regime_volatile_threshold_add, 0.0, 0.40);
    case State::HOSTILE:
        return std::clamp(cfg.probabilistic_regime_hostile_threshold_add, 0.0, 0.60);
    case State::NORMAL:
    default:
        return 0.0;
    }
}

double sizeMultiplier(const engine::EngineConfig& cfg, State state) {
    if (!cfg.probabilistic_regime_spec_enabled) {
        return 1.0;
    }
    switch (state) {
    case State::VOLATILE:
        return std::clamp(cfg.probabilistic_regime_volatile_size_multiplier, 0.05, 1.0);
    case State::HOSTILE:
        return std::clamp(cfg.probabilistic_regime_hostile_size_multiplier, 0.01, 1.0);
    case State::NORMAL:
    default:
        return 1.0;
    }
}

bool blockNewEntries(const engine::EngineConfig& cfg, State state) {
    return cfg.probabilistic_regime_spec_enabled &&
           cfg.probabilistic_regime_hostile_block_new_entries &&
           state == State::HOSTILE;
}

const char* stateLabel(State state) {
    switch (state) {
    case State::NORMAL:
        return "normal";
    case State::VOLATILE:
        return "volatile";
    case State::HOSTILE:
        return "hostile";
    default:
        return "normal";
    }
}

} // namespace probabilistic_regime
} // namespace common
} // namespace autolife
