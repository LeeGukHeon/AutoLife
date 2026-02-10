#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include <memory>
#include <mutex>
#include <deque>
#include <array>
#include <set>
#include "engine/TradingEngine.h"

namespace autolife {
namespace strategy {

// ===== Market Regime (HMM 기반) =====

enum class MarketRegime {
    STRONG_UPTREND,      // 강한 상승
    WEAK_UPTREND,        // 약한 상승
    SIDEWAYS,            // 횡보
    WEAK_DOWNTREND,      // 약한 하락
    STRONG_DOWNTREND     // 강한 하락
};

// Hidden Markov Model
struct RegimeModel {
    std::array<std::array<double, 5>, 5> transition_prob;  // 상태 전이 확률
    std::array<double, 5> current_prob;                     // 현재 상태 확률
    
    RegimeModel() {
        // 기본 전이 확률 초기화 (대각선 우세 = 상태 유지 경향)
        for (int i = 0; i < 5; ++i) {
            for (int j = 0; j < 5; ++j) {
                transition_prob[i][j] = (i == j) ? 0.7 : 0.075;
            }
            current_prob[i] = 0.2;  // 균등 분포로 시작
        }
    }
};

// ===== Advanced Order Flow Metrics =====

struct AdvancedOrderFlowMetrics {
    double bid_ask_spread;              // 호가 스프레드 (%)
    double order_book_pressure;         // 호가 압력 (-1 ~ +1)
    double large_order_imbalance;       // 큰 주문 불균형
    double vwap_deviation;              // VWAP 대비 현재가 괴리도 (%)
    double order_book_depth_ratio;      // 호가창 깊이 비율
    double cumulative_delta;            // 누적 델타 (매수-매도 누적)
    double microstructure_score;        // 종합 미세구조 점수 (0-1)
    
    // Volume Profile
    struct VolumeProfile {
        double point_of_control;        // POC (최대 거래량 가격)
        double value_area_high;         // Value Area 상단
        double value_area_low;          // Value Area 하단
    } volume_profile;
    
    AdvancedOrderFlowMetrics()
        : bid_ask_spread(0), order_book_pressure(0), large_order_imbalance(0)
        , vwap_deviation(0), order_book_depth_ratio(0), cumulative_delta(0)
        , microstructure_score(0)
    {
        volume_profile.point_of_control = 0;
        volume_profile.value_area_high = 0;
        volume_profile.value_area_low = 0;
    }
};

// ===== Multi-Timeframe Signal =====

struct MultiTimeframeSignal {
    bool tf_1m_bullish;                 // 1분봉 상승
    bool tf_5m_bullish;                 // 5분봉 상승
    bool tf_15m_bullish;                // 15분봉 상승
    double alignment_score;             // 시간프레임 정렬 점수 (0-1)
    
    struct TimeframeMetrics {
        double rsi;
        double macd_histogram;
        double trend_strength;
    };
    
    TimeframeMetrics tf_1m;
    TimeframeMetrics tf_5m;
    TimeframeMetrics tf_15m;
    
    MultiTimeframeSignal()
        : tf_1m_bullish(false), tf_5m_bullish(false)
        , tf_15m_bullish(false), alignment_score(0)
    {}
};

// ===== Position Metrics (Kelly + Volatility) =====

struct PositionMetrics {
    double kelly_fraction;              // Full Kelly 비율
    double half_kelly;                  // Half Kelly (실제 사용)
    double volatility_adjusted;         // 변동성 조정 후
    double final_position_size;         // 최종 포지션 크기
    double expected_sharpe;             // 예상 Sharpe Ratio
    double max_loss_amount;             // 최대 손실 금액
    
    PositionMetrics()
        : kelly_fraction(0), half_kelly(0), volatility_adjusted(0)
        , final_position_size(0), expected_sharpe(0), max_loss_amount(0)
    {}
};

// ===== Dynamic Stops =====

struct DynamicStops {
    double stop_loss;                   // 손절가
    double take_profit_1;               // 1차 익절 (50%)
    double take_profit_2;               // 2차 익절 (100%)
    double trailing_start;              // Trailing 시작가
    double chandelier_exit;             // Chandelier Exit
    double parabolic_sar;               // Parabolic SAR
    
    DynamicStops()
        : stop_loss(0), take_profit_1(0), take_profit_2(0)
        , trailing_start(0), chandelier_exit(0), parabolic_sar(0)
    {}
};

// ===== Rolling Statistics =====

struct RollingStatistics {
    double rolling_sharpe_30d;          // 30일 Sharpe
    double rolling_sharpe_90d;          // 90일 Sharpe
    double rolling_sortino_30d;         // 30일 Sortino
    double rolling_calmar;              // Calmar Ratio
    double rolling_max_dd_30d;          // 30일 최대 낙폭
    double rolling_win_rate_100;        // 최근 100거래 승률
    double rolling_profit_factor;       // Profit Factor
    
    RollingStatistics()
        : rolling_sharpe_30d(0), rolling_sharpe_90d(0), rolling_sortino_30d(0)
        , rolling_calmar(0), rolling_max_dd_30d(0), rolling_win_rate_100(0)
        , rolling_profit_factor(0)
    {}
};

// ===== Walk-Forward Result =====

struct WalkForwardResult {
    double in_sample_sharpe;            // In-Sample Sharpe
    double out_sample_sharpe;           // Out-of-Sample Sharpe
    double degradation_ratio;           // 성능 저하 비율
    bool is_robust;                     // 견고성 여부
    
    WalkForwardResult()
        : in_sample_sharpe(0), out_sample_sharpe(0)
        , degradation_ratio(0), is_robust(false)
    {}
};

// ===== Advanced Momentum Strategy =====

class MomentumStrategy : public IStrategy {
public:
    MomentumStrategy(std::shared_ptr<network::UpbitHttpClient> client);
    
    // IStrategy 구현
    StrategyInfo getInfo() const override;
    
    Signal generateSignal(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        double current_price,
        double available_capital
    ) override;
    
    bool shouldEnter(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        double current_price
    ) override;
    
    bool shouldExit(
        const std::string& market,
        double entry_price,
        double current_price,
        double holding_time_seconds
    ) override;
    
    double calculateStopLoss(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) override;
    
    double calculateTakeProfit(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) override;
    
    double calculatePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics
    ) override;
    
    void setEnabled(bool enabled) override;
    bool isEnabled() const override;
    
    Statistics getStatistics() const override;
    void updateStatistics(const std::string& market, bool is_win, double profit_loss) override;
    bool onSignalAccepted(const Signal& signal, double allocated_capital) override;
    
    // === 추가 공개 메서드 ===
    
    // Trailing Stop 업데이트
    double updateTrailingStop(
        double entry_price,
        double highest_price,
        double current_price,
        const std::vector<analytics::Candle>& recent_candles
    );
    
    // 롤링 통계 조회
    RollingStatistics getRollingStatistics() const;
    
private:
        // 포지션 상태 업데이트
        void updateState(const std::string& market, double current_price) override;

    std::shared_ptr<network::UpbitHttpClient> client_;
    bool enabled_;
    Statistics stats_;
    mutable std::recursive_mutex mutex_;
    
    // [신규 추가] 중복 매수 방지용
    std::set<std::string> active_positions_;
    
    // 이력 데이터 (1000개)
    std::deque<double> recent_returns_;
    std::deque<double> recent_volatility_;
    std::deque<long long> trade_timestamps_;
    
    // 롤링 통계
    RollingStatistics rolling_stats_;
    
    // Regime Model
    RegimeModel regime_model_;
    
    // 마지막 신호 시간 (과매매 방지)
    long long last_signal_time_;
    
    // ===== 파라미터 =====
    
    static constexpr double BASE_TAKE_PROFIT = 0.05;       // 5%
    static constexpr double BASE_STOP_LOSS = 0.02;         // 2%
    static constexpr double MAX_HOLDING_TIME = 7200.0;     // 2시간
    static constexpr double FEE_RATE = 0.0005;             // 0.05%
    static constexpr double EXPECTED_SLIPPAGE = 0.0002;    // 0.02%
    static constexpr double CONFIDENCE_LEVEL = 0.95;       // 95%
    static constexpr double MIN_SHARPE_RATIO = 1.0;
    static constexpr double MAX_POSITION_SIZE = 0.10;      // 10%
    static constexpr double HALF_KELLY_FRACTION = 0.5;
    static constexpr double MIN_LIQUIDITY = 50.0;
    static constexpr int MIN_SIGNAL_INTERVAL_SEC = 300;    // 5분
    static constexpr double MIN_RISK_REWARD_RATIO = 2.5;
    static constexpr double MIN_EXPECTED_SHARPE = 1.5;
    
    // ===== 1. Market Regime (HMM) =====
    
    MarketRegime detectMarketRegime(
        const std::vector<analytics::Candle>& candles
    );
    
    void updateRegimeModel(
        const std::vector<analytics::Candle>& candles,
        RegimeModel& model
    );
    
    // ===== 2. Statistical Significance =====
    
    bool isVolumeSurgeSignificant(
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateZScore(
        double value,
        const std::vector<double>& history
    ) const;
    
    bool isTTestSignificant(
        const std::vector<double>& sample1,
        const std::vector<double>& sample2,
        double alpha = 0.05
    ) const;
    
    bool isKSTestPassed(
        const std::vector<double>& sample
    ) const;
    
    // ===== 3. Multi-Timeframe Analysis =====
    
    MultiTimeframeSignal analyzeMultiTimeframe(
        const std::vector<analytics::Candle>& candles_1m
    ) const;
    
    std::vector<analytics::Candle> resampleTo5m(
        const std::vector<analytics::Candle>& candles_1m
    ) const;
    
    std::vector<analytics::Candle> resampleTo15m(
        const std::vector<analytics::Candle>& candles_1m
    ) const;
    
    bool isBullishOnTimeframe(
        const std::vector<analytics::Candle>& candles,
        MultiTimeframeSignal::TimeframeMetrics& metrics
    ) const;
    
    // ===== 4. Advanced Order Flow =====
    
    AdvancedOrderFlowMetrics analyzeAdvancedOrderFlow(
        const std::string& market,
        double current_price
    ) const;
    
    double calculateVWAPDeviation(
        const std::vector<analytics::Candle>& candles,
        double current_price
    ) const;
    
    AdvancedOrderFlowMetrics::VolumeProfile calculateVolumeProfile(
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateCumulativeDelta(
        const nlohmann::json& orderbook
    ) const;
    
    // ===== 5. Position Sizing (Kelly + Volatility) =====
    
    PositionMetrics calculateAdvancedPositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateKellyFraction(
        double win_rate,
        double avg_win,
        double avg_loss
    ) const;
    
    double adjustForVolatility(
        double kelly_size,
        double volatility
    ) const;
    
    // ===== 6. Dynamic Stops =====
    
    DynamicStops calculateDynamicStops(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateATRBasedStop(
        double entry_price,
        const std::vector<analytics::Candle>& candles,
        double multiplier
    ) const;
    
    double findNearestSupport(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) const;
    
    double calculateChandelierExit(
        double highest_price,
        double atr,
        double multiplier = 3.0
    ) const;
    
    double calculateParabolicSAR(
        const std::vector<analytics::Candle>& candles,
        double acceleration = 0.02,
        double max_af = 0.20
    ) const;
    
    // ===== 7. Trade Cost Analysis =====
    
    bool isWorthTrading(
        double expected_return,
        double expected_sharpe
    ) const;
    
    double calculateExpectedSlippage(
        const analytics::CoinMetrics& metrics
    ) const;
    
    // ===== 8. Signal Strength =====
    
    double calculateSignalStrength(
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        const MultiTimeframeSignal& mtf_signal,
        const AdvancedOrderFlowMetrics& order_flow,
        MarketRegime regime
    ) const;
    
    // ===== 9. Risk Management =====
    
    double calculateCVaR(
        double position_size,
        double volatility,
        double confidence_level
    ) const;
    
    double calculateExpectedShortfall(
        const std::vector<double>& returns,
        double confidence_level
    ) const;
    
    // ===== 10. Performance Metrics =====
    
    double calculateSharpeRatio(
        double risk_free_rate = 0.03,
        int periods_per_year = 525600  // 분봉 기준
    ) const;
    
    double calculateSortinoRatio(
        double mar = 0.0,
        int periods_per_year = 525600
    ) const;
    
    double calculateCalmarRatio() const;
    
    double calculateMaxDrawdown() const;
    
    void updateRollingStatistics();
    
    // ===== 11. Walk-Forward Validation =====
    
    WalkForwardResult validateStrategy(
        const std::vector<analytics::Candle>& historical_data
    ) const;
    
    // ===== 12. Helpers =====
    
    double calculateMean(const std::vector<double>& values) const;
    double calculateStdDev(const std::vector<double>& values, double mean) const;
    double calculateVolatility(const std::vector<analytics::Candle>& candles) const;
    
    long long getCurrentTimestamp() const;
    engine::EngineConfig engine_config_;
    bool shouldGenerateSignal(
        double expected_return,
        double expected_sharpe
    ) const;
};

} // namespace strategy
} // namespace autolife
