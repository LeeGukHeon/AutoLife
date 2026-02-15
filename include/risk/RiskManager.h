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

// ?ъ????뺣낫
struct Position {
    std::string market;
    double entry_price;
    double current_price;
    double quantity;
    double invested_amount;
    long long entry_time;
    
    // ?먯씡
    double unrealized_pnl;      // 誘몄떎???먯씡
    double unrealized_pnl_pct;  // 誘몄떎???먯씡瑜?
    
    // ?먯젅/?듭젅媛
    double stop_loss;
    double take_profit_1;       // 1李??듭젅 (50%)
    double take_profit_2;       // 2李??듭젅 (100%)
    bool half_closed;           // 1李??듭젅 ?꾨즺 ?щ?
    
    // [異붽?] Trailing Stop Loss??
    double highest_price;       // ?ъ???吏꾩엯 ??理쒓퀬媛 湲곕줉 (?먯젅???곸듅??
    double breakeven_trigger;   // 蹂몄쟾 ?대룞 ?몃━嫄?媛寃?
    double trailing_start;      // ?몃젅?쇰쭅 ?쒖옉 媛寃?
    
    // ?꾨왂 ?뺣낫
    std::string strategy_name;
    
    // [NEW] ML ?숈뒿???좏샇 ?뺣낫
    double signal_filter;       // entry-time adaptive filter value
    double signal_strength;     // entry signal strength (0.0~1.0)
    analytics::MarketRegime market_regime; // entry-time market regime
    std::string entry_archetype;
    double liquidity_score;     // 吏꾩엯 ???좊룞???먯닔
    double volatility;          // 吏꾩엯 ??蹂?숈꽦
    double expected_value;      // 吏꾩엯 ??湲곕?媛?
    double reward_risk_ratio;   // 吏꾩엯 ??RR
    
        // [NEW] ?쒕뵫 二쇰Ц 異붿쟻 (Limit Order ??Market ?대갚 ?꾪빐)
        std::string pending_order_uuid;     // ?쒕뵫 以묒씤 二쇰Ц UUID
        long long pending_order_time;       // ?쒕뵫 二쇰Ц ?쒓컙 (ms, epoch)
        std::string pending_order_type;     // "sell" or "partial_sell"
        double pending_order_price;         // ?쒕뵫 以묒씤 二쇰Ц媛寃?
    
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

// 嫄곕옒 ?대젰
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
    
    // [NEW] ML ?숈뒿???꾪꽣 ?뺣낫
    double signal_filter;       // 嫄곕옒 吏꾩엯 ???곸슜???좏샇 ?꾪꽣媛?(0.45~0.55)
    double signal_strength;     // 嫄곕옒 吏꾩엯 ?좏샇??媛뺣룄 (0.0~1.0)
    analytics::MarketRegime market_regime; // 嫄곕옒 吏꾩엯 ???쒖옣 ?덉쭚
    std::string entry_archetype;
    double liquidity_score;     // 嫄곕옒 吏꾩엯 ???좊룞???먯닔
    double volatility;          // 嫄곕옒 吏꾩엯 ??蹂?숈꽦
    double expected_value;      // 嫄곕옒 吏꾩엯 ??湲곕?媛?
    double reward_risk_ratio;   // 嫄곕옒 吏꾩엯 ??RR
    
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

// Risk Manager - 由ъ뒪??愿由?諛??ъ???愿由?
class RiskManager {
public:
    RiskManager(double initial_capital);
    
    // ===== ?ъ???愿由?=====
    
    // ?ъ???吏꾩엯 媛???щ? 泥댄겕
    bool canEnterPosition(
        const std::string& market,
        double entry_price,
        double position_size_ratio,
        const std::string& strategy_name
    );
    
    // ?ъ???吏꾩엯
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
    
    // ?ъ????낅뜲?댄듃 (?꾩옱媛 媛깆떊)
    void updatePosition(const std::string& market, double current_price);
    
    // ?ъ???泥?궛 泥댄겕 (?먯젅/?듭젅 ?щ? ?먮떒)
    bool shouldExitPosition(const std::string& market);
    
    // ?ъ???泥?궛
    void exitPosition(
        const std::string& market,
        double exit_price,
        const std::string& exit_reason
    );
    
    // 1李??듭젅 (50% 泥?궛)
    void partialExit(const std::string& market, double exit_price);
    
    // [Fix] ?뚯븸 ?ъ??섏씠??遺遺??듭젅??紐삵븳 寃쎌슦, ?뚮옒洹몃쭔 媛뺤젣濡?耳쒓린 (?먮낯 蹂???놁쓬)
    void setHalfClosed(const std::string& market, bool half_closed);
    
    // [Phase 3] 遺遺?泥닿껐 ???섎웾留??낅뜲?댄듃 (?ъ????좎?)
    void updatePositionQuantity(const std::string& market, double new_quantity);
    bool applyPartialSellFill(
        const std::string& market,
        double exit_price,
        double sell_quantity,
        const std::string& exit_reason
    );
    
    // ?꾩옱 ?ъ???議고쉶
    Position* getPosition(const std::string& market);
    std::vector<Position> getAllPositions() const;
    
    // ===== ?먯젅 怨꾩궛 =====
    
    // Dynamic Stop Loss (ATR + Support 議고빀)
    double calculateDynamicStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    );
    
    // ATR 湲곕컲 ?먯젅
    double calculateATRStopLoss(
        double entry_price,
        const std::vector<Candle>& candles,
        double multiplier = 2.0
    );
    
    // Support 湲곕컲 ?먯젅
    double calculateSupportStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    );
    
    // Break-even Stop (蹂몄쟾 ?대룞)
    void moveStopToBreakeven(const std::string& market);

    // Stop Loss ?곹뼢 媛깆떊 (?몃젅?쇰쭅??
    void updateStopLoss(const std::string& market, double new_stop_loss, const std::string& reason);

    // ?몃젅?쇰쭅/釉뚮젅?댄겕?대툙 ?뚮씪誘명꽣 ?ㅼ젙
    void setPositionTrailingParams(
        const std::string& market,
        double breakeven_trigger,
        double trailing_start
    );
    
    // ===== 二쇰Ц ?湲??먮낯 愿由?=====
    // ?쒖텧?먯?留??꾩쭅 泥닿껐 ????二쇰Ц 湲덉븸??異붿쟻?섏뿬 以묐났 二쇰Ц 諛⑹?
    void reservePendingCapital(double amount) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ += amount;
        LOG_INFO("?뮥 ?쒕뵫 ?먮낯 ?덉빟: +{:.0f} (珥??쒕뵫: {:.0f})", amount, pending_order_capital_);
    }
    void releasePendingCapital(double amount) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ -= amount;
        if (pending_order_capital_ < 0) pending_order_capital_ = 0.0;
        LOG_INFO("?뮥 ?쒕뵫 ?먮낯 ?댁젣: -{:.0f} (珥??쒕뵫: {:.0f})", amount, pending_order_capital_);
    }
    void clearPendingCapital() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        pending_order_capital_ = 0.0;
    }

    // ===== ?ъ????ъ씠吏?=====
    
    // Kelly Criterion 湲곕컲 ?ъ????ъ씠吏?
    double calculateKellyPositionSize(
        double capital,
        double win_rate,
        double avg_win,
        double avg_loss
    );
    
    // Fee瑜?怨좊젮??理쒖쟻 ?ъ????ш린
    double calculateFeeAwarePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        double take_profit,
        double fee_rate = 0.0005  // [Phase 3] 0.05% (?낅퉬??KRW ?ㅼ젣 ?섏닔猷뚯? ?쇱튂)
    );
    
    // ===== 由ъ뒪??愿由?=====
    
    // 嫄곕옒 鍮덈룄 ?쒗븳 泥댄겕
    bool canTradeMarket(const std::string& market);
    
    // ?쇱씪 理쒕? 嫄곕옒 ?잛닔 泥댄겕
    bool hasReachedDailyTradeLimit();
    
    // Drawdown 泥댄겕 (?곗냽 ?먯떎 ??嫄곕옒 以묐떒)
    bool isDrawdownExceeded();
    
    // 理쒕? ?ъ???媛쒖닔 泥댄겕
    bool hasReachedMaxPositions();
    
    // ===== ?듦퀎 諛?紐⑤땲?곕쭅 =====
    
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
    
    // [NEW] ?ъ??섏쓽 ?좏샇 ?뺣낫 ?ㅼ젙 (ML ?숈뒿??
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

    // ===== 洹몃━???먮낯/泥닿껐 泥섎━ =====
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
    
    // [??異붽?] ?ㅼ쟾 留ㅻℓ ?? ?ㅼ젣 ?붽퀬濡??먮낯湲덉쓣 ??뼱?곌린 ?꾪븳 ?⑥닔
    void resetCapital(double actual_balance) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        current_capital_ = actual_balance; // ?꾩옱 ?먮낯湲?援먯껜
        pending_order_capital_ = 0.0;       // ?숆린 ???쒕뵫 珥덇린??
        initial_capital_ = actual_balance; // 湲곗????먭툑)??援먯껜 (MDD 怨꾩궛??
        max_capital_ = actual_balance;
        LOG_INFO("?먯궛 ?숆린???꾨즺: RiskManager ?먮낯湲??ъ꽕??-> {:.0f} KRW", actual_balance);
    }

    // ?ㅼ젙
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
    double pending_order_capital_ = 0.0;    // ?쒖텧?먯?留??꾩쭅 泥닿껐 ????二쇰Ц 湲덉븸
    
    std::map<std::string, Position> positions_;
    std::vector<TradeHistory> trade_history_;
    
    // 嫄곕옒 ?쒗븳
    std::map<std::string, long long> last_trade_time_;  // 留덉폆蹂?留덉?留?嫄곕옒 ?쒓컙
    int daily_trade_count_;
    long long daily_reset_time_;
    double daily_start_capital_;
    double daily_loss_limit_pct_;
    double daily_loss_limit_krw_;
    long long daily_start_date_;

    double min_order_krw_;
    double recommended_min_enter_krw_;
    
    // ?ㅼ젙
    int max_positions_;
    int max_daily_trades_;
    double max_drawdown_pct_;
    double max_exposure_pct_; // 珥??먮낯 ?鍮??덉슜 ?ъ옄 鍮꾩쑉 (?? 0.7 = 70%)
    int min_reentry_interval_;  // 珥?
    
    // ?듦퀎
    mutable double max_capital_;      // <- mutable 異붽?
    mutable double total_fees_paid_;  // <- mutable 異붽?

    std::map<std::string, double> reserved_grid_capital_;
    std::map<std::string, GridInventory> grid_inventory_;
    
    mutable std::recursive_mutex mutex_;
    
    // ?ы띁 ?⑥닔
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

