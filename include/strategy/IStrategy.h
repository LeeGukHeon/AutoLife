#pragma once

#include "common/Types.h"
#include "analytics/TechnicalIndicators.h"
#include "analytics/MarketScanner.h"
#include "analytics/RegimeDetector.h"
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

// [✅ 추가] 주문 타입 정책 (매수/매도 시 지정가 vs 시장가)
enum class OrderTypePolicy {
    LIMIT,                  // 지정가 (안전, 미체결 위험)
    MARKET,                 // 시장가 (즉시 체결, SLIP 위험)
    LIMIT_WITH_FALLBACK,    // 지정가 시도 후 미체결 시 시장가로 변경
    LIMIT_AGGRESSIVE        // 유리한 쪽으로 지정가 (매수: 낮게, 매도: 높게)
};

// 주문 요청/결과 (엔진이 집행)
enum class OrderSide {
    BUY,
    SELL
};

struct OrderRequest {
    std::string market;
    OrderSide side;
    double price;
    double quantity;
    int level_id;
    std::string reason;

    OrderRequest()
        : side(OrderSide::BUY)
        , price(0.0)
        , quantity(0.0)
        , level_id(-1)
    {}
};

struct OrderResult {
    std::string market;
    OrderSide side;
    bool success;
    double executed_price;
    double executed_volume;
    int level_id;
    std::string reason;

    OrderResult()
        : side(OrderSide::BUY)
        , success(false)
        , executed_price(0.0)
        , executed_volume(0.0)
        , level_id(-1)
    {}
};

// 신호 강도 (0.0 ~ 1.0)
struct Signal {
    SignalType type;
    std::string market;                 // 마켓 코드
    std::string strategy_name;          // 전략 이름
    double strength;                    // 신호 강도 (0.0 ~ 1.0)
    double entry_price;                 // 진입 가격
    double entry_amount;                // [NEW] 진입 예정 금액 (KRW) - 비율이 아닌 확정 금액
    double stop_loss;                   // 손절가
    double take_profit_1;               // [✅ 추가] 1차 익절가 (50% 청산)
    double take_profit_2;               // [✅ 추가] 2차 익절가 (100% 청산)
    double breakeven_trigger;           // 본전 이동 트리거 가격
    double trailing_start;              // 트레일링 시작 가격
    double position_size;               // 포지션 크기 (비율, 참고용)
    
    // [✅ 추가] 주문 실행 정책
    OrderTypePolicy buy_order_type;     // 매수 주문 타입
    OrderTypePolicy sell_order_type;    // 매도 주문 타입
    
    // [✅ 추가] 재시도 정책
    int max_retries;                    // 최대 재시도 횟수 (기본 3회)
    int retry_wait_ms;                  // 재시도 대기 시간 (밀리초, 기본 1000ms)
    
    // [NEW] ML 학습용: 신호 필터값과 강도 저장
    double signal_filter;               // 신호 생성 시 적용된 동적 필터값 (0.45~0.55)

    // [NEW] 품질/리스크 메타
    double expected_return_pct;         // 기대 수익률 (TP2 기준)
    double expected_risk_pct;           // 기대 손실률 (SL 기준)
    double expected_value;              // 기대값 (EV)
    double liquidity_score;             // 유동성 점수 (0~100)
    double volatility;                  // 변동성 (% 단위)
    double strategy_win_rate;           // 전략 승률
    double strategy_profit_factor;      // 전략 PF
    int strategy_trade_count;           // 전략 거래 수
    double score;                       // 통합 점수
    analytics::MarketRegime market_regime; // signal-time market regime
    
    std::string reason;                 // 신호 발생 이유
    long long timestamp;                // 신호 발생 시간
    
    Signal()
        : type(SignalType::NONE)
        , strength(0.0)
        , entry_price(0.0)
        , stop_loss(0.0)
        , take_profit_1(0.0)            // ✅ 초기화
        , take_profit_2(0.0)            // ✅ 초기화
        , breakeven_trigger(0.0)
        , trailing_start(0.0)
        , position_size(0.0)
        , buy_order_type(OrderTypePolicy::LIMIT_WITH_FALLBACK)      // 기본값
        , sell_order_type(OrderTypePolicy::LIMIT_WITH_FALLBACK)     // 기본값
        , max_retries(3)
        , retry_wait_ms(1000)
        , signal_filter(0.5)            // NEW: 기본값으로 중간 필터값
        , expected_return_pct(0.0)
        , expected_risk_pct(0.0)
        , expected_value(0.0)
        , liquidity_score(0.0)
        , volatility(0.0)
        , strategy_win_rate(0.0)
        , strategy_profit_factor(0.0)
        , strategy_trade_count(0)
        , score(0.0)
        , market_regime(analytics::MarketRegime::UNKNOWN)
        , timestamp(0)
    {}
    
    // [✅ 추가] take_profit_2 기본값 설정 헬퍼 (하위 호환성용)
    double getTakeProfitForLegacy() const {
        return (take_profit_2 > 0) ? take_profit_2 : take_profit_1;
    }
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
    


// ...

    // 신호 생성 (핵심 메서드)
    // 추가 매개변수: 현재 엔진의 가용자본(원화), 시장 국면 분석(Regime)
    virtual Signal generateSignal(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        double available_capital,
        const analytics::RegimeAnalysis& regime
    ) = 0;
    
    // 진입 조건 체크
    virtual bool shouldEnter(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        const analytics::RegimeAnalysis& regime
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
        const std::vector<Candle>& candles
    ) = 0;
    
    // 익절 가격 계산
    virtual double calculateTakeProfit(
        double entry_price,
        const std::vector<Candle>& candles
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
    virtual void setStatistics(const Statistics& stats) { (void)stats; }
    virtual void updateState(const std::string&, double) {}
    virtual bool onSignalAccepted(const Signal&, double) { return false; }
    virtual std::vector<OrderRequest> drainOrderRequests() { return {}; }
    virtual void onOrderResult(const OrderResult&) {}
    virtual std::vector<std::string> drainReleasedMarkets() { return {}; }
    virtual std::vector<std::string> getActiveMarkets() const { return {}; }
};

} // namespace strategy
} // namespace autolife
