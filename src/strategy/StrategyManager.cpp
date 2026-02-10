#include "strategy/StrategyManager.h"
#include "common/Logger.h"
#include <algorithm>
#include <numeric>

namespace autolife {
namespace strategy {

StrategyManager::StrategyManager(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
{
    LOG_INFO("StrategyManager 초기화");
}

void StrategyManager::registerStrategy(std::shared_ptr<IStrategy> strategy) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto info = strategy->getInfo();
    strategies_.push_back(strategy);
    
    LOG_INFO("전략 등록: {} (승률: {:.1f}%, 리스크: {}/10)",
             info.name, info.expected_winrate * 100, info.risk_level);
}

// [추가된 함수] 이름으로 전략 찾기
std::shared_ptr<IStrategy> StrategyManager::getStrategy(const std::string& name) {
    for (const auto& strategy : strategies_) {
        // 전략의 이름과 요청받은 이름(Position에 저장된 이름)이 같은지 확인
        if (strategy->getInfo().name == name) {
            return strategy;
        }
    }
    // 못 찾으면 nullptr 반환
    return nullptr;
}

std::vector<Signal> StrategyManager::collectSignals(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price,
    double available_capital
) {
    std::vector<Signal> signals;
    
    LOG_INFO("{} - 전략 분석 시작 (변동: {:.2f}%, 유동성: {:.1f})", 
             market, metrics.price_change_rate, metrics.liquidity_score);
    
    for (auto& strategy : strategies_) {
        if (!strategy->isEnabled()) {
            continue;
        }
        
        try {
            auto signal = strategy->generateSignal(market, metrics, candles, current_price, available_capital);
            
            if (signal.type != SignalType::NONE) {
                signals.push_back(signal);
                LOG_INFO("{} - {} 신호 발견: 강도 {:.2f}", 
                         market, signal.strategy_name, signal.strength);
            } else {
                LOG_INFO("{} - {} 신호 없음", market, strategy->getInfo().name);
            }
            
        } catch (const std::exception& e) {
            LOG_ERROR("{} - {} 분석 실패: {}", 
                      market, strategy->getInfo().name, e.what());
        }
    }
    
    return signals;
}

Signal StrategyManager::selectBestSignal(const std::vector<Signal>& signals) {
    if (signals.empty()) {
        return Signal(); // NONE
    }
    
    // 신호 점수 계산 후 최고점 선택
    auto best = std::max_element(signals.begin(), signals.end(),
        [this](const Signal& a, const Signal& b) {
            return calculateSignalScore(a) < calculateSignalScore(b);
        }
    );
    
    return *best;
}

std::vector<Signal> StrategyManager::filterSignals(
    const std::vector<Signal>& signals,
    double min_strength
) {
    std::vector<Signal> filtered;
    
    for (const auto& signal : signals) {
        if (signal.strength >= min_strength) {
            filtered.push_back(signal);
        }
    }
    
    return filtered;
}

Signal StrategyManager::synthesizeSignals(const std::vector<Signal>& signals) {
    if (signals.empty()) {
        return Signal();
    }
    
    Signal synthesized;
    
    // 매수 신호 개수 vs 매도 신호 개수
    int buy_count = 0, sell_count = 0;
    double total_strength = 0.0;
    
    for (const auto& signal : signals) {
        if (signal.type == SignalType::BUY || signal.type == SignalType::STRONG_BUY) {
            buy_count++;
        } else if (signal.type == SignalType::SELL || signal.type == SignalType::STRONG_SELL) {
            sell_count++;
        }
        
        total_strength += signal.strength;
    }
    
    // 다수결 + 가중 평균
    if (buy_count > sell_count) {
        synthesized.type = buy_count > sell_count * 2 ? SignalType::STRONG_BUY : SignalType::BUY;
    } else if (sell_count > buy_count) {
        synthesized.type = sell_count > buy_count * 2 ? SignalType::STRONG_SELL : SignalType::SELL;
    } else {
        synthesized.type = SignalType::HOLD;
    }
    
    // 평균 강도
    synthesized.strength = total_strength / signals.size();
    
    // 진입가, 손절가, 익절가는 중간값 사용
    std::vector<double> entry_prices, stop_losses, take_profit1s, take_profit2s;
    for (const auto& signal : signals) {
        if (signal.entry_price > 0) entry_prices.push_back(signal.entry_price);
        if (signal.stop_loss > 0) stop_losses.push_back(signal.stop_loss);
        if (signal.take_profit_1 > 0) take_profit1s.push_back(signal.take_profit_1);
        if (signal.take_profit_2 > 0) take_profit2s.push_back(signal.take_profit_2);
    }

    if (!entry_prices.empty()) {
        std::sort(entry_prices.begin(), entry_prices.end());
        synthesized.entry_price = entry_prices[entry_prices.size() / 2];
    }

    if (!stop_losses.empty()) {
        std::sort(stop_losses.begin(), stop_losses.end());
        synthesized.stop_loss = stop_losses[stop_losses.size() / 2];
    }

    // 합성 익절: 1차와 2차 모두 고려. 기본적으로 2차(완전 청산)을 우선 사용
    if (!take_profit2s.empty()) {
        std::sort(take_profit2s.begin(), take_profit2s.end());
        synthesized.take_profit_2 = take_profit2s[take_profit2s.size() / 2];
    }
    if (!take_profit1s.empty()) {
        std::sort(take_profit1s.begin(), take_profit1s.end());
        synthesized.take_profit_1 = take_profit1s[take_profit1s.size() / 2];
    }
    
    synthesized.reason = "Synthesized from " + std::to_string(signals.size()) + " strategies";
    
    return synthesized;
}

std::map<std::string, IStrategy::Statistics> StrategyManager::getAllStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::map<std::string, IStrategy::Statistics> stats_map;
    
    for (const auto& strategy : strategies_) {
        auto info = strategy->getInfo();
        stats_map[info.name] = strategy->getStatistics();
    }
    
    return stats_map;
}

void StrategyManager::enableStrategy(const std::string& name, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& strategy : strategies_) {
        if (strategy->getInfo().name == name) {
            strategy->setEnabled(enabled);
            LOG_INFO("전략 {}: {}", name, enabled ? "활성화" : "비활성화");
            return;
        }
    }
}

std::vector<std::string> StrategyManager::getActiveStrategies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> active;
    
    for (const auto& strategy : strategies_) {
        if (strategy->isEnabled()) {
            active.push_back(strategy->getInfo().name);
        }
    }
    
    return active;
}

std::vector<std::shared_ptr<IStrategy>> StrategyManager::getStrategies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return strategies_;
}

double StrategyManager::getOverallWinRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int total_trades = 0;
    int total_wins = 0;
    
    for (const auto& strategy : strategies_) {
        auto stats = strategy->getStatistics();
        total_trades += stats.winning_trades + stats.losing_trades;
        total_wins += stats.winning_trades;
    }
    
    if (total_trades == 0) return 0.0;
    
    return static_cast<double>(total_wins) / total_trades;
}

double StrategyManager::calculateSignalScore(const Signal& signal) const {
    double score = signal.strength;
    
    // SignalType에 따른 가중치
    switch (signal.type) {
        case SignalType::STRONG_BUY:
        case SignalType::STRONG_SELL:
            score *= 1.5;
            break;
        case SignalType::BUY:
        case SignalType::SELL:
            score *= 1.0;
            break;
        default:
            score *= 0.5;
            break;
    }
    
    // Risk/Reward Ratio 고려
    double tp = signal.getTakeProfitForLegacy();
    if (signal.entry_price > 0 && signal.stop_loss > 0 && tp > 0) {
        double risk = std::abs(signal.entry_price - signal.stop_loss);
        double reward = std::abs(tp - signal.entry_price);
        
        if (risk > 0) {
            double rr_ratio = reward / risk;
            score *= std::min(2.0, rr_ratio / 2.0); // RR 2:1 이상이면 가산점
        }
    }
    
    return score;
}

} // namespace strategy
} // namespace autolife