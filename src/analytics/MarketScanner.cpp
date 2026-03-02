#include "analytics/MarketScanner.h"
#include "analytics/OrderbookAnalyzer.h"
#include "common/MarketDataWindowPolicy.h"
#include "common/Logger.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <thread>

#undef max
#undef min

namespace {
constexpr int kScanMarketLimit = 25;
constexpr int kTf5mMarketLimit = kScanMarketLimit;
constexpr int kTf15mMarketLimit = kScanMarketLimit;
constexpr int kTf1hMarketLimit = kScanMarketLimit;
constexpr int kTf4hMarketLimit = kScanMarketLimit;
constexpr int kTf1dMarketLimit = kScanMarketLimit;
constexpr int kIncrementalFetchCount = 3;
constexpr auto kMinCandleApiInterval = std::chrono::milliseconds(120);
constexpr auto kMinFullSyncInterval = std::chrono::milliseconds(30LL * 60LL * 1000LL);
constexpr auto kMaxFullSyncInterval = std::chrono::milliseconds(72LL * 60LL * 60LL * 1000LL);
constexpr double kLiquidityScoreReferenceNotionalKrw = 100000000000.0; // 1000??KRW
}

namespace autolife {
namespace analytics {

MarketScanner::MarketScanner(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
    , last_scan_time_(std::chrono::steady_clock::now())
    , last_candle_api_call_time_(std::chrono::steady_clock::now())
{
    LOG_INFO("MarketScanner ì´ˆê¸°??);
}

std::vector<CoinMetrics> MarketScanner::scanAllMarkets() {
    LOG_INFO("?„ì²´ ë§ˆì¼“ ?¤ìº” ?œì‘ (ìµœì ??ê²½ë¡œ)...");
    
    auto start_time = std::chrono::steady_clock::now();
    auto all_markets = getAllKRWMarkets();
    std::vector<CoinMetrics> all_metrics;
    
    const int BATCH_SIZE = 100;
    
    // 1?¨ê³„: ë°°ì¹˜ë¡??„ì²´ Ticker ì¡°íšŒ
    LOG_INFO("1?¨ê³„: {} ì¢…ëª© Ticker ë°°ì¹˜ ì¡°íšŒ...", all_markets.size());
    std::map<std::string, nlohmann::json> ticker_map;
    
    for (size_t i = 0; i < all_markets.size(); i += BATCH_SIZE) {
        size_t end = std::min(i + BATCH_SIZE, all_markets.size());
        std::vector<std::string> batch(all_markets.begin() + i, all_markets.begin() + end);
        
        try {
            auto tickers = client_->getTickerBatch(batch);
            for (const auto& ticker : tickers) {
                std::string market = ticker["market"].get<std::string>();
                // [ì¶”ê?] ?¤í…Œ?´ë¸” ì½”ì¸ ?„í„°ë§?(USDT, USDC ??
                // 'KRW-USDT'??'KRW-USDC'ë¥??¬ê¸°???ì²œ ì°¨ë‹¨?©ë‹ˆ??
                if (market == "KRW-USDT" || market == "KRW-USDC") {
                    continue; 
                }
                ticker_map[market] = ticker;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Ticker ë°°ì¹˜ ì¡°íšŒ ?¤íŒ¨: {}", e.what());
        }
    }
    LOG_INFO("Ticker ì¡°íšŒ ?„ë£Œ: {} ì¢…ëª©", ticker_map.size());
    
    // 2?¨ê³„: ê±°ë˜?€ê¸?ê¸°ì? 1ì°??„í„°ë§?
    LOG_INFO("2?¨ê³„: ê±°ë˜?€ê¸????•ë ¬...");
    std::vector<std::pair<std::string, double>> volume_ranks;
    
    for (const auto& [market, ticker] : ticker_map) {
        // [?˜ì •] 24h ?„ì ???„ë‹ˆ???¹ì¼ ?„ì (acc_trade_price) ?¬ìš©
        // ë§Œì•½ ?„ë“œê°€ ?†ë‹¤ë©?24hë¡?fallback ì²˜ë¦¬
        double volume = 0.0;
        volume = ticker["acc_trade_price_24h"].get<double>();
        volume_ranks.emplace_back(market, volume);
    }
    
    std::sort(volume_ranks.begin(), volume_ranks.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // 2?¨ê³„ ?•ë ¬ ì§í›„ ë¡œê·¸ ì¶”ê?
    LOG_INFO("TOP 5 ê±°ë˜?€ê¸?ì¢…ëª©:");
    for(int i=0; i<std::min(5, (int)volume_ranks.size()); ++i) {
        LOG_INFO("  #{} {} : {:.0f}??, i+1, volume_ranks[i].first, volume_ranks[i].second / 100000000.0);
    }

    // 3?¨ê³„: ?ìœ„ ?œí•œ ì¢…ëª©ë§??ì„¸ ?˜ì§‘ (?¤ê±°??ì§€???¸ì¶œ???ˆê°)
    int detail_count = std::min(kScanMarketLimit, (int)volume_ranks.size());
    LOG_INFO("3?¨ê³„: ?ìœ„ {} ì¢…ëª© ?°ì´???˜ì§‘...", detail_count);
    
    std::vector<std::string> top_markets;
    for (int i = 0; i < detail_count; ++i) {
        top_markets.push_back(volume_ranks[i].first);
    }
    
    // OrderBook ë°°ì¹˜ ì¡°íšŒ (50ê°œë? 10ê°œì”© ?˜ëˆ ??
    std::map<std::string, nlohmann::json> orderbook_map;
    const int OB_BATCH = 10;  // OrderBook?€ 10ê°œì”© (ì´ˆë‹¹ 10???œí•œ)
    
    for (size_t i = 0; i < top_markets.size(); i += OB_BATCH) {
        size_t end = std::min(i + OB_BATCH, top_markets.size());
        std::vector<std::string> batch(top_markets.begin() + i, top_markets.begin() + end);
        
        try {
            auto orderbooks = client_->getOrderBookBatch(batch);
            for (const auto& ob : orderbooks) {
                std::string market = ob["market"].get<std::string>();
                orderbook_map[market] = ob;
            }
            
            // Rate Limit ì¤€?˜ë? ?„í•´ 0.1ì´??€ê¸?
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
        } catch (const std::exception& e) {
            LOG_ERROR("OrderBook ë°°ì¹˜ ì¡°íšŒ ?¤íŒ¨: {}", e.what());
        }
    }
    LOG_INFO("OrderBook ?˜ì§‘ ?„ë£Œ: {} ì¢…ëª©", orderbook_map.size());
    
    // Candles ì¡°íšŒ (?œê³„??ìºì‹œ ?¬ìš©)
    std::map<std::string, std::vector<Candle>> candles_map_1m;
    std::map<std::string, std::vector<Candle>> candles_map_5m;
    std::map<std::string, std::vector<Candle>> candles_map_15m;
    std::map<std::string, std::vector<Candle>> candles_map_1h;
    std::map<std::string, std::vector<Candle>> candles_map_4h;
    std::map<std::string, std::vector<Candle>> candles_map_1d;

    const int CANDLES_1M = static_cast<int>(common::targetBarsForTimeframe("1m", 200));
    const int CANDLES_5M = static_cast<int>(common::targetBarsForTimeframe("5m", 120));
    const int CANDLES_15M = static_cast<int>(common::targetBarsForTimeframe("15m", 120));
    const int CANDLES_1H = static_cast<int>(common::targetBarsForTimeframe("1h", 120));
    const int CANDLES_4H = static_cast<int>(common::targetBarsForTimeframe("4h", 90));
    const int CANDLES_1D = static_cast<int>(common::targetBarsForTimeframe("1d", 60));

    for (const auto& market : top_markets) {
        auto candles = getCandlesWithRollingCache(market, "1", CANDLES_1M, false);
        if (!candles.empty()) {
            candles_map_1m[market] = std::move(candles);
        }
    }
    LOG_INFO("Candles(1m) ?˜ì§‘ ?„ë£Œ: {} ì¢…ëª©", candles_map_1m.size());

    // ì¶”ê? ?€?„í”„?ˆì„?€ ?ìœ„ ?¼ë? ì¢…ëª©ë§??˜ì§‘ (API ?œí•œ ê³ ë ¤)
    int tf5m_limit = std::min(kTf5mMarketLimit, (int)top_markets.size());
    int tf15m_limit = std::min(kTf15mMarketLimit, (int)top_markets.size());
    int tf1h_limit = std::min(kTf1hMarketLimit, (int)top_markets.size());
    int tf4h_limit = std::min(kTf4hMarketLimit, (int)top_markets.size());
    int tf1d_limit = std::min(kTf1dMarketLimit, (int)top_markets.size());

    for (int i = 0; i < tf5m_limit; ++i) {
        const auto& market = top_markets[i];
        auto candles = getCandlesWithRollingCache(market, "5", CANDLES_5M, false);
        if (!candles.empty()) {
            candles_map_5m[market] = std::move(candles);
        }
    }

    for (int i = 0; i < tf15m_limit; ++i) {
        const auto& market = top_markets[i];
        auto candles = getCandlesWithRollingCache(market, "15", CANDLES_15M, false);
        if (!candles.empty()) {
            candles_map_15m[market] = std::move(candles);
        }
    }

    for (int i = 0; i < tf1h_limit; ++i) {
        const auto& market = top_markets[i];
        auto candles = getCandlesWithRollingCache(market, "60", CANDLES_1H, false);
        if (!candles.empty()) {
            candles_map_1h[market] = std::move(candles);
        }
    }

    for (int i = 0; i < tf4h_limit; ++i) {
        const auto& market = top_markets[i];
        auto candles = getCandlesWithRollingCache(market, "240", CANDLES_4H, false);
        if (!candles.empty()) {
            candles_map_4h[market] = std::move(candles);
        }
    }

    for (int i = 0; i < tf1d_limit; ++i) {
        const auto& market = top_markets[i];
        auto candles = getCandlesWithRollingCache(market, "1", CANDLES_1D, true);
        if (!candles.empty()) {
            candles_map_1d[market] = std::move(candles);
        }
    }

// 4?¨ê³„: ?˜ì§‘???°ì´?°ë¡œ ë¶„ì„ (API ?¸ì¶œ ?†ìŒ!)
LOG_INFO("4?¨ê³„: ì¢…ëª© ë¶„ì„ ì¤?..");

// 4?¨ê³„ ?˜ì •ë³?
for (size_t i = 0; i < top_markets.size(); ++i) {
    const auto& market = top_markets[i];
    try {
        CoinMetrics metrics;
        metrics.market = market;

 // ===== Ticker ?°ì´??ë¶„ì„ =====
if (ticker_map.find(market) != ticker_map.end()) {
    const auto& ticker = ticker_map[market];
    
    metrics.current_price = ticker["trade_price"].get<double>();
    
    // [?˜ì •] volume(ê°œìˆ˜)???„ë‹ˆ??price(ê±°ë˜?€ê¸?ë¥?ê°€?¸ì???? ë™??ë¶„ì„???•í™•??
    metrics.volume_24h = ticker["acc_trade_price_24h"].get<double>(); 
    
    // [?˜ì •] 0.0015 -> 0.15 (%) ?¨ìœ„ë¡?ë³€??
    metrics.price_change_rate = ticker["signed_change_rate"].get<double>() * 100.0;
    
    // [?? ê³ ê?/?€ê°€ ?€ë¹??„ì¬ ?„ì¹˜ ë¶„ì„???„í•´ ì¶”ê??˜ë©´ ì¢‹ì? ?°ì´??
    // metrics.high_24h = ticker["high_price"].get<double>();
    // metrics.low_24h = ticker["low_price"].get<double>();
}

// ===== OrderBook ?°ì´??ë¶„ì„ =====
    if (orderbook_map.find(market) != orderbook_map.end()) {
        const auto& ob = orderbook_map[market];
        
        // [ì£¼ì˜] ob ?„ì²´ê°€ ?„ë‹ˆ??orderbook_units ë°°ì—´???˜ê²¨?????•ë¥ ???’ìŒ
        if (ob.contains("orderbook_units")) {
            const auto& units = ob["orderbook_units"];

            metrics.orderbook_snapshot = OrderbookAnalyzer::analyze(units, 1000000.0);
            metrics.order_book_imbalance = metrics.orderbook_snapshot.imbalance;
            metrics.orderbook_units = units;
            
            auto [buy_walls, sell_walls] = analyzeWalls(units);
            metrics.buy_wall_count = buy_walls;
            metrics.sell_wall_count = sell_walls;
        }
    }

        if (candles_map_1m.find(market) != candles_map_1m.end()) {
            // [ì¤‘ìš”] ?•ë ¬??êµ¬ì¡°ì²??°ì´???ì„±
            metrics.candles = candles_map_1m[market];
            metrics.candles_by_tf["1m"] = metrics.candles;

            if (!metrics.candles.empty()) {
                metrics.volume_surge_ratio = analyzeVolumeSurge(metrics.candles); 
                metrics.volatility = analyzeVolatility(metrics.candles);
                metrics.price_momentum = analyzeMomentum(metrics.candles);
            }
        }

        if (candles_map_5m.find(market) != candles_map_5m.end()) {
            metrics.candles_by_tf["5m"] = candles_map_5m[market];
        }

        if (candles_map_15m.find(market) != candles_map_15m.end()) {
            metrics.candles_by_tf["15m"] = candles_map_15m[market];
        }

        if (candles_map_1h.find(market) != candles_map_1h.end()) {
            metrics.candles_by_tf["1h"] = candles_map_1h[market];
        }

        if (candles_map_4h.find(market) != candles_map_4h.end()) {
            metrics.candles_by_tf["4h"] = candles_map_4h[market];
        }

        if (candles_map_1d.find(market) != candles_map_1d.end()) {
            metrics.candles_by_tf["1d"] = candles_map_1d[market];
        }

        common::trimCandlesByPolicy(metrics.candles_by_tf);
        auto tf_1m = metrics.candles_by_tf.find("1m");
        if (tf_1m != metrics.candles_by_tf.end()) {
            metrics.candles = tf_1m->second;
        }

        // [?˜ì •] ? ë™???ìˆ˜ ê¸°ì? ?„ì‹¤??(?? 1,000??ê¸°ì? 100??
        double trade_price_24h = ticker_map[market]["acc_trade_price_24h"].get<double>();
        metrics.liquidity_score = std::min(
            100.0,
            (trade_price_24h / kLiquidityScoreReferenceNotionalKrw) * 100.0
        );
        metrics.volume_24h = trade_price_24h;
        all_metrics.push_back(metrics);
        
    } catch (const std::exception& e) {
        LOG_ERROR("ë§ˆì¼“ ë¶„ì„ ?¤íŒ¨: {} - {}", market, e.what());
    }
}

    
    cached_metrics_ = all_metrics;
    last_scan_time_ = std::chrono::steady_clock::now();
    
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time
    ).count();
    
    LOG_INFO("ë§ˆì¼“ ?¤ìº” ?„ë£Œ: {} ì¢…ëª© ë¶„ì„, {}ì´??Œìš”", all_metrics.size(), elapsed);
    
    return all_metrics;
}

std::vector<std::string> MarketScanner::getTopMarkets(int count) {
    if (cached_metrics_.empty()) {
        scanAllMarkets();
    }
    
    std::vector<std::string> top_markets;
    int limit = std::min(count, static_cast<int>(cached_metrics_.size()));
    
    for (int i = 0; i < limit; ++i) {
        top_markets.push_back(cached_metrics_[i].market);
    }
    
    return top_markets;
}

CoinMetrics MarketScanner::analyzeMarket(const std::string& market) {
    CoinMetrics metrics;
    metrics.market = market;
    
    try {
        // 1. ?„ì¬ ?œì„¸ ì¡°íšŒ
        auto ticker = client_->getTicker(market);
        if (!ticker.empty()) {
            metrics.current_price = ticker[0]["trade_price"].get<double>();
            // [?˜ì •] ê°œìˆ˜ê°€ ?„ë‹Œ ê±°ë˜?€ê¸?Price)??ê°€?¸ì? ?•í•©??? ì?
            metrics.volume_24h = ticker[0]["acc_trade_price_24h"].get<double>();
            // [?˜ì •] ?¨ìœ„ë¥?%ë¡?ë³€??(0.0015 -> 0.15)
            metrics.price_change_rate = ticker[0]["signed_change_rate"].get<double>() * 100.0;
        }
        
        // 2. Volume Surge ê°ì?
        metrics.volume_surge_ratio = detectVolumeSurge(market);
        
        // 3. Order Book ë¶„ì„
        auto orderbook = client_->getOrderBook(market);
        if (!orderbook.empty() && orderbook[0].contains("orderbook_units")) {
            const auto& units = orderbook[0]["orderbook_units"];
            metrics.orderbook_snapshot = OrderbookAnalyzer::analyze(units, 1000000.0);
            metrics.order_book_imbalance = metrics.orderbook_snapshot.imbalance;
            metrics.orderbook_units = units;

            auto [buy_walls, sell_walls] = analyzeWalls(units);
            metrics.buy_wall_count = buy_walls;
            metrics.sell_wall_count = sell_walls;
        }
        
        // 5. ë³€?™ì„± ê³„ì‚°
        metrics.volatility = calculateVolatility(market);
        
        // 6. ? ë™???ìˆ˜
        metrics.liquidity_score = calculateLiquidityScore(market);
        
        // 7. ê°€ê²?ëª¨ë©˜?€
        metrics.price_momentum = calculatePriceMomentum(market);

        // 7-1. ë©€???€?„í”„?ˆì„ ìº”ë“¤ ìºì‹œ
        metrics.candles = getRecentCandles(market, "1", 120);
        if (!metrics.candles.empty()) {
            metrics.candles_by_tf["1m"] = metrics.candles;
        }

        auto candles_5m = getRecentCandles(market, "5", 120);
        if (!candles_5m.empty()) {
            metrics.candles_by_tf["5m"] = candles_5m;
        }

        auto candles_15m = getRecentCandles(market, "15", 120);
        if (!candles_15m.empty()) {
            metrics.candles_by_tf["15m"] = candles_15m;
        }

        auto candles_1h = getRecentCandles(market, "60", 120);
        if (!candles_1h.empty()) {
            metrics.candles_by_tf["1h"] = candles_1h;
        }

        auto candles_4h = getRecentCandles(market, "240", 90);
        if (!candles_4h.empty()) {
            metrics.candles_by_tf["4h"] = candles_4h;
        }

        auto candles_1d = getRecentDayCandles(market, 60);
        if (!candles_1d.empty()) {
            metrics.candles_by_tf["1d"] = candles_1d;
        }
        
        // 8. ì¢…í•© ?ìˆ˜ ê³„ì‚°
        
    } catch (const std::exception& e) {
        LOG_ERROR("ë§ˆì¼“ ë¶„ì„ ?¤ë¥˜ {}: {}", market, e.what());
    }
    
    return metrics;
}

double MarketScanner::detectVolumeSurge(const std::string& market) {
    try {
        // Backtest?€ ?™ì¼???˜ë???ratio ?¤ì???1.0=?‰ê· )??? ì??œë‹¤.
        // 1m ìµœê·¼ ìº”ë“¤??notional(close*volume) ê¸°ì??¼ë¡œ ê¸‰ì¦ ë°°ìœ¨??ê³„ì‚°?œë‹¤.
        auto recent_1m = getRecentCandles(market, "1", 20);
        if (!recent_1m.empty()) {
            return analyzeVolumeSurge(recent_1m);
        }
        return 1.0;
    } catch (const std::exception&) {
        return 1.0;
    }
}

double MarketScanner::calculateOrderBookImbalance(const std::string& market) {
    try {
        auto orderbook = client_->getOrderBook(market);
        if (orderbook.empty()) return 0.0;
        
        auto units = orderbook[0]["orderbook_units"];
        
        double total_bid_volume = 0.0;
        double total_ask_volume = 0.0;
        
        // ?ìœ„ 15?¸ê?ê¹Œì? ë¶„ì„
        int depth = std::min(15, static_cast<int>(units.size()));
        
        for (int i = 0; i < depth; ++i) {
            double bid_price = units[i]["bid_price"].get<double>();
            double bid_size = units[i]["bid_size"].get<double>();
            double ask_price = units[i]["ask_price"].get<double>();
            double ask_size = units[i]["ask_size"].get<double>();
            
            // ê¸ˆì•¡ ê¸°ì??¼ë¡œ ê³„ì‚°
            total_bid_volume += bid_price * bid_size;
            total_ask_volume += ask_price * ask_size;
        }
        
        // OBI ê³„ì‚°: -1 (ê°•ë ¥ ë§¤ë„) ~ +1 (ê°•ë ¥ ë§¤ìˆ˜)
        if (total_bid_volume + total_ask_volume < 0.0001) return 0.0;
        
        double obi = (total_bid_volume - total_ask_volume) / 
                     (total_bid_volume + total_ask_volume);
        
        return obi;
        
    } catch (const std::exception&) {
        return 0.0;
    }
}

std::vector<Wall> MarketScanner::detectBuyWalls(const std::string& market) {
    std::vector<Wall> walls;
    
    try {
        auto orderbook = client_->getOrderBook(market);
        if (orderbook.empty()) return walls;
        
        auto units = orderbook[0]["orderbook_units"];
        
        // ?‰ê·  ë§¤ìˆ˜ ?¸ê? ?¬ê¸° ê³„ì‚°
        double total_bid_size = 0.0;
        int count = std::min(15, static_cast<int>(units.size()));
        
        for (int i = 0; i < count; ++i) {
            total_bid_size += units[i]["bid_size"].get<double>();
        }
        
        double avg_bid_size = total_bid_size / count;
        
        // ?‰ê· ??5ë°??´ìƒ?´ë©´ Buy Wallë¡??ë‹¨
        double wall_threshold = avg_bid_size * 5.0;
        
        for (int i = 0; i < count; ++i) {
            double bid_price = units[i]["bid_price"].get<double>();
            double bid_size = units[i]["bid_size"].get<double>();
            
            if (bid_size > wall_threshold) {
                double strength = bid_size / avg_bid_size;
                walls.emplace_back(bid_price, bid_size, strength);
            }
        }
        
    } catch (const std::exception&) {
        // ?ëŸ¬ ë¬´ì‹œ
    }
    
    return walls;
}

std::vector<Wall> MarketScanner::detectSellWalls(const std::string& market) {
    std::vector<Wall> walls;
    
    try {
        auto orderbook = client_->getOrderBook(market);
        if (orderbook.empty()) return walls;
        
        auto units = orderbook[0]["orderbook_units"];
        
        double total_ask_size = 0.0;
        int count = std::min(15, static_cast<int>(units.size()));
        
        for (int i = 0; i < count; ++i) {
            total_ask_size += units[i]["ask_size"].get<double>();
        }
        
        double avg_ask_size = total_ask_size / count;
        double wall_threshold = avg_ask_size * 5.0;
        
        for (int i = 0; i < count; ++i) {
            double ask_price = units[i]["ask_price"].get<double>();
            double ask_size = units[i]["ask_size"].get<double>();
            
            if (ask_size > wall_threshold) {
                double strength = ask_size / avg_ask_size;
                walls.emplace_back(ask_price, ask_size, strength);
            }
        }
        
    } catch (const std::exception&) {
        // ?ëŸ¬ ë¬´ì‹œ
    }
    
    return walls;
}

double MarketScanner::calculateVolatility(const std::string& market) {
    try {
        auto candles = getRecentCandles(market, 20);
        if (candles.size() < 10) return 0.0;
        
        std::vector<double> high_low_diff;
        
        for (const auto& candle : candles) {
            high_low_diff.push_back(candle.high - candle.low);
        }
        
        // ATR (Average True Range) ê³„ì‚°
        double atr = std::accumulate(high_low_diff.begin(), high_low_diff.end(), 0.0) 
                     / high_low_diff.size();
        
        // ?„ì¬ê°€ ?€ë¹?ë³€?™ì„± ë¹„ìœ¨
        double current_price = candles.back().close;
        if (current_price < 0.0001) return 0.0;
        
        return (atr / current_price) * 100.0;
        
    } catch (const std::exception&) {
        return 0.0;
    }
}

double MarketScanner::calculateLiquidityScore(const std::string& market) {
    try {
        auto ticker = client_->getTicker(market);
        if (ticker.empty()) return 0.0;
        
        double volume_24h = ticker[0]["acc_trade_price_24h"].get<double>(); // ê±°ë˜?€ê¸?
        
        double score = std::min(
            100.0,
            (volume_24h / kLiquidityScoreReferenceNotionalKrw) * 100.0
        );
        
        return score;
        
    } catch (const std::exception&) {
        return 0.0;
    }
}

double MarketScanner::calculatePriceMomentum(const std::string& market) {
    try {
        auto candles = getRecentCandles(market, 14);
        if (candles.size() < 14) return 50.0; // ì¤‘ë¦½
        
        // RSI ê³„ì‚°
        std::vector<double> gains, losses;
        
        for (size_t i = 1; i < candles.size(); ++i) {
            double prev_close = candles[i - 1].close;
            double curr_close = candles[i].close;
            double change = curr_close - prev_close;
            
            if (change > 0) {
                gains.push_back(change);
                losses.push_back(0);
            } else {
                gains.push_back(0);
                losses.push_back(std::abs(change));
            }
        }
        
        double avg_gain = std::accumulate(gains.begin(), gains.end(), 0.0) / gains.size();
        double avg_loss = std::accumulate(losses.begin(), losses.end(), 0.0) / losses.size();
        
        if (avg_loss < 0.0001) return 100.0;
        
        double rs = avg_gain / avg_loss;
        double rsi = 100.0 - (100.0 / (1.0 + rs));
        
        return rsi;
        
    } catch (const std::exception&) {
        return 50.0;
    }
}

// Private ?¬í¼ ?¨ìˆ˜??

std::vector<std::string> MarketScanner::getAllKRWMarkets() {
    std::vector<std::string> krw_markets;
    
    try {
        auto markets = client_->getMarkets();
        
        for (const auto& market : markets) {
            std::string market_code = market["market"].get<std::string>();
            if (market_code.find("KRW-") == 0) {
                krw_markets.push_back(market_code);
            }
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("ë§ˆì¼“ ëª©ë¡ ì¡°íšŒ ?¤íŒ¨: {}", e.what());
    }
    
    return krw_markets;
}

double MarketScanner::getAverageVolume(const std::string& market, int hours) {
    try {
        int candle_count = hours;
        auto candles = getRecentCandles(market, candle_count);
        
        if (candles.empty()) return 0.0;
        
        double total_volume = 0.0;
        for (const auto& candle : candles) {
            total_volume += candle.volume;
        }
        
        return total_volume / candles.size();
        
    } catch (const std::exception&) {
        return 0.0;
    }
}

std::vector<Candle> MarketScanner::getRecentCandles(const std::string& market, int count) {
    return getRecentCandles(market, "60", count);
}

std::vector<Candle> MarketScanner::getRecentCandles(
    const std::string& market,
    const std::string& unit,
    int count) {
    return getCandlesWithRollingCache(market, unit, count, false);
}

std::vector<Candle> MarketScanner::getRecentDayCandles(
    const std::string& market,
    int count) {
    return getCandlesWithRollingCache(market, "1", count, true);
}

std::vector<Candle> MarketScanner::getCandlesWithRollingCache(
    const std::string& market,
    const std::string& unit,
    int count,
    bool day_candle) {
    if (count <= 0) {
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    const auto frame_ms = std::chrono::milliseconds(getCandleFrameMs(unit, day_candle));
    const auto update_interval = std::max(
        std::chrono::milliseconds(5000),
        frame_ms - std::chrono::milliseconds(day_candle ? 600000 : 5000));
    const auto full_sync_interval = std::clamp(frame_ms * 12, kMinFullSyncInterval, kMaxFullSyncInterval);
    const auto key = getCandleCacheKey(market, unit, day_candle);
    auto& entry = candle_cache_[key];

    const bool cache_empty = entry.candles.empty();
    const bool need_more_history = static_cast<int>(entry.candles.size()) < count;
    const bool update_due = cache_empty || need_more_history || (now - entry.last_update_time) >= update_interval;
    const bool full_sync_due =
        cache_empty ||
        need_more_history ||
        entry.last_full_sync_time.time_since_epoch().count() == 0 ||
        (now - entry.last_full_sync_time) >= full_sync_interval;

    if (update_due) {
        try {
            throttleCandleApiCall();
            nlohmann::json json;

            if (full_sync_due) {
                json = day_candle
                    ? client_->getCandlesDays(market, count)
                    : client_->getCandles(market, unit, count);

                entry.candles = keepRecentCandles(TechnicalIndicators::jsonToCandles(json), count);
                entry.last_full_sync_time = now;
            } else {
                const int incremental_count = std::min(count, kIncrementalFetchCount);
                json = day_candle
                    ? client_->getCandlesDays(market, incremental_count)
                    : client_->getCandles(market, unit, incremental_count);

                auto incoming = TechnicalIndicators::jsonToCandles(json);
                mergeCandles(entry.candles, incoming, count);
            }

            entry.last_update_time = now;
        } catch (const std::exception& e) {
            LOG_WARN("Candle cache update failed: {} {} {}", market, (day_candle ? "1d" : unit), e.what());
        }
    }

    return keepRecentCandles(entry.candles, count);
}

long long MarketScanner::getCandleFrameMs(const std::string& unit, bool day_candle) {
    if (day_candle) {
        return 24LL * 60LL * 60LL * 1000LL;
    }

    int minutes = 1;
    try {
        minutes = std::max(1, std::stoi(unit));
    } catch (...) {
        minutes = 1;
    }
    return static_cast<long long>(minutes) * 60LL * 1000LL;
}

std::string MarketScanner::getCandleCacheKey(
    const std::string& market,
    const std::string& unit,
    bool day_candle) {
    return market + "|" + (day_candle ? std::string("1d") : unit);
}

std::vector<Candle> MarketScanner::keepRecentCandles(const std::vector<Candle>& candles, int count) {
    if (count <= 0 || candles.empty()) {
        return {};
    }
    if (static_cast<int>(candles.size()) <= count) {
        return candles;
    }
    return std::vector<Candle>(candles.end() - count, candles.end());
}

void MarketScanner::mergeCandles(std::vector<Candle>& base, const std::vector<Candle>& incoming, int max_count) {
    if (incoming.empty()) {
        return;
    }
    if (base.empty()) {
        base = keepRecentCandles(incoming, max_count);
        return;
    }

    bool need_resort = false;
    for (const auto& candle : incoming) {
        if (candle.timestamp == 0) {
            base.push_back(candle);
            need_resort = true;
            continue;
        }

        auto it = std::lower_bound(
            base.begin(),
            base.end(),
            candle.timestamp,
            [](const Candle& existing, long long ts) {
                return existing.timestamp < ts;
            });

        if (it != base.end() && it->timestamp == candle.timestamp) {
            *it = candle;
        } else {
            base.insert(it, candle);
        }
    }

    if (need_resort) {
        std::stable_sort(base.begin(), base.end(), [](const Candle& a, const Candle& b) {
            return a.timestamp < b.timestamp;
        });
    }

    if (max_count > 0 && static_cast<int>(base.size()) > max_count) {
        base.erase(base.begin(), base.end() - max_count);
    }
}

void MarketScanner::throttleCandleApiCall() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - last_candle_api_call_time_;
    if (elapsed < kMinCandleApiInterval) {
        std::this_thread::sleep_for(kMinCandleApiInterval - elapsed);
    }
    last_candle_api_call_time_ = std::chrono::steady_clock::now();
}

// 1. ?¸ê? ë¶ˆê· ??ë¶„ì„ (?˜ëŸ‰ ì¤‘ì‹¬ + ê°€ì¤‘ì¹˜ ë¶€??
double MarketScanner::analyzeOrderBookImbalance(const nlohmann::json& orderbook) {
    try {
        nlohmann::json units;
        if (orderbook.is_array()) {
            units = orderbook;
        } else if (orderbook.contains("orderbook_units")) {
            units = orderbook["orderbook_units"];
        } else {
            return 0.0;
        }
        double weighted_bids = 0.0;
        double weighted_asks = 0.0;
        int depth = std::min(10, (int)units.size()); // ?ˆë¬´ ê¹Šì? ?¸ê????¸ì´ì¦ˆì´ë¯€ë¡?10?¨ê³„ë¡??œí•œ

        for (int i = 0; i < depth; ++i) {
            // ?„ì¬ê°€??ê°€ê¹Œìš¸?˜ë¡(iê°€ ?‘ì„?˜ë¡) ???’ì? ê°€ì¤‘ì¹˜ ë¶€??
            double weight = 1.0 / (i + 1); 
            
            double bid_size = units[i]["bid_size"].get<double>();
            double ask_size = units[i]["ask_size"].get<double>();
            
            weighted_bids += bid_size * weight;
            weighted_asks += ask_size * weight;
        }
        
        double total = weighted_bids + weighted_asks;
        if (total < 0.0001) return 0.0;
        
        // ê²°ê³¼ê°? 1??ê°€ê¹Œìš¸?˜ë¡ ê°•ë ¥??ë§¤ìˆ˜ ?°ìœ„, -1??ê°€ê¹Œìš¸?˜ë¡ ë§¤ë„ ?°ìœ„
        return (weighted_bids - weighted_asks) / total;
        
    } catch (...) { return 0.0; }
}

// 2. ë§¤ìˆ˜/ë§¤ë„ë²?ë¶„ì„ (?ë???ê°•ë„ ì¸¡ì •)
std::pair<int, int> MarketScanner::analyzeWalls(const nlohmann::json& orderbook) {
    try {
        auto units = orderbook["orderbook_units"];
        int count = std::min(15, (int)units.size());
        
        std::vector<double> bid_sizes, ask_sizes;
        for (int i = 0; i < count; ++i) {
            bid_sizes.push_back(units[i]["bid_size"].get<double>());
            ask_sizes.push_back(units[i]["ask_size"].get<double>());
        }

        // ì¤‘ì•™ê°?Median) ê¸°ë°˜?¼ë¡œ '?‰ìƒ?? ?¸ê? ?”ëŸ‰ ?Œì•… (?‰ê· ë³´ë‹¤ ?ˆë§¤??ê°ì???ê°•í•¨)
        auto get_median = [](std::vector<double> v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };

        double median_bid = get_median(bid_sizes);
        double median_ask = get_median(ask_sizes);

        int buy_walls = 0, sell_walls = 0;
        for (int i = 0; i < count; ++i) {
            // ì¤‘ì•™ê°??€ë¹?7ë°??´ìƒ??ê²½ìš°ë§?'ë²??¼ë¡œ ?¸ì •
            // (5ë°°ëŠ” ?ê°ë³´ë‹¤ ?ì£¼ ë°œìƒ?˜ì—¬ ë³€ë³„ë ¥????„ ???ˆìŒ)
            if (bid_sizes[i] > median_bid * 7.0) buy_walls++;
            if (ask_sizes[i] > median_ask * 7.0) sell_walls++;
        }
        
        return {buy_walls, sell_walls};
        
    } catch (...) { return {0, 0}; }
}

// 1. ê±°ë˜??ê¸‰ì¦ ë¶„ì„
double MarketScanner::analyzeVolumeSurge(const std::vector<Candle>& candles) {
    try {
        // BacktestRuntime::buildCoinMetricsFromCandleê³?ê°™ì? ?¤ì???
        // ?„ì¬ notional / ì§ì „ 19ê°??‰ê·  notional (1.0 = ?‰ê· )
        if (candles.size() < 20) {
            return 1.0;
        }

        const size_t last = candles.size() - 1;
        double avg_vol = 0.0;
        double avg_notional = 0.0;
        for (size_t i = candles.size() - 20; i < last; ++i) {
            avg_vol += candles[i].volume;
            avg_notional += (candles[i].close * candles[i].volume);
        }
        avg_vol /= 19.0;
        avg_notional /= 19.0;

        const double current_notional = candles[last].close * candles[last].volume;
        if (avg_notional > 0.0) {
            return current_notional / avg_notional;
        }
        return (avg_vol > 0.0) ? (candles[last].volume / avg_vol) : 1.0;
    } catch (...) { return 1.0; }
}

// 2. ë³€?™ì„± ë¶„ì„ (ATR ê¸°ë°˜)
double MarketScanner::analyzeVolatility(const std::vector<Candle>& candles) {
    try {
        if (candles.size() < 10) return 0.0;
        
        double total_range = 0.0;
        for (const auto& c : candles) {
            total_range += (c.high - c.low);
        }
        
        double atr = total_range / candles.size();
        double current_price = candles.back().close; // ìµœì‹ ê°€
        
        if (current_price < 0.0001) return 0.0;
        return (atr / current_price) * 100.0;
    } catch (...) { return 0.0; }
}

// 3. ëª¨ë©˜?€ ë¶„ì„ (RSI ê¸°ë°˜)
double MarketScanner::analyzeMomentum(const std::vector<Candle>& candles) {
    try {
        // RSI 14??ê¸°ì?
        if (candles.size() < 15) return 50.0;
        
        std::vector<double> gains, losses;
        // ê³¼ê±° -> ?„ì¬ ë°©í–¥?¼ë¡œ ì°¨ì´ ê³„ì‚°
        // candles.size() - 14 ë¶€???ê¹Œì§€ ?œíšŒ
        size_t start_idx = candles.size() - 14;
        for (size_t i = start_idx; i < candles.size(); ++i) {
            double prev = candles[i-1].close;
            double curr = candles[i].close;
            double change = curr - prev;
            
            if (change > 0) {
                gains.push_back(change);
                losses.push_back(0);
            } else {
                gains.push_back(0);
                losses.push_back(std::abs(change));
            }
        }
        
        double avg_gain = std::accumulate(gains.begin(), gains.end(), 0.0) / gains.size();
        double avg_loss = std::accumulate(losses.begin(), losses.end(), 0.0) / losses.size();
        
        if (avg_loss < 0.0001) return 100.0;
        
        double rs = avg_gain / avg_loss;
        return 100.0 - (100.0 / (1.0 + rs));
    } catch (...) { return 50.0; }
}


} // namespace analytics
} // namespace autolife

