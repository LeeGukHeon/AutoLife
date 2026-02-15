#pragma once

#include "common/Types.h"
#include "network/UpbitHttpClient.h"
#include "analytics/TechnicalIndicators.h"  // ✅ 이미 있는지 확인, 없으면 추가
#include "analytics/OrderbookAnalyzer.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <chrono>

namespace autolife {
namespace analytics {

// 코인별 수급 메트릭
struct CoinMetrics {
    std::string market;                  // 마켓 코드
    double current_price;                 // 현재가
    double volume_24h;                    // 24시간 거래량
    double volume_surge_ratio;            // 거래량 급증률 (평균 대비 %)
    double price_change_rate;             // 가격 변동률
    double price_momentum;                // 가격 모멘텀 (RSI 기반)
    double order_book_imbalance;         // 호가 불균형 (-1 ~ +1)
    double buy_pressure;                  // 매수 압력
    double sell_pressure;                 // 매도 압력
    int buy_wall_count;                   // 매수벽 개수
    int sell_wall_count;                  // 매도벽 개수
    double volatility;                    // 변동성
    double liquidity_score;               // 유동성 점수
    double composite_score;               // 종합 점수 (0-100)
    OrderbookSnapshot orderbook_snapshot; // 주문서 스냅샷
    nlohmann::json orderbook_units;       // 주문서 유닛 캐시

    // ✅ 캔들 데이터 추가
    std::vector<Candle> candles;
    std::map<std::string, std::vector<Candle>> candles_by_tf;
    
    CoinMetrics() : current_price(0), volume_24h(0), volume_surge_ratio(0),
                    price_change_rate(0), price_momentum(0), 
                    order_book_imbalance(0), buy_pressure(0), sell_pressure(0),
                    buy_wall_count(0), sell_wall_count(0),
                    volatility(0), liquidity_score(0), composite_score(0) {}
};

// 매수/매도벽 정보
struct Wall {
    double price;
    double size;
    double strength;  // 평균 대비 강도
    
    Wall(double p, double s, double st) : price(p), size(s), strength(st) {}
};

// Market Scanner - 전 종목 스캔 및 분석
class MarketScanner {
public:
    MarketScanner(std::shared_ptr<network::UpbitHttpClient> client);
    
    // 전체 마켓 스캔 (KRW 마켓만)
    std::vector<CoinMetrics> scanAllMarkets();
    
    // 상위 N개 종목 추출
    std::vector<std::string> getTopMarkets(int count = 10);
    
    // 특정 마켓 상세 분석
    CoinMetrics analyzeMarket(const std::string& market);
    
    // Volume Surge 감지 (거래량 급증)
    double detectVolumeSurge(const std::string& market);
    
    // Order Book Imbalance 계산
    double calculateOrderBookImbalance(const std::string& market);
    
    // 매수/매도벽 감지
    std::vector<Wall> detectBuyWalls(const std::string& market);
    std::vector<Wall> detectSellWalls(const std::string& market);
    
    // Fake Wall 탐지 (허수 주문)
    bool isFakeWall(const Wall& wall, const std::string& market);
    
    // 변동성 계산 (ATR 기반)
    double calculateVolatility(const std::string& market);
    
    // 유동성 점수 계산
    double calculateLiquidityScore(const std::string& market);
    
    // 가격 모멘텀 계산
    double calculatePriceMomentum(const std::string& market);
    
    // 종합 점수 계산 (가중치 적용)
    double calculateCompositeScore(const CoinMetrics& metrics);
    
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
    
    // 헬퍼 함수들
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
        // 이미 조회된 데이터로 분석 (API 호출 없음)
    double analyzeOrderBookImbalance(const nlohmann::json& orderbook);
    std::pair<int, int> analyzeWalls(const nlohmann::json& orderbook);  // {buy_walls, sell_walls}
    double analyzeVolumeSurge(const std::vector<Candle>& candles);
    double analyzeVolatility(const std::vector<Candle>& candles);
    double analyzeMomentum(const std::vector<Candle>& candles);
};

} // namespace analytics
} // namespace autolife
