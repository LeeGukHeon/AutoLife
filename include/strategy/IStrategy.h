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

// 留ㅻℓ ?좏샇
enum class SignalType {
    NONE,           // ?좏샇 ?놁쓬
    STRONG_BUY,     // 媛뺣젰 留ㅼ닔
    BUY,            // 留ㅼ닔
    HOLD,           // 蹂댁쑀
    SELL,           // 留ㅻ룄
    STRONG_SELL     // 媛뺣젰 留ㅻ룄
};

// [??異붽?] 二쇰Ц ????뺤콉 (留ㅼ닔/留ㅻ룄 ??吏?뺢? vs ?쒖옣媛)
enum class OrderTypePolicy {
    LIMIT,                  // 吏?뺢? (?덉쟾, 誘몄껜寃??꾪뿕)
    MARKET,                 // ?쒖옣媛 (利됱떆 泥닿껐, SLIP ?꾪뿕)
    LIMIT_WITH_FALLBACK,    // 吏?뺢? ?쒕룄 ??誘몄껜寃????쒖옣媛濡?蹂寃?
    LIMIT_AGGRESSIVE        // ?좊━??履쎌쑝濡?吏?뺢? (留ㅼ닔: ??쾶, 留ㅻ룄: ?믨쾶)
};

// 二쇰Ц ?붿껌/寃곌낵 (?붿쭊??吏묓뻾)
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

// ?좏샇 媛뺣룄 (0.0 ~ 1.0)
struct Signal {
    SignalType type;
    std::string market;                 // 留덉폆 肄붾뱶
    std::string strategy_name;          // ?꾨왂 ?대쫫
    double strength;                    // ?좏샇 媛뺣룄 (0.0 ~ 1.0)
    double entry_price;                 // 吏꾩엯 媛寃?
    double entry_amount;                // [NEW] 吏꾩엯 ?덉젙 湲덉븸 (KRW) - 鍮꾩쑉???꾨땶 ?뺤젙 湲덉븸
    double stop_loss;                   // ?먯젅媛
    double take_profit_1;               // [??異붽?] 1李??듭젅媛 (50% 泥?궛)
    double take_profit_2;               // [??異붽?] 2李??듭젅媛 (100% 泥?궛)
    double breakeven_trigger;           // 蹂몄쟾 ?대룞 ?몃━嫄?媛寃?
    double trailing_start;              // ?몃젅?쇰쭅 ?쒖옉 媛寃?
    double position_size;               // ?ъ????ш린 (鍮꾩쑉, 李멸퀬??
    
    // [??異붽?] 二쇰Ц ?ㅽ뻾 ?뺤콉
    OrderTypePolicy buy_order_type;     // 留ㅼ닔 二쇰Ц ???
    OrderTypePolicy sell_order_type;    // 留ㅻ룄 二쇰Ц ???
    
    // [??異붽?] ?ъ떆???뺤콉
    int max_retries;                    // 理쒕? ?ъ떆???잛닔 (湲곕낯 3??
    int retry_wait_ms;                  // ?ъ떆???湲??쒓컙 (諛由ъ큹, 湲곕낯 1000ms)
    
    // [NEW] ML ?숈뒿?? ?좏샇 ?꾪꽣媛믨낵 媛뺣룄 ???
    double signal_filter;               // ?좏샇 ?앹꽦 ???곸슜???숈쟻 ?꾪꽣媛?(0.45~0.55)

    // [NEW] ?덉쭏/由ъ뒪??硫뷀?
    double expected_return_pct;         // 湲곕? ?섏씡瑜?(TP2 湲곗?)
    double expected_risk_pct;           // 湲곕? ?먯떎瑜?(SL 湲곗?)
    double expected_value;              // 湲곕?媛?(EV)
    double liquidity_score;             // ?좊룞???먯닔 (0~100)
    double volatility;                  // 蹂?숈꽦 (% ?⑥쐞)
    double strategy_win_rate;           // ?꾨왂 ?밸쪧
    double strategy_profit_factor;      // ?꾨왂 PF
    int strategy_trade_count;           // strategy trade count
    double score;                       // integrated entry score
    analytics::MarketRegime market_regime; // signal-time market regime
    std::string entry_archetype;        // normalized entry archetype label
    bool used_preloaded_tf_5m;          // scanner preloaded 5m candles used
    bool used_preloaded_tf_1h;          // scanner preloaded 1h candles used
    bool used_resampled_tf_fallback;    // fallback to in-strategy resampling path
    
    std::string reason;                 // ?좏샇 諛쒖깮 ?댁쑀
    long long timestamp;                // ?좏샇 諛쒖깮 ?쒓컙
    
    Signal()
        : type(SignalType::NONE)
        , strength(0.0)
        , entry_price(0.0)
        , stop_loss(0.0)
        , take_profit_1(0.0)            // ??珥덇린??
        , take_profit_2(0.0)            // ??珥덇린??
        , breakeven_trigger(0.0)
        , trailing_start(0.0)
        , position_size(0.0)
        , buy_order_type(OrderTypePolicy::LIMIT_WITH_FALLBACK)      // 湲곕낯媛?
        , sell_order_type(OrderTypePolicy::LIMIT_WITH_FALLBACK)     // 湲곕낯媛?
        , max_retries(3)
        , retry_wait_ms(1000)
        , signal_filter(0.5)            // NEW: 湲곕낯媛믪쑝濡?以묎컙 ?꾪꽣媛?
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
        , entry_archetype("UNSPECIFIED")
        , used_preloaded_tf_5m(false)
        , used_preloaded_tf_1h(false)
        , used_resampled_tf_fallback(false)
        , timestamp(0)
    {}
    
    // [??異붽?] take_profit_2 湲곕낯媛??ㅼ젙 ?ы띁 (?섏쐞 ?명솚?깆슜)
    double getTakeProfitForLegacy() const {
        return (take_profit_2 > 0) ? take_profit_2 : take_profit_1;
    }
};

// ?꾨왂 湲곕낯 ?뺣낫
struct StrategyInfo {
    std::string name;           // ?꾨왂 ?대쫫
    std::string description;    // ?ㅻ챸
    std::string timeframe;      // ?쒓컙 ?꾨젅??(1m, 5m, 15m, 1h, 4h, 1d)
    double min_capital;         // 理쒖냼 ?먮낯湲?
    double expected_winrate;    // ?덉긽 ?밸쪧
    double risk_level;          // 由ъ뒪???덈꺼 (1-10)
    
    StrategyInfo()
        : min_capital(0)
        , expected_winrate(0.5)
        , risk_level(5.0)
    {}
};

// ?꾨왂 ?명꽣?섏씠??(紐⑤뱺 ?꾨왂??援ы쁽?댁빞 ??
class IStrategy {
public:
    virtual ~IStrategy() = default;
    
    // ?꾨왂 ?뺣낫 諛섑솚
    virtual StrategyInfo getInfo() const = 0;
    


// ...

    // ?좏샇 ?앹꽦 (?듭떖 硫붿꽌??
    // 異붽? 留ㅺ컻蹂?? ?꾩옱 ?붿쭊??媛?⑹옄蹂??먰솕), ?쒖옣 援?㈃ 遺꾩꽍(Regime)
    virtual Signal generateSignal(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        double available_capital,
        const analytics::RegimeAnalysis& regime
    ) = 0;
    
    // 吏꾩엯 議곌굔 泥댄겕
    virtual bool shouldEnter(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<Candle>& candles,
        double current_price,
        const analytics::RegimeAnalysis& regime
    ) = 0;
    
    // 泥?궛 議곌굔 泥댄겕
    virtual bool shouldExit(
        const std::string& market,
        double entry_price,
        double current_price,
        double holding_time_seconds
    ) = 0;
    
    // ?먯젅 媛寃?怨꾩궛
    virtual double calculateStopLoss(
        double entry_price,
        const std::vector<Candle>& candles
    ) = 0;
    
    // ?듭젅 媛寃?怨꾩궛
    virtual double calculateTakeProfit(
        double entry_price,
        const std::vector<Candle>& candles
    ) = 0;
    
    // ?ъ????ъ씠利?怨꾩궛 (?먮낯 ?鍮?鍮꾩쑉)
    virtual double calculatePositionSize(
        double capital,
        double entry_price,
        double stop_loss,
        const analytics::CoinMetrics& metrics
    ) = 0;
    
    // ?꾨왂 ?쒖꽦??鍮꾪솢?깊솕
    virtual void setEnabled(bool enabled) = 0;
    virtual bool isEnabled() const = 0;
    
    // ?듦퀎 議고쉶
    struct Statistics {
        int total_signals;
        int winning_trades;
        int losing_trades;
        double total_profit;
        double total_loss;
        double win_rate;
        double avg_profit;
        double avg_loss;
        double profit_factor;       // 珥??섏씡 / 珥??먯떎
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


