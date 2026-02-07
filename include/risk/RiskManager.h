#pragma once

#include "common/Types.h"
#include "analytics/TechnicalIndicators.h"
#include "strategy/IStrategy.h"
#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <mutex>

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
    
    // 전략 정보
    std::string strategy_name;
    
    Position()
        : entry_price(0), current_price(0), quantity(0)
        , invested_amount(0), entry_time(0)
        , unrealized_pnl(0), unrealized_pnl_pct(0)
        , stop_loss(0), take_profit_1(0), take_profit_2(0)
        , half_closed(false)
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
    
    TradeHistory()
        : entry_price(0), exit_price(0), quantity(0)
        , profit_loss(0), profit_loss_pct(0), fee_paid(0)
        , entry_time(0), exit_time(0)
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
        const std::string& strategy_name
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
    
    // 현재 포지션 조회
    Position* getPosition(const std::string& market);
    std::vector<Position> getAllPositions() const;
    
    // ===== 손절 계산 =====
    
    // Dynamic Stop Loss (ATR + Support 조합)
    double calculateDynamicStopLoss(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    );
    
    // ATR 기반 손절
    double calculateATRStopLoss(
        double entry_price,
        const std::vector<analytics::Candle>& candles,
        double multiplier = 2.0
    );
    
    // Support 기반 손절
    double calculateSupportStopLoss(
        double entry_price,
        const std::vector<analytics::Candle>& candles
    );
    
    // Break-even Stop (본전 이동)
    void moveStopToBreakeven(const std::string& market);
    
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
        double fee_rate = 0.001  // 0.1%
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
            , unrealized_pnl(0), realized_pnl(0), total_pnl(0), total_pnl_pct(0)
            , total_trades(0), winning_trades(0), losing_trades(0), win_rate(0)
            , max_drawdown(0), current_drawdown(0)
            , sharpe_ratio(0), profit_factor(0)
            , active_positions(0), max_positions(5)
        {}
    };
    
    RiskMetrics getRiskMetrics() const;
    std::vector<TradeHistory> getTradeHistory() const;
    
    // 설정
    void setMaxPositions(int max_positions);
    void setMaxDailyTrades(int max_trades);
    void setMaxDrawdown(double max_drawdown_pct);
    void setMinReentryInterval(int seconds);
    
private:
    double initial_capital_;
    double current_capital_;
    
    std::map<std::string, Position> positions_;
    std::vector<TradeHistory> trade_history_;
    
    // 거래 제한
    std::map<std::string, long long> last_trade_time_;  // 마켓별 마지막 거래 시간
    int daily_trade_count_;
    long long daily_reset_time_;
    
    // 설정
    int max_positions_;
    int max_daily_trades_;
    double max_drawdown_pct_;
    int min_reentry_interval_;  // 초
    
    // 통계
    mutable double max_capital_;      // <- mutable 추가
    mutable double total_fees_paid_;  // <- mutable 추가
    
    mutable std::mutex mutex_;
    
    // 헬퍼 함수
    double calculateFee(double amount) const;
    void updateCapital();
    void recordTrade(const Position& pos, double exit_price, const std::string& exit_reason);
    long long getCurrentTimestamp() const;
    void resetDailyCountIfNeeded();
    void updateCapital(double amount_change);
};

} // namespace risk
} // namespace autolife
