#pragma once

#include "common/Types.h"
#include "analytics/TechnicalIndicators.h"
#include "analytics/MarketScanner.h"
#include <string>
#include <vector>
#include <memory>

namespace autolife {
namespace strategy {

// 매매 신호
enum class SignalType {
    NONE,           // 신호 없음
    STRONG_BUY,     // 강력 매수
    BUY,            // 매수
    HOLD,           // 보유
    SELL,           // 매도
    STRONG_SELL     // 강력 매도
};

// 신호 강도 (0.0 ~ 1.0)
struct Signal {
    SignalType type;
    std::string market;         // ✅ 추가 - 마켓 코드
    std::string strategy_name;  // ✅ 추가 - 전략 이름
    double strength;            // 신호 강도 (0.0 ~ 1.0)
    double entry_price;         // 진입 가격
    double stop_loss;           // 손절가
    double take_profit;         // 익절가
    double position_size;       // 포지션 크기 (비율)
    std::string reason;         // 신호 발생 이유
    long long timestamp;        // 신호 발생 시간
    
    Signal()
        : type(SignalType::NONE)
        , strength(0.0)
        , entry_price(0.0)
        , stop_loss(0.0)
        , take_profit(0.0)
        , position_size(0.0)
        , timestamp(0)
    {}
};

// 전략 기본 정보
struct StrategyInfo {
    std::string name;           // 전략 이름
    std::string description;    // 설명
    std::string timeframe;      // 시간 프레임 (1m, 5m, 15m, 1h, 4h, 1d)
    double min_capital;         // 최소 자본금
    double expected_winrate;    // 예상 승률
    double risk_level;          // 리스크 레벨 (1-10)
    
    StrategyInfo()
        : min_capital(0)
        , expected_winrate(0.5)
        , risk_level(5.0)
    {}
};

// 전략 인터페이스 (모든 전략이 구현해야 함)
class IStrategy {
public:
    virtual ~IStrategy() = default;
    
    // 전략 정보 반환
    virtual StrategyInfo getInfo() const = 0;
    
    // 신호 생성 (핵심 메서드)
    virtual Signal generateSignal(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        double current_price
    ) = 0;
    
    // 진입 조건 체크
    virtual bool shouldEnter(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        double current_price
    ) = 0;
    
    // 청산 조건 체크
    virtual bool shouldExit(
        const std::string& market,
        double entry_price,
        double current_price,
        double holding_time_seconds
    ) = 0;
    
    // 손절 가격 계산
    virtual double calculateStopLoss(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) = 0;
    
    // 익절 가격 계산
    virtual double calculateTakeProfit(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    ) = 0;
    
    // 포지션 사이즈 계산 (자본 대비 비율)
    virtual double calculatePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics
    ) = 0;
    
    // 전략 활성화/비활성화
    virtual void setEnabled(bool enabled) = 0;
    virtual bool isEnabled() const = 0;
    
    // 통계 조회
    struct Statistics {
        int total_signals;
        int winning_trades;
        int losing_trades;
        double total_profit;
        double total_loss;
        double win_rate;
        double avg_profit;
        double avg_loss;
        double profit_factor;       // 총 수익 / 총 손실
        double sharpe_ratio;
        
        Statistics()
            : total_signals(0), winning_trades(0), losing_trades(0)
            , total_profit(0), total_loss(0), win_rate(0)
            , avg_profit(0), avg_loss(0), profit_factor(0), sharpe_ratio(0)
        {}
    };
    
    virtual Statistics getStatistics() const = 0;
    virtual void updateStatistics(const std::string& market, bool is_win, double profit_loss) = 0;
};

} // namespace strategy
} // namespace autolife
