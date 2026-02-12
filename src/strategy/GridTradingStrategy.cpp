#include "strategy/GridTradingStrategy.h"
#include "common/Logger.h"
#include "analytics/TechnicalIndicators.h"
#include <cmath>
#include <algorithm>
#include <numeric>

// Prevent Windows macros from interfering with std::min/max
#undef max
#undef min
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
    const std::vector<Candle>& candles,
    double current_price,
    double available_capital,
    const analytics::RegimeAnalysis& regime)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    last_metrics_cache_[market] = metrics;
    last_candles_cache_[market] = candles;
    last_price_cache_[market] = current_price;
    
    Signal signal;
    signal.type = SignalType::NONE;
    signal.market = market;
    signal.strategy_name = "Grid Trading Strategy";
    
    // ===== Hard Gates =====
    if (active_positions_.find(market) != active_positions_.end()) return signal;
    if (active_grids_.find(market) != active_grids_.end()) return signal;
    if (candles.size() < 100) return signal;
    if (available_capital <= 0) return signal;
    if (!canTradeNow()) return signal;
    
    checkCircuitBreaker();
    if (isCircuitBreakerActive()) return signal;
    
    // ===== Score-Based Evaluation =====
    double total_score = 0.0;
    
    // --- (1) 그리드 분석 (최대 0.40) ---
    GridSignalMetrics grid_signal = analyzeGridOpportunity(market, metrics, candles, current_price);
    total_score += grid_signal.strength * 0.40;
    
    // --- (2) 유동성 (최대 0.10) ---
    if (metrics.liquidity_score >= MIN_LIQUIDITY_SCORE) total_score += 0.10;
    else if (metrics.liquidity_score >= MIN_LIQUIDITY_SCORE * 0.5) total_score += 0.05;
    
    // --- (3) 레짐 (최대 0.20) ---
    double regime_score = 0.0;
    switch (regime.regime) {
        case analytics::MarketRegime::RANGING: regime_score = 0.20; break;     // 그리드에 최적
        case analytics::MarketRegime::HIGH_VOLATILITY: regime_score = 0.10; break;
        case analytics::MarketRegime::TRENDING_UP: regime_score = 0.05; break;
        case analytics::MarketRegime::TRENDING_DOWN: regime_score = 0.02; break;
        default: regime_score = 0.05; break;
    }
    total_score += regime_score;
    
    // --- (4) 변동성 적합도 (최대 0.15) ---
    auto prices = analytics::TechnicalIndicators::extractClosePrices(candles);
    double rsi = analytics::TechnicalIndicators::calculateRSI(prices, 14);
    // 그리드는 RSI 40~60 (횡보) 구간이 최적
    if (rsi >= 40 && rsi <= 60) total_score += 0.15;
    else if (rsi >= 30 && rsi <= 70) total_score += 0.08;
    else total_score += 0.02;
    
    // --- (5) 거래량 (최대 0.15) ---
    if (metrics.volume_surge_ratio >= 1.5) total_score += 0.15;
    else if (metrics.volume_surge_ratio >= 1.0) total_score += 0.10;
    else total_score += 0.03;
    
    // ===== 최종 판정 =====
    signal.strength = std::clamp(total_score, 0.0, 1.0);
    
    if (signal.strength < 0.40) return signal;
    
    // ===== 신호 생성 =====
    signal.type = SignalType::BUY;
    signal.entry_price = current_price;
    signal.stop_loss = calculateStopLoss(current_price, candles);
    double risk_grid = current_price - signal.stop_loss;
    signal.take_profit_1 = current_price + (risk_grid * 3.0 * 0.5);
    signal.take_profit_2 = calculateTakeProfit(current_price, candles);
    signal.buy_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.sell_order_type = strategy::OrderTypePolicy::LIMIT_WITH_FALLBACK;
    signal.max_retries = 2;
    signal.retry_wait_ms = 300;
    
    // 포지션 사이징
    signal.position_size = calculatePositionSize(available_capital, current_price, signal.stop_loss, metrics);
    
    double order_amount = available_capital * signal.position_size;
    if (order_amount < MIN_CAPITAL_PER_GRID * grid_signal.optimal_grid_count) {
        signal.type = SignalType::NONE;
        return signal;
    }
    
    if (signal.strength >= 0.70) {
        signal.type = SignalType::STRONG_BUY;
    }
    
    spdlog::info("[GridTrading] 🎯 GRID Signal - {} | Strength: {:.3f} | Grids: {} | Spacing: {:.2f}%",
                 market, signal.strength,
                 grid_signal.optimal_grid_count,
                 grid_signal.optimal_spacing_pct * 100);
    
    last_signal_time_ = getCurrentTimestamp();
    
    return signal;
}

// ===== Entry Decision =====

bool GridTradingStrategy::shouldEnter(
    const std::string& market,
    const analytics::CoinMetrics& metrics,
    const std::vector<Candle>& candles,
    double current_price,
    const analytics::RegimeAnalysis& regime)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)regime; // Used for future enhancements

    if (active_positions_.find(market) != active_positions_.end()) {
        return false;
    }

    if (candles.size() < 100) return false;
    if (metrics.liquidity_score < MIN_LIQUIDITY_SCORE) return false;

    if (active_grids_.find(market) != active_grids_.end()) {
        return false;
    }

    if (!canTradeNow()) return false;

    checkCircuitBreaker();
    if (isCircuitBreakerActive()) return false;

    long long now = getCurrentTimestamp();
    if ((now - last_signal_time_) < MIN_SIGNAL_INTERVAL_SEC * 1000) {
        return false;
    }

    GridSignalMetrics grid_signal = analyzeGridOpportunity(market, metrics, candles, current_price);
    return shouldGenerateGridSignal(grid_signal);
}

// ===== Exit Decision =====

bool GridTradingStrategy::shouldExit(
    const std::string& market,
    double entry_price,
    double current_price,
    double holding_time_seconds)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)entry_price;  // entry_price 파라미터 미사용
    
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
        return false; 
    }
    
    // ===== 2. Stop Loss 체크 (전체 손익 기준) =====
    if (checkStopLoss(grid, current_price)) {
        spdlog::warn("[GridTrading] Stop Loss Hit - {} | Drawdown: {:.2f}%",
                     market, grid.current_drawdown * 100);
        // exitGrid 호출 (통계 업데이트 및 목록 제거)
        exitGrid(market, ExitReason::STOP_LOSS, current_price);
        return false;
    }
    
    // ===== 3. Max Holding Time =====
    if (holding_hours >= MAX_HOLDING_TIME_HOURS) {
        spdlog::info("[GridTrading] Max Holding Time - {} | Hours: {:.1f}", market, holding_hours);
        exitGrid(market, ExitReason::MAX_TIME, current_price);
        return false;
    }
    
    // ===== 4. Rebalancing / Breakout 체크 =====
    if (shouldRebalanceGrid(market, current_price)) {
        std::vector<Candle> candles = getCachedCandles(market, 100);
        RangeDetectionMetrics new_range = detectRange(candles, current_price);
        
        if (new_range.is_ranging) {
            // 횡보장이면 그리드 재설정 (청산 아님)
            rebalanceGrid(market, current_price, new_range);
            return false; // 그리드 유지
        } else {
            // 추세장이면 그리드 종료 (청산)
            spdlog::info("[GridTrading] Range Lost (Trend) - {} | Exiting", market);
            exitGrid(market, ExitReason::BREAKOUT, current_price);
            return false;
        }
    }
    
    return false;
}

// ===== Calculate Stop Loss =====

double GridTradingStrategy::calculateStopLoss(
    double entry_price,
    const std::vector<Candle>& candles)
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
    const std::vector<Candle>& candles)
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
    (void)capital;      // capital 파라미터 미사용
    (void)entry_price;  // entry_price 파라미터 미사용
    (void)stop_loss;    // stop_loss 파라미터 미사용
    
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

void GridTradingStrategy::setStatistics(const Statistics& stats)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stats_ = stats;
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

bool GridTradingStrategy::onSignalAccepted(const Signal& signal, double allocated_capital)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (signal.market.empty()) {
        return false;
    }

    if (active_grids_.count(signal.market)) {
        return false;
    }

    if (allocated_capital <= 0.0) {
        return false;
    }

    auto metrics_it = last_metrics_cache_.find(signal.market);
    auto candles_it = last_candles_cache_.find(signal.market);

    if (metrics_it == last_metrics_cache_.end()) {
        spdlog::warn("[GridTrading] Activation skipped (no metrics cache): {}", signal.market);
        return false;
    }

    std::vector<Candle> candles;
    if (candles_it != last_candles_cache_.end()) {
        candles = candles_it->second;
    }

    if (candles.size() < 50) {
        candles = getCachedCandles(signal.market, 200);
    }

    if (candles.size() < 50) {
        spdlog::warn("[GridTrading] Activation skipped (insufficient candles): {}", signal.market);
        return false;
    }

    double current_price = signal.entry_price;
    if (current_price <= 0.0) {
        auto price_it = last_price_cache_.find(signal.market);
        if (price_it != last_price_cache_.end()) {
            current_price = price_it->second;
        }
    }
    if (current_price <= 0.0) {
        spdlog::warn("[GridTrading] Activation skipped (invalid price): {}", signal.market);
        return false;
    }

    RangeDetectionMetrics range = detectRange(candles, current_price);
    GridConfiguration config = createGridConfiguration(
        signal.market, range, metrics_it->second, current_price, allocated_capital
    );

    auto levels = generateGridLevels(config);
    if (levels.empty()) {
        spdlog::warn("[GridTrading] Activation skipped (no levels): {}", signal.market);
        return false;
    }

    GridPositionData grid;
    grid.market = signal.market;
    grid.config = config;
    grid.status = GridStatus::ACTIVE;
    grid.levels = levels;
    grid.total_invested = allocated_capital;
    grid.creation_timestamp = getCurrentTimestamp();
    grid.last_price = current_price;
    grid.last_price_update_timestamp = grid.creation_timestamp;

    active_positions_.insert(signal.market);
    active_grids_[signal.market] = grid;

    recordTrade();
    rolling_stats_.total_grids_created++;

    spdlog::info("[GridTrading] Grid Activated - {} | Capital: {:.0f} | Grids: {}",
                 signal.market, allocated_capital, config.num_grids);

    return true;
}

std::vector<OrderRequest> GridTradingStrategy::drainOrderRequests()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<OrderRequest> drained;
    drained.reserve(pending_orders_.size());
    while (!pending_orders_.empty()) {
        drained.push_back(pending_orders_.front());
        pending_orders_.pop_front();
    }
    return drained;
}

void GridTradingStrategy::onOrderResult(const OrderResult& result)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto grid_it = active_grids_.find(result.market);
    if (grid_it == active_grids_.end()) {
        spdlog::warn("[GridTrading] Order result ignored (no grid): {}", result.market);
        return;
    }

    auto& grid = grid_it->second;
    auto level_it = grid.levels.find(result.level_id);
    if (level_it == grid.levels.end()) {
        spdlog::warn("[GridTrading] Order result ignored (no level): {} #{}", result.market, result.level_id);
        return;
    }

    auto& level = level_it->second;
    long long now = getCurrentTimestamp();

    if (!result.success || result.executed_volume <= 0.0) {
        if (result.side == OrderSide::BUY) {
            level.buy_order_placed = false;
            level.buy_order_filled = false;
        } else {
            level.sell_order_placed = false;
            level.sell_order_filled = false;
        }
        spdlog::warn("[GridTrading] Order failed - {} | Level: {}", result.market, level.level_id);
        return;
    }

    if (result.side == OrderSide::BUY) {
        level.buy_order_placed = true;
        level.buy_order_filled = true;
        level.buy_timestamp = now;
        level.quantity = result.executed_volume;
        if (result.executed_price > 0.0) {
            level.price = result.executed_price;
        }

        spdlog::info("[GridTrading] Buy Filled - {} | Level: {} | Price: {:.2f}",
                     result.market, level.level_id, level.price);
        return;
    }

    level.sell_order_placed = true;
    level.sell_order_filled = true;
    level.sell_timestamp = now;

    double buy_price = level.price;
    double sell_price = result.executed_price > 0.0 ? result.executed_price : buy_price;
    double notional_buy = buy_price * level.quantity;
    double notional_sell = sell_price * level.quantity;
    double fee_krw = (notional_buy + notional_sell) * UPBIT_FEE_RATE;
    double profit_krw = (sell_price - buy_price) * level.quantity - fee_krw;
    double profit_pct = (notional_buy > 0.0) ? (profit_krw / notional_buy) : 0.0;

    level.profit_loss = profit_pct;
    level.cumulative_profit += profit_pct;
    level.round_trips++;
    grid.realized_pnl += profit_krw;
    grid.completed_cycles++;

    if (level.buy_timestamp > 0) {
        double cycle_time = static_cast<double>(level.sell_timestamp - level.buy_timestamp);
        cycle_times_.push_back(cycle_time);
        if (cycle_times_.size() > 100) {
            cycle_times_.pop_front();
        }
    }

    spdlog::info("[GridTrading] Sell Filled - {} | Level: {} | Profit: {:.3f}% | Trips: {}",
                 result.market, level.level_id, profit_pct * 100, level.round_trips);

    level.buy_order_filled = false;
    level.sell_order_filled = false;
    level.buy_order_placed = false;
    level.sell_order_placed = false;

    if (grid.exit_requested) {
        bool all_closed = true;
        for (const auto& item : grid.levels) {
            if (item.second.buy_order_filled) {
                all_closed = false;
                break;
            }
        }

        if (all_closed) {
            double total_pnl_krw = grid.realized_pnl + grid.unrealized_pnl;
            double total_pnl_pct = (grid.total_invested > 0.0)
                ? (total_pnl_krw / grid.total_invested)
                : 0.0;
            bool is_win = (total_pnl_krw > 0.0);

            updateStatistics(result.market, is_win, total_pnl_pct);

            const char* reason_str[] = {"NONE", "PROFIT", "STOP_LOSS", "BREAKOUT", "FLASH_CRASH", "MAX_TIME", "MANUAL"};
            spdlog::info("[GridTrading] Grid Exited - {} | Reason: {} | P&L: {:.3f}% | Cycles: {}",
                         result.market,
                         reason_str[static_cast<int>(grid.exit_reason)],
                         total_pnl_pct * 100,
                         grid.completed_cycles);

            released_markets_.push_back(result.market);
        }
    }
}

std::vector<std::string> GridTradingStrategy::drainReleasedMarkets()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> released;
    released.reserve(released_markets_.size());
    while (!released_markets_.empty()) {
        released.push_back(released_markets_.front());
        released_markets_.pop_front();
    }
    return released;
}

std::vector<std::string> GridTradingStrategy::getActiveMarkets() const
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> markets;
    markets.reserve(active_grids_.size());
    for (const auto& item : active_grids_) {
        markets.push_back(item.first);
    }
    return markets;
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
    if (grid.status == GridStatus::EMERGENCY_EXIT) {
        return;
    }
    grid.last_price = current_price;
    grid.last_price_update_timestamp = getCurrentTimestamp();
    
    double target_spacing = std::max(grid.config.grid_spacing_pct, MIN_GRID_SPACING_PCT);

    // 각 레벨 체크
    for (auto& [level_id, level] : grid.levels) {
        if (level.quantity <= 0.0 && grid.config.capital_per_grid > 0.0 && level.price > 0.0) {
            level.quantity = grid.config.capital_per_grid / level.price;
        }

        // Buy 주문 체크
        if (shouldPlaceBuyOrder(level, current_price) && !level.buy_order_placed) {
            executeGridBuy(market, level, current_price);
        }
        
        // Sell 주문 체크
        if (shouldPlaceSellOrder(level, current_price, target_spacing) && !level.sell_order_placed) {
            executeGridSell(market, level, current_price);
        }
    }
    
    // P&L 업데이트
    grid.unrealized_pnl = calculateGridUnrealizedPnL(grid, current_price);
    if (grid.total_invested > 0.0) {
        grid.current_drawdown = (grid.unrealized_pnl < 0)
            ? std::abs(grid.unrealized_pnl / grid.total_invested)
            : 0.0;
    } else {
        grid.current_drawdown = 0.0;
    }
    
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
    std::vector<Candle> candles = getCachedCandles(market, 20);
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

std::vector<Candle> GridTradingStrategy::getCachedCandles(
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
        return std::vector<Candle>();
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
        return std::vector<Candle>();
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
    const std::vector<Candle>& candles,
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
    
    for (int i = static_cast<int>(candles.size()) - 1; i >= 0 && i >= static_cast<int>(candles.size()) - 50; i--) {
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
    const std::vector<Candle>& candles,
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
    const std::vector<Candle>& candles,
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
    const std::vector<Candle>& candles,
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)market;  // market 파라미터 미사용
    // available_capital은 grid sizing에 사용
    
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
    if (config.num_grids <= 0 || available_capital < MIN_CAPITAL_PER_GRID) {
        config.num_grids = 0;
        config.capital_per_grid = 0.0;
        return config;
    }
    
    // 4. Optimal Spacing 계산
    config.grid_spacing_pct = calculateOptimalSpacing(range.range_width_pct, volatility, config.num_grids);
    
    // 5. Capital Allocation
    config.total_capital_allocated = available_capital;
    config.capital_per_grid = available_capital / config.num_grids;
    
    // 최소 자본 확인
    if (config.capital_per_grid < MIN_CAPITAL_PER_GRID) {
        config.num_grids = static_cast<int>(available_capital / MIN_CAPITAL_PER_GRID);
        if (config.num_grids <= 0) {
            config.capital_per_grid = 0.0;
            return config;
        }
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
    if (config.num_grids <= 0 || config.capital_per_grid <= 0.0) {
        return {};
    }

    std::map<int, GridLevel> levels;

    switch (config.type) {
        case GridType::ARITHMETIC:
            levels = generateArithmeticGrid(
                config.center_price,
                config.grid_spacing_pct * config.center_price,
                config.num_grids
            );
            break;
        
        case GridType::GEOMETRIC:
            levels = generateGeometricGrid(
                config.center_price,
                config.grid_spacing_pct,
                config.num_grids
            );
            break;
        
        case GridType::FIBONACCI:
            levels = generateFibonacciGrid(
                config.center_price,
                config.upper_bound - config.lower_bound,
                config.num_grids
            );
            break;
        
        case GridType::DYNAMIC:
        default:
            levels = generateArithmeticGrid(
                config.center_price,
                config.grid_spacing_pct * config.center_price,
                config.num_grids
            );
            break;
    }

    if (config.capital_per_grid > 0.0) {
        for (auto& item : levels) {
            if (item.second.price > 0.0) {
                item.second.quantity = config.capital_per_grid / item.second.price;
            }
        }
    }

    return levels;
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
    const std::vector<Candle>& candles)
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
    const std::vector<Candle>& candles,
    double current_price)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    (void)market;  // market 파라미터 미사용
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
    signal.strength += range.confidence * 0.35;
    signal.strength += (range.adx < ADX_RANGING_THRESHOLD ? 0.25 : 0.0);
    signal.strength += (signal.is_profitable_after_fees ? 0.2 : 0.0);
    signal.strength += (metrics.liquidity_score / 100.0) * 0.1;
    signal.strength += (1.0 - signal.risk_score) * 0.1;
    
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
    (void)current_price;  // current_price 파라미터 미사용
    if (level.buy_order_filled) {
        return;
    }

    double quantity = level.quantity;
    if (quantity <= 0.0) {
        spdlog::warn("[GridTrading] Buy skipped (invalid quantity): {} | Level: {}",
                     market, level.level_id);
        return;
    }

    double order_amount = level.price * quantity;
    if (order_amount < MIN_ORDER_AMOUNT_KRW) {
        spdlog::warn("[GridTrading] Buy skipped (min order): {} | Level: {} | Amount: {:.0f}",
                     market, level.level_id, order_amount);
        return;
    }

    OrderRequest request;
    request.market = market;
    request.side = OrderSide::BUY;
    request.price = level.price;
    request.quantity = quantity;
    request.level_id = level.level_id;
    request.reason = "grid_buy";

    pending_orders_.push_back(request);
    level.buy_order_placed = true;

    spdlog::info("[GridTrading] Buy Requested - {} | Level: {} | Price: {:.2f}",
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

    double quantity = level.quantity;
    if (quantity <= 0.0) {
        return;
    }

    double order_amount = current_price * quantity;
    if (order_amount < MIN_ORDER_AMOUNT_KRW) {
        spdlog::warn("[GridTrading] Sell skipped (min order): {} | Level: {} | Amount: {:.0f}",
                     market, level.level_id, order_amount);
        return;
    }

    OrderRequest request;
    request.market = market;
    request.side = OrderSide::SELL;
    request.price = current_price;
    request.quantity = quantity;
    request.level_id = level.level_id;
    request.reason = "grid_sell";

    pending_orders_.push_back(request);
    level.sell_order_placed = true;

    spdlog::info("[GridTrading] Sell Requested - {} | Level: {} | Price: {:.2f}",
                 market, level.level_id, current_price);
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

bool GridTradingStrategy::shouldPlaceSellOrder(
    const GridLevel& level,
    double current_price,
    double spacing_pct) const {
    if (!level.buy_order_filled || level.sell_order_placed) return false;
    
    // [수정] 내가 산 가격보다 한 칸(level_spacing) 위에 도달하면 매도
    // 혹은 설정된 최소 수익(spacing)만큼 올랐을 때 매도
    double target_spacing = std::max(spacing_pct, MIN_GRID_SPACING_PCT);
    double min_profit_price = level.price * (1.0 + target_spacing);
    return current_price >= min_profit_price;
}

// ===== Risk Monitoring =====

bool GridTradingStrategy::checkStopLoss(
    const GridPositionData& grid,
    double current_price) const
{
    (void)current_price;
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
    const std::vector<Candle>& candles,
    double current_price)
{
    FlashCrashMetrics flash;
    (void)market;
    
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
            for (size_t i = candles.size() - 1; i > 0; --i) {
                if (candles[i].close < candles[i - 1].close) {
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

    if (grid.exit_requested) {
        return;
    }

    grid.exit_requested = true;
    grid.exit_reason = reason;
    grid.status = GridStatus::EMERGENCY_EXIT;

    // 모든 레벨 청산 요청
    liquidateAllLevels(grid, current_price);

    bool all_closed = true;
    for (const auto& item : grid.levels) {
        if (item.second.buy_order_filled) {
            all_closed = false;
            break;
        }
    }

    if (all_closed) {
        double total_pnl_krw = grid.realized_pnl + grid.unrealized_pnl;
        double total_pnl_pct = (grid.total_invested > 0.0)
            ? (total_pnl_krw / grid.total_invested)
            : 0.0;
        bool is_win = (total_pnl_krw > 0.0);

        updateStatistics(market, is_win, total_pnl_pct);

        const char* reason_str[] = {"NONE", "PROFIT", "STOP_LOSS", "BREAKOUT", "FLASH_CRASH", "MAX_TIME", "MANUAL"};
        spdlog::info("[GridTrading] Grid Exited - {} | Reason: {} | P&L: {:.3f}% | Cycles: {}",
                 market, reason_str[static_cast<int>(reason)], total_pnl_pct * 100, grid.completed_cycles);

        released_markets_.push_back(market);
    }
}

void GridTradingStrategy::liquidateAllLevels(
    GridPositionData& grid,
    double current_price)
{
    for (auto& [level_id, level] : grid.levels) {
        if (!level.buy_order_filled || level.sell_order_filled) {
            continue;
        }

        double quantity = level.quantity;
        if (quantity <= 0.0) {
            continue;
        }

        double order_amount = current_price * quantity;
        if (order_amount < MIN_ORDER_AMOUNT_KRW) {
            spdlog::warn("[GridTrading] Liquidation skipped (min order): {} | Level: {} | Amount: {:.0f}",
                         grid.market, level.level_id, order_amount);
            continue;
        }

        OrderRequest request;
        request.market = grid.market;
        request.side = OrderSide::SELL;
        request.price = current_price;
        request.quantity = quantity;
        request.level_id = level.level_id;
        request.reason = "grid_liquidation";

        pending_orders_.push_back(request);
        level.sell_order_placed = true;

        spdlog::info("[GridTrading] Liquidation Requested - {} | Level: {} | Price: {:.2f}",
                     grid.market, level.level_id, current_price);
    }
}

// ===== Profit Management =====

double GridTradingStrategy::calculateGridUnrealizedPnL(
    const GridPositionData& grid,
    double current_price) const
{
    double unrealized_krw = 0.0;
    
    for (const auto& [level_id, level] : grid.levels) {
        if (level.buy_order_filled && !level.sell_order_filled) {
            double notional_buy = level.price * level.quantity;
            double notional_sell = current_price * level.quantity;
            double fee_krw = (notional_buy + notional_sell) * UPBIT_FEE_RATE;
            double profit_krw = (current_price - level.price) * level.quantity - fee_krw;
            unrealized_krw += profit_krw;
        }
    }
    
    return unrealized_krw;
}

void GridTradingStrategy::compoundProfits(GridPositionData& grid)
{
    // 수익 재투자
    double realized_pct = (grid.total_invested > 0.0)
        ? (grid.realized_pnl / grid.total_invested)
        : 0.0;
    if (realized_pct > 0.05) {  // 5% 이상 수익시
        double additional_capital = grid.realized_pnl;
        
        grid.config.total_capital_allocated += additional_capital;
        if (grid.config.num_grids > 0) {
            grid.config.capital_per_grid = grid.config.total_capital_allocated / grid.config.num_grids;
        }
        
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
    
    int total_levels = static_cast<int>(grid.levels.size());
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

double GridTradingStrategy::calculateVolatility(const std::vector<Candle>& candles) const {
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
    const std::vector<Candle>& candles,
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
    const std::vector<Candle>& candles,
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

std::vector<Candle> GridTradingStrategy::parseCandlesFromJson(
    const nlohmann::json& json_data) const
{
    return analytics::TechnicalIndicators::jsonToCandles(json_data);
}

void GridTradingStrategy::updateState(const std::string& market, double current_price) {
    // 내부 로직인 updateGridLevels를 호출하여 그물망 감시/주문 수행
    updateGridLevels(market, current_price);
}

} // namespace strategy
} // namespace autolife
