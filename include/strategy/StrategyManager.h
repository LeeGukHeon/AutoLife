#pragma once

#include "strategy/IStrategy.h"
#include "network/UpbitHttpClient.h"
#include "analytics/MarketScanner.h"
#include <vector>
#include <map>
#include <memory>
#include <mutex>

namespace autolife {
namespace strategy {

// 전략 관리자 - 여러 전략 통합 관리
class StrategyManager {
public:
    StrategyManager(std::shared_ptr<network::UpbitHttpClient> client);
    
    // 전략 등록
    void registerStrategy(std::shared_ptr<IStrategy> strategy);
    
    // [추가됨] 이름으로 전략 찾기 (TradingEngine에서 호출)
    std::shared_ptr<IStrategy> getStrategy(const std::string& name);
    
    // 모든 전략에서 신호 수집
    std::vector<Signal> collectSignals(
        const std::string& market,
        const analytics::CoinMetrics& metrics,
        const std::vector<analytics::Candle>& candles,
        double current_price
    );
    
    // 최적 신호 선택 (여러 전략 중 가장 강한 신호)
    Signal selectBestSignal(const std::vector<Signal>& signals);
    
    // 신호 필터링 (조건에 맞는 신호만)
    std::vector<Signal> filterSignals(
        const std::vector<Signal>& signals,
        double min_strength = 0.6
    );
    
    // 신호 합성 (여러 전략의 신호를 가중 평균)
    Signal synthesizeSignals(const std::vector<Signal>& signals);
    
    // 전략별 통계 조회
    std::map<std::string, IStrategy::Statistics> getAllStatistics() const;
    
    // 전략 활성화/비활성화
    void enableStrategy(const std::string& name, bool enabled);
    
    // 활성화된 전략 목록
    std::vector<std::string> getActiveStrategies() const;
    
    // 전체 승률 계산
    double getOverallWinRate() const;
    
private:
    std::vector<std::shared_ptr<IStrategy>> strategies_;
    std::shared_ptr<network::UpbitHttpClient> client_;
    mutable std::mutex mutex_;
    
    // 신호 강도 계산
    double calculateSignalScore(const Signal& signal) const;
};

} // namespace strategy
} // namespace autolife
