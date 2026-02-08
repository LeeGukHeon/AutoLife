#include "strategy/GridTradingStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>
#include <spdlog/spdlog.h>

namespace autolife {
namespace strategy {

// ===== Constructor =====

GridTradingStrategy::GridTradingStrategy(std::shared_ptr<network::UpbitHttpClient> client)
    : client_(client)
    , enabled_(true)
    , last_signal_time_(0)
    , last_orderbook_fetch_time_(0)
    , daily_trades_count_(0)
    , hourly_trades_count_(0)
    , current_day_start_(0)
    , current_hour_start_(0)
    , consecutive_losses_(0)
    , circuit_breaker_active_(false)
    , circuit_breaker_until_(0)
{
    stats_.total_signals = 0;
    stats_.winning_trades = 0;
    stats_.losing_trades = 0;
    stats_.total_profit = 0.0;
    stats_.total_loss = 0.0;
    stats_.win_rate = 0.0;
    stats_.avg_profit = 0.0;
    stats_.avg_loss = 0.0;
    stats_.profit_factor = 0.0;
    stats_.sharpe_ratio = 0.0;
    
    rolling_stats_.rolling_win_rate = 0.0;
    rolling_stats_.avg_profit_per_cycle = 0.0;
    rolling_stats_.avg_cycle_time_minutes = 0.0;
    rolling_stats_.rolling_profit_factor = 0.0;
    rolling_stats_.grid_efficiency = 0.0;
    rolling_stats_.avg_daily_return = 0.0;
    rolling_stats_.total_grids_created = 0;
    rolling_stats_.successful_grids = 0;
    rolling_stats_.failed_grids = 0;
    rolling_stats_.emergency_exits = 0;
    rolling_stats_.avg_range_accuracy = 0.0;
    rolling_stats_.sharpe_ratio = 0.0;
    
    spdlog::info("[GridTradingStrategy] Initialized - Adaptive Grid with ADX Range Detection");
}

// ===== Strategy Info =====

StrategyInfo GridTradingStrategy::getInfo() const
{
    StrategyInfo info;
    info.name = "Grid Trading Strategy";
    info.description = "Adaptive grid trading with ADX range detection, dynamic rebalancing, and flash crash protection";
    info.risk_level = 5.5;
    info.timeframe = "1h";
    info.min_capital = 200000.0;
    info.expected_winrate = 0.72;
    return info;
}

// ===== Main Signal Generation =====

Signal GridTradingStrategy::generateSignal(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    Signal signal;
    signal.type = SignalType::NONE;
    signal.market = market;
    signal.strategy_name = "Grid Trading";
    
    // [수정] 중복 진입 방지: 이미 진입한 종목이면 신호 생성 안 함
    if (active_positions_.find(market) != active_positions_.end()) {
        return signal;
    }
    
    // ===== 1. 기본 검증 =====
    if (candles.size() < 100) return signal;
    if (metrics.liquidity_score < MIN_LIQUIDITY_SCORE) return signal;
    
    // ===== 2. 이미 그리드 활성화 중인지 확인 (이중 체크) =====
    // active_positions_와 active_grids_가 동기화되어야 함
    if (active_grids_.find(market) != active_grids_.end()) {
        return signal;  // 이미 그리드 운영 중
    }
    
    // ===== 3. 거래 가능 확인 =====
    if (!canTradeNow()) return signal;
    
    // ===== 4. 서킷 브레이커 =====
    checkCircuitBreaker();
    if (isCircuitBreakerActive()) return signal;
    
    // ===== 5. 신호 간격 =====
    long long now = getCurrentTimestamp();
    if ((now - last_signal_time_) < MIN_SIGNAL_INTERVAL_SEC * 1000) {
        return signal;
    }
    
    // ===== 6. Grid Opportunity 분석 =====
    GridSignalMetrics grid_signal = analyzeGridOpportunity(market, metrics, candles, current_price);
    
    if (!shouldGenerateGridSignal(grid_signal)) {
        return signal;
    }
    
    // ===== 7. 매수 신호 생성 =====
    signal.type = SignalType::BUY;
    signal.strength = grid_signal.strength;
    signal.entry_price = current_price;
    
    // ===== 8. 손절/익절 계산 (그리드 전체 기준) =====
    signal.stop_loss = calculateStopLoss(current_price, candles);
    signal.take_profit = calculateTakeProfit(current_price, candles); // Range High
    
    // ===== 9. 포지션 사이징 =====
    double capital = engine_config_.initial_capital; // 엔진에서 할당
    signal.position_size = calculatePositionSize(capital, current_price, signal.stop_loss, metrics);
    
    // ===== 10. 최소 주문 금액 확인 =====
    double order_amount = capital * signal.position_size;
    if (order_amount < MIN_CAPITAL_PER_GRID * grid_signal.optimal_grid_count) {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    // ===== 11. 로깅 =====
    spdlog::info("[GridTrading] GRID Signal - {} | Strength: {:.3f} | Type: {} | Grids: {} | Spacing: {:.2f}%",
                 market, signal.strength, 
                 static_cast<int>(grid_signal.recommended_type),
                 grid_signal.optimal_grid_count,
                 grid_signal.optimal_spacing_pct * 100);
    
    last_signal_time_ = now;
    
    return signal;
}

// ===== Entry Decision =====

bool GridTradingStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price)
{
    // generateSignal 호출 (내부 락 있음)
    Signal signal = generateSignal(market, metrics, candles, current_price);
    
    if (signal.type == SignalType::BUY && signal.strength >= MIN_SIGNAL_STRENGTH) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        
        // [수정] 1. 중복 방지 목록 등록
        active_positions_.insert(market);
        
        // Range Detection (재호출 비용 아까우면 generateSignal에서 리턴받아야 하나 구조상 재호출)
        RangeDetectionMetrics range = detectRange(candles, current_price);
        
        // Grid Configuration 생성
        double available_capital = 1000000.0 * MAX_GRID_CAPITAL_PCT;
        GridConfiguration config = createGridConfiguration(market, range, metrics, current_price, available_capital);
        
        // Grid Levels 생성
        auto levels = generateGridLevels(config);
        
        // [수정] 2. 그리드 데이터 초기화 (active_grids_ 사용)
        GridPositionData grid;
        grid.market = market;
        grid.config = config;
        grid.status = GridStatus::ACTIVE;
        grid.levels = levels;
        grid.creation_timestamp = getCurrentTimestamp();
        grid.last_price = current_price;
        grid.last_price_update_timestamp = grid.creation_timestamp;
        
        active_grids_[market] = grid;
        
        recordTrade();
        rolling_stats_.total_grids_created++;
        
        spdlog::info("[GridTrading] Grid Created - {} | Center: {:.2f} | Range: {:.2f}-{:.2f} | Grids: {}",
                     market, config.center_price, config.lower_bound, config.upper_bound, config.num_grids);
        
        return true;
    }
    
    return false;
}

// ===== Exit Decision =====

bool GridTradingStrategy::shouldExit(
    const std::string& market,
    double entry_price,
    double current_price,
    double holding_time_seconds)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (active_grids_.find(market) == active_grids_.end()) {
        return false;
    }
    
    auto& grid = active_grids_[market];
    double holding_hours = holding_time_seconds / 3600.0;
    
    // ===== 1. Emergency Exit 체크 (급락, 급등) =====
    if (shouldEmergencyExit(market, current_price)) {
        // shouldEmergencyExit 내부에서 emergencyLiquidateGrid -> exitGrid 호출됨.
        // exitGrid는 updateStatistics를 호출하여 정리함.
        // 여기서 true를 반환하면 엔진도 매도를 시도하므로 중복 처리될 수 있음.
        // 하지만 안전을 위해 true 반환 (엔진은 잔고가 없으면 실패 처리될 것임)
        return true; 
    }
    
    // ===== 2. Stop Loss 체크 (전체 손익 기준) =====
    if (checkStopLoss(grid, current_price)) {
        spdlog::warn("[GridTrading] Stop Loss Hit - {} | Drawdown: {:.2f}%",
                     market, grid.current_drawdown * 100);
        // exitGrid 호출 (통계 업데이트 및 목록 제거)
        exitGrid(market, ExitReason::STOP_LOSS, current_price);
        return true;
    }
    
    // ===== 3. Max Holding Time =====
    if (holding_hours >= MAX_HOLDING_TIME_HOURS) {
        spdlog::info("[GridTrading] Max Holding Time - {} | Hours: {:.1f}", market, holding_hours);
        exitGrid(market, ExitReason::MAX_TIME, current_price);
        return true;
    }
    
    // ===== 4. Rebalancing / Breakout 체크 =====
    if (shouldRebalanceGrid(market, current_price)) {
        std::vector<analytics::Candle> candles = getCachedCandles(market, 100);
        RangeDetectionMetrics new_range = detectRange(candles, current_price);
        
        if (new_range.is_ranging) {
            // 횡보장이면 그리드 재설정 (청산 아님)
            rebalanceGrid(market, current_price, new_range);
            return false; // 그리드 유지
        } else {
            // 추세장이면 그리드 종료 (청산)
            spdlog::info("[GridTrading] Range Lost (Trend) - {} | Exiting", market);
            exitGrid(market, ExitReason::BREAKOUT, current_price);
            return true;
        }
    }
    
    return false;
}

// ===== Calculate Stop Loss =====

double GridTradingStrategy::calculateStopLoss(
    double entry_price,
    const std::vector<analytics::Candle>& candles)
{
    //std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    double atr = calculateATR(candles, 14);
    double atr_pct = atr / entry_price;
    
    double stop_pct = std::max(GRID_STOP_LOSS_PCT, atr_pct * 2.0);
    stop_pct = std::min(stop_pct, 0.15);  // 최대 15%
    
    return entry_price * (1.0 - stop_pct);
}

// ===== Calculate Take Profit =====

double GridTradingStrategy::calculateTakeProfit(
    double entry_price,
    const std::vector<analytics::Candle>& candles)
{
    //std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Grid는 Take Profit이 명확하지 않음 (계속 순환)
    // Upper Bound를 반환
    RangeDetectionMetrics range = detectRange(candles, entry_price);
    return range.range_high;
}

// ===== Calculate Position Size =====

double GridTradingStrategy::calculatePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    const analytics::CoinMetrics& metrics)
{
    //std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Grid는 자본의 30%까지만 할당
    double position_size = MAX_GRID_CAPITAL_PCT;
    
    // 유동성에 따라 조정
    double liquidity_factor = std::min(metrics.liquidity_score / 100.0, 1.0);
    position_size *= liquidity_factor;
    
    return std::clamp(position_size, 0.10, MAX_GRID_CAPITAL_PCT);
}

// ===== Enabled =====

void GridTradingStrategy::setEnabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    enabled_ = enabled;
    spdlog::info("[GridTradingStrategy] Enabled: {}", enabled);
}

bool GridTradingStrategy::isEnabled() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return enabled_;
}

// ===== Statistics =====

IStrategy::Statistics GridTradingStrategy::getStatistics() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return stats_;
}

// [수정] market 인자 추가 및 포지션 목록 삭제 로직
void GridTradingStrategy::updateStatistics(const std::string& market, bool is_win, double profit_loss)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // [중요] 포지션 목록에서 제거 (재진입 허용)
    if (active_positions_.erase(market)) {
        // active_grids_는 exitGrid 등에서 이미 처리되었을 수 있음
        if (active_grids_.count(market)) {
            active_grids_.erase(market);
        }
        spdlog::info("[GridTrading] Position Cleared - {} (Ready for next trade)", market);
    } else {
        active_grids_.erase(market); // 혹시 모르니
    }
    
    // --- 통계 업데이트 ---
    stats_.total_signals++;
    
    if (is_win) {
        stats_.winning_trades++;
        stats_.total_profit += profit_loss;
        consecutive_losses_ = 0;
        rolling_stats_.successful_grids++; // 그리드 성공 횟수 증가
    } else {
        stats_.losing_trades++;
        stats_.total_loss += std::abs(profit_loss);
        consecutive_losses_++;
        rolling_stats_.failed_grids++;
    }
    
    if (stats_.total_signals > 0) {
        stats_.win_rate = static_cast<double>(stats_.winning_trades) / stats_.total_signals;
    }
    
    if (stats_.winning_trades > 0) {
        stats_.avg_profit = stats_.total_profit / stats_.winning_trades;
    }
    
    if (stats_.losing_trades > 0) {
        stats_.avg_loss = stats_.total_loss / stats_.losing_trades;
    }
    
    if (stats_.total_loss > 0) {
        stats_.profit_factor = stats_.total_profit / stats_.total_loss;
    }
    
    recent_returns_.push_back(profit_loss);
    if (recent_returns_.size() > 1000) {
        recent_returns_.pop_front();
    }
    
    trade_timestamps_.push_back(getCurrentTimestamp());
    if (trade_timestamps_.size() > 1000) {
        trade_timestamps_.pop_front();
    }
    
    updateRollingStatistics();
    checkCircuitBreaker();
}

// ===== Grid Specific Functions =====

bool GridTradingStrategy::shouldRebalanceGrid(
    const std::string& market,
    double current_price)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (active_grids_.find(market) == active_grids_.end()) {
        return false;
    }
    
    return needsRebalancing(active_grids_[market], current_price);
}

void GridTradingStrategy::updateGridLevels(
    const std::string& market,
    double current_price)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (active_grids_.find(market) == active_grids_.end()) {
        return;
    }
    
    auto& grid = active_grids_[market];
    grid.last_price = current_price;
    grid.last_price_update_timestamp = getCurrentTimestamp();
    
    // 각 레벨 체크
    for (auto& [level_id, level] : grid.levels) {
        // Buy 주문 체크
        if (shouldPlaceBuyOrder(level, current_price) && !level.buy_order_placed) {
            executeGridBuy(market, level, current_price);
        }
        
        // Sell 주문 체크
        if (shouldPlaceSellOrder(level, current_price) && !level.sell_order_placed) {
            executeGridSell(market, level, current_price);
        }
    }
    
    // P&L 업데이트
    grid.unrealized_pnl = calculateGridUnrealizedPnL(grid, current_price);
    grid.current_drawdown = (grid.unrealized_pnl < 0) ? 
        std::abs(grid.unrealized_pnl / grid.total_invested) : 0.0;
    
    if (grid.current_drawdown > grid.max_drawdown) {
        grid.max_drawdown = grid.current_drawdown;
    }
}

bool GridTradingStrategy::shouldEmergencyExit(
    const std::string& market,
    double current_price)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (active_grids_.find(market) == active_grids_.end()) {
        return false;
    }
    
    auto& grid = active_grids_[market];
    
    // Max Drawdown 초과
    if (isMaxDrawdownExceeded(grid)) {
        spdlog::error("[GridTrading] Max Drawdown Exceeded - {} | {:.2f}%",
                     market, grid.current_drawdown * 100);
        emergencyLiquidateGrid(market, ExitReason::STOP_LOSS);
        return true;
    }
    
    // Flash Crash 감지
    std::vector<analytics::Candle> candles = getCachedCandles(market, 20);
    FlashCrashMetrics flash = detectFlashCrash(market, candles, current_price);
    
    if (flash.detected) {
        spdlog::error("[GridTrading] Flash Crash Detected - {} | Drop: {:.2f}% | Speed: {:.2f}%/min",
                     market, flash.price_drop_pct * 100, flash.drop_speed);
        emergencyLiquidateGrid(market, ExitReason::FLASH_CRASH);
        return true;
    }
    
    // Breakout 감지
    RangeDetectionMetrics range = detectRange(candles, current_price);
    if (detectBreakout(grid, range, current_price)) {
        spdlog::warn("[GridTrading] Breakout Detected - {} | State: {}",
                    market, static_cast<int>(range.state));
        emergencyLiquidateGrid(market, ExitReason::BREAKOUT);
        return true;
    }
    
    return false;
}

void GridTradingStrategy::emergencyLiquidateGrid(
    const std::string& market,
    ExitReason reason)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    if (active_grids_.find(market) == active_grids_.end()) {
        return;
    }
    
    auto& grid = active_grids_[market];
    grid.status = GridStatus::EMERGENCY_EXIT;
    
    liquidateAllLevels(grid, grid.last_price);
    
    rolling_stats_.emergency_exits++;
    
    exitGrid(market, reason, grid.last_price);
}

GridRollingStatistics GridTradingStrategy::getRollingStatistics() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return rolling_stats_;
}

// ===== Rolling Statistics =====

void GridTradingStrategy::updateRollingStatistics()
{
    rolling_stats_.rolling_win_rate = stats_.win_rate;
    rolling_stats_.rolling_profit_factor = stats_.profit_factor;
    
    if (!cycle_times_.empty()) {
        double sum = std::accumulate(cycle_times_.begin(), cycle_times_.end(), 0.0);
        rolling_stats_.avg_cycle_time_minutes = (sum / cycle_times_.size()) / 60.0;
    }
    
    if (rolling_stats_.total_grids_created > 0) {
        double success_rate = static_cast<double>(rolling_stats_.successful_grids) / 
                             rolling_stats_.total_grids_created;
        rolling_stats_.avg_range_accuracy = success_rate;
    }
    
    // Grid Efficiency 계산
    double total_efficiency = 0.0;
    int active_count = 0;
    for (const auto& [market, grid] : active_grids_) {
        total_efficiency += calculateGridEfficiency(grid);
        active_count++;
    }
    if (active_count > 0) {
        rolling_stats_.grid_efficiency = total_efficiency / active_count;
    }
    
    // Sharpe Ratio 계산
    if (recent_returns_.size() >= 30) {
        std::vector<double> returns_vec(recent_returns_.begin(), recent_returns_.end());
        double mean = calculateMean(returns_vec);
        double std_dev = calculateStdDev(returns_vec, mean);
        
        if (std_dev > 0) {
            double annual_mean = mean * 8760;  // 시간 단위
            double annual_std = std_dev * std::sqrt(8760);
            rolling_stats_.sharpe_ratio = annual_mean / annual_std;
            stats_.sharpe_ratio = rolling_stats_.sharpe_ratio;
        }
    }
}

// ===== API Call Management =====

bool GridTradingStrategy::canMakeOrderBookAPICall() const
{
    long long now = getCurrentTimestamp();
    
    int recent_calls = 0;
    for (auto it = api_call_timestamps_.rbegin(); it != api_call_timestamps_.rend(); ++it) {
        if (now - *it < 1000) {
            recent_calls++;
        } else {
            break;
        }
    }
    
    return recent_calls < MAX_ORDERBOOK_CALLS_PER_SEC;
}

bool GridTradingStrategy::canMakeCandleAPICall() const
{
    long long now = getCurrentTimestamp();
    
    int recent_calls = 0;
    for (auto it = api_call_timestamps_.rbegin(); it != api_call_timestamps_.rend(); ++it) {
        if (now - *it < 1000) {
            recent_calls++;
        } else {
            break;
        }
    }
    
    return recent_calls < MAX_CANDLE_CALLS_PER_SEC;
}

void GridTradingStrategy::recordAPICall() const
{
    long long now = getCurrentTimestamp();
    api_call_timestamps_.push_back(now);
    
    while (!api_call_timestamps_.empty() && (now - api_call_timestamps_.front()) > 5000) {
        api_call_timestamps_.pop_front();
    }
}

nlohmann::json GridTradingStrategy::getCachedOrderBook(const std::string& market)
{
    long long now = getCurrentTimestamp();
    
    if (!cached_orderbook_.empty() && (now - last_orderbook_fetch_time_) < ORDERBOOK_CACHE_MS) {
        return cached_orderbook_;
    }
    
    if (!canMakeOrderBookAPICall()) {
        return cached_orderbook_;
    }
    
    try {
        cached_orderbook_ = client_->getOrderBook(market);
        last_orderbook_fetch_time_ = now;
        recordAPICall();
    } catch (const std::exception& e) {
        spdlog::warn("[GridTrading] Failed to fetch orderbook: {}", e.what());
    }
    
    return cached_orderbook_;
}

std::vector<analytics::Candle> GridTradingStrategy::getCachedCandles(
    const std::string& market,
    int count)
{
    long long now = getCurrentTimestamp();
    
    if (candle_cache_.count(market) && 
        (now - candle_cache_time_[market]) < CANDLE_CACHE_MS) {
        return candle_cache_[market];
    }
    
    if (!canMakeCandleAPICall()) {
        if (candle_cache_.count(market)) {
            return candle_cache_[market];
        }
        return std::vector<analytics::Candle>();
    }
    
    try {
        nlohmann::json json_data = client_->getCandles(market, "1", count);
        auto candles = parseCandlesFromJson(json_data);
        
        candle_cache_[market] = candles;
        candle_cache_time_[market] = now;
        recordAPICall();
        return candles;
    } catch (const std::exception& e) {
        spdlog::warn("[GridTrading] Failed to fetch candles: {}", e.what());
        if (candle_cache_.count(market)) {
            return candle_cache_[market];
        }
        return std::vector<analytics::Candle>();
    }
}

// ===== Trade Frequency Management =====

bool GridTradingStrategy::canTradeNow()
{
    resetDailyCounters();
    resetHourlyCounters();
    
    if (daily_trades_count_ >= MAX_DAILY_GRID_TRADES) {
        return false;
    }
    
    if (hourly_trades_count_ >= MAX_HOURLY_GRID_TRADES) {
        return false;
    }
    
    return true;
}

void GridTradingStrategy::recordTrade()
{
    daily_trades_count_++;
    hourly_trades_count_++;
    
    long long now = getCurrentTimestamp();
    trade_timestamps_.push_back(now);
    
    if (trade_timestamps_.size() > 100) {
        trade_timestamps_.pop_front();
    }
}

void GridTradingStrategy::resetDailyCounters()
{
    long long now = getCurrentTimestamp();
    long long day_ms = 24 * 60 * 60 * 1000;
    
    if (current_day_start_ == 0 || (now - current_day_start_) >= day_ms) {
        daily_trades_count_ = 0;
        current_day_start_ = now;
    }
}

void GridTradingStrategy::resetHourlyCounters()
{
    long long now = getCurrentTimestamp();
    long long hour_ms = 60 * 60 * 1000;
    
    if (current_hour_start_ == 0 || (now - current_hour_start_) >= hour_ms) {
        hourly_trades_count_ = 0;
        current_hour_start_ = now;
    }
}

// ===== Circuit Breaker =====

void GridTradingStrategy::checkCircuitBreaker()
{
    long long now = getCurrentTimestamp();
    
    if (circuit_breaker_active_ && now >= circuit_breaker_until_) {
        circuit_breaker_active_ = false;
        spdlog::info("[GridTrading] Circuit breaker deactivated");
    }
    
    if (consecutive_losses_ >= MAX_CONSECUTIVE_LOSSES && !circuit_breaker_active_) {
        activateCircuitBreaker();
    }
}

void GridTradingStrategy::activateCircuitBreaker()
{
    circuit_breaker_active_ = true;
    circuit_breaker_until_ = getCurrentTimestamp() + CIRCUIT_BREAKER_COOLDOWN_MS;
    
    spdlog::warn("[GridTrading] Circuit breaker activated - {} consecutive losses", 
                 consecutive_losses_);
}

bool GridTradingStrategy::isCircuitBreakerActive() const
{
    return circuit_breaker_active_;
}

// ===== Range Detection =====

RangeDetectionMetrics GridTradingStrategy::detectRange(
    const std::vector<analytics::Candle>& candles,
    double current_price) const
{
    RangeDetectionMetrics metrics;
    
    if (candles.size() < 50) {
        return metrics;
    }
    
    // 1. Donchian Channel (Range 경계)
    int lookback = std::min(static_cast<int>(candles.size()), 50);
    size_t start_idx = candles.size() - lookback;
    
    // [수정] 최근 데이터(맨 뒤) 기준으로 High/Low 탐색
    metrics.range_high = candles.back().high;
    metrics.range_low = candles.back().low;
    
    for (size_t i = start_idx; i < candles.size(); ++i) {
        metrics.range_high = std::max(metrics.range_high, candles[i].high);
        metrics.range_low = std::min(metrics.range_low, candles[i].low);
    }
    
    metrics.range_center = (metrics.range_high + metrics.range_low) / 2.0;
    metrics.range_width_pct = (metrics.range_high - metrics.range_low) / metrics.range_center;
    
    // 2. ADX 계산
    metrics.adx = calculateADX(candles, 14);
    calculateDMI(candles, 14, metrics.plus_di, metrics.minus_di);
    
    // 3. Bollinger Bands Width
    std::vector<double> prices = extractPrices(candles, "close");
    if (prices.size() >= 20) {
        std::vector<double> recent_20(prices.end() - 20, prices.end());
        double mean = calculateMean(recent_20);
        double std_dev = calculateStdDev(recent_20, mean);
        metrics.bb_width = (std_dev * 2 * 2) / mean;
    }
    
    // 4. ATR
    metrics.atr = calculateATR(candles, 14);
    
    // 5. Donchian Width
    metrics.donchian_width = metrics.range_width_pct;
    
    // 6. Consolidation 판정
    metrics.consolidation_bars = 0;
    double consolidation_threshold = metrics.range_width_pct * 0.3;
    
    for (int i = candles.size() - 1; i >= 0 && i >= static_cast<int>(candles.size()) - 50; i--) {
        double bar_range = (candles[i].high - candles[i].low) / candles[i].close;
        if (bar_range < consolidation_threshold) {
            metrics.consolidation_bars++;
        } else {
            break;
        }
    }
    
    // 7. Range State 판정
    if (metrics.adx < ADX_RANGING_THRESHOLD && 
        metrics.range_width_pct >= MIN_RANGE_WIDTH_PCT &&
        metrics.range_width_pct <= MAX_RANGE_WIDTH_PCT &&
        metrics.consolidation_bars >= MIN_CONSOLIDATION_BARS) {
        
        metrics.state = RangeState::RANGING;
        metrics.is_ranging = true;
        
    } else if (metrics.adx >= ADX_STRONG_TREND) {
        if (metrics.plus_di > metrics.minus_di) {
            metrics.state = RangeState::TRENDING_UP;
        } else {
            metrics.state = RangeState::TRENDING_DOWN;
        }
    } else {
        // Breakout 체크
        if (current_price > metrics.range_high * 1.02) {
            metrics.state = RangeState::BREAKOUT_UP;
        } else if (current_price < metrics.range_low * 0.98) {
            metrics.state = RangeState::BREAKOUT_DOWN;
        } else {
            metrics.state = RangeState::UNKNOWN;
        }
    }
    
    // 8. Confidence 계산
    metrics.confidence = 0.0;
    if (metrics.adx < ADX_RANGING_THRESHOLD) metrics.confidence += 0.3;
    if (metrics.consolidation_bars >= MIN_CONSOLIDATION_BARS) metrics.confidence += 0.3;
    if (metrics.range_width_pct >= MIN_RANGE_WIDTH_PCT && 
        metrics.range_width_pct <= MAX_RANGE_WIDTH_PCT) metrics.confidence += 0.2;
    if (metrics.bb_width < 0.1) metrics.confidence += 0.2;
    
    return metrics;
}

double GridTradingStrategy::calculateADX(
    const std::vector<analytics::Candle>& candles,
    int period) const
{
    if (candles.size() < static_cast<size_t>(period + 1)) {
        return 0.0;
    }
    
    std::vector<double> dx_values;
    
    for (size_t i = 1; i < candles.size(); i++) {
        double high_diff = candles[i].high - candles[i-1].high;
        double low_diff = candles[i-1].low - candles[i].low;
        
        double plus_dm = (high_diff > low_diff && high_diff > 0) ? high_diff : 0;
        double minus_dm = (low_diff > high_diff && low_diff > 0) ? low_diff : 0;
        
        double tr = std::max({
            candles[i].high - candles[i].low,
            std::abs(candles[i].high - candles[i-1].close),
            std::abs(candles[i].low - candles[i-1].close)
        });
        
        if (tr > 0) {
            double plus_di = 100.0 * plus_dm / tr;
            double minus_di = 100.0 * minus_dm / tr;
            
            double di_sum = plus_di + minus_di;
            if (di_sum > 0) {
                double dx = 100.0 * std::abs(plus_di - minus_di) / di_sum;
                dx_values.push_back(dx);
            }
        }
    }
    
    if (dx_values.size() < static_cast<size_t>(period)) {
        return 0.0;
    }
    
    // ADX = SMA of DX
    std::vector<double> recent_dx(dx_values.end() - period, dx_values.end());
    return calculateMean(recent_dx);
}

void GridTradingStrategy::calculateDMI(
    const std::vector<analytics::Candle>& candles,
    int period,
    double& plus_di,
    double& minus_di) const
{
    plus_di = 0.0;
    minus_di = 0.0;
    
    if (candles.size() < static_cast<size_t>(period + 1)) {
        return;
    }
    
    double sum_plus_dm = 0.0;
    double sum_minus_dm = 0.0;
    double sum_tr = 0.0;
    
    for (size_t i = candles.size() - period; i < candles.size(); i++) {
        double high_diff = candles[i].high - candles[i-1].high;
        double low_diff = candles[i-1].low - candles[i].low;
        
        sum_plus_dm += (high_diff > low_diff && high_diff > 0) ? high_diff : 0;
        sum_minus_dm += (low_diff > high_diff && low_diff > 0) ? low_diff : 0;
        
        sum_tr += std::max({
            candles[i].high - candles[i].low,
            std::abs(candles[i].high - candles[i-1].close),
            std::abs(candles[i].low - candles[i-1].close)
        });
    }
    
    if (sum_tr > 0) {
        plus_di = 100.0 * sum_plus_dm / sum_tr;
        minus_di = 100.0 * sum_minus_dm / sum_tr;
    }
}

bool GridTradingStrategy::isConsolidating(
    const std::vector<analytics::Candle>& candles,
    int lookback) const
{
    if (candles.size() < static_cast<size_t>(lookback)) {
        return false;
    }
    
    double high = candles[candles.size() - lookback].high;
    double low = candles[candles.size() - lookback].low;
    
    for (size_t i = candles.size() - lookback; i < candles.size(); i++) {
        high = std::max(high, candles[i].high);
        low = std::min(low, candles[i].low);
    }
    
    double range_pct = (high - low) / ((high + low) / 2.0);
    
    return range_pct < 0.1;  // 10% 이내
}

// ===== Grid Configuration =====

GridConfiguration GridTradingStrategy::createGridConfiguration(
    const std::string& market,
    const RangeDetectionMetrics& range,
    const analytics::CoinMetrics& metrics,
    double current_price,
    double available_capital)
{
    GridConfiguration config;
    
    // 1. Grid Type 선택
    double volatility = calculateVolatility(metrics.candles);
    config.type = selectGridType(range, volatility, current_price);
    
    // 2. Range 설정
    config.center_price = range.range_center;
    config.upper_bound = range.range_high;
    config.lower_bound = range.range_low;
    
    // 3. Optimal Grid Count 계산
    config.num_grids = calculateOptimalGridCount(range.range_width_pct, volatility, available_capital);
    
    // 4. Optimal Spacing 계산
    config.grid_spacing_pct = calculateOptimalSpacing(range.range_width_pct, volatility, config.num_grids);
    
    // 5. Capital Allocation
    config.total_capital_allocated = available_capital;
    config.capital_per_grid = available_capital / config.num_grids;
    
    // 최소 자본 확인
    if (config.capital_per_grid < MIN_CAPITAL_PER_GRID) {
        config.num_grids = static_cast<int>(available_capital / MIN_CAPITAL_PER_GRID);
        config.capital_per_grid = available_capital / config.num_grids;
    }
    
    // 6. Auto Rebalance 설정
    config.auto_rebalance = true;
    config.rebalance_threshold_pct = REBALANCE_THRESHOLD_PCT;
    
    // 7. Risk Limits 설정
    config.risk_limits.stop_loss_pct = GRID_STOP_LOSS_PCT;
    config.risk_limits.max_drawdown_pct = 0.15;
    config.risk_limits.flash_crash_threshold = FLASH_CRASH_THRESHOLD_PCT;
    config.risk_limits.breakout_tolerance_pct = BREAKOUT_EXIT_TOLERANCE;
    config.risk_limits.max_holding_time_ms = static_cast<long long>(MAX_HOLDING_TIME_HOURS * 3600000);
    config.risk_limits.auto_liquidate_on_breakout = true;
    
    return config;
}

int GridTradingStrategy::calculateOptimalGridCount(
    double range_width_pct,
    double volatility,
    double capital) const
{
    // 1. 자본 제약
    int max_by_capital = static_cast<int>(capital / MIN_CAPITAL_PER_GRID);
    
    // 2. Range Width 기반
    double atr_ratio = range_width_pct / (volatility * 2.0);
    int optimal_by_range = static_cast<int>(atr_ratio * 10);
    
    // 3. 수수료 고려
    // 최소 간격 = 수수료의 3배
    double min_spacing = (UPBIT_FEE_RATE * 2) * 3;
    int max_by_fee = static_cast<int>(range_width_pct / min_spacing);
    
    // 4. 최종 결정
    int optimal = std::min({max_by_capital, optimal_by_range, max_by_fee});
    optimal = std::clamp(optimal, MIN_GRID_COUNT, MAX_GRID_COUNT);
    
    return optimal;
}

double GridTradingStrategy::calculateOptimalSpacing(
    double range_width_pct,
    double volatility,
    int grid_count) const
{
    // 1. 균등 분할 기준
    double equal_spacing = range_width_pct / grid_count;
    
    // 2. 변동성 기반 조정
    double volatility_adjusted = volatility * 1.5;
    
    // 3. 최종 간격
    double spacing = std::max(equal_spacing, volatility_adjusted);
    
    // 4. 수수료 검증
    double min_profitable_spacing = (UPBIT_FEE_RATE * 2 + EXPECTED_SLIPPAGE) * 3;
    spacing = std::max(spacing, min_profitable_spacing);
    
    // 5. 범위 제한
    spacing = std::clamp(spacing, MIN_GRID_SPACING_PCT, MAX_GRID_SPACING_PCT);
    
    return spacing;
}

GridType GridTradingStrategy::selectGridType(
    const RangeDetectionMetrics& range,
    double volatility,
    double price_level) const
{
    // 1. 고가 코인 (10만원 이상) = Geometric
    if (price_level > 100000.0) {
        return GridType::GEOMETRIC;
    }
    
    // 2. 변동성 높음 = Dynamic
    if (volatility > 0.03) {
        return GridType::DYNAMIC;
    }
    
    // 3. Wide Range = Fibonacci
    if (range.range_width_pct > 0.10) {
        return GridType::FIBONACCI;
    }
    
    // 4. 기본 = Arithmetic
    return GridType::ARITHMETIC;
}

// ===== Grid Generation =====

std::map<int, GridLevel> GridTradingStrategy::generateGridLevels(
    const GridConfiguration& config)
{
    switch (config.type) {
        case GridType::ARITHMETIC:
            return generateArithmeticGrid(config.center_price, 
                                         config.grid_spacing_pct * config.center_price,
                                         config.num_grids);
        
        case GridType::GEOMETRIC:
            return generateGeometricGrid(config.center_price,
                                        config.grid_spacing_pct,
                                        config.num_grids);
        
        case GridType::FIBONACCI:
            return generateFibonacciGrid(config.center_price,
                                        config.upper_bound - config.lower_bound,
                                        config.num_grids);
        
        case GridType::DYNAMIC:
        default:
            return generateArithmeticGrid(config.center_price,
                                         config.grid_spacing_pct * config.center_price,
                                         config.num_grids);
    }
}

std::map<int, GridLevel> GridTradingStrategy::generateArithmeticGrid(
    double center,
    double spacing,
    int count)
{
    std::map<int, GridLevel> levels;
    
    int half_count = count / 2;
    
    // Center level
    GridLevel center_level;
    center_level.level_id = 0;
    center_level.price = center;
    levels[0] = center_level;
    
    // Upper levels (매도 레벨)
    for (int i = 1; i <= half_count; i++) {
        GridLevel level;
        level.level_id = i;
        level.price = center + (spacing * i);
        levels[i] = level;
    }
    
    // Lower levels (매수 레벨)
    for (int i = 1; i <= half_count; i++) {
        GridLevel level;
        level.level_id = -i;
        level.price = center - (spacing * i);
        levels[-i] = level;
    }
    
    return levels;
}

std::map<int, GridLevel> GridTradingStrategy::generateGeometricGrid(
    double center,
    double spacing_pct,
    int count)
{
    std::map<int, GridLevel> levels;
    
    int half_count = count / 2;
    
    // Center level
    GridLevel center_level;
    center_level.level_id = 0;
    center_level.price = center;
    levels[0] = center_level;
    
    // Upper levels (복리 증가)
    double price = center;
    for (int i = 1; i <= half_count; i++) {
        price = price * (1.0 + spacing_pct);
        GridLevel level;
        level.level_id = i;
        level.price = price;
        levels[i] = level;
    }
    
    // Lower levels (복리 감소)
    price = center;
    for (int i = 1; i <= half_count; i++) {
        price = price * (1.0 - spacing_pct);
        GridLevel level;
        level.level_id = -i;
        level.price = price;
        levels[-i] = level;
    }
    
    return levels;
}

std::map<int, GridLevel> GridTradingStrategy::generateFibonacciGrid(
    double center,
    double range_width,
    int count)
{
    std::map<int, GridLevel> levels;
    
    // Fibonacci 비율
    std::vector<double> fib_ratios = {0.0, 0.236, 0.382, 0.5, 0.618, 0.786, 1.0};
    
    // Center level
    GridLevel center_level;
    center_level.level_id = 0;
    center_level.price = center;
    levels[0] = center_level;
    
    int level_id = 1;
    
    // Upper levels
    for (double ratio : fib_ratios) {
        if (level_id > count / 2) break;
        
        GridLevel level;
        level.level_id = level_id;
        level.price = center + (range_width * 0.5 * ratio);
        levels[level_id] = level;
        level_id++;
    }
    
    level_id = -1;
    
    // Lower levels
    for (double ratio : fib_ratios) {
        if (std::abs(level_id) > count / 2) break;
        
        GridLevel level;
        level.level_id = level_id;
        level.price = center - (range_width * 0.5 * ratio);
        levels[level_id] = level;
        level_id--;
    }
    
    return levels;
}

std::map<int, GridLevel> GridTradingStrategy::generateDynamicGrid(
    const GridConfiguration& config,
    const std::vector<analytics::Candle>& candles)
{
    // Dynamic은 ATR 기반으로 간격 조정
    double atr = calculateATR(candles, 14);
    double dynamic_spacing = atr * 1.5;
    
    return generateArithmeticGrid(config.center_price, dynamic_spacing, config.num_grids);
}

// ===== Grid Signal Analysis =====

GridSignalMetrics GridTradingStrategy::analyzeGridOpportunity(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<analytics::Candle>& candles,
    double current_price)
{
    GridSignalMetrics signal;
    
    // 1. Range Detection
    RangeDetectionMetrics range = detectRange(candles, current_price);
    
    if (!range.is_ranging) {
        return signal;
    }
    
    // 2. Volatility 계산
    double volatility = calculateVolatility(candles);
    
    // 3. Optimal Grid Count
    signal.optimal_grid_count = calculateOptimalGridCount(
        range.range_width_pct,
        volatility,
        1000000.0 * MAX_GRID_CAPITAL_PCT
    );
    
    // 4. Optimal Spacing
    signal.optimal_spacing_pct = calculateOptimalSpacing(
        range.range_width_pct,
        volatility,
        signal.optimal_grid_count
    );
    
    // 5. Grid Type 추천
    signal.recommended_type = selectGridType(range, volatility, current_price);
    
    // 6. Expected Profit Per Cycle
    signal.expected_profit_per_cycle = calculateExpectedProfitPerCycle(
        signal.optimal_spacing_pct,
        UPBIT_FEE_RATE,
        EXPECTED_SLIPPAGE
    );
    
    // 7. Fee Validation
    signal.is_profitable_after_fees = validateProfitabilityAfterFees(signal.optimal_spacing_pct);
    
    if (!signal.is_profitable_after_fees) {
        return signal;
    }
    
    // 8. Expected Cycles Per Day
    if (volatility > 0) {
        signal.expected_cycles_per_day = (volatility * 24.0) / signal.optimal_spacing_pct;
        signal.expected_cycles_per_day = std::clamp(signal.expected_cycles_per_day, 0.5, 10.0);
    }
    
    // 9. Fee Adjusted Profit
    signal.fee_adjusted_profit = signal.expected_profit_per_cycle * signal.expected_cycles_per_day;
    
    // 10. Risk Score
    signal.risk_score = calculateGridRiskScore(range, volatility);
    
    // 11. Signal Strength
    signal.strength = 0.0;
    signal.strength += range.confidence * 0.4;
    signal.strength += (range.adx < ADX_RANGING_THRESHOLD ? 0.3 : 0.0);
    signal.strength += (signal.is_profitable_after_fees ? 0.2 : 0.0);
    signal.strength += (metrics.liquidity_score / 100.0) * 0.1;
    
    // 12. Validation
    signal.is_valid = (signal.strength >= MIN_SIGNAL_STRENGTH &&
                      signal.is_profitable_after_fees &&
                      range.is_ranging);
    
    return signal;
}

double GridTradingStrategy::calculateExpectedProfitPerCycle(
    double grid_spacing_pct,
    double fee_rate,
    double slippage) const
{
    // Buy → Sell 한 사이클
    double gross_profit = grid_spacing_pct;
    double total_fee = fee_rate * 2;  // Buy + Sell
    double total_slippage = slippage * 2;
    
    double net_profit = gross_profit - total_fee - total_slippage;
    
    return net_profit;
}

bool GridTradingStrategy::validateProfitabilityAfterFees(
    double grid_spacing_pct) const
{
    double total_cost = (UPBIT_FEE_RATE * 2) + (EXPECTED_SLIPPAGE * 2);
    double min_spacing = total_cost * 3;  // 최소 3배 마진
    
    return grid_spacing_pct >= min_spacing;
}

double GridTradingStrategy::calculateGridRiskScore(
    const RangeDetectionMetrics& range,
    double volatility) const
{
    double risk = 0.0;
    
    // 1. ADX (높을수록 위험)
    risk += (range.adx / 100.0) * 0.3;
    
    // 2. Range Width (너무 좁거나 넓으면 위험)
    double ideal_width = 0.08;  // 8%
    double width_deviation = std::abs(range.range_width_pct - ideal_width) / ideal_width;
    risk += std::min(width_deviation, 1.0) * 0.3;
    
    // 3. Volatility (높을수록 위험)
    risk += std::min(volatility / 0.05, 1.0) * 0.2;
    
    // 4. Consolidation (짧을수록 위험)
    if (range.consolidation_bars < MIN_CONSOLIDATION_BARS) {
        risk += 0.2;
    }
    
    return std::clamp(risk, 0.0, 1.0);
}

// ===== Grid Management =====

void GridTradingStrategy::executeGridBuy(
    const std::string& market,
    GridLevel& level,
    double current_price)
{
    if (level.buy_order_filled) {
        return;
    }
    
   // [✅ 추가] 실전 주문 전송 로직 시작 ==============================
    // 수량 계산 (혹은 level.quantity 사용)
    double quantity = level.quantity;
    if (quantity <= 0) quantity = (level.price * 0.1) / level.price; // 안전장치

    // 문자열 변환 (소수점 처리 주의 - 여기선 간단히 to_string 사용)
    std::string price_str = std::to_string((long long)level.price); // 원화는 정수
    std::string vol_str = std::to_string(quantity);

    try {
        // 실제 API 호출 (TradingEngine의 client_ 사용)
        // 주의: client_가 protected여야 함. private라면 getClient() 같은 게 필요하지만
        // 보통 상속 구조에선 protected로 둡니다.
        auto res = client_->placeOrder(market, "bid", vol_str, price_str, "limit");
        
        if (res.contains("uuid")) {
            spdlog::info("✅ [Grid] Real Buy Order: {} @ {}", market, level.price);
        } else {
            spdlog::error("❌ [Grid] Buy Order Fail: No UUID returned");
            return; // 주문 실패 시 내부 상태 업데이트 안 하고 리턴
        }
    } catch (const std::exception& e) {
        spdlog::error("❌ [Grid] Buy Order Error: {}", e.what());
        return; // 주문 실패 시 리턴
    }
    // ================================================================

    // [기존 코드 유지] 내부 상태 업데이트
    level.buy_order_placed = true;
    level.buy_order_filled = true; // (엄밀히는 체결 확인 후 바꿔야 하지만, 지정가는 거의 체결된다고 가정)
    level.buy_timestamp = getCurrentTimestamp();
    level.quantity = quantity; // 수량 확정
    
    spdlog::info("[GridTrading] Buy Executed - {} | Level: {} | Price: {:.2f}",
                 market, level.level_id, level.price);
}

void GridTradingStrategy::executeGridSell(
    const std::string& market,
    GridLevel& level,
    double current_price)
{
    if (!level.buy_order_filled || level.sell_order_filled) {
        return;
    }
    
    // [✅ 추가] 실전 주문 전송 로직 시작 ==============================
    double quantity = level.quantity; // 매수했던 수량 그대로 매도
    std::string price_str = std::to_string((long long)current_price); // 혹은 level.price + margin
    std::string vol_str = std::to_string(quantity);

    try {
        // 시장가 매도 ("market") 혹은 지정가 매도 ("limit")
        // 여기선 current_price에 즉시 팔기 위해 시장가 매도로 예시 (지정가로 하셔도 됨)
        // 시장가 매도시 price는 null("")이어야 함
        auto res = client_->placeOrder(market, "ask", vol_str, "", "market");
        
        if (res.contains("uuid")) {
            spdlog::info("✅ [Grid] Real Sell Order: {} @ Market", market);
        } else {
            spdlog::error("❌ [Grid] Sell Order Fail");
            return;
        }
    } catch (const std::exception& e) {
        spdlog::error("❌ [Grid] Sell Order Error: {}", e.what());
        return;
    }
    // ================================================================

    // [기존 코드 유지] 내부 상태 업데이트
    level.sell_order_placed = true;
    level.sell_order_filled = true;
    level.sell_timestamp = getCurrentTimestamp();
    // Profit 계산
    double buy_price = level.price;
    double sell_price = current_price;
    double profit_pct = (sell_price - buy_price) / buy_price;
    profit_pct -= (UPBIT_FEE_RATE * 2);  // 수수료 차감
    
    level.profit_loss = profit_pct;
    level.cumulative_profit += profit_pct;
    level.round_trips++;
    
    // Cycle Time 기록
    if (level.buy_timestamp > 0) {
        double cycle_time = static_cast<double>(level.sell_timestamp - level.buy_timestamp);
        cycle_times_.push_back(cycle_time);
        if (cycle_times_.size() > 100) {
            cycle_times_.pop_front();
        }
    }
    
    spdlog::info("[GridTrading] Sell Executed - {} | Level: {} | Profit: {:.3f}% | Trips: {}",
                market, level.level_id, profit_pct * 100, level.round_trips);
    
    // Reset for next cycle
    level.buy_order_filled = false;
    level.sell_order_filled = false;
    level.buy_order_placed = false;
    level.sell_order_placed = false;
}

bool GridTradingStrategy::shouldPlaceBuyOrder(
    const GridLevel& level,
    double current_price) const
{
    if (level.buy_order_placed || level.buy_order_filled) {
        return false;
    }
    
    // 현재가가 레벨 이하로 내려왔을 때
    return current_price <= level.price * 1.001;  // 0.1% 허용
}

bool GridTradingStrategy::shouldPlaceSellOrder(const GridLevel& level, double current_price) const {
    if (!level.buy_order_filled || level.sell_order_placed) return false;
    
    // [수정] 내가 산 가격보다 한 칸(level_spacing) 위에 도달하면 매도
    // 혹은 설정된 최소 수익(spacing)만큼 올랐을 때 매도
    double min_profit_price = level.price * (1.0 + MIN_GRID_SPACING_PCT);
    return current_price >= min_profit_price;
}

// ===== Risk Monitoring =====

bool GridTradingStrategy::checkStopLoss(
    const GridPositionData& grid,
    double current_price) const
{
    if (grid.current_drawdown >= grid.config.risk_limits.stop_loss_pct) {
        return true;
    }
    
    return false;
}

bool GridTradingStrategy::detectBreakout(
    const GridPositionData& grid,
    const RangeDetectionMetrics& range,
    double current_price) const
{
    // Upper Breakout
    if (current_price > grid.config.upper_bound * (1.0 + BREAKOUT_EXIT_TOLERANCE)) {
        return true;
    }
    
    // Lower Breakout
    if (current_price < grid.config.lower_bound * (1.0 - BREAKOUT_EXIT_TOLERANCE)) {
        return true;
    }
    
    // Range State 변화
    if (range.state == RangeState::BREAKOUT_UP || 
        range.state == RangeState::BREAKOUT_DOWN ||
        range.state == RangeState::TRENDING_UP ||
        range.state == RangeState::TRENDING_DOWN) {
        return true;
    }
    
    return false;
}

FlashCrashMetrics GridTradingStrategy::detectFlashCrash(
    const std::string& market,
    const std::vector<analytics::Candle>& candles,
    double current_price)
{
    FlashCrashMetrics flash;
    
    if (candles.size() < 5) {
        return flash;
    }
    
    // 최근 5분 하락폭 체크
    double price_5min_ago = candles[candles.size() - 5].close;
    flash.price_drop_pct = (price_5min_ago - current_price) / price_5min_ago;
    
    if (flash.price_drop_pct >= FLASH_CRASH_THRESHOLD_PCT) {
        flash.drop_speed = flash.price_drop_pct / 5.0;  // %/분
        
        if (flash.drop_speed >= FLASH_CRASH_SPEED) {
            flash.detected = true;
            flash.detection_time = getCurrentTimestamp();
            
            // 연속 하락 캔들 카운트
            for (int i = candles.size() - 1; i >= 1; i--) {
                if (candles[i].close < candles[i-1].close) {
                    flash.consecutive_drops++;
                } else {
                    break;
                }
            }
        }
    }
    
    return flash;
}

bool GridTradingStrategy::isMaxDrawdownExceeded(
    const GridPositionData& grid) const
{
    return grid.current_drawdown >= grid.config.risk_limits.max_drawdown_pct;
}

// ===== Grid Rebalancing =====

bool GridTradingStrategy::needsRebalancing(
    const GridPositionData& grid,
    double current_price) const
{
    // 1. Time Check
    long long now = getCurrentTimestamp();
    if ((now - grid.last_rebalance_timestamp) < MIN_REBALANCE_INTERVAL_MS) {
        return false;
    }
    
    // 2. Price Deviation Check
    double deviation = std::abs(current_price - grid.config.center_price) / grid.config.center_price;
    
    if (deviation >= grid.config.rebalance_threshold_pct) {
        return true;
    }
    
    return false;
}

void GridTradingStrategy::rebalanceGrid(
    const std::string& market,
    double current_price,
    const RangeDetectionMetrics& new_range)
{
    auto& grid = active_grids_[market];
    
    spdlog::info("[GridTrading] Rebalancing Grid - {} | Old Center: {:.2f} | New Center: {:.2f}",
                market, grid.config.center_price, new_range.range_center);
    
    grid.status = GridStatus::REBALANCING;
    
    // 기존 포지션 정리
    liquidateAllLevels(grid, current_price);
    
    // 새 Configuration 생성
    analytics::CoinMetrics metrics;  // 임시
    GridConfiguration new_config = createGridConfiguration(
        market, new_range, metrics, current_price, grid.config.total_capital_allocated
    );
    
    // 새 Grid Levels 생성
    auto new_levels = generateGridLevels(new_config);
    
    // 업데이트
    grid.config = new_config;
    grid.levels = new_levels;
    grid.status = GridStatus::ACTIVE;
    grid.last_rebalance_timestamp = getCurrentTimestamp();
    
    spdlog::info("[GridTrading] Rebalancing Complete - {} | New Grids: {}", market, new_config.num_grids);
}

// ===== Exit Management =====

void GridTradingStrategy::exitGrid(
    const std::string& market,
    ExitReason reason,
    double current_price)
{
    if (active_grids_.find(market) == active_grids_.end()) {
        return;
    }
    
    auto& grid = active_grids_[market];
    
    // 모든 레벨 청산
    liquidateAllLevels(grid, current_price);
    
    // P&L 계산
    double total_pnl = grid.realized_pnl + grid.unrealized_pnl;
    bool is_win = (total_pnl > 0);
    
    // Statistics 업데이트
    updateStatistics(market, is_win, total_pnl); // market 추가
    
    // 로깅
    const char* reason_str[] = {"NONE", "PROFIT", "STOP_LOSS", "BREAKOUT", "FLASH_CRASH", "MAX_TIME", "MANUAL"};
    spdlog::info("[GridTrading] Grid Exited - {} | Reason: {} | P&L: {:.3f}% | Cycles: {}",
                market, reason_str[static_cast<int>(reason)], total_pnl * 100, grid.completed_cycles);
    
// [참고] updateStatistics 안에서 지웠다면 여기선 안 지워도 됨.
    // 명시적으로 남겨둔다면 updateStatistics 내부의 erase를 제거하는 것을 추천.
    if (active_grids_.count(market)) {
        active_grids_.erase(market);
    }
}

void GridTradingStrategy::liquidateAllLevels(
    GridPositionData& grid,
    double current_price)
{
    for (auto& [level_id, level] : grid.levels) {
        if (level.buy_order_filled && !level.sell_order_filled) {
            // 강제 매도
            double profit_pct = (current_price - level.price) / level.price;
            profit_pct -= (UPBIT_FEE_RATE * 2);
            
            grid.realized_pnl += profit_pct;
            
            spdlog::debug("[GridTrading] Liquidate Level {} | Price: {:.2f} | P&L: {:.3f}%",
                         level_id, level.price, profit_pct * 100);
        }
    }
}

// ===== Profit Management =====

double GridTradingStrategy::calculateGridUnrealizedPnL(
    const GridPositionData& grid,
    double current_price) const
{
    double unrealized = 0.0;
    
    for (const auto& [level_id, level] : grid.levels) {
        if (level.buy_order_filled && !level.sell_order_filled) {
            double profit_pct = (current_price - level.price) / level.price;
            profit_pct -= (UPBIT_FEE_RATE * 2);
            unrealized += profit_pct;
        }
    }
    
    return unrealized;
}

void GridTradingStrategy::compoundProfits(GridPositionData& grid)
{
    // 수익 재투자
    if (grid.realized_pnl > 0.05) {  // 5% 이상 수익시
        double additional_capital = grid.config.total_capital_allocated * grid.realized_pnl;
        
        grid.config.total_capital_allocated += additional_capital;
        grid.config.capital_per_grid = grid.config.total_capital_allocated / grid.config.num_grids;
        
        grid.realized_pnl = 0.0;
        
        spdlog::info("[GridTrading] Compounding Profits - {} | New Capital: {:.0f}",
                    grid.market, grid.config.total_capital_allocated);
    }
}

// ===== Performance Tracking =====

double GridTradingStrategy::calculateGridEfficiency(
    const GridPositionData& grid) const
{
    if (grid.levels.empty()) {
        return 0.0;
    }
    
    int total_levels = grid.levels.size();
    int active_levels = 0;
    int completed_trips = 0;
    
    for (const auto& [level_id, level] : grid.levels) {
        if (level.buy_order_filled || level.sell_order_filled) {
            active_levels++;
        }
        completed_trips += level.round_trips;
    }
    
    double utilization = static_cast<double>(active_levels) / total_levels;
    double productivity = static_cast<double>(completed_trips) / total_levels;
    
    return (utilization * 0.5 + productivity * 0.5);
}

// ===== Helpers =====

double GridTradingStrategy::calculateMean(const std::vector<double>& values) const
{
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

double GridTradingStrategy::calculateStdDev(
    const std::vector<double>& values,
    double mean) const
{
    if (values.size() < 2) return 0.0;
    
    double sum_sq = 0.0;
    for (double val : values) {
        double diff = val - mean;
        sum_sq += diff * diff;
    }
    
    return std::sqrt(sum_sq / (values.size() - 1));
}

double GridTradingStrategy::calculateVolatility(const std::vector<analytics::Candle>& candles) const {
    if (candles.size() < 21) return 0.02;
    
    std::vector<double> returns;
    // [수정] 데이터의 맨 뒤(현재) 30개 구간의 수익률 계산
    size_t count = std::min(candles.size(), size_t(31));
    for (size_t i = candles.size() - count + 1; i < candles.size(); ++i) {
        double ret = (candles[i].close - candles[i-1].close) / candles[i-1].close;
        returns.push_back(ret);
    }
    
    return calculateStdDev(returns, calculateMean(returns));
}

double GridTradingStrategy::calculateATR(
    const std::vector<analytics::Candle>& candles,
    int period) const
{
   if (candles.size() < static_cast<size_t>(period + 1)) {
        return 0.0;
    }
    
    std::vector<double> true_ranges;
    
    // [수정] 가장 최근 'period' 개수의 캔들만 순회
    // candles.size() - period 부터 끝까지
    size_t start_idx = candles.size() - period;
    
    for (size_t i = start_idx; i < candles.size(); i++) {
        double high_low = candles[i].high - candles[i].low;
        double high_close = std::abs(candles[i].high - candles[i-1].close);
        double low_close = std::abs(candles[i].low - candles[i-1].close);
        
        true_ranges.push_back(std::max({high_low, high_close, low_close}));
    }
    
    return calculateMean(true_ranges);
}

std::vector<double> GridTradingStrategy::extractPrices(
    const std::vector<analytics::Candle>& candles,
    const std::string& type) const
{
    std::vector<double> prices;
    
    for (const auto& candle : candles) {
        if (type == "close") {
            prices.push_back(candle.close);
        } else if (type == "open") {
            prices.push_back(candle.open);
        } else if (type == "high") {
            prices.push_back(candle.high);
        } else if (type == "low") {
            prices.push_back(candle.low);
        } else {
            prices.push_back(candle.close);
        }
    }
    
    return prices;
}

long long GridTradingStrategy::getCurrentTimestamp() const
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

bool GridTradingStrategy::shouldGenerateGridSignal(
    const GridSignalMetrics& metrics) const
{
    if (!metrics.is_valid) {
        return false;
    }
    
    if (metrics.strength < MIN_SIGNAL_STRENGTH) {
        return false;
    }
    
    if (!metrics.is_profitable_after_fees) {
        return false;
    }
    
    if (metrics.optimal_grid_count < MIN_GRID_COUNT) {
        return false;
    }
    
    return true;
}

std::vector<analytics::Candle> GridTradingStrategy::parseCandlesFromJson(
    const nlohmann::json& json_data) const
{
    std::vector<analytics::Candle> candles;
    
    if (!json_data.is_array()) {
        return candles;
    }
    
    for (const auto& item : json_data) {
        analytics::Candle candle;
        
        candle.timestamp = item.value("timestamp", 0LL);
        candle.open = item.value("opening_price", 0.0);
        candle.high = item.value("high_price", 0.0);
        candle.low = item.value("low_price", 0.0);
        candle.close = item.value("trade_price", 0.0);
        candle.volume = item.value("candle_acc_trade_volume", 0.0);
        
        candles.push_back(candle);
    }
    
    std::sort(candles.begin(), candles.end(), 
              [](const analytics::Candle& a, const analytics::Candle& b) {
                  return a.timestamp < b.timestamp;
              });
    
    return candles;
}

void GridTradingStrategy::updateState(const std::string& market, double current_price) {
    // 내부 로직인 updateGridLevels를 호출하여 그물망 감시/주문 수행
    updateGridLevels(market, current_price);
}

} // namespace strategy
} // namespace autolife
