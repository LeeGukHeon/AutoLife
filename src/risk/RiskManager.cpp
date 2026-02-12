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
    , max_daily_trades_(100)   // [Phase 2] 20→100 (전략 다양화 지원)
    , max_drawdown_pct_(0.10)  // 10%
    , max_exposure_pct_(0.85)  // 총 자본 대비 기본 85%
    , min_reentry_interval_(60)   // [Phase 2] 300초→60초 (기회 손실 방지)
    , max_capital_(initial_capital)
    , total_fees_paid_(0)
    , min_order_krw_(5000.0)
    , recommended_min_enter_krw_(6000.0)
{
    LOG_INFO("RiskManager 초기화 - 초기 자본: {:.0f} KRW", initial_capital);
    resetDailyCountIfNeeded();
    resetDailyLossIfNeeded();
}

// ===== 포지션 진입 =====

bool RiskManager::canEnterPosition(
    const std::string& market,
    double entry_price,
    double position_size_ratio,
    const std::string& strategy_name
) {
    // 스레드 안전성
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    // [자정 리셋] daily_trade_count 자동 리셋
    resetDailyCountIfNeeded();
    resetDailyLossIfNeeded();
    
    // [1] 이미 포지션 보유 중인지 확인
    if (getPosition(market) != nullptr) {
        LOG_WARN("{} 이미 포지션 보유 중", market);
        return false;
    }
    
    // [2] 최대 포지션 개수 초과
    if (hasReachedMaxPositions()) {
        LOG_WARN("최대 포지션 개수 도달 ({}/{})", positions_.size(), max_positions_);
        return false;
    }
    
    // [3] 일일 거래 횟수 제한
    if (hasReachedDailyTradeLimit()) {
        LOG_WARN("일일 거래 횟수 제한 도달 ({}/{})", daily_trade_count_, max_daily_trades_);
        return false;
    }
    
    // [4] 재진입 대기 시간
    if (!canTradeMarket(market)) {
        LOG_WARN("{} 재진입 대기 중", market);
        return false;
    }
    
    // [5] Drawdown 초과
    if (isDrawdownExceeded()) {
        LOG_ERROR("Drawdown 한계 초과! 거래 중단");
        return false;
    }
    
    // ========== [핵심] 자금 관리 로직 ==========
    
    // [투자액 계산] 모든 진행 중인 포지션의 총 투자액
    double invested_sum = 0.0;
    for (const auto& [m, pos] : positions_) {
        invested_sum += pos.invested_amount;
    }

    double reserved_sum = 0.0;
    for (const auto& [m, amount] : reserved_grid_capital_) {
        reserved_sum += amount;
    }
    
    // [가용자본] 현금 잔고 기준 (투자 중인 금액은 이미 포지션에 묶여 있음)
    double available_cash = current_capital_ - reserved_sum;
    if (available_cash < 0) available_cash = 0.0;
    
    // [최소값 기준]
    const double MIN_ORDER_KRW = Config::getInstance().getMinOrderKrw();
    const double RECOMMENDED_MIN_ENTER_KRW = MIN_ORDER_KRW * 1.05; // [Phase 2] 5,250원 (소액 시드 지원)
    
    // [추가] 기본 손절 3% 적용 후에도 최소 주문금액+매도 수수료를 만족해야 함
    const double BASE_STOP_LOSS_PCT = 0.03;
    const double FEE_RATE = Config::getInstance().getFeeRate();
    
    // [position_size_ratio 정규화]
    double normalized_ratio = std::clamp(position_size_ratio, 0.0, 1.0);
    if (std::fabs(normalized_ratio - position_size_ratio) > 0.001) {
        LOG_WARN("{} - position_size_ratio 비정상: {:.4f} → {:.4f} (정규화됨)",
                 market, position_size_ratio, normalized_ratio);
    }
    
    // [가용자본 확인] 최소값 미만이면 진입 불가
    if (available_cash < RECOMMENDED_MIN_ENTER_KRW) {
        LOG_WARN("{}★ 매수 불가: 가용자본 {:.0f}원 < 권장최소진입 {:.0f}원",
                 market, available_cash, RECOMMENDED_MIN_ENTER_KRW);
        return false;
    }
    
    // [필요액 계산]
    double required_amount = available_cash * normalized_ratio;
    
    // [최소 주문액 체크]
    if (required_amount < MIN_ORDER_KRW) {
        LOG_WARN("{}★ 매수 불가: 진입액 {:.0f}원 < 업비트최소주문 {:.0f}원 (비중 {:.4f}→추천불가)",
                 market, required_amount, MIN_ORDER_KRW, normalized_ratio);
        return false;
    }

    // [Phase 2] 손절 후 매도 최소금액 검사 → 경고만 (소액 시드에서 진입 차단 방지)
    double min_required_for_exit = MIN_ORDER_KRW / ((1.0 - BASE_STOP_LOSS_PCT) * (1.0 - FEE_RATE));
    if (required_amount < min_required_for_exit) {
        LOG_WARN("{}⚠ 주의: 손절 시 최소 매도금액 미충족 가능 (필요 {:.0f}원, 현재 {:.0f}원)",
                 market, min_required_for_exit, required_amount);
        // [Phase 2] hard block 제거 - 소액 시드에서도 진입 허용
    }
    
    // [권장 최소값 체크]
    if (required_amount < RECOMMENDED_MIN_ENTER_KRW) {
        LOG_WARN("{}★ 매수 불가: 진입액 {:.0f}원 < 권장최소진입 {:.0f}원 (비중 {:.4f})",
                 market, required_amount, RECOMMENDED_MIN_ENTER_KRW, normalized_ratio);
        return false;
    }
    
    // [범위 확인]
    if (required_amount > available_cash) {
        LOG_ERROR("[내부오류] 계산 오류: 필요액 {:.0f} > 가용액 {:.0f}", 
                  required_amount, available_cash);
        return false;
    }
    
    // [최악의 경우 손실 추정]
    double max_drawdown_per_trade = current_capital_ * (max_drawdown_pct_ / max_positions_);
    const double WORST_CASE_PRICE_MOVE_PCT = 0.02;   // 2% worst-case move
    const double ESTIMATED_TOTAL_FEE_PCT = 0.002;    // 0.2% total fees
    double worst_case_loss_pct = WORST_CASE_PRICE_MOVE_PCT + ESTIMATED_TOTAL_FEE_PCT;
    double estimated_worst_loss = required_amount * worst_case_loss_pct;

    if (estimated_worst_loss > max_drawdown_per_trade) {
        LOG_WARN("거래액이 최대 손실 한계 초과: 예상최대손실 {:.0f} > 허용한도 {:.0f}",
                 estimated_worst_loss, max_drawdown_per_trade);
        return false;
    }

    // [총 노출 확인]
    auto metrics = getRiskMetrics();
    double total_capital = metrics.total_capital;
    double allowed_investment = total_capital * max_exposure_pct_;
    if ((invested_sum + reserved_sum + required_amount) > allowed_investment) {
        LOG_WARN("허용 투자 한도 초과: 현재투자 {:.0f} + 필요 {:.0f} > 허용 {:.0f} (비율 {:.2f})",
                 invested_sum + reserved_sum, required_amount, allowed_investment, max_exposure_pct_);
        return false;
    }
    
    // [성공]
    LOG_INFO("{}✅ 매수 검증 성공: 가용자본 {:.0f}원, 진입액 {:.0f}원 (비중 {:.4f}={}%)",
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
    LOG_INFO("일 손실 한도 설정: {:.2f}%", daily_loss_limit_pct_ * 100.0);
}

void RiskManager::setDailyLossLimitKrw(double krw) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (krw < 0.0) krw = 0.0;
    daily_loss_limit_krw_ = krw;
    LOG_INFO("일 손실 한도 설정: {:.0f} KRW", daily_loss_limit_krw_);
}

void RiskManager::setMinOrderKrw(double min_order_krw) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (min_order_krw < 0.0) min_order_krw = 0.0;
    min_order_krw_ = min_order_krw;
    recommended_min_enter_krw_ = std::max(6000.0, min_order_krw_ * 1.2);
    LOG_INFO("최소 주문 금액 설정: {:.0f} KRW (권장 진입: {:.0f} KRW)",
             min_order_krw_, recommended_min_enter_krw_);
}

bool RiskManager::isDailyLossLimitExceeded() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const_cast<RiskManager*>(this)->resetDailyLossIfNeeded();

    if (daily_start_capital_ <= 0.0) return false;

    double unrealized = 0.0;
    for (const auto& [market, pos] : positions_) {
        unrealized += pos.unrealized_pnl;
    }

    double equity = current_capital_ + unrealized;
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

    double unrealized = 0.0;
    for (const auto& [market, pos] : positions_) {
        unrealized += pos.unrealized_pnl;
    }

    double equity = current_capital_ + unrealized;
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
    // [🔒 스레드 안전성] 포지션 추가는 TradingEngine 메인 스레드에서만 호출되므로
    // 교착 상태 위험 없음. 단 recursive_mutex로 설정되어 있어 재진입도 안전.
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    // [핵심 수정] 진입 금액 계산: 수수료 미리 포함
    // 업비트 진입 수수료는 1회 0.05% (진입시) → invested_amount에 포함해야 함
    double base_invested = entry_price * quantity;
    double entry_fee = calculateFee(base_invested);  // 진입 수수료 (0.05%)
    
    // Position 객체에 수수료를 포함한 투자액 저장
    Position pos;
    pos.market = market;
    pos.entry_price = entry_price;
    pos.current_price = entry_price;
    pos.quantity = quantity;
    pos.invested_amount = base_invested;  // [수정] 수수료는 현금에서 차감, 원금은 별도 추적
    pos.entry_time = getCurrentTimestamp();
    pos.stop_loss = stop_loss;
    pos.take_profit_1 = take_profit_1;
    pos.take_profit_2 = take_profit_2;
    pos.strategy_name = strategy_name;
    pos.half_closed = false;
    pos.highest_price = entry_price;  // [추가] Trailing SL용 최고가 초기화
    pos.breakeven_trigger = breakeven_trigger;
    pos.trailing_start = trailing_start;
    
    // [핵심] 자본금에서 수수료 차감
    // 현금: 주식 구매비 + 수수료 모두 차감됨
    current_capital_ -= entry_fee;
    total_fees_paid_ += entry_fee;
    
    // 포지션 등록
    positions_[market] = pos;
    
    // 거래 제한 및 통계 기록
    last_trade_time_[market] = getCurrentTimestamp();
    daily_trade_count_++;
    
    LOG_INFO("🔵 포지션 진입: {} | 전략: {} | 수량: {:.4f} | 구매가: {:.0f} | 수수료: {:.0f} | 투자원금: {:.0f} | 남은현금: {:.0f}",
             market, strategy_name, quantity, base_invested, entry_fee, base_invested, current_capital_);
    LOG_INFO("   └ 손절 {:.0f} ({:+.2f}%), 익절1 {:.0f} ({:+.2f}%), 익절2 {:.0f} ({:+.2f}%)",
             stop_loss, (stop_loss - entry_price) / entry_price * 100.0,
             take_profit_1, (take_profit_1 - entry_price) / entry_price * 100.0,
             take_profit_2, (take_profit_2 - entry_price) / entry_price * 100.0);
}

// ===== 포지션 업데이트 =====

void RiskManager::updatePosition(const std::string& market, double current_price) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    pos.current_price = current_price;
    if (pos.highest_price <= 0.0 || current_price > pos.highest_price) {
        pos.highest_price = current_price;
    }
    
    // 미실현 손익 계산
    double current_value = current_price * pos.quantity;
    pos.unrealized_pnl = current_value - pos.invested_amount;
    pos.unrealized_pnl_pct = (current_price - pos.entry_price) / pos.entry_price;
}

// ===== 포지션 청산 체크 =====

bool RiskManager::shouldExitPosition(const std::string& market) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    
    // 1. 매도 총액 계산
    double exit_value = exit_price * pos.quantity;
    
    // [개선] 청산 수수료 계산 및 차감
    // 업비트 청산 수수료: 0.25%
    double exit_fee = calculateFee(exit_value);
    
    // 2. 순수 확정 손익 계산
    // Net Profit = (매도총액 - 매도수수료) - 진입금액
    // (진입 수수료는 이미 차감됨)
    double net_profit = (exit_value - exit_fee) - pos.invested_amount;
    
    // 3. 자본금 업데이트
    // 현금 = 현금 + (매도총액 - 청산수수료)
    current_capital_ += (exit_value - exit_fee);
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
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

// [Phase 3] 부분 체결 시 수량만 감소 (포지션 유지)
void RiskManager::updatePositionQuantity(const std::string& market, double new_quantity) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    auto it = positions_.find(market);
    if (it == positions_.end()) {
        LOG_WARN("updatePositionQuantity: 포지션 없음 - {}", market);
        return;
    }
    
    auto& pos = it->second;
    double old_quantity = pos.quantity;
    double sold_quantity = old_quantity - new_quantity;
    
    // 매도된 만큼 자본금 회수
    double freed_capital = sold_quantity * pos.entry_price;
    current_capital_ += freed_capital;
    
    // 포지션 수량 및 투자금 업데이트
    pos.quantity = new_quantity;
    pos.invested_amount = new_quantity * pos.entry_price;
    
    LOG_INFO("📊 포지션 수량 업데이트: {} ({:.8f} → {:.8f}), 자본 회수: {:.0f}원",
             market, old_quantity, new_quantity, freed_capital);
}

void RiskManager::moveStopToBreakeven(const std::string& market) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) return;
    
    auto& pos = it->second;
    pos.stop_loss = pos.entry_price;
    
    LOG_INFO("{} 손절선 본전 이동: {:.0f}", market, pos.entry_price);
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
    LOG_INFO("{} 손절선 상향 ({}): {:.0f}", market, reason, new_stop_loss);
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

// ===== 손절 계산 =====

double RiskManager::calculateDynamicStopLoss(
    double entry_price,
    const std::vector<Candle>& candles
) {
    if (candles.size() < 14) {
        return entry_price * 0.975; // Fallback: -2.5%
    }
    
    // 1. Hard Stop (긴급 손절) - 완화됨
    double hard_stop = entry_price * 0.975; // -2.5%
    
    // 2. ATR-based Stop (배수 상향: 더 큰 손절 범위)
    double atr_stop = calculateATRStopLoss(entry_price, candles, 2.5);
    
    // 3. Support-based Stop
    double support_stop = calculateSupportStopLoss(entry_price, candles);
    
    // 보수적인(완충 폭이 큰) 손절가 선택: 가장 낮은(=더 멀리) 손절가를 선택하여
    // 과도한 조기 청산을 방지합니다.
    double final_stop = std::min({hard_stop, atr_stop, support_stop});

    // 안전장치: 진입가보다 높으면 기본 -2.5%로 설정
    if (final_stop >= entry_price) {
        final_stop = entry_price * 0.975;
    }

    LOG_INFO("손절가 계산: Hard={:.0f}, ATR={:.0f}, Support={:.0f}, Final={:.0f}",
              hard_stop, atr_stop, support_stop, final_stop);
    LOG_INFO("손절 정책: 보수적(완충) 선택 적용, 최소 -2.5% 보장");

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
    
    // 최소 -2.5%, 최대 -3.5% (손절 범위 완화)
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
    
    // 진입가보다 낮은 가장 가까운 지지선 찾기
    double nearest_support = 0;
    for (auto level : support_levels) {
        if (level < entry_price && level > nearest_support) {
            nearest_support = level;
        }
    }
    
    if (nearest_support > 0) {
        // 지지선 아래 0.5% (여유있게)
        return nearest_support * 0.995;
    }
    
    return entry_price * 0.975; // -2.5%
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
    // [🔒 스레드 안전성] const 메서드이지만 mutable 멤버(max_capital_, total_fees_paid_)에
    // 접근하므로 mutex 획득 필요. recursive_mutex이므로 재진입 안전.
    // 업비트 API 호출 규칙 준수: 이 함수는 내부 메모리 계산만 수행하므로 API 부담 없음.
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    RiskMetrics metrics;
    
    // ========== [개선된 계산 로직] ==========
    
    // 1. 투자 중인 자본 및 미실현 손익 계산
    metrics.invested_capital = 0;
    metrics.unrealized_pnl = 0;
    
    for (const auto& [market, pos] : positions_) {
        metrics.invested_capital += pos.invested_amount;
        metrics.unrealized_pnl += pos.unrealized_pnl;
    }
    
    // 2. 가용 현금
    // = 현재 현금 잔고(current_capital_) - 예약 자본
    // (투자 중인 자본은 이미 포지션에 묶여 있어 현금에서 차감된 상태)
    metrics.reserved_capital = 0.0;
    for (const auto& [m, amount] : reserved_grid_capital_) {
        metrics.reserved_capital += amount;
    }

    metrics.available_capital = current_capital_ - metrics.reserved_capital;
    if (metrics.available_capital < 0) metrics.available_capital = 0.0;
    
    // 3. 총 자산 가치 (Equity)
    // = 현금 (current_capital_) + 포지션 평가액 + 미실현 손익
    double current_equity = current_capital_ + metrics.unrealized_pnl;
    
    // 4. 총 손익 계산
    metrics.realized_pnl = current_capital_ - initial_capital_;  // 확정 손익
    metrics.total_pnl = metrics.realized_pnl + metrics.unrealized_pnl;  // 전체 손익
    metrics.total_pnl_pct = (initial_capital_ > 0) ? (metrics.total_pnl / initial_capital_) : 0.0;
    
    // 5. 총 자산 (올바른 계산)
    // 총 자산 = 현금 + 미실현 손익
    // (invested_capital은 이미 current_capital_에서 차감되었으므로 중복 계산 금지)
    metrics.total_capital = current_capital_ + metrics.unrealized_pnl;
    
    // 6. MDD (Max Drawdown) 계산
    double peak = std::max(max_capital_, current_equity);
    if (peak > 0) {
        metrics.max_drawdown = (peak - current_equity) / peak;
    } else {
        metrics.max_drawdown = 0.0;
    }
    metrics.current_drawdown = metrics.max_drawdown;
    
    // 7. 통계 매핑
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

// [NEW] 포지션의 신호 정보 설정 (ML 학습용)
void RiskManager::setPositionSignalInfo(
    const std::string& market,
    double signal_filter,
    double signal_strength
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    
    auto it = positions_.find(market);
    if (it == positions_.end()) {
        LOG_WARN("{} 포지션을 찾을 수 없어 신호 정보 설정 실패", market);
        return;
    }
    
    it->second.signal_filter = signal_filter;
    it->second.signal_strength = signal_strength;
    LOG_INFO("신호 정보 저장: {} (필터: {:.3f}, 강도: {:.3f})", 
             market, signal_filter, signal_strength);
}

bool RiskManager::reserveGridCapital(
    const std::string& market,
    double amount,
    const std::string& strategy_name
) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;

    resetDailyCountIfNeeded();

    if (reserved_grid_capital_.count(market)) {
        LOG_WARN("{} 그리드 자본 이미 예약됨", market);
        return false;
    }

    if (!canTradeMarket(market)) {
        LOG_WARN("{} 재진입 대기 중", market);
        return false;
    }

    if (hasReachedDailyTradeLimit()) {
        LOG_WARN("일일 거래 횟수 제한 도달 ({}/{})", daily_trade_count_, max_daily_trades_);
        return false;
    }

    if (isDrawdownExceeded()) {
        LOG_ERROR("Drawdown 한계 초과! 거래 중단");
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
        LOG_WARN("{} 그리드 자본 예약 실패: 필요 {:.0f} > 가용 {:.0f}", market, amount, available_cash);
        return false;
    }

    auto metrics = getRiskMetrics();
    double allowed_investment = metrics.total_capital * max_exposure_pct_;
    if ((invested_sum + reserved_sum + amount) > allowed_investment) {
        LOG_WARN("허용 투자 한도 초과: 현재투자 {:.0f} + 필요 {:.0f} > 허용 {:.0f} (비율 {:.2f})",
                 invested_sum + reserved_sum, amount, allowed_investment, max_exposure_pct_);
        return false;
    }

    reserved_grid_capital_[market] = amount;
    last_trade_time_[market] = getCurrentTimestamp();
    daily_trade_count_++;

    LOG_INFO("그리드 자본 예약: {} | 금액 {:.0f} | 전략: {}", market, amount, strategy_name);
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

    LOG_INFO("그리드 자본 해제: {} | 금액 {:.0f}", market, it->second);
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
        LOG_WARN("그리드 자본 예약 없음: {}", market);
        return false;
    }

    if (side == strategy::OrderSide::BUY) {
        double total_cost = amount + fee;
        if (reserved_it->second < total_cost) {
            LOG_WARN("그리드 예약 자본 부족: {:.0f} < {:.0f}", reserved_it->second, total_cost);
            return false;
        }
        if (current_capital_ < total_cost) {
            LOG_WARN("그리드 매수 자본 부족: {:.0f} < {:.0f}", current_capital_, total_cost);
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
        LOG_WARN("그리드 매도 실패(재고 없음): {}", market);
        return false;
    }

    auto& inv = inv_it->second;
    if (inv.quantity < quantity) {
        LOG_WARN("그리드 매도 수량 초과: {:.8f} > {:.8f}", quantity, inv.quantity);
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

// ===== 설정 =====

void RiskManager::setMaxPositions(int max_positions) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    max_positions_ = max_positions;
    LOG_INFO("최대 포지션 설정: {}", max_positions);
}

void RiskManager::setMaxDailyTrades(int max_trades) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    max_daily_trades_ = max_trades;
    LOG_INFO("일일 최대 거래 설정: {}", max_trades);
}

void RiskManager::setMaxDrawdown(double max_drawdown_pct) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    max_drawdown_pct_ = max_drawdown_pct;
    LOG_INFO("최대 Drawdown 설정: {:.1f}%", max_drawdown_pct * 100);
}

void RiskManager::setMaxExposurePct(double pct) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    if (pct <= 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    max_exposure_pct_ = pct;
    LOG_INFO("최대 노출 비율 설정: {:.2f}%", max_exposure_pct_ * 100.0);
}

void RiskManager::setMinReentryInterval(int seconds) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);;
    min_reentry_interval_ = seconds;
    LOG_INFO("재진입 대기 시간 설정: {}초", seconds);
}

// ===== Private 헬퍼 함수 =====

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
    
    // [NEW] ML 학습용 신호 정보 저장
    trade.signal_filter = pos.signal_filter;
    trade.signal_strength = pos.signal_strength;
    
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
    struct tm* tm_now = gmtime(&now); // UTC 기준 구조체 (POSIX 호환)
    
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
    
    // 최고 자본금 갱신 (MDD 계산용)
    if (current_capital_ > max_capital_) {
        max_capital_ = current_capital_;
    }
}

} // namespace risk
} // namespace autolife
