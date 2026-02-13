#pragma once

#include "common/Types.h"
#include "analytics/TechnicalIndicators.h"
#include "strategy/IStrategy.h"
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <mutex>
#include "common/Logger.h"

namespace autolife {
namespace risk {

// 포지션 정보
struct Position {
    std::string market;
    double entry_price;
    double current_price;
    double quantity;
    double invested_amount;
    long long entry_time;
    
    // 손익
    double unrealized_pnl;      // 미실현 손익
    double unrealized_pnl_pct;  // 미실현 손익률
    
    // 손절/익절가
    double stop_loss;
    double take_profit_1;       // 1차 익절 (50%)
    double take_profit_2;       // 2차 익절 (100%)
    bool half_closed;           // 1차 익절 완료 여부
    
    // [추가] Trailing Stop Loss용
    double highest_price;       // 포지션 진입 후 최고가 기록 (손절선 상승용)
    double breakeven_trigger;   // 본전 이동 트리거 가격
    double trailing_start;      // 트레일링 시작 가격
    
    // 전략 정보
    std::string strategy_name;
    
    // [NEW] ML 학습용 신호 정보
    double signal_filter;       // 진입 시 적용된 동적 필터값
    double signal_strength;     // 진입 신호의 강도
    analytics::MarketRegime market_regime; // 진입 시 시장 레짐
    double liquidity_score;     // 진입 시 유동성 점수
    double volatility;          // 진입 시 변동성
    double expected_value;      // 진입 시 기대값
    double reward_risk_ratio;   // 진입 시 RR
    
        // [NEW] 펜딩 주문 추적 (Limit Order → Market 폴백 위해)
        std::string pending_order_uuid;     // 펜딩 중인 주문 UUID
        long long pending_order_time;       // 펜딩 주문 시간 (ms, epoch)
        std::string pending_order_type;     // "sell" or "partial_sell"
        double pending_order_price;         // 펜딩 중인 주문가격
    
    Position()
        : entry_price(0), current_price(0), quantity(0)
        , invested_amount(0), entry_time(0)
        , unrealized_pnl(0), unrealized_pnl_pct(0)
        , stop_loss(0), take_profit_1(0), take_profit_2(0)
        , half_closed(false), highest_price(0)
        , breakeven_trigger(0), trailing_start(0)
        , pending_order_time(0), pending_order_price(0)
        , signal_filter(0.5), signal_strength(0.0)
        , market_regime(analytics::MarketRegime::UNKNOWN)
        , liquidity_score(0.0), volatility(0.0)
        , expected_value(0.0), reward_risk_ratio(0.0)
    {}
};

// 거래 이력
struct TradeHistory {
    std::string market;
    double entry_price;
    double exit_price;
    double quantity;
    double profit_loss;
    double profit_loss_pct;
    double fee_paid;
    long long entry_time;
    long long exit_time;
    std::string strategy_name;
    std::string exit_reason;    // "take_profit", "stop_loss", "time_stop"
    
    // [NEW] ML 학습용 필터 정보
    double signal_filter;       // 거래 진입 시 적용된 신호 필터값 (0.45~0.55)
    double signal_strength;     // 거래 진입 신호의 강도 (0.0~1.0)
    analytics::MarketRegime market_regime; // 거래 진입 시 시장 레짐
    double liquidity_score;     // 거래 진입 시 유동성 점수
    double volatility;          // 거래 진입 시 변동성
    double expected_value;      // 거래 진입 시 기대값
    double reward_risk_ratio;   // 거래 진입 시 RR
    
    TradeHistory()
        : entry_price(0), exit_price(0), quantity(0)
        , profit_loss(0), profit_loss_pct(0), fee_paid(0)
        , entry_time(0), exit_time(0)
        , signal_filter(0.5), signal_strength(0.0)
        , market_regime(analytics::MarketRegime::UNKNOWN)
        , liquidity_score(0.0), volatility(0.0)
        , expected_value(0.0), reward_risk_ratio(0.0)
    {}
};

// Risk Manager - 리스크 관리 및 포지션 관리
class RiskManager {
public:
    RiskManager(double initial_capital);
    
    // ===== 포지션 관리 =====
    
    // 포지션 진입 가능 여부 체크
    bool canEnterPosition(
        const std::string& market,
        double entry_price,
        double position_size_ratio,
        const std::string& strategy_name
    );
    
    // 포지션 진입
    void enterPosition(
        const std::string& market,
        double entry_price,
        double quantity,
        double stop_loss,
        double take_profit_1,
        double take_profit_2,
        const std::string& strategy_name,
        double breakeven_trigger = 0.0,
        double trailing_start = 0.0
    );
    
    // 포지션 업데이트 (현재가 갱신)
    void updatePosition(const std::string& market, double current_price);
    
    // 포지션 청산 체크 (손절/익절 여부 판단)
    bool shouldExitPosition(const std::string& market);
    
    // 포지션 청산
    void exitPosition(
        const std::string& market,
        double exit_price,
        const std::string& exit_reason
    );
    
    // 1차 익절 (50% 청산)
    void partialExit(const std::string& market, double exit_price);
    
    // [Fix] 소액 포지션이라 부분 익절을 못한 경우, 플래그만 강제로 켜기 (자본 변동 없음)
    void setHalfClosed(const std::string& market, bool half_closed);
    
    // [Phase 3] 부분 체결 시 수량만 업데이트 (포지션 유지)
    void updatePositionQuantity(const std::string& market, double new_quantity);
    bool applyPartialSellFill(
        const std::string& market,
        double exit_price,
        double sell_quantity,
        const std::string& exit_reason
    );
    
    // 현재 포지션 조회
    Position* getPosition(const std::string& market);
    std::vector<Position> getAllPositions() const;
    
    // ===== 손절 계산 =====
    
    // Dynamic Stop Loss (ATR + Support 조합)
    double calculateDynamicStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    );
    
    // ATR 기반 손절
    double calculateATRStopLoss(
        double entry_price,
        const std::vector<Candle>& candles,
        double multiplier = 2.0
    );
    
    // Support 기반 손절
    double calculateSupportStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    );
    
    // Break-even Stop (본전 이동)
    void moveStopToBreakeven(const std::string& market);

    // Stop Loss 상향 갱신 (트레일링용)
    void updateStopLoss(const std::string& market, double new_stop_loss, const std::string& reason);

    // 트레일링/브레이크이븐 파라미터 설정
    void setPositionTrailingParams(
        const std::string& market,
        double breakeven_trigger,
        double trailing_start
    );
    
    // ===== 주문 대기 자본 관리 =====
    // 제출됐지만 아직 체결 안 된 주문 금액을 추적하여 중복 주문 방지
    void reservePendingCapital(double amount) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ += amount;
        LOG_INFO("💰 펜딩 자본 예약: +{:.0f} (총 펜딩: {:.0f})", amount, pending_order_capital_);
    }
    void releasePendingCapital(double amount) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ -= amount;
        if (pending_order_capital_ < 0) pending_order_capital_ = 0.0;
        LOG_INFO("💰 펜딩 자본 해제: -{:.0f} (총 펜딩: {:.0f})", amount, pending_order_capital_);
    }
    void clearPendingCapital() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ = 0.0;
    }

    // ===== 포지션 사이징 =====
    
    // Kelly Criterion 기반 포지션 사이징
    double calculateKellyPositionSize(
        double capital,
        double win_rate,
        double avg_win,
        double avg_loss
    );
    
    // Fee를 고려한 최적 포지션 크기
    double calculateFeeAwarePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        double take_profit,
        double fee_rate = 0.0005  // [Phase 3] 0.05% (업비트 KRW 실제 수수료와 일치)
    );
    
    // ===== 리스크 관리 =====
    
    // 거래 빈도 제한 체크
    bool canTradeMarket(const std::string& market);
    
    // 일일 최대 거래 횟수 체크
    bool hasReachedDailyTradeLimit();
    
    // Drawdown 체크 (연속 손실 시 거래 중단)
    bool isDrawdownExceeded();
    
    // 최대 포지션 개수 체크
    bool hasReachedMaxPositions();
    
    // ===== 통계 및 모니터링 =====
    
    struct RiskMetrics {
        double total_capital;
        double available_capital;
        double invested_capital;
        double reserved_capital;
        double unrealized_pnl;
        double realized_pnl;
        double total_pnl;
        double total_pnl_pct;
        
        int total_trades;
        int winning_trades;
        int losing_trades;
        double win_rate;
        
        double max_drawdown;
        double current_drawdown;
        
        double sharpe_ratio;
        double profit_factor;
        
        int active_positions;
        int max_positions;
        
        RiskMetrics()
            : total_capital(0), available_capital(0), invested_capital(0)
            , reserved_capital(0)
            , unrealized_pnl(0), realized_pnl(0), total_pnl(0), total_pnl_pct(0)
            , total_trades(0), winning_trades(0), losing_trades(0), win_rate(0)
            , max_drawdown(0), current_drawdown(0)
            , sharpe_ratio(0), profit_factor(0)
            , active_positions(0), max_positions(10)
        {}
    };
    
    RiskMetrics getRiskMetrics() const;
    std::vector<TradeHistory> getTradeHistory() const;
    void replaceTradeHistory(const std::vector<TradeHistory>& history);
    void appendTradeHistory(const TradeHistory& trade);
    
    // [NEW] 포지션의 신호 정보 설정 (ML 학습용)
    void setPositionSignalInfo(
        const std::string& market,
        double signal_filter,
        double signal_strength,
        analytics::MarketRegime market_regime = analytics::MarketRegime::UNKNOWN,
        double liquidity_score = 0.0,
        double volatility = 0.0,
        double expected_value = 0.0,
        double reward_risk_ratio = 0.0
    );

    // ===== 그리드 자본/체결 처리 =====
    bool reserveGridCapital(
        const std::string& market,
        double amount,
        const std::string& strategy_name
    );
    double getReservedGridCapital(const std::string& market) const;
    void releaseGridCapital(const std::string& market);
    bool applyGridFill(
        const std::string& market,
        strategy::OrderSide side,
        double price,
        double quantity
    );
    
    // [✅ 추가] 실전 매매 시, 실제 잔고로 자본금을 덮어쓰기 위한 함수
    void resetCapital(double actual_balance) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        current_capital_ = actual_balance; // 현재 자본금 교체
        pending_order_capital_ = 0.0;       // 동기 후 펜딩 초기화
        initial_capital_ = actual_balance; // 기준점(원금)도 교체 (MDD 계산용)
        max_capital_ = actual_balance;
        LOG_INFO("자산 동기화 완료: RiskManager 자본금 재설정 -> {:.0f} KRW", actual_balance);
    }

    // 설정
    void setMaxPositions(int max_positions);
    void setMaxDailyTrades(int max_trades);
    void setMaxDrawdown(double max_drawdown_pct);
    void setMaxExposurePct(double pct);
    void setMinReentryInterval(int seconds);
    void setMinOrderKrw(double min_order_krw);
    void setDailyLossLimitPct(double pct);
    void setDailyLossLimitKrw(double krw);
    bool isDailyLossLimitExceeded() const;
    double getDailyLossPct() const;
    
private:
    struct GridInventory {
        double quantity;
        double avg_price;
        long long last_buy_time;

        GridInventory()
            : quantity(0.0)
            , avg_price(0.0)
            , last_buy_time(0)
        {}
    };

    double initial_capital_;
    double current_capital_;
    double pending_order_capital_ = 0.0;    // 제출됐지만 아직 체결 안 된 주문 금액
    
    std::map<std::string, Position> positions_;
    std::vector<TradeHistory> trade_history_;
    
    // 거래 제한
    std::map<std::string, long long> last_trade_time_;  // 마켓별 마지막 거래 시간
    int daily_trade_count_;
    long long daily_reset_time_;
    double daily_start_capital_;
    double daily_loss_limit_pct_;
    double daily_loss_limit_krw_;
    long long daily_start_date_;

    double min_order_krw_;
    double recommended_min_enter_krw_;
    
    // 설정
    int max_positions_;
    int max_daily_trades_;
    double max_drawdown_pct_;
    double max_exposure_pct_; // 총 자본 대비 허용 투자 비율 (예: 0.7 = 70%)
    int min_reentry_interval_;  // 초
    
    // 통계
    mutable double max_capital_;      // <- mutable 추가
    mutable double total_fees_paid_;  // <- mutable 추가

    std::map<std::string, double> reserved_grid_capital_;
    std::map<std::string, GridInventory> grid_inventory_;
    
    mutable std::recursive_mutex mutex_;
    
    // 헬퍼 함수
    double calculateFee(double amount) const;
    void updateCapital();
    void recordTrade(const Position& pos, double exit_price, const std::string& exit_reason);
    long long getCurrentTimestamp() const;
    void resetDailyCountIfNeeded();
    void resetDailyLossIfNeeded();
    void updateCapital(double amount_change);
};

} // namespace risk
} // namespace autolife
