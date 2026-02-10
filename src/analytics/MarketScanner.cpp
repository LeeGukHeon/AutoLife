#include "analytics/MarketScanner.h"
#include "analytics/OrderbookAnalyzer.h"
#include "common/Logger.h"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace autolife {
namespace analytics {

MarketScanner::MarketScanner(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
    , last_scan_time_(std::chrono::steady_clock::now())
{
    LOG_INFO("MarketScanner 초기화");
}

std::vector<CoinMetrics> MarketScanner::scanAllMarkets() {
    LOG_INFO("전체 마켓 스캔 시작 (최적화 v2)...");
    
    auto start_time = std::chrono::steady_clock::now();
    auto all_markets = getAllKRWMarkets();
    std::vector<CoinMetrics> all_metrics;
    
    const int BATCH_SIZE = 100;
    
    // 1단계: 배치로 전체 Ticker 조회
    LOG_INFO("1단계: {} 종목 Ticker 배치 조회...", all_markets.size());
    std::map<std::string, nlohmann::json> ticker_map;
    
    for (size_t i = 0; i < all_markets.size(); i += BATCH_SIZE) {
        size_t end = std::min(i + BATCH_SIZE, all_markets.size());
        std::vector<std::string> batch(all_markets.begin() + i, all_markets.begin() + end);
        
        try {
            auto tickers = client_->getTickerBatch(batch);
            for (const auto& ticker : tickers) {
                std::string market = ticker["market"].get<std::string>();
                // [추가] 스테이블 코인 필터링 (USDT, USDC 등)
                // 'KRW-USDT'나 'KRW-USDC'를 여기서 원천 차단합니다.
                if (market == "KRW-USDT" || market == "KRW-USDC") {
                    continue; 
                }
                ticker_map[market] = ticker;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Ticker 배치 조회 실패: {}", e.what());
        }
    }
    LOG_INFO("Ticker 조회 완료: {} 종목", ticker_map.size());
    
    // 2단계: 거래대금 기준 1차 필터링
    LOG_INFO("2단계: 거래대금 순 정렬...");
    std::vector<std::pair<std::string, double>> volume_ranks;
    
    for (const auto& [market, ticker] : ticker_map) {
        // [수정] 24h 누적이 아니라 당일 누적(acc_trade_price) 사용
        // 만약 필드가 없다면 24h로 fallback 처리
        double volume = 0.0;
        volume = ticker["acc_trade_price_24h"].get<double>();
        volume_ranks.emplace_back(market, volume);
    }
    
    std::sort(volume_ranks.begin(), volume_ranks.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // 2단계 정렬 직후 로그 추가
    LOG_INFO("TOP 5 거래대금 종목:");
    for(int i=0; i<std::min(5, (int)volume_ranks.size()); ++i) {
        LOG_INFO("  #{} {} : {:.0f}억", i+1, volume_ranks[i].first, volume_ranks[i].second / 100000000.0);
    }

    // 3단계: 상위 50개의 OrderBook + Candles 배치 조회
    int detail_count = std::min(50, (int)volume_ranks.size());
    LOG_INFO("3단계: 상위 {} 종목 데이터 수집...", detail_count);
    
    std::vector<std::string> top_markets;
    for (int i = 0; i < detail_count; ++i) {
        top_markets.push_back(volume_ranks[i].first);
    }
    
    // OrderBook 배치 조회 (50개를 10개씩 나눠서)
    std::map<std::string, nlohmann::json> orderbook_map;
    const int OB_BATCH = 10;  // OrderBook은 10개씩 (초당 10회 제한)
    
    for (size_t i = 0; i < top_markets.size(); i += OB_BATCH) {
        size_t end = std::min(i + OB_BATCH, top_markets.size());
        std::vector<std::string> batch(top_markets.begin() + i, top_markets.begin() + end);
        
        try {
            auto orderbooks = client_->getOrderBookBatch(batch);
            for (const auto& ob : orderbooks) {
                std::string market = ob["market"].get<std::string>();
                orderbook_map[market] = ob;
            }
            
            // Rate Limit 준수를 위해 0.1초 대기
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
        } catch (const std::exception& e) {
            LOG_ERROR("OrderBook 배치 조회 실패: {}", e.what());
        }
    }
    LOG_INFO("OrderBook 수집 완료: {} 종목", orderbook_map.size());
    
    // Candles 조회 (각 종목마다 순차 - 이건 배치 불가능)
    std::map<std::string, nlohmann::json> candles_map_1m;
    std::map<std::string, nlohmann::json> candles_map_5m;
    std::map<std::string, nlohmann::json> candles_map_1h;
    std::map<std::string, nlohmann::json> candles_map_4h;
    std::map<std::string, nlohmann::json> candles_map_1d;

    const int CANDLES_1M = 200;
    const int CANDLES_5M = 120;
    const int CANDLES_1H = 120;
    const int CANDLES_4H = 90;
    const int CANDLES_1D = 60;

    int candle_count = 0;
    for (const auto& market : top_markets) {
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));

            auto candles = client_->getCandles(market, "1", CANDLES_1M);
            candles_map_1m[market] = candles;
            candle_count++;

        } catch (const std::exception& e) {
            LOG_ERROR("Candles(1m) 조회 실패: {} - {}", market, e.what());

            if (std::string(e.what()).find("429") != std::string::npos ||
                std::string(e.what()).find("too_many_requests") != std::string::npos) {
                LOG_WARN("Rate Limit 도달, 1초 대기 후 재시도...");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    LOG_INFO("Candles(1m) 수집 완료: {} 종목", candles_map_1m.size());

    // 추가 타임프레임은 상위 일부 종목만 수집 (API 제한 고려)
    int tf5m_limit = std::min(20, (int)top_markets.size());
    int tf1h_limit = std::min(20, (int)top_markets.size());
    int tf4h_limit = std::min(10, (int)top_markets.size());
    int tf1d_limit = std::min(10, (int)top_markets.size());

    for (int i = 0; i < tf5m_limit; ++i) {
        const auto& market = top_markets[i];
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            candles_map_5m[market] = client_->getCandles(market, "5", CANDLES_5M);
        } catch (const std::exception& e) {
            LOG_ERROR("Candles(5m) 조회 실패: {} - {}", market, e.what());
        }
    }

    for (int i = 0; i < tf1h_limit; ++i) {
        const auto& market = top_markets[i];
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            candles_map_1h[market] = client_->getCandles(market, "60", CANDLES_1H);
        } catch (const std::exception& e) {
            LOG_ERROR("Candles(1h) 조회 실패: {} - {}", market, e.what());
        }
    }

    for (int i = 0; i < tf4h_limit; ++i) {
        const auto& market = top_markets[i];
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            candles_map_4h[market] = client_->getCandles(market, "240", CANDLES_4H);
        } catch (const std::exception& e) {
            LOG_ERROR("Candles(4h) 조회 실패: {} - {}", market, e.what());
        }
    }

    for (int i = 0; i < tf1d_limit; ++i) {
        const auto& market = top_markets[i];
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            candles_map_1d[market] = client_->getCandlesDays(market, CANDLES_1D);
        } catch (const std::exception& e) {
            LOG_ERROR("Candles(1d) 조회 실패: {} - {}", market, e.what());
        }
    }

// 4단계: 수집된 데이터로 분석 (API 호출 없음!)
LOG_INFO("4단계: 종목 분석 중...");

// 4단계 수정본
for (size_t i = 0; i < top_markets.size(); ++i) {
    const auto& market = top_markets[i];
    try {
        CoinMetrics metrics;
        metrics.market = market;

 // ===== Ticker 데이터 분석 =====
if (ticker_map.find(market) != ticker_map.end()) {
    const auto& ticker = ticker_map[market];
    
    metrics.current_price = ticker["trade_price"].get<double>();
    
    // [수정] volume(개수)이 아니라 price(거래대금)를 가져와야 유동성 분석이 정확함
    metrics.volume_24h = ticker["acc_trade_price_24h"].get<double>(); 
    
    // [수정] 0.0015 -> 0.15 (%) 단위로 변환
    metrics.price_change_rate = ticker["signed_change_rate"].get<double>() * 100.0;
    
    // [팁] 고가/저가 대비 현재 위치 분석을 위해 추가하면 좋은 데이터
    // metrics.high_24h = ticker["high_price"].get<double>();
    // metrics.low_24h = ticker["low_price"].get<double>();
}

// ===== OrderBook 데이터 분석 =====
    if (orderbook_map.find(market) != orderbook_map.end()) {
        const auto& ob = orderbook_map[market];
        
        // [주의] ob 전체가 아니라 orderbook_units 배열을 넘겨야 할 확률이 높음
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
            // [중요] 정렬된 구조체 데이터 생성
            metrics.candles = TechnicalIndicators::jsonToCandles(candles_map_1m[market]);
            metrics.candles_by_tf["1m"] = metrics.candles;
            
            if (!metrics.candles.empty()) {
                metrics.volume_surge_ratio = analyzeVolumeSurge(metrics.candles); 
                metrics.volatility = analyzeVolatility(metrics.candles);
                metrics.price_momentum = analyzeMomentum(metrics.candles);
            }
        }

        if (candles_map_5m.find(market) != candles_map_5m.end()) {
            metrics.candles_by_tf["5m"] = TechnicalIndicators::jsonToCandles(candles_map_5m[market]);
        }

        if (candles_map_1h.find(market) != candles_map_1h.end()) {
            metrics.candles_by_tf["1h"] = TechnicalIndicators::jsonToCandles(candles_map_1h[market]);
        }

        if (candles_map_4h.find(market) != candles_map_4h.end()) {
            metrics.candles_by_tf["4h"] = TechnicalIndicators::jsonToCandles(candles_map_4h[market]);
        }

        if (candles_map_1d.find(market) != candles_map_1d.end()) {
            metrics.candles_by_tf["1d"] = TechnicalIndicators::jsonToCandles(candles_map_1d[market]);
        }

        // [수정] 유동성 점수 기준 현실화 (예: 1,000억 기준 100점)
        double trade_price_24h = ticker_map[market]["acc_trade_price_24h"].get<double>();
        metrics.liquidity_score = std::min(100.0, (trade_price_24h / 100000000000.0) * 100.0);
        metrics.volume_24h = trade_price_24h;
        
        metrics.composite_score = calculateCompositeScore(metrics);
        all_metrics.push_back(metrics);
        
    } catch (const std::exception& e) {
        LOG_ERROR("마켓 분석 실패: {} - {}", market, e.what());
    }
}

    
    // 종합 점수로 정렬
    std::sort(all_metrics.begin(), all_metrics.end(),
        [](const CoinMetrics& a, const CoinMetrics& b) {
            return a.composite_score > b.composite_score;
        }
    );
    
    cached_metrics_ = all_metrics;
    last_scan_time_ = std::chrono::steady_clock::now();
    
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time
    ).count();
    
    LOG_INFO("마켓 스캔 완료: {} 종목 분석, {}초 소요", all_metrics.size(), elapsed);
    
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
        // 1. 현재 시세 조회
        auto ticker = client_->getTicker(market);
        if (!ticker.empty()) {
            metrics.current_price = ticker[0]["trade_price"].get<double>();
            // [수정] 개수가 아닌 거래대금(Price)을 가져와 정합성 유지
            metrics.volume_24h = ticker[0]["acc_trade_price_24h"].get<double>();
            // [수정] 단위를 %로 변환 (0.0015 -> 0.15)
            metrics.price_change_rate = ticker[0]["signed_change_rate"].get<double>() * 100.0;
        }
        
        // 2. Volume Surge 감지
        metrics.volume_surge_ratio = detectVolumeSurge(market);
        
        // 3. Order Book 분석
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
        
        // 5. 변동성 계산
        metrics.volatility = calculateVolatility(market);
        
        // 6. 유동성 점수
        metrics.liquidity_score = calculateLiquidityScore(market);
        
        // 7. 가격 모멘텀
        metrics.price_momentum = calculatePriceMomentum(market);

        // 7-1. 멀티 타임프레임 캔들 캐시
        metrics.candles = getRecentCandles(market, "1", 120);
        if (!metrics.candles.empty()) {
            metrics.candles_by_tf["1m"] = metrics.candles;
        }

        auto candles_5m = getRecentCandles(market, "5", 120);
        if (!candles_5m.empty()) {
            metrics.candles_by_tf["5m"] = candles_5m;
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
        
        // 8. 종합 점수 계산
        metrics.composite_score = calculateCompositeScore(metrics);
        
    } catch (const std::exception& e) {
        LOG_ERROR("마켓 분석 오류 {}: {}", market, e.what());
    }
    
    return metrics;
}

double MarketScanner::detectVolumeSurge(const std::string& market) {
    try {
        auto ticker = client_->getTicker(market);
        if (ticker.empty()) return 0.0;
        
        double current_volume = ticker[0]["acc_trade_volume_24h"].get<double>();
        
        // 최근 7일 평균 거래량 계산
        double avg_volume = getAverageVolume(market, 168); // 168시간 = 7일
        
        if (avg_volume < 0.0001) return 0.0;
        
        // 급증률 계산 (%)
        double surge_ratio = (current_volume / avg_volume) * 100.0;
        
        return surge_ratio;
        
    } catch (const std::exception&) {
        return 0.0;
    }
}

double MarketScanner::calculateOrderBookImbalance(const std::string& market) {
    try {
        auto orderbook = client_->getOrderBook(market);
        if (orderbook.empty()) return 0.0;
        
        auto units = orderbook[0]["orderbook_units"];
        
        double total_bid_volume = 0.0;
        double total_ask_volume = 0.0;
        
        // 상위 15호가까지 분석
        int depth = std::min(15, static_cast<int>(units.size()));
        
        for (int i = 0; i < depth; ++i) {
            double bid_price = units[i]["bid_price"].get<double>();
            double bid_size = units[i]["bid_size"].get<double>();
            double ask_price = units[i]["ask_price"].get<double>();
            double ask_size = units[i]["ask_size"].get<double>();
            
            // 금액 기준으로 계산
            total_bid_volume += bid_price * bid_size;
            total_ask_volume += ask_price * ask_size;
        }
        
        // OBI 계산: -1 (강력 매도) ~ +1 (강력 매수)
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
        
        // 평균 매수 호가 크기 계산
        double total_bid_size = 0.0;
        int count = std::min(15, static_cast<int>(units.size()));
        
        for (int i = 0; i < count; ++i) {
            total_bid_size += units[i]["bid_size"].get<double>();
        }
        
        double avg_bid_size = total_bid_size / count;
        
        // 평균의 5배 이상이면 Buy Wall로 판단
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
        // 에러 무시
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
        // 에러 무시
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
        
        // ATR (Average True Range) 계산
        double atr = std::accumulate(high_low_diff.begin(), high_low_diff.end(), 0.0) 
                     / high_low_diff.size();
        
        // 현재가 대비 변동성 비율
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
        
        double volume_24h = ticker[0]["acc_trade_price_24h"].get<double>(); // 거래대금
        
        // 거래대금 기준 점수 (10억원 이상 = 100점)
        double score = std::min(100.0, (volume_24h / 1000000000.0) * 100.0);
        
        return score;
        
    } catch (const std::exception&) {
        return 0.0;
    }
}

double MarketScanner::calculatePriceMomentum(const std::string& market) {
    try {
        auto candles = getRecentCandles(market, 14);
        if (candles.size() < 14) return 50.0; // 중립
        
        // RSI 계산
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

double MarketScanner::calculateCompositeScore(const CoinMetrics& metrics) {
    // 가중치 재분배 (총 1.0)
    const double LIQUIDITY_WEIGHT = 0.40;   // 유동성(절대 거래대금) 비중 대폭 상향 (15% -> 40%)
    const double VOLUME_WEIGHT = 0.20;      // 거래량 급증 비중 하향 (30% -> 20%)
    const double MOMENTUM_WEIGHT = 0.20;    // 가격 모멘텀 유지
    const double OBI_WEIGHT = 0.10;         // 호가 불균형 하향 (단기 노이즈 방지)
    const double VOLATILITY_WEIGHT = 0.10;  // 변동성 유지
    
    double score = 0.0;
    
    // 1. Liquidity (이제 점수의 40%를 결정)
    // liquidity_score가 이미 0~100으로 스케일링 되어 있다면 그대로 사용
    score += metrics.liquidity_score * LIQUIDITY_WEIGHT;

    // 2. Volume Surge (대형주 배려를 위해 분모 상향조정 또는 로그 스케일 검토)
    // 평소 대비 150%만 터져도 대형주에겐 큰 의미이므로 기준을 150으로 낮춤
    double volume_score = std::min(100.0, (metrics.volume_surge_ratio / 150.0) * 100.0);
    score += volume_score * VOLUME_WEIGHT;
    
    // 3. Price Momentum
    score += metrics.price_momentum * MOMENTUM_WEIGHT;
    
    // 4. Order Book Imbalance (중요하지만 보조 지표로 활용)
    double obi_score = (metrics.order_book_imbalance + 1.0) * 50.0;
    score += obi_score * OBI_WEIGHT;
    
    // 5. Volatility (변동성이 너무 낮아도 주도주라면 가산점)
    double volatility_score = 0.0;
    if (metrics.volatility >= 1.0 && metrics.volatility <= 5.0) { // 하한선을 1%로 완화
        volatility_score = 100.0;
    } else {
        volatility_score = std::max(0.0, 100.0 - std::abs(metrics.volatility - 3.0) * 20.0);
    }
    score += volatility_score * VOLATILITY_WEIGHT;
    
    return std::min(100.0, score);
}

// Private 헬퍼 함수들

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
        LOG_ERROR("마켓 목록 조회 실패: {}", e.what());
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
    try {
        auto json = client_->getCandles(market, unit, count);
        return TechnicalIndicators::jsonToCandles(json);
    } catch (const std::exception&) {
        return {};
    }
}

std::vector<Candle> MarketScanner::getRecentDayCandles(
    const std::string& market,
    int count) {
    try {
        auto json = client_->getCandlesDays(market, count);
        return TechnicalIndicators::jsonToCandles(json);
    } catch (const std::exception&) {
        return {};
    }
}

// 1. 호가 불균형 분석 (수량 중심 + 가중치 부여)
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
        int depth = std::min(10, (int)units.size()); // 너무 깊은 호가는 노이즈이므로 10단계로 제한

        for (int i = 0; i < depth; ++i) {
            // 현재가에 가까울수록(i가 작을수록) 더 높은 가중치 부여
            double weight = 1.0 / (i + 1); 
            
            double bid_size = units[i]["bid_size"].get<double>();
            double ask_size = units[i]["ask_size"].get<double>();
            
            weighted_bids += bid_size * weight;
            weighted_asks += ask_size * weight;
        }
        
        double total = weighted_bids + weighted_asks;
        if (total < 0.0001) return 0.0;
        
        // 결과값: 1에 가까울수록 강력한 매수 우위, -1에 가까울수록 매도 우위
        return (weighted_bids - weighted_asks) / total;
        
    } catch (...) { return 0.0; }
}

// 2. 매수/매도벽 분석 (상대적 강도 측정)
std::pair<int, int> MarketScanner::analyzeWalls(const nlohmann::json& orderbook) {
    try {
        auto units = orderbook["orderbook_units"];
        int count = std::min(15, (int)units.size());
        
        std::vector<double> bid_sizes, ask_sizes;
        for (int i = 0; i < count; ++i) {
            bid_sizes.push_back(units[i]["bid_size"].get<double>());
            ask_sizes.push_back(units[i]["ask_size"].get<double>());
        }

        // 중앙값(Median) 기반으로 '평상시' 호가 잔량 파악 (평균보다 허매수 감지에 강함)
        auto get_median = [](std::vector<double> v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };

        double median_bid = get_median(bid_sizes);
        double median_ask = get_median(ask_sizes);

        int buy_walls = 0, sell_walls = 0;
        for (int i = 0; i < count; ++i) {
            // 중앙값 대비 7배 이상인 경우만 '벽'으로 인정
            // (5배는 생각보다 자주 발생하여 변별력이 낮을 수 있음)
            if (bid_sizes[i] > median_bid * 7.0) buy_walls++;
            if (ask_sizes[i] > median_ask * 7.0) sell_walls++;
        }
        
        return {buy_walls, sell_walls};
        
    } catch (...) { return {0, 0}; }
}

// 1. 거래량 급증 분석
double MarketScanner::analyzeVolumeSurge(const std::vector<Candle>& candles) {
    try {
        if (candles.size() < 2) return 0.0;
        
        // 최신 캔들 (마지막 인덱스)
        double recent_volume = candles.back().volume;
        
        // 과거 평균 거래량 (최신 제외)
        double total_volume = 0.0;
        for (size_t i = 0; i < candles.size() - 1; ++i) {
            total_volume += candles[i].volume;
        }
        
        double avg_volume = total_volume / (candles.size() - 1);
        if (avg_volume < 0.0001) return 0.0;
        
        return (recent_volume / avg_volume) * 100.0;
    } catch (...) { return 0.0; }
}

// 2. 변동성 분석 (ATR 기반)
double MarketScanner::analyzeVolatility(const std::vector<Candle>& candles) {
    try {
        if (candles.size() < 10) return 0.0;
        
        double total_range = 0.0;
        for (const auto& c : candles) {
            total_range += (c.high - c.low);
        }
        
        double atr = total_range / candles.size();
        double current_price = candles.back().close; // 최신가
        
        if (current_price < 0.0001) return 0.0;
        return (atr / current_price) * 100.0;
    } catch (...) { return 0.0; }
}

// 3. 모멘텀 분석 (RSI 기반)
double MarketScanner::analyzeMomentum(const std::vector<Candle>& candles) {
    try {
        // RSI 14일 기준
        if (candles.size() < 15) return 50.0;
        
        std::vector<double> gains, losses;
        // 과거 -> 현재 방향으로 차이 계산
        // candles.size() - 14 부터 끝까지 순회
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
