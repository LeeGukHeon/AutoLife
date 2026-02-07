#include "risk/RiskManager.h"
#include "common/Logger.h"
#include <algorithm>
#include <cmath>
#include <chrono>

namespace autolife {
namespace risk {

RiskManager::RiskManager(double initial_capital)
    : initial_capital_(initial_capital)
    , current_capital_(initial_capital)
    , daily_trade_count_(0)
    , daily_reset_time_(0)
    , max_positions_(5)
    , max_daily_trades_(20)
    , max_drawdown_pct_(0.10)  // 10%
    , min_reentry_interval_(300)  // 5분
    , max_capital_(initial_capital)
    , total_fees_paid_(0)
{
    LOG_INFO("RiskManager 초기화 - 초기 자본: {:.0f} KRW", initial_capital);
}

// ===== 포지션 진입 =====

bool RiskManager::canEnterPosition(
    const std::string& market,
    double entry_price,
    double position_size_ratio,
    const std::string& strategy_name
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 미사용 매개변수 경고 제거
    (void)entry_price;
    (void)strategy_name;
    
    // 1. 이미 포지션 보유 중
    if (positions_.find(market) != positions_.end()) {
        LOG_WARN("{} 이미 포지션 보유 중", market);
        return false;
    }
    
    // 2. 최대 포지션 개수 초과
    if (hasReachedMaxPositions()) {
        LOG_WARN("최대 포지션 개수 도달 ({}/{})", positions_.size(), max_positions_);
        return false;
    }
    
    // 3. 일일 거래 횟수 제한
    if (hasReachedDailyTradeLimit()) {
        LOG_WARN("일일 거래 횟수 제한 도달 ({}/{})", daily_trade_count_, max_daily_trades_);
        return false;
    }
    
    // 4. 재진입 대기 시간
    if (!canTradeMarket(market)) {
        LOG_WARN("{} 재진입 대기 중", market);
        return false;
    }
    
    // 5. Drawdown 초과
    if (isDrawdownExceeded()) {
        LOG_ERROR("Drawdown 한계 초과! 거래 중단");
        return false;
    }
    
    // 6. 자본 부족 체크
    // current_capital_은 현재 '예수금 + 평가손익' 개념이 섞여있으므로
    // 가용 현금(Available Cash)을 정확히 계산해야 함
    
    double invested_sum = 0;
    for (const auto& [m, pos] : positions_) {
        invested_sum += pos.invested_amount;
    }
    
    double available_cash = current_capital_ - invested_sum;
    double required_amount = current_capital_ * position_size_ratio;
    
    if (required_amount > available_cash) {
        LOG_WARN("자본 부족: 필요 {:.0f} > 가용 {:.0f}", required_amount, available_cash);
        return false;
    }
    
    return true;
}

void RiskManager::enterPosition(
    const std::string& market,
    double entry_price,
    double quantity,
    double stop_loss,
    double take_profit_1,
    double take_profit_2,
    const std::string& strategy_name
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Position pos;
    pos.market = market;
    pos.entry_price = entry_price;
    pos.current_price = entry_price;
    pos.quantity = quantity;
    pos.invested_amount = entry_price * quantity; // 순수 진입 금액
    pos.entry_time = getCurrentTimestamp();
    pos.stop_loss = stop_loss;
    pos.take_profit_1 = take_profit_1;
    pos.take_profit_2 = take_profit_2;
    pos.strategy_name = strategy_name;
    pos.half_closed = false;
    
    // 수수료 차감 로직 개선
    double entry_fee = calculateFee(pos.invested_amount);
    
    // 자본금에서 수수료만큼 즉시 차감
    current_capital_ -= entry_fee;
    total_fees_paid_ += entry_fee;
    
    // 포지션 등록
    positions_[market] = pos;
    
    // 거래 제한 및 통계 기록
    last_trade_time_[market] = getCurrentTimestamp();
    daily_trade_count_++;
    
    LOG_INFO("🔵 포지션 진입: {} | 수량: {:.4f} | 투자: {:.0f} | 수수료: {:.0f} | 자본: {:.0f}",
             market, quantity, pos.invested_amount, entry_fee, current_capital_);
    LOG_INFO("   └ 설정: 손절 {:.0f} ({:.2f}%), 익절1 {:.0f} ({:.2f}%), 익절2 {:.0f} ({:.2f}%)",
             stop_loss, (stop_loss - entry_price) / entry_price * 100.0,
             take_profit_1, (take_profit_1 - entry_price) / entry_price * 100.0,
             take_profit_2, (take_profit_2 - entry_price) / entry_price * 100.0);
}

// ===== 포지션 업데이트 =====

void RiskManager::updatePosition(const std::string& market, double current_price) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    pos.current_price = current_price;
    
    // 미실현 손익 계산
    double current_value = current_price * pos.quantity;
    pos.unrealized_pnl = current_value - pos.invested_amount;
    pos.unrealized_pnl_pct = (current_price - pos.entry_price) / pos.entry_price;
}

// ===== 포지션 청산 체크 =====

bool RiskManager::shouldExitPosition(const std::string& market) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return false;
    
    const auto& pos = it->second;
    double current_price = pos.current_price;
    
    // 1. 손절
    if (current_price <= pos.stop_loss) {
        LOG_WARN("{} 손절가 도달: {:.0f} <= {:.0f}", market, current_price, pos.stop_loss);
        return true;
    }
    
    // 2. 2차 익절 (전체 청산)
    if (current_price >= pos.take_profit_2) {
        LOG_INFO("{} 2차 익절가 도달: {:.0f} >= {:.0f}", market, current_price, pos.take_profit_2);
        return true;
    }
    
    // 3. 1차 익절 (50% 청산) - 아직 안했으면
    if (!pos.half_closed && current_price >= pos.take_profit_1) {
        LOG_INFO("{} 1차 익절가 도달: {:.0f} >= {:.0f}", market, current_price, pos.take_profit_1);
        // 실제 청산은 partialExit()에서 처리
        return false;
    }
    
    return false;
}

void RiskManager::exitPosition(
    const std::string& market,
    double exit_price,
    const std::string& exit_reason
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    
    // 1. 매도 총액 및 수수료 계산
    double exit_value = exit_price * pos.quantity;
    double exit_fee = calculateFee(exit_value);
    
    // 2. 순수 확정 손익 계산
    // Net Profit = (매도총액 - 매도수수료) - 진입금액
    double net_profit = (exit_value - exit_fee) - pos.invested_amount;
    
    // 3. 자본금 업데이트
    current_capital_ += net_profit;
    total_fees_paid_ += exit_fee;
    
    // 4. 최고 자본금(High Water Mark) 갱신 - MDD 기준점
    if (current_capital_ > max_capital_) {
        max_capital_ = current_capital_;
    }
    
    // 통계용 변동률
    double raw_pnl_pct = (exit_price - pos.entry_price) / pos.entry_price;
    
    // 거래 이력 기록
    recordTrade(pos, exit_price, exit_reason);
    
    // 포지션 삭제
    positions_.erase(it);
    
    LOG_INFO("🔴 포지션 청산: {} | 손익: {:.0f} ({:+.2f}%) | 이유: {} | 현재자본: {:.0f}",
             market, net_profit, raw_pnl_pct * 100.0, exit_reason, current_capital_);
}

void RiskManager::partialExit(const std::string& market, double exit_price) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    
    if (pos.half_closed) return; // 이미 1차 익절 완료
    
    // 50% 청산
    double exit_quantity = pos.quantity * 0.5;
    double exit_value = exit_price * exit_quantity;
    double fee = calculateFee(exit_value);
    double net_value = exit_value - fee;
    
    // 자본 업데이트
    current_capital_ += net_value;
    total_fees_paid_ += fee;
    
    // 포지션 업데이트
    pos.quantity -= exit_quantity;
    pos.invested_amount *= 0.5;
    pos.half_closed = true;
    
    // 손절선을 본전으로 이동
    pos.stop_loss = pos.entry_price;
    
    double profit = net_value - (pos.invested_amount);
    
    LOG_INFO("1차 익절 (50%): {} - 청산가: {:.0f}, 수익: {:.0f}, 손절선 본전 이동",
             market, exit_price, profit);
}

void RiskManager::moveStopToBreakeven(const std::string& market) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    pos.stop_loss = pos.entry_price;
    
    LOG_INFO("{} 손절선 본전 이동: {:.0f}", market, pos.entry_price);
}

Position* RiskManager::getPosition(const std::string& market) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return nullptr;
    
    return &it->second;
}

std::vector<Position> RiskManager::getAllPositions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Position> positions;
    for (const auto& [market, pos] : positions_) {
        positions.push_back(pos);
    }
    
    return positions;
}

// ===== 손절 계산 =====

double RiskManager::calculateDynamicStopLoss(
    double entry_price,
    const std::vector<analytics::Candle>& candles
) {
    if (candles.size() < 14) {
        return entry_price * 0.985; // Fallback: -1.5%
    }
    
    // 1. Hard Stop (긴급 손절)
    double hard_stop = entry_price * 0.985; // -1.5%
    
    // 2. ATR-based Stop
    double atr_stop = calculateATRStopLoss(entry_price, candles, 2.0);
    
    // 3. Support-based Stop
    double support_stop = calculateSupportStopLoss(entry_price, candles);
    
    // 가장 가까운 (높은) 손절가 선택
    double final_stop = std::max({hard_stop, atr_stop, support_stop});
    
    // 진입가보다 높으면 안됨
    if (final_stop >= entry_price) {
        final_stop = entry_price * 0.985;
    }
    
    LOG_INFO("손절가 계산: Hard={:.0f}, ATR={:.0f}, Support={:.0f}, Final={:.0f}",
              hard_stop, atr_stop, support_stop, final_stop);
    
    return final_stop;
}

double RiskManager::calculateATRStopLoss(
    double entry_price,
    const std::vector<analytics::Candle>& candles,
    double multiplier
) {
    double atr = analytics::TechnicalIndicators::calculateATR(candles, 14);
    
    if (atr < 0.0001) {
        return entry_price * 0.985;
    }
    
    // ATR 기반 변동성 비율
    double atr_percent = (atr / entry_price) * 100;
    
    // 변동성에 따라 multiplier 조정
    if (atr_percent < 1.0) {
        multiplier = 1.5; // 낮은 변동성
    } else if (atr_percent < 3.0) {
        multiplier = 2.0; // 중간 변동성
    } else {
        multiplier = 2.5; // 높은 변동성
    }
    
    double stop_loss = entry_price - (atr * multiplier);
    
    // 최소 -1.5%, 최대 -3.0%
    double min_stop = entry_price * 0.985;
    double max_stop = entry_price * 0.970;
    
    return std::clamp(stop_loss, max_stop, min_stop);
}

double RiskManager::calculateSupportStopLoss(
    double entry_price,
    const std::vector<analytics::Candle>& candles
) {
    auto support_levels = analytics::TechnicalIndicators::findSupportLevels(candles, 20);
    
    if (support_levels.empty()) {
        return entry_price * 0.985;
    }
    
    // 진입가보다 낮은 가장 가까운 지지선 찾기
    double nearest_support = 0;
    for (auto level : support_levels) {
        if (level < entry_price && level > nearest_support) {
            nearest_support = level;
        }
    }
    
    if (nearest_support > 0) {
        // 지지선 아래 0.2%
        return nearest_support * 0.998;
    }
    
    return entry_price * 0.985;
}

// ===== 포지션 사이징 =====

double RiskManager::calculateKellyPositionSize(
    double capital,
    double win_rate,
    double avg_win,
    double avg_loss
) {
    if (avg_loss < 0.0001) return 0.05; // Fallback
    
    // Kelly Criterion: f = (p * b - q) / b
    // p = 승률, q = 패율, b = 평균수익/평균손실
    
    double b = avg_win / avg_loss;
    double p = win_rate;
    double q = 1.0 - win_rate;
    
    double kelly = (p * b - q) / b;
    
    // Kelly의 25% 사용 (안전하게)
    double position_ratio = kelly * 0.25;
    
    // 최소 1%, 최대 10%
    return std::clamp(position_ratio, 0.01, 0.10);
}

double RiskManager::calculateFeeAwarePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    double take_profit,
    double fee_rate
) {
    // Risk/Reward 계산 (수수료 반영)
    double risk = std::abs(entry_price - stop_loss) / entry_price;
    double reward = std::abs(take_profit - entry_price) / entry_price;
    
    // 실제 Risk/Reward (수수료 포함)
    double actual_risk = risk + fee_rate;
    double actual_reward = reward - fee_rate;
    
    if (actual_reward <= 0 || actual_risk <= 0) {
        return 0.0; // 수익 불가능
    }
    
    double rr_ratio = actual_reward / actual_risk;
    
    // RR 비율에 따라 포지션 조정
    // RR >= 2.0: 5%
    // RR >= 1.5: 3%
    // RR < 1.5: 진입 안함
    
    if (rr_ratio >= 2.0) {
        return 0.05;
    } else if (rr_ratio >= 1.5) {
        return 0.03;
    } else {
        return 0.0;
    }
}

// ===== 리스크 관리 =====

bool RiskManager::canTradeMarket(const std::string& market) {
    auto it = last_trade_time_.find(market);
    if (it == last_trade_time_.end()) {
        return true; // 첫 거래
    }
    
    long long elapsed = getCurrentTimestamp() - it->second;
    return elapsed >= min_reentry_interval_ * 1000; // ms 단위
}

bool RiskManager::hasReachedDailyTradeLimit() {
    resetDailyCountIfNeeded();
    return daily_trade_count_ >= max_daily_trades_;
}

bool RiskManager::isDrawdownExceeded() {
    auto metrics = getRiskMetrics();
    return metrics.current_drawdown >= max_drawdown_pct_;
}

bool RiskManager::hasReachedMaxPositions() {
    return positions_.size() >= static_cast<size_t>(max_positions_);
}

// ===== 통계 및 모니터링 =====

RiskManager::RiskMetrics RiskManager::getRiskMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    RiskMetrics metrics;
    
    // 1. 미실현 손익 및 평가 자산 계산
    metrics.invested_capital = 0;
    metrics.unrealized_pnl = 0;
    
    for (const auto& [market, pos] : positions_) {
        metrics.invested_capital += pos.invested_amount;
        metrics.unrealized_pnl += pos.unrealized_pnl;
    }
    
    // 현재 총 자산 가치 (Equity)
    double total_equity = current_capital_ + metrics.unrealized_pnl;
    
    metrics.total_capital = total_equity;
    metrics.available_capital = current_capital_ - metrics.invested_capital;
    metrics.realized_pnl = current_capital_ - initial_capital_;
    metrics.total_pnl = metrics.realized_pnl + metrics.unrealized_pnl;
    metrics.total_pnl_pct = (initial_capital_ > 0) ? (metrics.total_pnl / initial_capital_) : 0.0;
    
    // 2. MDD (Max Drawdown) 계산 - 조회만 수행 (max_capital_ 수정 없음)
    double peak = std::max(max_capital_, total_equity);
    
    if (peak > 0) {
        metrics.max_drawdown = (peak - total_equity) / peak;
    } else {
        metrics.max_drawdown = 0.0;
    }
    metrics.current_drawdown = metrics.max_drawdown;
    
    // 3. 통계 매핑
    metrics.total_trades = static_cast<int>(trade_history_.size());
    metrics.winning_trades = 0;
    metrics.losing_trades = 0;
    
    double total_profit_sum = 0;
    double total_loss_sum = 0;
    
    for (const auto& trade : trade_history_) {
        if (trade.profit_loss > 0) {
            metrics.winning_trades++;
            total_profit_sum += trade.profit_loss;
        } else {
            metrics.losing_trades++;
            total_loss_sum += std::abs(trade.profit_loss);
        }
    }
    
    if (metrics.total_trades > 0) {
        metrics.win_rate = static_cast<double>(metrics.winning_trades) / metrics.total_trades;
    }
    
    // Profit Factor
    if (total_loss_sum > 0) {
        metrics.profit_factor = total_profit_sum / total_loss_sum;
    } else if (total_profit_sum > 0) {
        metrics.profit_factor = 99.9;
    }

    // [복구됨] Sharpe Ratio
    if (trade_history_.size() >= 10) {
        std::vector<double> returns;
        returns.reserve(trade_history_.size());
        
        for (const auto& trade : trade_history_) {
            returns.push_back(trade.profit_loss_pct);
        }
        
        double mean_return = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        
        double variance = 0.0;
        for (double ret : returns) {
            variance += (ret - mean_return) * (ret - mean_return);
        }
        variance /= returns.size();
        double std_dev = std::sqrt(variance);
        
        if (std_dev > 0.0001) {
            metrics.sharpe_ratio = mean_return / std_dev;
        }
    }
    
    metrics.active_positions = static_cast<int>(positions_.size());
    metrics.max_positions = max_positions_;
    
    return metrics;
}

std::vector<TradeHistory> RiskManager::getTradeHistory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trade_history_;
}

// ===== 설정 =====

void RiskManager::setMaxPositions(int max_positions) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_positions_ = max_positions;
    LOG_INFO("최대 포지션 설정: {}", max_positions);
}

void RiskManager::setMaxDailyTrades(int max_trades) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_daily_trades_ = max_trades;
    LOG_INFO("일일 최대 거래 설정: {}", max_trades);
}

void RiskManager::setMaxDrawdown(double max_drawdown_pct) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_drawdown_pct_ = max_drawdown_pct;
    LOG_INFO("최대 Drawdown 설정: {:.1f}%", max_drawdown_pct * 100);
}

void RiskManager::setMinReentryInterval(int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_reentry_interval_ = seconds;
    LOG_INFO("재진입 대기 시간 설정: {}초", seconds);
}

// ===== Private 헬퍼 함수 =====

double RiskManager::calculateFee(double amount) const {
    return amount * 0.0005; // 0.05% (업비트 수수료)
}

void RiskManager::recordTrade(
    const Position& pos,
    double exit_price,
    const std::string& exit_reason
) {
    TradeHistory trade;
    trade.market = pos.market;
    trade.entry_price = pos.entry_price;
    trade.exit_price = exit_price;
    trade.quantity = pos.quantity;
    trade.entry_time = pos.entry_time;
    trade.exit_time = getCurrentTimestamp();
    trade.strategy_name = pos.strategy_name;
    trade.exit_reason = exit_reason;
    
    // 손익 계산
    double exit_value = exit_price * pos.quantity;
    double entry_fee = calculateFee(pos.invested_amount);
    double exit_fee = calculateFee(exit_value);
    
    trade.profit_loss = exit_value - pos.invested_amount - entry_fee - exit_fee;
    trade.profit_loss_pct = (exit_price - pos.entry_price) / pos.entry_price;
    trade.fee_paid = entry_fee + exit_fee;
    
    trade_history_.push_back(trade);
}

long long RiskManager::getCurrentTimestamp() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

void RiskManager::resetDailyCountIfNeeded() {
    // 한국 표준시(KST) 기준 오전 9시 리셋
    // UTC 기준 날짜가 바뀌는 시점이 한국 시간 오전 9시입니다.
    
    time_t now = time(nullptr);
    struct tm* tm_now = gmtime(&now); // UTC 기준 구조체
    
    // 오늘 날짜를 정수로 변환 (YYYYMMDD)
    long long current_day = (tm_now->tm_year + 1900) * 10000 + (tm_now->tm_mon + 1) * 100 + tm_now->tm_mday;
    
    if (daily_reset_time_ == 0) {
        daily_reset_time_ = current_day;
    }
    
    // 저장된 날짜와 다르면 (즉, 하루가 지났으면) 리셋
    if (current_day != daily_reset_time_) {
        LOG_INFO("📅 날짜 변경 (UTC 00:00 / KST 09:00) -> 일일 거래량 초기화");
        daily_trade_count_ = 0;
        daily_reset_time_ = current_day;
    }
}

void RiskManager::updateCapital(double amount_change) {
    current_capital_ += amount_change;
    
    // 최고 자본금 갱신 (MDD 계산용)
    if (current_capital_ > max_capital_) {
        max_capital_ = current_capital_;
    }
}

} // namespace risk
} // namespace autolife
