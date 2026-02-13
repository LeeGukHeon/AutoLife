#include "risk/RiskManager.h"
#include "common/Logger.h"
#include "common/Config.h"
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
    , daily_start_capital_(initial_capital)
    , daily_loss_limit_pct_(0.05)
    , daily_loss_limit_krw_(50000.0)
    , daily_start_date_(0)
    , max_positions_(10)
    , max_daily_trades_(100)   // [Phase 2] from 20 to 100 (strategy diversification)
    , max_drawdown_pct_(0.10)  // 10%
    , max_exposure_pct_(0.85)  // default exposure cap 85%
    , min_reentry_interval_(60)   // [Phase 2] from 300s to 60s
    , max_capital_(initial_capital)
    , total_fees_paid_(0)
    , min_order_krw_(5000.0)
    , recommended_min_enter_krw_(6000.0)
{
    LOG_INFO("RiskManager initialized - initial capital {:.0f} KRW", initial_capital);
    resetDailyCountIfNeeded();
    resetDailyLossIfNeeded();
}

// ===== Position Entry =====

bool RiskManager::canEnterPosition(
    const std::string& market,
    double entry_price,
    double position_size_ratio,
    const std::string& strategy_name
) {
    // Thread-safe guard
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // Daily reset checks
    resetDailyCountIfNeeded();
    resetDailyLossIfNeeded();
    
    // 1) already has position
    if (getPosition(market) != nullptr) {
        LOG_WARN("{} already has an open position", market);
        return false;
    }
    
    // 2) max active positions
    if (hasReachedMaxPositions()) {
        LOG_WARN("max positions reached ({}/{})", positions_.size(), max_positions_);
        return false;
    }
    
    // 3) daily trade limit
    if (hasReachedDailyTradeLimit()) {
        LOG_WARN("daily trade limit reached ({}/{})", daily_trade_count_, max_daily_trades_);
        return false;
    }
    
    // 4) re-entry cooldown
    if (!canTradeMarket(market)) {
        LOG_WARN("{} re-entry cooldown active", market);
        return false;
    }
    
    // 5) drawdown guard
    if (isDrawdownExceeded()) {
        LOG_ERROR("Drawdown limit exceeded, trading blocked");
        return false;
    }
    
    // ===== Capital safety checks =====
    
    // Invested amount in open positions
    double invested_sum = 0.0;
    for (const auto& [m, pos] : positions_) {
        invested_sum += pos.invested_amount;
    }

    double reserved_sum = 0.0;
    for (const auto& [m, amount] : reserved_grid_capital_) {
        reserved_sum += amount;
    }
    
    // Cash available for new entries (subtract reserves and pending orders)
    double available_cash = current_capital_ - reserved_sum - pending_order_capital_;
    if (available_cash < 0) available_cash = 0.0;
    
    // Exchange minimums
    const double MIN_ORDER_KRW = min_order_krw_;
    const double RECOMMENDED_MIN_ENTER_KRW = MIN_ORDER_KRW;
    
    // Base assumptions for stop/fee feasibility check
    const double BASE_STOP_LOSS_PCT = 0.03;
    const double FEE_RATE = Config::getInstance().getFeeRate();
    
    // Normalize requested ratio
    double normalized_ratio = std::clamp(position_size_ratio, 0.0, 1.0);
    if (std::fabs(normalized_ratio - position_size_ratio) > 0.001) {
        LOG_WARN("{} - normalized position_size_ratio {:.4f} -> {:.4f}",
                 market, position_size_ratio, normalized_ratio);
    }
    
    // Minimum available cash check
    if (available_cash < RECOMMENDED_MIN_ENTER_KRW) {
        LOG_WARN("{} buy blocked: available {:.0f} < min required {:.0f}",
                 market, available_cash, RECOMMENDED_MIN_ENTER_KRW);
        return false;
    }
    
    // Required amount for requested ratio
    double required_amount = available_cash * normalized_ratio;
    
    // Hard minimum order check
    if (required_amount < MIN_ORDER_KRW) {
        LOG_WARN("{} buy blocked: {:.0f} < min order {:.0f} (ratio {:.4f})",
                 market, required_amount, MIN_ORDER_KRW, normalized_ratio);
        return false;
    }

    // Soft warning: after stop-loss and fee, exit amount may be near minimum
    double min_required_for_exit = MIN_ORDER_KRW / ((1.0 - BASE_STOP_LOSS_PCT) * (1.0 - FEE_RATE));
    if (required_amount < min_required_for_exit) {
        LOG_WARN("{} warning: stop-loss exit may violate minimum order (need {:.0f}, got {:.0f})",
                 market, min_required_for_exit, required_amount);
    }
    
    // Recommended minimum check
    if (required_amount < RECOMMENDED_MIN_ENTER_KRW) {
        LOG_WARN("{} buy blocked: {:.0f} < recommended minimum {:.0f} (ratio {:.4f})",
                 market, required_amount, RECOMMENDED_MIN_ENTER_KRW, normalized_ratio);
        return false;
    }
    
    // Bound check
    if (required_amount > available_cash) {
        LOG_ERROR("internal error: required {:.0f} > available {:.0f}",
                  required_amount, available_cash);
        return false;
    }
    
    // Worst-case per-trade drawdown check
    double max_drawdown_per_trade = current_capital_ * (max_drawdown_pct_ / max_positions_);
    const double WORST_CASE_PRICE_MOVE_PCT = 0.02;   // 2% worst-case move
    const double ESTIMATED_TOTAL_FEE_PCT = 0.002;    // 0.2% total fees
    double worst_case_loss_pct = WORST_CASE_PRICE_MOVE_PCT + ESTIMATED_TOTAL_FEE_PCT;
    double estimated_worst_loss = required_amount * worst_case_loss_pct;

    if (estimated_worst_loss > max_drawdown_per_trade) {
        LOG_WARN("entry blocked: estimated worst loss {:.0f} > allowed {:.0f}",
                 estimated_worst_loss, max_drawdown_per_trade);
        return false;
    }

    // Total exposure check
    auto metrics = getRiskMetrics();
    double total_capital = metrics.total_capital;
    double allowed_investment = total_capital * max_exposure_pct_;
    if ((invested_sum + reserved_sum + required_amount) > allowed_investment) {
        LOG_WARN("exposure exceeded: current {:.0f} + new {:.0f} > allowed {:.0f} (ratio {:.2f})",
                 invested_sum + reserved_sum, required_amount, allowed_investment, max_exposure_pct_);
        return false;
    }
    
    LOG_INFO("{} buy validated: available {:.0f}, amount {:.0f} (ratio {:.4f}={}%)",
             market, available_cash, required_amount, normalized_ratio, 
             static_cast<int>(normalized_ratio * 100));
    
    (void)entry_price;
    (void)strategy_name;
    
    return true;
}

void RiskManager::setDailyLossLimitPct(double pct) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (pct < 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    daily_loss_limit_pct_ = pct;
    LOG_INFO("Daily loss limit set: {:.2f}%", daily_loss_limit_pct_ * 100.0);
}

void RiskManager::setDailyLossLimitKrw(double krw) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (krw < 0.0) krw = 0.0;
    daily_loss_limit_krw_ = krw;
    LOG_INFO("Daily loss limit set: {:.0f} KRW", daily_loss_limit_krw_);
}

void RiskManager::setMinOrderKrw(double min_order_krw) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (min_order_krw < 0.0) min_order_krw = 0.0;
    min_order_krw_ = min_order_krw;
    recommended_min_enter_krw_ = std::max(6000.0, min_order_krw_ * 1.2);
    LOG_INFO("Min order set: {:.0f} KRW (recommended entry: {:.0f} KRW)",
             min_order_krw_, recommended_min_enter_krw_);
}

bool RiskManager::isDailyLossLimitExceeded() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const_cast<RiskManager*>(this)->resetDailyLossIfNeeded();

    if (daily_start_capital_ <= 0.0) return false;

    double invested = 0.0;
    double unrealized = 0.0;
    for (const auto& [market, pos] : positions_) {
        invested += pos.invested_amount;
        unrealized += pos.unrealized_pnl;
    }

    double equity = current_capital_ + invested + unrealized;
    double loss_krw = daily_start_capital_ - equity;
    double loss_pct = loss_krw / daily_start_capital_;

    if (daily_loss_limit_krw_ > 0.0 && loss_krw >= daily_loss_limit_krw_) {
        return true;
    }

    return loss_pct >= daily_loss_limit_pct_;
}

double RiskManager::getDailyLossPct() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const_cast<RiskManager*>(this)->resetDailyLossIfNeeded();

    if (daily_start_capital_ <= 0.0) return 0.0;

    double invested = 0.0;
    double unrealized = 0.0;
    for (const auto& [market, pos] : positions_) {
        invested += pos.invested_amount;
        unrealized += pos.unrealized_pnl;
    }

    double equity = current_capital_ + invested + unrealized;
    return (daily_start_capital_ - equity) / daily_start_capital_;
}

void RiskManager::enterPosition(
    const std::string& market,
    double entry_price,
    double quantity,
    double stop_loss,
    double take_profit_1,
    double take_profit_2,
    const std::string& strategy_name,
    double breakeven_trigger,
    double trailing_start
) {
    // [???????곸쓨?????源놁벁?? ???????⑤베堉???TradingEngine 癲ル슢????????곸쓨??筌?諭??筌먦끉???嶺뚮ㅎ???????
    // ?????뤆????ㅺ컼????ш낄援?????⑤챶苡? ??recursive_mutex?????源놁젳??筌뚯슦苑????怨쀪퐨 ??鶯????녳뵣 ???源놁벁.
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    // [????????쒓낯?? 癲ル슣??????ヂ???쎈눀???節뚮쳮雅? ??筌뚯슜鍮??雅?퍔瑗띰㎖??????
    // ????뽯４??癲ル슣??????筌뚯슜鍮??룸챷???1??0.05% (癲ル슣????? ??invested_amount????????⑤；????
    double base_invested = entry_price * quantity;
    double entry_fee = calculateFee(base_invested);  // 癲ル슣??????筌뚯슜鍮??(0.05%)
    
    // Position ??좊즵??꼯?????筌뚯슜鍮??룸챷?? ??????????????
    Position pos;
    pos.market = market;
    pos.entry_price = entry_price;
    pos.current_price = entry_price;
    pos.quantity = quantity;
    pos.invested_amount = base_invested;  // [???쒓낯?? ??筌뚯슜鍮??룸챷?????ш낄猷??????癲ル슓堉곤쭗?ㅒ? ?????? ?怨뚮옓??????⑤베毓??
    pos.entry_time = getCurrentTimestamp();
    pos.stop_loss = stop_loss;
    pos.take_profit_1 = take_profit_1;
    pos.take_profit_2 = take_profit_2;
    pos.strategy_name = strategy_name;
    pos.half_closed = false;
    pos.highest_price = entry_price;  // [??⑤베堉?] Trailing SL??癲ル슔?됭짆??筌? ?縕?猿녿뎨??
    pos.breakeven_trigger = breakeven_trigger;
    pos.trailing_start = trailing_start;
    
    // [????? ???亦낆떓堉????산덩????筌뚯슜鍮??癲ル슓堉곤쭗?ㅒ?
    // ??ш낄猷?? ??낆뒩???????쭕???+ ??筌뚯슜鍮??癲ル슢?꾤땟?嶺?癲ル슓堉곤쭗?ㅒ??
    current_capital_ -= (base_invested + entry_fee);
    total_fees_paid_ += entry_fee;
    
    // ??????濚밸Ŧ援욃ㅇ?
    positions_[market] = pos;
    
    // 癲꾧퀗????????モ뵲 ?????????れ삀??쎈뭄?
    last_trade_time_[market] = getCurrentTimestamp();
    daily_trade_count_++;
    
    LOG_INFO("Position entered: {} | strategy={} | qty={:.4f} | notional={:.0f} | fee={:.0f} | reserved={:.0f} | cash={:.0f}",
             market, strategy_name, quantity, base_invested, entry_fee, base_invested, current_capital_);
    LOG_INFO("   risk plan: SL={:.0f} ({:+.2f}%), TP1={:.0f} ({:+.2f}%), TP2={:.0f} ({:+.2f}%)",
             stop_loss, (stop_loss - entry_price) / entry_price * 100.0,
             take_profit_1, (take_profit_1 - entry_price) / entry_price * 100.0,
             take_profit_2, (take_profit_2 - entry_price) / entry_price * 100.0);
}

// ===== ?????????녿ぅ??熬곣뫀肄?=====

void RiskManager::updatePosition(const std::string& market, double current_price) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    pos.current_price = current_price;
    if (pos.highest_price <= 0.0 || current_price > pos.highest_price) {
        pos.highest_price = current_price;
    }
    
    // 雅?퍔瑗띰㎖??????????節뚮쳮雅?
    double current_value = current_price * pos.quantity;
    pos.unrealized_pnl = current_value - pos.invested_amount;
    pos.unrealized_pnl_pct = (current_price - pos.entry_price) / pos.entry_price;
}

// ===== ?????癲?雅?癲ル슪???띿물?=====

bool RiskManager::shouldExitPosition(const std::string& market) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return false;
    
    const auto& pos = it->second;
    double current_price = pos.current_price;
    
    // 1. ?????
    if (current_price <= pos.stop_loss) {
        LOG_WARN("{} stop-loss hit: {:.0f} <= {:.0f}", market, current_price, pos.stop_loss);
        return true;
    }
    
    // 2. 2癲??????(??ш끽維??癲?雅?
    if (current_price >= pos.take_profit_2) {
        LOG_INFO("{} take-profit-2 hit: {:.0f} >= {:.0f}", market, current_price, pos.take_profit_2);
        return true;
    }
    
    // 3. 1癲??????(50% 癲?雅? - ??ш끽維쀧빊?????낅뭄??野껊갭??
    if (!pos.half_closed && current_price >= pos.take_profit_1) {
        LOG_INFO("{} take-profit-1 hit: {:.0f} >= {:.0f}", market, current_price, pos.take_profit_1);
        // ???源놁졆 癲?雅?? partialExit()?????癲ル슪?ｇ몭??
        return false;
    }
    
    return false;
}

void RiskManager::exitPosition(
    const std::string& market,
    double exit_price,
    const std::string& exit_reason
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    
    // 1. 癲ル슢???믩쇀???쒒뜮?뚮눀???節뚮쳮雅?
    double exit_value = exit_price * pos.quantity;
    
    // [??좊즵獒뺣돀?? 癲?雅???筌뚯슜鍮????節뚮쳮雅???癲ル슓堉곤쭗?ㅒ?
    // ????뽯４??癲?雅???筌뚯슜鍮?? 0.25%
    double exit_fee = calculateFee(exit_value);
    
    // 2. ??筌?鍮??嶺뚮Ĳ??????????節뚮쳮雅?
    // Net Profit = (癲ル슢???믩쇀熬곥끇竊?獄쏅챶彛?- 癲ル슢???믩쇀??筌뚯슜鍮?? - 癲ル슣???????怨뺤춸
    // (癲ル슣??????筌뚯슜鍮??룸챷??????? 癲ル슓堉곤쭗?ㅒ??
    double net_profit = (exit_value - exit_fee) - pos.invested_amount;
    
    // 3. ???亦낆떓堉?????녿ぅ??熬곣뫀肄?
    // ??ш낄猷??= ??ш낄猷??+ (癲ル슢???믩쇀熬곥끇竊?獄쏅챶彛?- 癲?雅??筌뚯슜鍮??
    current_capital_ += (exit_value - exit_fee);
    total_fees_paid_ += exit_fee;
    
    // 4. 癲ル슔?됭짆?????亦낆떓堉?High Water Mark) ??좊즲???- MDD ??れ삀????
    if (current_capital_ > max_capital_) {
        max_capital_ = current_capital_;
    }
    
    // ???????怨뚮뼚?????ｊ콪
    double raw_pnl_pct = (exit_price - pos.entry_price) / pos.entry_price;
    
    // 癲꾧퀗???????????れ삀??쎈뭄?
    recordTrade(pos, exit_price, exit_reason);
    
    // ?????????
    positions_.erase(it);
    
    LOG_INFO("Position exited: {} | pnl {:.0f} ({:+.2f}%) | reason={} | cash {:.0f}",
             market, net_profit, raw_pnl_pct * 100.0, exit_reason, current_capital_);
}

void RiskManager::partialExit(const std::string& market, double exit_price) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    
    if (pos.half_closed) return; // ???? 1癲????????ш끽維??
    
    // 50% 癲?雅?
    double exit_quantity = pos.quantity * 0.5;
    double exit_value = exit_price * exit_quantity;
    double fee = calculateFee(exit_value);
    double net_value = exit_value - fee;
    
    // ???亦?????녿ぅ??熬곣뫀肄?
    current_capital_ += net_value;
    total_fees_paid_ += fee;
    
    // ?????????녿ぅ??熬곣뫀肄?
    pos.quantity -= exit_quantity;
    pos.invested_amount *= 0.5;
    pos.half_closed = true;
    
    // ???????モ섋キ??怨뚮옖筌????⑥???????
    pos.stop_loss = pos.entry_price;
    
    double profit = net_value - (pos.invested_amount);
    
    LOG_INFO("First TP hit (50%): {} - exit price: {:.0f}, profit: {:.0f}, stop moved to breakeven",
             market, exit_price, profit);
}

// [Fix] ???筌???????쒓낮?????딅텑?????????癲ル슢履뉑쾮????濡ろ뜑??? ?????μ쐺獄쏅챷援▼＄???좊즴甕?影?곷퓠???諛몄툏??(???亦??怨뚮뼚??????⑤챶苡?
void RiskManager::setHalfClosed(const std::string& market, bool half_closed) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = positions_.find(market);
    if (it != positions_.end()) {
        it->second.half_closed = half_closed;
        LOG_INFO("{} half-closed flag set: {}", market, half_closed ? "TRUE" : "FALSE");
    }
}

// [Phase 3] ??딅텑???癲ル슪?????????嚥??몌┼???좊즴???(????????)
void RiskManager::updatePositionQuantity(const std::string& market, double new_quantity) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    auto it = positions_.find(market);
    if (it == positions_.end()) {
        LOG_WARN("updatePositionQuantity: position not found - {}", market);
        return;
    }
    
    auto& pos = it->second;
    double old_quantity = pos.quantity;
    double sold_quantity = old_quantity - new_quantity;
    
    // 癲ル슢???믩쇀??癲ル슢???移????亦낆떓堉??????
    double freed_capital = sold_quantity * pos.entry_price;
    current_capital_ += freed_capital;
    
    // ???????嚥????????熬곻퐢糾?????녿ぅ??熬곣뫀肄?
    pos.quantity = new_quantity;
    pos.invested_amount = new_quantity * pos.entry_price;
    
    LOG_INFO("Position quantity updated: {} ({:.8f} -> {:.8f}), capital released: {:.0f}",
             market, old_quantity, new_quantity, freed_capital);
}

bool RiskManager::applyPartialSellFill(
    const std::string& market,
    double exit_price,
    double sell_quantity,
    const std::string& exit_reason
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto it = positions_.find(market);
    if (it == positions_.end()) {
        return false;
    }
    if (exit_price <= 0.0 || sell_quantity <= 0.0) {
        return false;
    }

    auto& pos = it->second;
    if (pos.quantity <= 0.0) {
        return false;
    }

    const double qty = std::min(sell_quantity, pos.quantity);
    const double ratio = qty / pos.quantity;
    const double allocated_entry_notional = pos.invested_amount * ratio;
    const double exit_notional = exit_price * qty;
    const double exit_fee = calculateFee(exit_notional);

    current_capital_ += (exit_notional - exit_fee);
    total_fees_paid_ += exit_fee;

    TradeHistory trade;
    trade.market = pos.market;
    trade.entry_price = pos.entry_price;
    trade.exit_price = exit_price;
    trade.quantity = qty;
    trade.entry_time = pos.entry_time;
    trade.exit_time = getCurrentTimestamp();
    trade.strategy_name = pos.strategy_name;
    trade.exit_reason = exit_reason;
    trade.signal_filter = pos.signal_filter;
    trade.signal_strength = pos.signal_strength;
    trade.market_regime = pos.market_regime;
    trade.liquidity_score = pos.liquidity_score;
    trade.volatility = pos.volatility;
    trade.expected_value = pos.expected_value;
    trade.reward_risk_ratio = pos.reward_risk_ratio;

    const double entry_fee = calculateFee(allocated_entry_notional);
    trade.fee_paid = entry_fee + exit_fee;
    trade.profit_loss = exit_notional - allocated_entry_notional - trade.fee_paid;
    trade.profit_loss_pct = (pos.entry_price > 0.0)
        ? ((exit_price - pos.entry_price) / pos.entry_price)
        : 0.0;
    trade_history_.push_back(trade);

    pos.quantity -= qty;
    pos.invested_amount -= allocated_entry_notional;
    if (pos.quantity < 1e-12) {
        pos.quantity = 0.0;
    }
    if (pos.invested_amount < 1e-8) {
        pos.invested_amount = 0.0;
    }

    if (pos.quantity <= 0.0) {
        positions_.erase(it);
    }

    return true;
}

void RiskManager::moveStopToBreakeven(const std::string& market) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    pos.stop_loss = pos.entry_price;
    
    LOG_INFO("{} moved stop to breakeven at {:.0f}", market, pos.entry_price);
}

void RiskManager::updateStopLoss(const std::string& market, double new_stop_loss, const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;

    auto it = positions_.find(market);
    if (it == positions_.end()) return;

    auto& pos = it->second;
    if (new_stop_loss <= 0.0) return;
    if (new_stop_loss <= pos.stop_loss) return;
    if (new_stop_loss >= pos.current_price) return;

    pos.stop_loss = new_stop_loss;
    LOG_INFO("{} stop updated ({}): {:.0f}", market, reason, new_stop_loss);
}

void RiskManager::setPositionTrailingParams(
    const std::string& market,
    double breakeven_trigger,
    double trailing_start
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;

    auto it = positions_.find(market);
    if (it == positions_.end()) return;

    it->second.breakeven_trigger = breakeven_trigger;
    it->second.trailing_start = trailing_start;
}

Position* RiskManager::getPosition(const std::string& market) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return nullptr;
    
    return &it->second;
}

std::vector<Position> RiskManager::getAllPositions() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    std::vector<Position> positions;
    for (const auto& [market, pos] : positions_) {
        positions.push_back(pos);
    }
    
    return positions;
}

// ===== ???????節뚮쳮雅?=====

double RiskManager::calculateDynamicStopLoss(
    double entry_price,
    const std::vector<Candle>& candles
) {
    if (candles.size() < 14) {
        return entry_price * 0.975; // Fallback: -2.5%
    }
    
    // 1. Hard Stop (??ヂ????????? - ??ш낄援???
    double hard_stop = entry_price * 0.975; // -2.5%
    
    // 2. ATR-based Stop (?袁⑸즲??????ㅺ강?? ??????????類????
    double atr_stop = calculateATRStopLoss(entry_price, candles, 2.5);
    
    // 3. Support-based Stop
    double support_stop = calculateSupportStopLoss(entry_price, candles);
    
    // ?怨뚮옖?????ㅼ굣????ш끽維뽭뇡??????? ?????섎쨬??쎛 ???ャ뀕?? ??좊읈??????(=??癲ル슢議??? ?????섎쨬??쎛?????ャ뀕???筌뚯슦肉?
    // ??貫?????釉뚰??㏓뎨?癲?雅???袁⑸젻泳???筌뤾퍓???
    double final_stop = std::min({hard_stop, atr_stop, support_stop});

    // ???源놁벁???? 癲ル슣????筌??怨뚮옖????亦껋꼨援?쭗?좎땡???れ삀???-2.5%?????源놁젳
    if (final_stop >= entry_price) {
        final_stop = entry_price * 0.975;
    }

    LOG_INFO("Stop loss calc: hard={:.0f}, atr={:.0f}, support={:.0f}, final={:.0f}",
              hard_stop, atr_stop, support_stop, final_stop);
    LOG_INFO("Adjusted final stop loss to -2.5% floor");

    return final_stop;
}

double RiskManager::calculateATRStopLoss(
    double entry_price,
    const std::vector<Candle>& candles,
    double multiplier
) {
    double atr = analytics::TechnicalIndicators::calculateATR(candles, 14);
    
    if (atr < 0.0001) {
        return entry_price * 0.975; // -2.5% fallback
    }
    
    // ATR ??れ삀??뫢??怨뚮뼚?????쀫렰 ?????
    double atr_percent = (atr / entry_price) * 100;
    
    // ?怨뚮뼚?????쀫렰?????ㅻ깹??multiplier ?釉뚰???
    if (atr_percent < 1.0) {
        multiplier = 1.5; // ??? ?怨뚮뼚?????쀫렰
    } else if (atr_percent < 3.0) {
        multiplier = 2.0; // 濚욌꼬?댄꺁???怨뚮뼚?????쀫렰
    } else {
        multiplier = 2.5; // ?亦? ?怨뚮뼚?????쀫렰
    }
    
    double stop_loss = entry_price - (atr * multiplier);
    
    // 癲ル슔?됭짆??-2.5%, 癲ル슔?됭짆? -3.5% (??????類??????ш낄援??
    double min_stop = entry_price * 0.975;
    double max_stop = entry_price * 0.965;
    
    return std::clamp(stop_loss, max_stop, min_stop);
}

double RiskManager::calculateSupportStopLoss(
    double entry_price,
    const std::vector<Candle>& candles
) {
    auto support_levels = analytics::TechnicalIndicators::findSupportLevels(candles, 20);
    
    if (support_levels.empty()) {
        return entry_price * 0.975; // -2.5%
    }
    
    // 癲ル슣????筌??怨뚮옖?????? ??좊읈?????좊읈?嚥싲갭큔???癲ル슣??癲ル슣????癲ル슓??젆癒る뎨?
    double nearest_support = 0;
    for (auto level : support_levels) {
        if (level < entry_price && level > nearest_support) {
            nearest_support = level;
        }
    }
    
    if (nearest_support > 0) {
        // 癲ル슣??癲ル슣??????ш끽維??0.5% (????????щ쿊)
        return nearest_support * 0.995;
    }
    
    return entry_price * 0.975; // -2.5%
}

// ===== ?????????ル쵐??=====

double RiskManager::calculateKellyPositionSize(
    double capital,
    double win_rate,
    double avg_win,
    double avg_loss
) {
    if (avg_loss < 0.0001) return 0.05; // Fallback
    
    // Kelly Criterion: f = (p * b - q) / b
    // p = ??꾩룆梨?? q = ???쒙쭗? b = ???????쒓낮???????????
    
    double b = avg_win / avg_loss;
    double p = win_rate;
    double q = 1.0 - win_rate;
    
    double kelly = (p * b - q) / b;
    
    // Kelly??25% ????(???源놁벁????곕쿊)
    double position_ratio = kelly * 0.25;
    
    // 癲ル슔?됭짆??1%, 癲ル슔?됭짆? 10%
    return std::clamp(position_ratio, 0.01, 0.10);
}

double RiskManager::calculateFeeAwarePositionSize(
    double capital,
    double entry_price,
    double stop_loss,
    double take_profit,
    double fee_rate
) {
    // Risk/Reward ??節뚮쳮雅?(??筌뚯슜鍮???袁⑸즵???
    double risk = std::abs(entry_price - stop_loss) / entry_price;
    double reward = std::abs(take_profit - entry_price) / entry_price;
    
    // ???源놁졆 Risk/Reward (??筌뚯슜鍮??????
    double actual_risk = risk + fee_rate;
    double actual_reward = reward - fee_rate;
    
    if (actual_reward <= 0 || actual_risk <= 0) {
        return 0.0; // ???쒓낮????됰씭????
    }
    
    double rr_ratio = actual_reward / actual_risk;
    
    // RR ?????????ㅻ깹????????釉뚰???
    // RR >= 2.0: 5%
    // RR >= 1.5: 3%
    // RR < 1.5: 癲ル슣????????낆?
    
    if (rr_ratio >= 2.0) {
        return 0.05;
    } else if (rr_ratio >= 1.5) {
        return 0.03;
    } else {
        return 0.0;
    }
}

// ===== ?域밸Ŧ遊얕짆?????굿??=====

bool RiskManager::canTradeMarket(const std::string& market) {
    auto it = last_trade_time_.find(market);
    if (it == last_trade_time_.end()) {
        return true; // 癲?癲꾧퀗????
    }
    
    long long elapsed = getCurrentTimestamp() - it->second;
    return elapsed >= min_reentry_interval_ * 1000; // ms ???쒙쭕?
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

// ===== ???????癲ル슢?꾤땟?????ㅻ깹??=====

RiskManager::RiskMetrics RiskManager::getRiskMetrics() const {
    // [???????곸쓨?????源놁벁?? const 癲ル슢??袁λ빝??筌믨퀡?꾬┼??넊?癲?mutable 癲ル슢???볥뼀?max_capital_, total_fees_paid_)??
    // ???쒋닪??????mutex ????볥뜪 ??ш끽維?? recursive_mutex????????鶯?????源놁벁.
    // ????뽯４??API ?嶺뚮ㅎ??????獒??濚욌꼬裕뼘?? ????縕??????? 癲ル슢?????袁⑤렓???節뚮쳮雅?굞猷듿퐲????얜Ŧ類?????API ??딅텑??????⑤챶苡?
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    RiskMetrics metrics;
    
    // ========== [??좊즵獒뺣돀?????節뚮쳮雅??棺??짆?먰맪? ==========
    
    // 1. ????濚욌꼬?댄꺍?????亦???雅?퍔瑗띰㎖??????????節뚮쳮雅?
    metrics.invested_capital = 0;
    metrics.unrealized_pnl = 0;
    
    for (const auto& [market, pos] : positions_) {
        metrics.invested_capital += pos.invested_amount;
        metrics.unrealized_pnl += pos.unrealized_pnl;
    }
    
    // 2. ??좊읈?????ш낄猷??
    // = ??ш끽維????ш낄猷????釉???current_capital_) - ???怨좊뭿 ???亦?
    // (????濚욌꼬?댄꺍?????亦?? ???? ?????筌뚯슧諭????뽯탞?????怨쀪퐨 ??ш낄猷??????癲ル슓堉곤쭗?ㅒ?????ㅺ컼??
    metrics.reserved_capital = 0.0;
    for (const auto& [m, amount] : reserved_grid_capital_) {
        metrics.reserved_capital += amount;
    }

    metrics.available_capital = current_capital_ - metrics.reserved_capital - pending_order_capital_;
    if (metrics.available_capital < 0) metrics.available_capital = 0.0;
    
    // 3. ?????????좊읈???(Equity)
    // = ??ш낄猷??(current_capital_) + ??????????+ 雅?퍔瑗띰㎖????????
    double current_equity = current_capital_ + metrics.invested_capital + metrics.unrealized_pnl;
    
    // 4. ?????????節뚮쳮雅?
    metrics.realized_pnl = (current_capital_ + metrics.invested_capital) - initial_capital_;
    metrics.total_pnl = current_equity - initial_capital_;
    metrics.total_pnl_pct = (initial_capital_ > 0) ? (metrics.total_pnl / initial_capital_) : 0.0;
    
    // 5. ???????(????筌???節뚮쳮雅?
    // ???????= ??ш낄猷??+ 雅?퍔瑗띰㎖????????
    // (invested_capital?? ???? current_capital_?????癲ル슓堉곤쭗?ㅒ??筌??????濚욌꼬?댄꺇????節뚮쳮雅???ヂ???)
    metrics.total_capital = current_equity;
    
    // 6. MDD (Max Drawdown) ??節뚮쳮雅?
    double peak = std::max(max_capital_, current_equity);
    if (peak > 0) {
        metrics.max_drawdown = (peak - current_equity) / peak;
    } else {
        metrics.max_drawdown = 0.0;
    }
    metrics.current_drawdown = metrics.max_drawdown;
    
    // 7. ?????癲ル슢???⑸눀?
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

    // Sharpe Ratio
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    return trade_history_;
}

void RiskManager::replaceTradeHistory(const std::vector<TradeHistory>& history) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    trade_history_ = history;
}

void RiskManager::appendTradeHistory(const TradeHistory& trade) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    trade_history_.push_back(trade);
}

// [NEW] ??????쒓낮爰????レ챺繹??嶺뚮㉡?€쾮????源놁젳 (ML ???????
void RiskManager::setPositionSignalInfo(
    const std::string& market,
    double signal_filter,
    double signal_strength,
    analytics::MarketRegime market_regime,
    double liquidity_score,
    double volatility,
    double expected_value,
    double reward_risk_ratio
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) {
        LOG_WARN("{} setPositionSignalInfo failed: position not found", market);
        return;
    }
    
    it->second.signal_filter = signal_filter;
    it->second.signal_strength = signal_strength;
    it->second.market_regime = market_regime;
    it->second.liquidity_score = liquidity_score;
    it->second.volatility = volatility;
    it->second.expected_value = expected_value;
    it->second.reward_risk_ratio = reward_risk_ratio;
    LOG_INFO("Signal metadata saved: {} (filter {:.3f}, strength {:.3f}, liq {:.1f}, vol {:.2f}, ev {:.5f}, rr {:.2f})",
             market, signal_filter, signal_strength, liquidity_score, volatility, expected_value, reward_risk_ratio);
}

bool RiskManager::reserveGridCapital(
    const std::string& market,
    double amount,
    const std::string& strategy_name
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;

    resetDailyCountIfNeeded();

    if (reserved_grid_capital_.count(market)) {
        LOG_WARN("{} grid capital already reserved", market);
        return false;
    }

    if (!canTradeMarket(market)) {
        LOG_WARN("{} re-entry cooldown active", market);
        return false;
    }

    if (hasReachedDailyTradeLimit()) {
        LOG_WARN("daily trade limit reached ({}/{})", daily_trade_count_, max_daily_trades_);
        return false;
    }

    if (isDrawdownExceeded()) {
        LOG_ERROR("Drawdown limit exceeded, trading blocked");
        return false;
    }

    if (amount <= 0.0) {
        return false;
    }

    double invested_sum = 0.0;
    for (const auto& [m, pos] : positions_) {
        invested_sum += pos.invested_amount;
    }

    double reserved_sum = 0.0;
    for (const auto& [m, reserved] : reserved_grid_capital_) {
        reserved_sum += reserved;
    }

    double available_cash = current_capital_ - reserved_sum;
    if (available_cash < 0.0) {
        available_cash = 0.0;
    }

    if (amount > available_cash) {
        LOG_WARN("{} grid reserve failed: required {:.0f} > available {:.0f}", market, amount, available_cash);
        return false;
    }

    auto metrics = getRiskMetrics();
    double allowed_investment = metrics.total_capital * max_exposure_pct_;
    if ((invested_sum + reserved_sum + amount) > allowed_investment) {
        LOG_WARN("exposure exceeded: current {:.0f} + new {:.0f} > allowed {:.0f} (ratio {:.2f})",
                 invested_sum + reserved_sum, amount, allowed_investment, max_exposure_pct_);
        return false;
    }

    reserved_grid_capital_[market] = amount;
    last_trade_time_[market] = getCurrentTimestamp();
    daily_trade_count_++;

    LOG_INFO("Grid capital reserved: {} | amount {:.0f} | strategy {}", market, amount, strategy_name);
    return true;
}

double RiskManager::getReservedGridCapital(const std::string& market) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    auto it = reserved_grid_capital_.find(market);
    if (it == reserved_grid_capital_.end()) {
        return 0.0;
    }
    return it->second;
}

void RiskManager::releaseGridCapital(const std::string& market) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;

    auto it = reserved_grid_capital_.find(market);
    if (it == reserved_grid_capital_.end()) {
        return;
    }

    LOG_INFO("Grid capital released: {} | amount {:.0f}", market, it->second);
    reserved_grid_capital_.erase(it);
}

bool RiskManager::applyGridFill(
    const std::string& market,
    strategy::OrderSide side,
    double price,
    double quantity
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;

    if (price <= 0.0 || quantity <= 0.0) {
        return false;
    }

    double amount = price * quantity;
    double fee = calculateFee(amount);

    auto reserved_it = reserved_grid_capital_.find(market);
    if (reserved_it == reserved_grid_capital_.end()) {
        LOG_WARN("Grid capital reservation missing: {}", market);
        return false;
    }

    if (side == strategy::OrderSide::BUY) {
        double total_cost = amount + fee;
        if (reserved_it->second < total_cost) {
            LOG_WARN("Grid reserved capital insufficient: {:.0f} < {:.0f}", reserved_it->second, total_cost);
            return false;
        }
        if (current_capital_ < total_cost) {
            LOG_WARN("Grid buy capital insufficient: {:.0f} < {:.0f}", current_capital_, total_cost);
            return false;
        }

        current_capital_ -= total_cost;
        reserved_it->second -= total_cost;
        total_fees_paid_ += fee;

        auto& inv = grid_inventory_[market];
        inv.last_buy_time = getCurrentTimestamp();
        double new_qty = inv.quantity + quantity;
        if (new_qty > 0.0) {
            inv.avg_price = (inv.avg_price * inv.quantity + amount) / new_qty;
        }
        inv.quantity = new_qty;
        return true;
    }

    auto inv_it = grid_inventory_.find(market);
    if (inv_it == grid_inventory_.end() || inv_it->second.quantity <= 0.0) {
        LOG_WARN("Grid sell failed (no inventory): {}", market);
        return false;
    }

    auto& inv = inv_it->second;
    if (inv.quantity < quantity) {
        LOG_WARN("Grid sell quantity exceeds inventory: {:.8f} > {:.8f}", quantity, inv.quantity);
        quantity = inv.quantity;
        amount = price * quantity;
        fee = calculateFee(amount);
    }

    current_capital_ += (amount - fee);
    reserved_it->second += (amount - fee);
    total_fees_paid_ += fee;

    TradeHistory trade;
    trade.market = market;
    trade.entry_price = inv.avg_price;
    trade.exit_price = price;
    trade.quantity = quantity;
    double entry_fee = calculateFee(inv.avg_price * quantity);
    trade.fee_paid = entry_fee + fee;
    trade.profit_loss = (price - inv.avg_price) * quantity - trade.fee_paid;
    trade.profit_loss_pct = (inv.avg_price > 0.0)
        ? (trade.profit_loss / (inv.avg_price * quantity))
        : 0.0;
    trade.entry_time = inv.last_buy_time;
    trade.exit_time = getCurrentTimestamp();
    trade.strategy_name = "Grid Trading Strategy";
    trade.exit_reason = "grid_cycle";
    trade_history_.push_back(trade);

    inv.quantity -= quantity;
    if (inv.quantity <= 0.0) {
        inv.quantity = 0.0;
        inv.avg_price = 0.0;
        inv.last_buy_time = 0;
    }

    return true;
}

// ===== ???源놁젳 =====

void RiskManager::setMaxPositions(int max_positions) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    max_positions_ = max_positions;
    LOG_INFO("Max positions set: {}", max_positions);
}

void RiskManager::setMaxDailyTrades(int max_trades) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    max_daily_trades_ = max_trades;
    LOG_INFO("Max daily trades set: {}", max_trades);
}

void RiskManager::setMaxDrawdown(double max_drawdown_pct) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    max_drawdown_pct_ = max_drawdown_pct;
    LOG_INFO("Max drawdown set: {:.1f}%", max_drawdown_pct * 100);
}

void RiskManager::setMaxExposurePct(double pct) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    if (pct <= 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    max_exposure_pct_ = pct;
    LOG_INFO("Max exposure set: {:.2f}%", max_exposure_pct_ * 100.0);
}

void RiskManager::setMinReentryInterval(int seconds) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    min_reentry_interval_ = seconds;
    LOG_INFO("Re-entry interval set: {}s", seconds);
}

// ===== Private ??????縕??=====

double RiskManager::calculateFee(double amount) const {
    return amount * Config::getInstance().getFeeRate();
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
    
    // [NEW] ML ??????????レ챺繹??嶺뚮㉡?€쾮?????
    trade.signal_filter = pos.signal_filter;
    trade.signal_strength = pos.signal_strength;
    trade.market_regime = pos.market_regime;
    trade.liquidity_score = pos.liquidity_score;
    trade.volatility = pos.volatility;
    trade.expected_value = pos.expected_value;
    trade.reward_risk_ratio = pos.reward_risk_ratio;
    
    // ???????節뚮쳮雅?
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
    // ??癰궽살쐿 ?????KST) ??れ삀?? ???源놁벁 9???域밸Ŧ遊??
    // UTC ??れ삀?? ???モ???좊읈? ?袁⑸즴?????獒???筌믨퀣?????癰궽살쐿 ??癰??????源놁벁 9??筌믨퀡?????덊렡.
    
    time_t now = time(nullptr);
    struct tm* tm_now = gmtime(&now); // UTC ??れ삀?? ????깼???삳읁?(POSIX ?嶺뚮ㅏ援??
    
    // ????몄툜 ???モ????嶺뚮Ĳ???띿뒙??怨뚮뼚???(YYYYMMDD)
    long long current_day = (tm_now->tm_year + 1900) * 10000 + (tm_now->tm_mon + 1) * 100 + tm_now->tm_mday;
    
    if (daily_reset_time_ == 0) {
        daily_reset_time_ = current_day;
    }
    
    // ???潁뺛꺈彛????モ??? ????렺?異?(癲? ??嚥〓끉???좊읈? 癲ル슣???????좎떵? ?域밸Ŧ遊??
    if (current_day != daily_reset_time_) {
        LOG_INFO("UTC day changed -> daily trade count reset");
        daily_trade_count_ = 0;
        daily_reset_time_ = current_day;
    }
}

void RiskManager::resetDailyLossIfNeeded() {
    time_t now = time(nullptr);
    struct tm* tm_now = gmtime(&now);
    long long current_day = (tm_now->tm_year + 1900) * 10000 + (tm_now->tm_mon + 1) * 100 + tm_now->tm_mday;

    if (daily_start_date_ == 0) {
        daily_start_date_ = current_day;
        daily_start_capital_ = current_capital_;
        return;
    }

    if (current_day != daily_start_date_) {
        daily_start_date_ = current_day;
        daily_start_capital_ = current_capital_;
    }
}

void RiskManager::updateCapital(double amount_change) {
    current_capital_ += amount_change;
    
    // 癲ル슔?됭짆?????亦낆떓堉???좊즲???(MDD ??節뚮쳮雅??
    if (current_capital_ > max_capital_) {
        max_capital_ = current_capital_;
    }
}

} // namespace risk
} // namespace autolife



