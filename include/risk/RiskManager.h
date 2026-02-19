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

// ?¬ì????•ë³´
struct Position {
    std::string market;
    double entry_price;
    double current_price;
    double quantity;
    double invested_amount;
    long long entry_time;
    
    // ?ìµ
    double unrealized_pnl;      // ë¯¸ì‹¤???ìµ
    double unrealized_pnl_pct;  // ë¯¸ì‹¤???ìµë¥?
    
    // ?ì ˆ/?µì ˆê°€
    double stop_loss;
    double take_profit_1;       // 1ì°??µì ˆ (50%)
    double take_profit_2;       // 2ì°??µì ˆ (100%)
    bool half_closed;           // 1ì°??µì ˆ ?„ë£Œ ?¬ë?
    
    // [ì¶”ê?] Trailing Stop Loss??
    double highest_price;       // ?¬ì???ì§„ì… ??ìµœê³ ê°€ ê¸°ë¡ (?ì ˆ???ìŠ¹??
    double breakeven_trigger;   // ë³¸ì „ ?´ë™ ?¸ë¦¬ê±?ê°€ê²?
    double trailing_start;      // ?¸ë ˆ?¼ë§ ?œì‘ ê°€ê²?
    
    // ?„ëµ ?•ë³´
    std::string strategy_name;
    
    // [NEW] ML ?™ìŠµ??? í˜¸ ?•ë³´
    double signal_filter;       // entry-time adaptive filter value
    double signal_strength;     // entry signal strength (0.0~1.0)
    analytics::MarketRegime market_regime; // entry-time market regime
    std::string entry_archetype;
    double liquidity_score;     // ì§„ì… ??? ë™???ìˆ˜
    double volatility;          // ì§„ì… ??ë³€?™ì„±
    double expected_value;      // ì§„ì… ??ê¸°ë?ê°?
    double reward_risk_ratio;   // ì§„ì… ??RR
    
        // [NEW] ?œë”© ì£¼ë¬¸ ì¶”ì  (Limit Order ??Market ?´ë°± ?„í•´)
        std::string pending_order_uuid;     // ?œë”© ì¤‘ì¸ ì£¼ë¬¸ UUID
        long long pending_order_time;       // ?œë”© ì£¼ë¬¸ ?œê°„ (ms, epoch)
        std::string pending_order_type;     // "sell" or "partial_sell"
        double pending_order_price;         // ?œë”© ì¤‘ì¸ ì£¼ë¬¸ê°€ê²?
    
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
        , entry_archetype("UNSPECIFIED")
        , liquidity_score(0.0), volatility(0.0)
        , expected_value(0.0), reward_risk_ratio(0.0)
    {}
};

// ê±°ë˜ ?´ë ¥
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
    
    // [NEW] ML ?™ìŠµ???„í„° ?•ë³´
    double signal_filter;       // ê±°ë˜ ì§„ì… ???ìš©??? í˜¸ ?„í„°ê°?(0.45~0.55)
    double signal_strength;     // ê±°ë˜ ì§„ì… ? í˜¸??ê°•ë„ (0.0~1.0)
    analytics::MarketRegime market_regime; // ê±°ë˜ ì§„ì… ???œì¥ ?ˆì§
    std::string entry_archetype;
    double liquidity_score;     // ê±°ë˜ ì§„ì… ??? ë™???ìˆ˜
    double volatility;          // ê±°ë˜ ì§„ì… ??ë³€?™ì„±
    double expected_value;      // ê±°ë˜ ì§„ì… ??ê¸°ë?ê°?
    double reward_risk_ratio;   // ê±°ë˜ ì§„ì… ??RR
    
    TradeHistory()
        : entry_price(0), exit_price(0), quantity(0)
        , profit_loss(0), profit_loss_pct(0), fee_paid(0)
        , entry_time(0), exit_time(0)
        , signal_filter(0.5), signal_strength(0.0)
        , market_regime(analytics::MarketRegime::UNKNOWN)
        , entry_archetype("UNSPECIFIED")
        , liquidity_score(0.0), volatility(0.0)
        , expected_value(0.0), reward_risk_ratio(0.0)
    {}
};

// Risk Manager - ë¦¬ìŠ¤??ê´€ë¦?ë°??¬ì???ê´€ë¦?
class RiskManager {
public:
    RiskManager(double initial_capital);
    
    // ===== ?¬ì???ê´€ë¦?=====
    
    // ?¬ì???ì§„ì… ê°€???¬ë? ì²´í¬
    bool canEnterPosition(
        const std::string& market,
        double entry_price,
        double position_size_ratio,
        const std::string& strategy_name
    );
    
    // ?¬ì???ì§„ì…
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
    
    // ?¬ì????…ë°?´íŠ¸ (?„ì¬ê°€ ê°±ì‹ )
    void updatePosition(const std::string& market, double current_price);
    
    // ?¬ì???ì²?‚° ì²´í¬ (?ì ˆ/?µì ˆ ?¬ë? ?ë‹¨)
    bool shouldExitPosition(const std::string& market);
    
    // ?¬ì???ì²?‚°
    void exitPosition(
        const std::string& market,
        double exit_price,
        const std::string& exit_reason
    );
    
    // 1ì°??µì ˆ (50% ì²?‚°)
    void partialExit(const std::string& market, double exit_price);
    
    // [Fix] ?Œì•¡ ?¬ì??˜ì´??ë¶€ë¶??µì ˆ??ëª»í•œ ê²½ìš°, ?Œë˜ê·¸ë§Œ ê°•ì œë¡?ì¼œê¸° (?ë³¸ ë³€???†ìŒ)
    void setHalfClosed(const std::string& market, bool half_closed);
    
    // [Phase 3] ë¶€ë¶?ì²´ê²° ???˜ëŸ‰ë§??…ë°?´íŠ¸ (?¬ì???? ì?)
    void updatePositionQuantity(const std::string& market, double new_quantity);
    bool applyPartialSellFill(
        const std::string& market,
        double exit_price,
        double sell_quantity,
        const std::string& exit_reason
    );
    
    // ?„ì¬ ?¬ì???ì¡°íšŒ
    Position* getPosition(const std::string& market);
    std::vector<Position> getAllPositions() const;
    
    // ===== ?ì ˆ ê³„ì‚° =====
    
    // Dynamic Stop Loss (ATR + Support ì¡°í•©)
    double calculateDynamicStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    );
    
    // ATR ê¸°ë°˜ ?ì ˆ
    double calculateATRStopLoss(
        double entry_price,
        const std::vector<Candle>& candles,
        double multiplier = 2.0
    );
    
    // Support ê¸°ë°˜ ?ì ˆ
    double calculateSupportStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    );
    
    // Break-even Stop (ë³¸ì „ ?´ë™)
    void moveStopToBreakeven(const std::string& market);

    // Stop Loss ?í–¥ ê°±ì‹  (?¸ë ˆ?¼ë§??
    void updateStopLoss(const std::string& market, double new_stop_loss, const std::string& reason);

    // ?¸ë ˆ?¼ë§/ë¸Œë ˆ?´í¬?´ë¸ ?Œë¼ë¯¸í„° ?¤ì •
    void setPositionTrailingParams(
        const std::string& market,
        double breakeven_trigger,
        double trailing_start
    );
    
    // ===== ì£¼ë¬¸ ?€ê¸??ë³¸ ê´€ë¦?=====
    // ?œì¶œ?ì?ë§??„ì§ ì²´ê²° ????ì£¼ë¬¸ ê¸ˆì•¡??ì¶”ì ?˜ì—¬ ì¤‘ë³µ ì£¼ë¬¸ ë°©ì?
    void reservePendingCapital(double amount) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ += amount;
        LOG_INFO("?’° ?œë”© ?ë³¸ ?ˆì•½: +{:.0f} (ì´??œë”©: {:.0f})", amount, pending_order_capital_);
    }
    void releasePendingCapital(double amount) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ -= amount;
        if (pending_order_capital_ < 0) pending_order_capital_ = 0.0;
        LOG_INFO("?’° ?œë”© ?ë³¸ ?´ì œ: -{:.0f} (ì´??œë”©: {:.0f})", amount, pending_order_capital_);
    }
    void clearPendingCapital() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ = 0.0;
    }

    // ===== ?¬ì????¬ì´ì§?=====
    
    // Kelly Criterion ê¸°ë°˜ ?¬ì????¬ì´ì§?
    double calculateKellyPositionSize(
        double capital,
        double win_rate,
        double avg_win,
        double avg_loss
    );
    
    // Feeë¥?ê³ ë ¤??ìµœì  ?¬ì????¬ê¸°
    double calculateFeeAwarePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        double take_profit,
        double fee_rate = 0.0005  // [Phase 3] 0.05% (?…ë¹„??KRW ?¤ì œ ?˜ìˆ˜ë£Œì? ?¼ì¹˜)
    );
    
    // ===== ë¦¬ìŠ¤??ê´€ë¦?=====
    
    // ê±°ë˜ ë¹ˆë„ ?œí•œ ì²´í¬
    bool canTradeMarket(const std::string& market);
    
    // ?¼ì¼ ìµœë? ê±°ë˜ ?Ÿìˆ˜ ì²´í¬
    bool hasReachedDailyTradeLimit();
    
    // Drawdown ì²´í¬ (?°ì† ?ì‹¤ ??ê±°ë˜ ì¤‘ë‹¨)
    bool isDrawdownExceeded();
    
    // ìµœë? ?¬ì???ê°œìˆ˜ ì²´í¬
    bool hasReachedMaxPositions();
    
    // ===== ?µê³„ ë°?ëª¨ë‹ˆ?°ë§ =====
    
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
    
    // [NEW] ?¬ì??˜ì˜ ? í˜¸ ?•ë³´ ?¤ì • (ML ?™ìŠµ??
    void setPositionSignalInfo(
        const std::string& market,
        double signal_filter,
        double signal_strength,
        analytics::MarketRegime market_regime = analytics::MarketRegime::UNKNOWN,
        double liquidity_score = 0.0,
        double volatility = 0.0,
        double expected_value = 0.0,
        double reward_risk_ratio = 0.0,
        const std::string& entry_archetype = ""
    );

    // ===== ê·¸ë¦¬???ë³¸/ì²´ê²° ì²˜ë¦¬ =====
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
    
    // [??ì¶”ê?] ?¤ì „ ë§¤ë§¤ ?? ?¤ì œ ?”ê³ ë¡??ë³¸ê¸ˆì„ ??–´?°ê¸° ?„í•œ ?¨ìˆ˜
    void resetCapital(double actual_balance) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        current_capital_ = actual_balance; // ?„ì¬ ?ë³¸ê¸?êµì²´
        pending_order_capital_ = 0.0;       // ?™ê¸° ???œë”© ì´ˆê¸°??
        initial_capital_ = actual_balance; // ê¸°ì????ê¸ˆ)??êµì²´ (MDD ê³„ì‚°??
        max_capital_ = actual_balance;
        LOG_INFO("?ì‚° ?™ê¸°???„ë£Œ: RiskManager ?ë³¸ê¸??¬ì„¤??-> {:.0f} KRW", actual_balance);
    }

    // ?¤ì •
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
    double pending_order_capital_ = 0.0;    // ?œì¶œ?ì?ë§??„ì§ ì²´ê²° ????ì£¼ë¬¸ ê¸ˆì•¡
    
    std::map<std::string, Position> positions_;
    std::vector<TradeHistory> trade_history_;
    
    // ê±°ë˜ ?œí•œ
    std::map<std::string, long long> last_trade_time_;  // ë§ˆì¼“ë³?ë§ˆì?ë§?ê±°ë˜ ?œê°„
    int daily_trade_count_;
    long long daily_reset_time_;
    double daily_start_capital_;
    double daily_loss_limit_pct_;
    double daily_loss_limit_krw_;
    long long daily_start_date_;

    double min_order_krw_;
    double recommended_min_enter_krw_;
    
    // ?¤ì •
    int max_positions_;
    int max_daily_trades_;
    double max_drawdown_pct_;
    double max_exposure_pct_; // ì´??ë³¸ ?€ë¹??ˆìš© ?¬ì ë¹„ìœ¨ (?? 0.7 = 70%)
    int min_reentry_interval_;  // ì´?
    
    // ?µê³„
    mutable double max_capital_;      // <- mutable ì¶”ê?
    mutable double total_fees_paid_;  // <- mutable ì¶”ê?

    std::map<std::string, double> reserved_grid_capital_;
    std::map<std::string, GridInventory> grid_inventory_;
    
    mutable std::recursive_mutex mutex_;
    
    // ?¬í¼ ?¨ìˆ˜
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


