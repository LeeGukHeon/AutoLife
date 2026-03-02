#pragma once

#include "common/Types.h"
#include "network/UpbitHttpClient.h"
#include "analytics/TechnicalIndicators.h"  // ???대? ?덈뒗吏 ?뺤씤, ?놁쑝硫?異붽?
#include "analytics/OrderbookAnalyzer.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>

namespace autolife {
namespace analytics {

// 肄붿씤蹂??섍툒 硫뷀듃由?
struct CoinMetrics {
    struct LiquidityVolumeGatePolicy {
        bool enabled = false;
        std::string mode = "static";
        int window_minutes = 60;
        double quantile_q = 0.20;
        int min_samples_required = 30;
        std::string low_conf_action = "hold";
    };
    struct FoundationStructureGatePolicy {
        bool enabled = false;
        std::string mode = "static";
        double relax_delta = 0.0;
    };
    struct BearReboundGuardPolicy {
        bool enabled = false;
        std::string mode = "static";
        int window_minutes = 60;
        double quantile_q = 0.20;
        int min_samples_required = 30;
        std::string low_conf_action = "hold";
        double static_threshold = 1.0;
    };
    struct StopLossRiskPolicy {
        bool enabled = false;
        double stop_loss_trending_multiplier = 1.0;
    };
    std::string market;                  // 留덉폆 肄붾뱶
    double current_price;                 // ?꾩옱媛
    double volume_24h;                    // 24?쒓컙 嫄곕옒??
    double volume_surge_ratio;            // 嫄곕옒??湲됱쬆 諛곗쑉 (1.0 = ?됯퇏)
    double price_change_rate;             // 媛寃?蹂?숇쪧
    double price_momentum;                // 媛寃?紐⑤찘? (RSI 湲곕컲)
    double order_book_imbalance;         // ?멸? 遺덇퇏??(-1 ~ +1)
    double buy_pressure;                  // 留ㅼ닔 ?뺣젰
    double sell_pressure;                 // 留ㅻ룄 ?뺣젰
    int buy_wall_count;                   // 留ㅼ닔踰?媛쒖닔
    int sell_wall_count;                  // 留ㅻ룄踰?媛쒖닔
    double volatility;                    // 蹂?숈꽦
    double liquidity_score;               // ?좊룞???먯닔
    double model_margin_score;            // probabilistic margin (p_h5_calibrated - threshold_h5)
    double model_prob_h5_calibrated;      // cached probability for diagnostics
    double model_selection_threshold_h5;  // cached threshold for diagnostics
    bool model_margin_valid;              // true when model_margin_score is available
    OrderbookSnapshot orderbook_snapshot; // 二쇰Ц???ㅻ깄??    nlohmann::json orderbook_units;       // 二쇰Ц???좊떅 罹먯떆

    // ??罹붾뱾 ?곗씠??異붽?
    std::vector<Candle> candles;
    std::map<std::string, std::vector<Candle>> candles_by_tf;
    LiquidityVolumeGatePolicy liq_vol_gate_policy;
    FoundationStructureGatePolicy foundation_structure_gate_policy;
    BearReboundGuardPolicy bear_rebound_guard_policy;
    StopLossRiskPolicy stop_loss_risk_policy;
    
    CoinMetrics() : current_price(0), volume_24h(0), volume_surge_ratio(1.0),
                    price_change_rate(0), price_momentum(0), 
                    order_book_imbalance(0), buy_pressure(0), sell_pressure(0),
                    buy_wall_count(0), sell_wall_count(0),
                    volatility(0), liquidity_score(0),
                    model_margin_score(0.0), model_prob_h5_calibrated(0.5),
                    model_selection_threshold_h5(0.5), model_margin_valid(false) {}
};

// 留ㅼ닔/留ㅻ룄踰??뺣낫
struct Wall {
    double price;
    double size;
    double strength;  // ?됯퇏 ?鍮?媛뺣룄
    
    Wall(double p, double s, double st) : price(p), size(s), strength(st) {}
};

// Market Scanner - ??醫낅ぉ ?ㅼ틪 諛?遺꾩꽍
class MarketScanner {
public:
    MarketScanner(std::shared_ptr<network::UpbitHttpClient> client);
    
    // ?꾩껜 留덉폆 ?ㅼ틪 (KRW 留덉폆留?
    std::vector<CoinMetrics> scanAllMarkets();
    
    // ?곸쐞 N媛?醫낅ぉ 異붿텧
    std::vector<std::string> getTopMarkets(int count = 10);
    
    // ?뱀젙 留덉폆 ?곸꽭 遺꾩꽍
    CoinMetrics analyzeMarket(const std::string& market);
    
    // Volume Surge 媛먯? (嫄곕옒??湲됱쬆)
    double detectVolumeSurge(const std::string& market);
    
    // Order Book Imbalance 怨꾩궛
    double calculateOrderBookImbalance(const std::string& market);
    
    // 留ㅼ닔/留ㅻ룄踰?媛먯?
    std::vector<Wall> detectBuyWalls(const std::string& market);
    std::vector<Wall> detectSellWalls(const std::string& market);
    
    // Fake Wall ?먯? (?덉닔 二쇰Ц)
    bool isFakeWall(const Wall& wall, const std::string& market);
    
    // 蹂?숈꽦 怨꾩궛 (ATR 湲곕컲)
    double calculateVolatility(const std::string& market);
    
    // ?좊룞???먯닔 怨꾩궛
    double calculateLiquidityScore(const std::string& market);
    
    // 媛寃?紐⑤찘? 怨꾩궛
    double calculatePriceMomentum(const std::string& market);
    
private:
    struct CandleCacheEntry {
        std::vector<Candle> candles;
        std::chrono::steady_clock::time_point last_update_time{};
        std::chrono::steady_clock::time_point last_full_sync_time{};
    };

    std::shared_ptr<network::UpbitHttpClient> client_;
    std::vector<CoinMetrics> cached_metrics_;
    std::chrono::steady_clock::time_point last_scan_time_;
    std::chrono::steady_clock::time_point last_candle_api_call_time_{};
    std::map<std::string, CandleCacheEntry> candle_cache_;
    
    // ?ы띁 ?⑥닔??
    std::vector<std::string> getAllKRWMarkets();
    double getAverageVolume(const std::string& market, int hours);
    std::vector<Candle> getRecentCandles(const std::string& market, int count);
    std::vector<Candle> getRecentCandles(const std::string& market, const std::string& unit, int count);
    std::vector<Candle> getRecentDayCandles(const std::string& market, int count);
    std::vector<Candle> getCandlesWithRollingCache(
        const std::string& market,
        const std::string& unit,
        int count,
        bool day_candle);
    static long long getCandleFrameMs(const std::string& unit, bool day_candle);
    static std::string getCandleCacheKey(const std::string& market, const std::string& unit, bool day_candle);
    static std::vector<Candle> keepRecentCandles(const std::vector<Candle>& candles, int count);
    static void mergeCandles(std::vector<Candle>& base, const std::vector<Candle>& incoming, int max_count);
    void throttleCandleApiCall();
        // ?대? 議고쉶???곗씠?곕줈 遺꾩꽍 (API ?몄텧 ?놁쓬)
    double analyzeOrderBookImbalance(const nlohmann::json& orderbook);
    std::pair<int, int> analyzeWalls(const nlohmann::json& orderbook);  // {buy_walls, sell_walls}
    double analyzeVolumeSurge(const std::vector<Candle>& candles);
    double analyzeVolatility(const std::vector<Candle>& candles);
    double analyzeMomentum(const std::vector<Candle>& candles);
};

} // namespace analytics
} // namespace autolife
