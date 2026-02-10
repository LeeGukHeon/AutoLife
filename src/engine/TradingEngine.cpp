#include "engine/TradingEngine.h"
#include "common/Logger.h"
#include "strategy/ScalpingStrategy.h"
#include "strategy/MomentumStrategy.h" 
#include "strategy/BreakoutStrategy.h"
#include "strategy/MeanReversionStrategy.h"
#include "strategy/GridTradingStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include "risk/RiskManager.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <map>
#include <cmath>
#include <set>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

// Using declarations for risk namespace types
using autolife::risk::TradeHistory;
using autolife::risk::Position;

namespace autolife {
namespace engine {

TradingEngine::TradingEngine(
    const EngineConfig& config,
    std::shared_ptr<network::UpbitHttpClient> http_client
)
    : config_(config)
    , http_client_(http_client)
    , running_(false)
    , start_time_(0)
    , total_scans_(0)
    , total_signals_(0)
{
    LOG_INFO("TradingEngine 초기화");
    LOG_INFO("모드: {}", config.mode == TradingMode::LIVE ? "LIVE" : "PAPER");
    LOG_INFO("초기 자본: {:.0f} KRW", config.initial_capital);
    
    // 안전 설정 로깅 (새로 추가)
    if (config.mode == TradingMode::LIVE) {
        LOG_INFO("실전 안전 설정:");
        LOG_INFO("  일일 최대 손실: {:.0f} KRW", config.max_daily_loss_krw);
        LOG_INFO("  단일 주문 최대: {:.0f} KRW", config.max_order_krw);
        LOG_INFO("  단일 주문 최소: {:.0f} KRW", config.min_order_krw);
        LOG_INFO("  Dry Run: {}", config.dry_run ? "ON" : "OFF");
    }
    
    // 모듈 초기화
    scanner_ = std::make_unique<analytics::MarketScanner>(http_client);
    strategy_manager_ = std::make_unique<strategy::StrategyManager>(http_client);
    risk_manager_ = std::make_unique<risk::RiskManager>(config.initial_capital);
    // Apply engine-level risk settings to RiskManager
    risk_manager_->setMaxPositions(config.max_positions);
    risk_manager_->setMaxDailyTrades(config.max_daily_trades);
    risk_manager_->setMaxDrawdown(config.max_drawdown);
    risk_manager_->setMaxExposurePct(config.max_exposure_pct);
    
    // 전략 등록: `config_.enabled_strategies`가 비어있으면 모든 전략을 등록합니다.
    std::set<std::string> enabled;
    for (const auto &s : config_.enabled_strategies) {
        enabled.insert(s);
    }

    auto should_register = [&](const std::string &name) {
        return enabled.empty() || enabled.count(name) > 0;
    };

    if (should_register("scalping")) {
        auto scalping = std::make_shared<strategy::ScalpingStrategy>(http_client);
        strategy_manager_->registerStrategy(scalping);
        LOG_INFO("스캘핑 전략 등록 완료");
    }

    if (should_register("momentum")) {
        auto momentum = std::make_shared<strategy::MomentumStrategy>(http_client);
        strategy_manager_->registerStrategy(momentum);
        LOG_INFO("모멘텀 전략 등록 완료");
    }

    if (should_register("breakout")) {
        auto breakout = std::make_shared<strategy::BreakoutStrategy>(http_client);
        strategy_manager_->registerStrategy(breakout);
        LOG_INFO("브레이크아웃 전략 등록 완료");
    }

    if (should_register("mean_reversion")) {
        auto mean_rev = std::make_shared<strategy::MeanReversionStrategy>(http_client);
        strategy_manager_->registerStrategy(mean_rev);
        LOG_INFO("MeanReversion 전략 등록 완료");
    }

    if (should_register("grid_trading")) {
        auto grid = std::make_shared<strategy::GridTradingStrategy>(http_client);
        strategy_manager_->registerStrategy(grid);
        LOG_INFO("GridTrading 전략 등록 완료");
    }
}

TradingEngine::~TradingEngine() {
    stop();
}

// ===== 엔진 제어 =====

bool TradingEngine::start() {
    if (running_) {
        LOG_WARN("엔진이 이미 실행 중입니다");
        return false;
    }
    
    LOG_INFO("========================================");
    LOG_INFO("거래 엔진 시작");
    LOG_INFO("========================================");
    
    // [추가] 시작하기 전에 내 지갑 상태 확인!
    if (config_.mode == TradingMode::LIVE) {
        syncAccountState();
    }

    running_ = true;
    start_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    // 워커 스레드 시작
    worker_thread_ = std::make_unique<std::thread>(&TradingEngine::run, this);
    
    // [NEW] Prometheus HTTP 서버 시작 (별도 스레드)
    prometheus_http_thread_ = std::make_unique<std::thread>(&TradingEngine::runPrometheusHttpServer, this, 8080);
    LOG_INFO("✅ Prometheus HTTP 서버 시작 (포트: 8080)");
    
    return true;
}

void TradingEngine::stop() {
    if (!running_) {
        return;
    }
    
    LOG_INFO("========================================");
    LOG_INFO("거래 엔진 중지");
    LOG_INFO("========================================");
    
    running_ = false;
    
    // [NEW] Prometheus HTTP 서버 종료
    prometheus_server_running_ = false;
    if (prometheus_http_thread_ && prometheus_http_thread_->joinable()) {
        prometheus_http_thread_->join();
    }
    
    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }
    
    // 최종 성과 출력
    logPerformance();
}

// ===== 메인 루프 =====

void TradingEngine::run() {
    LOG_INFO("🚀 메인 거래 루프 시작");
    
    auto scan_interval = std::chrono::seconds(config_.scan_interval_seconds);
    auto last_scan_time = std::chrono::steady_clock::now() - scan_interval;  
    // 포지션 감시 주기 (고정: 0.5초) - 안정
    auto monitor_interval = std::chrono::milliseconds(500); // 0.5초 주기
    // [✅ 추가] 마지막 계좌 동기화 시간 기록용 변수
    auto last_account_sync_time = std::chrono::steady_clock::now();
    // 동기화 주기 (예: 5분 = 300초)
    auto account_sync_interval = std::chrono::seconds(300);

    while (running_) {
        auto tick_start = std::chrono::steady_clock::now();

        try {
            // ==========================================
            // 1. [Fast Track] 포지션 감시 (최우선 순위)
            // ==========================================
            // 스캔 중이라도 내 돈(보유 포지션)은 지켜야 함
            monitorPositions();
            // =========================================================
            // [✅ 추가] 정기 계좌 동기화 (입출금 감지용)
            // =========================================================
            auto now = std::chrono::steady_clock::now();
            if (config_.mode == TradingMode::LIVE) { // 실전 모드일 때만
                if (now - last_account_sync_time >= account_sync_interval) {
                    LOG_INFO("🔄 정기 계좌 동기화 (입출금 내역 갱신)");
                    syncAccountState(); // 여기서 잔고를 다시 긁어와서 RiskManager에 덮어씁니다.
                    last_account_sync_time = now;
                }
            }
            // =========================================================
            // ==========================================
            // 2. [Slow Track] 시장 스캔 및 신규 진입
            // ==========================================
            auto elapsed_since_scan = std::chrono::duration_cast<std::chrono::seconds>(now - last_scan_time);
            
            // 스캔 주기가 되었을 때만 실행
            if (elapsed_since_scan >= scan_interval) {
                
                LOG_INFO("🔍 정기 스캔 수행 (지난 스캔 후 {}초 경과)", elapsed_since_scan.count());
                
                // 스캔 -> 신호 생성 -> 매수 실행
                scanMarkets();
                generateSignals();
                
                // ===== [NEW] ML 기반 필터값 자동 학습 (스캔 주기마다) =====
                learnOptimalFilterValue();
                
                executeSignals();
                
                // 스캔 시간 갱신
                last_scan_time = std::chrono::steady_clock::now();
                
                // 메트릭 업데이트 (너무 자주 할 필요 없음)
                updateMetrics();
            }

            // ==========================================
            // 3. [Smart Sleep] 남은 시간만큼만 대기
            // ==========================================
            auto tick_end = std::chrono::steady_clock::now();
            auto tick_duration = tick_end - tick_start;
            auto sleep_duration = monitor_interval - tick_duration;

            if (sleep_duration.count() > 0) {
                std::this_thread::sleep_for(sleep_duration);
            }

        } catch (const std::exception& e) {
            LOG_ERROR("메인 루프 치명적 에러: {}", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(1)); // 에러 시 잠시 대기
        }
    }
    
    LOG_INFO("메인 거래 루프 종료");
}

// ===== 시장 스캔 (수정) =====

void TradingEngine::scanMarkets() {
    LOG_INFO("===== 시장 스캔 시작 =====");
    
    total_scans_++;
    
    // 전체 마켓 스캔
    scanned_markets_ = scanner_->scanAllMarkets();
    
    if (scanned_markets_.empty()) {
        LOG_WARN("스캔 결과 없음");
        return;
    }
    
    // 점수 순으로 정렬 (내림차순)
    std::sort(scanned_markets_.begin(), scanned_markets_.end(),
        [](const analytics::CoinMetrics& a, const analytics::CoinMetrics& b) {
            return a.composite_score > b.composite_score;
        });
    
    // 최소 거래량 필터링
    std::vector<analytics::CoinMetrics> filtered;
    for (const auto& coin : scanned_markets_) {
        if (coin.volume_24h >= config_.min_volume_krw) {
            filtered.push_back(coin);
        }
    }
    
    // 상위 20개만 유지
    if (filtered.size() > 20) {
        filtered.resize(20);
    }
    
    scanned_markets_ = filtered;
    
    LOG_INFO("스캔 완료: {}개 코인 발견 (거래량 필터링 후)", scanned_markets_.size());
    
    // 상위 5개 출력
    int count = 0;
    for (const auto& coin : scanned_markets_) {
        if (count++ >= 5) break;
        
        LOG_INFO("  #{} {} - 점수: {:.1f}, 거래량: {:.0f}억, 변동성: {:.2f}%",
                 count, coin.market, coin.composite_score,
                 coin.volume_24h / 100000000.0,
                 coin.volatility);
    }
}

// ===== 매매 신호 생성 (수정) =====

void TradingEngine::generateSignals() {
    if (scanned_markets_.empty()) {
        return;
    }
    
    LOG_INFO("===== 매매 신호 생성 =====");
    
    pending_signals_.clear();
    
    for (const auto& coin : scanned_markets_) {
        // 이미 포지션 보유 중이면 스킵
        if (risk_manager_->getPosition(coin.market) != nullptr) {
            continue;
        }
        
        try {
            // ✅ 캔들 조회 제거 - Scanner에서 이미 포함됨
            const auto& candles = coin.candles;
            
            if (candles.empty()) {
                LOG_INFO("{} - 캔들 데이터 없음", coin.market);
                continue;
            }
            
            double current_price = candles.back().close;
            
            // 전략에서 신호 수집
            auto signals = strategy_manager_->collectSignals(
                coin.market,
                coin,
                candles,
                current_price,
                risk_manager_->getRiskMetrics().available_capital
            );
            
            if (signals.empty()) {
                continue;
            }
            
            // 신호 필터링 (강도 0.6 이상)
            // [🔧 수정] 신호 강도 필터: 0.6 → 0.5 (금융공학 권장)
            // 보정 사유: 기관급 트레이더는 50%+ 신뢰도로 수익성 확보 가능
            // 너무 높은 필터(0.6)는 우수한 기회를 놓칠 위험 증가
            auto filtered = strategy_manager_->filterSignals(signals, 0.5);
            
            if (filtered.empty()) {
                continue;
            }
            
            // 최적 신호 선택
            auto best_signal = strategy_manager_->selectBestSignal(filtered);
            
            if (best_signal.type != strategy::SignalType::NONE) {
                pending_signals_.push_back(best_signal);
                total_signals_++;
                
                LOG_INFO("신호 발견: {} - {} (강도: {:.2f})",
                         coin.market,
                         best_signal.type == strategy::SignalType::STRONG_BUY ? "강력 매수" : "매수",
                         best_signal.strength);
            }
            
        } catch (const std::exception& e) {
            LOG_ERROR("신호 생성 실패: {} - {}", coin.market, e.what());
        }
    }
    
    LOG_INFO("총 {}개 신호 생성", pending_signals_.size());
}


// ===== 신호 실행 =====

void TradingEngine::executeSignals() {
    if (pending_signals_.empty()) {
        return;
    }
    
    LOG_INFO("===== 신호 실행 =====");
    
    // ===== [NEW] 동적 필터값 계산 =====
    double current_filter = calculateDynamicFilterValue();
    LOG_INFO("📊 현재 신호 필터값: {:.3f} (범위: 0.45~0.55)", current_filter);
    
    // ===== [NEW] 포지션 확대 배수 계산 =====
    double current_scale = calculatePositionScaleMultiplier();
    LOG_INFO("📈 포지션 확대 배수: {:.2f}배", current_scale);
    
    // 강도 순으로 정렬
    std::sort(pending_signals_.begin(), pending_signals_.end(),
        [](const strategy::Signal& a, const strategy::Signal& b) {
            return a.strength > b.strength;
        });
    
    int executed = 0;
    int filtered_out = 0;
    
    for (auto& signal : pending_signals_) {  // [수정] const → auto& (신호 수정 가능)
        // 매수 신호만 처리
        if (signal.type != strategy::SignalType::BUY && 
            signal.type != strategy::SignalType::STRONG_BUY) {
            continue;
        }
        
        // [NEW] 신호에 현재 필터값 저장 (ML 학습용)
        signal.signal_filter = current_filter;
        
        // ===== [NEW] 동적 필터 적용 =====
        // 현재 신호 강도가 동적 필터값 이상이어야만 실행
        if (signal.strength < current_filter) {
            LOG_INFO("{} 신호 필터 제외 (강도: {:.3f} < 필터: {:.3f})",
                     signal.market, signal.strength, current_filter);
            filtered_out++;
            continue;
        }
        
        // ===== [🔧 순서 변경] 포지션 크기 조정을 canEnterPosition 호출 BEFORE에 배치 =====
        // Win Rate ≥ 60%, Profit Factor ≥ 1.5일 때만 확대
        // 기관급 기준에 따른 자동 포지션 확대
        signal.position_size *= current_scale;

        // [NEW] 신호 강도 기반 비중 조정 (강한 신호일수록 비중 확대)
        double strength_multiplier = std::clamp(0.5 + signal.strength, 0.75, 1.5);
        signal.position_size *= strength_multiplier;
        
        LOG_INFO("📊 신호 준비 - {} (강도: {:.3f}, 확대: {:.2f}배, 강도배수: {:.2f}배 → {:.4f})",
             signal.market, signal.strength, current_scale, strength_multiplier, signal.position_size);
            // ===== [NEW] 최소 요구액 기반 position_size 보정 (executeSignals 단계) =====
            // 문제: canEnterPosition 호출 전에 position_size가 불충분할 수 있음
            // 해결: 미리 available_capital으로 필요한 최소 ratio 계산하여 보정
            auto pre_check_metrics = risk_manager_->getRiskMetrics();
        
            // [조정] 최소 요구액: 6000 × 1.2 = 7200원 (소액 자본 지원)
            // 예시: 가용자본 7537원이면 7200원 < 7537원이므로 진입 가능
            const double MIN_REQUIRED_KRW = RECOMMENDED_MIN_ENTER_KRW * 1.2;
                // [조정] 1.5배 → 1.2배 (7200원) - 소액 자본 진입 허용
        
            // 1. 가용자본 부족 확인
            if (pre_check_metrics.available_capital < MIN_REQUIRED_KRW) {
                LOG_WARN("{} 스킵 - 가용자본 부족 (현재: {:.0f} < 필요: {:.0f})",
                         signal.market, pre_check_metrics.available_capital, MIN_REQUIRED_KRW);
                continue;
            }
        
            // 2. 필요한 최소 position_size 계산
            double min_position_size = MIN_REQUIRED_KRW / pre_check_metrics.available_capital;
        
            // 3. 신호의 position_size가 최소값 미만이면 보정
            if (signal.position_size < min_position_size) {
                LOG_INFO("{} 포지션 보정: {:.4f} → {:.4f} (최소 투자액 {:.0f}원 충족 위해)",
                         signal.market, signal.position_size, min_position_size, MIN_REQUIRED_KRW);
                signal.position_size = min_position_size;
            }
        
            // 4. position_size 상한선 제어 (100% 초과 방지)
            if (signal.position_size > 1.0) {
                LOG_WARN("{} 포지션 상한선 적용: {:.4f} → 1.0 (100%)", 
                         signal.market, signal.position_size);
                signal.position_size = 1.0;
            }
        
        auto strategy_ptr = strategy_manager_ ? strategy_manager_->getStrategy(signal.strategy_name) : nullptr;
        bool is_grid_strategy = (signal.strategy_name == "Grid Trading Strategy");

        if (is_grid_strategy && strategy_ptr) {
            auto grid_metrics = risk_manager_->getRiskMetrics();
            double allocated_capital = grid_metrics.available_capital * signal.position_size;

            if (!risk_manager_->reserveGridCapital(signal.market, allocated_capital, signal.strategy_name)) {
                LOG_WARN("{} 그리드 활성화 실패 (자본 예약 실패)", signal.market);
                continue;
            }

            if (!strategy_ptr->onSignalAccepted(signal, allocated_capital)) {
                risk_manager_->releaseGridCapital(signal.market);
                LOG_WARN("{} 그리드 활성화 실패 (전략 초기화 실패)", signal.market);
                continue;
            }

            LOG_INFO("{} 그리드 활성화 완료 (할당 자본: {:.0f})", signal.market, allocated_capital);
            executed++;
            continue;
        }

        // [🔧 중요] canEnterPosition이 position_size 보정 및 모든 리스크 검증을 처리함
        // 따라서 제한 로직은 canEnterPosition 내부에서만 수행
        if (!risk_manager_->canEnterPosition(
            signal.market,
            signal.entry_price,
            signal.position_size,
            signal.strategy_name
        )) {
            LOG_WARN("{} 진입 불가 (리스크 제한)", signal.market);
            continue;
        }
        
        // [기존] Post-Entry 차일드 포지션 여유금 재검증
        {
            auto metrics = risk_manager_->getRiskMetrics();
                double current_required = metrics.available_capital * signal.position_size;
            double remaining_after = metrics.available_capital - current_required;
            
            // [완화] 소액 자본 지원: 1.5배 → 1.1배
            double min_remaining = RECOMMENDED_MIN_ENTER_KRW * 1.1;
            
            if (remaining_after < min_remaining) {
                LOG_WARN("{} 진입 불가 (차일드 여유 부족: {:.0f} < {:.0f})",
                         signal.market, remaining_after, min_remaining);
                continue;
            }
        }
        
        // 주문 실행
        if (executeBuyOrder(signal.market, signal)) {
            // [NEW] 포지션에 신호 정보 저장 (필터값, 강도)
            risk_manager_->setPositionSignalInfo(signal.market, signal.signal_filter, signal.strength);
            executed++;
        }
    }
    
    LOG_INFO("{}개 신호 실행 완료 (필터링: {}개)", executed, filtered_out);
    pending_signals_.clear();
}

bool TradingEngine::executeBuyOrder(
    const std::string& market,
    const strategy::Signal& signal
) {
    LOG_INFO("🔵 매수 시도: {} (신호 강도: {:.2f})", market, signal.strength);
    
    try {
        // 1. [정밀도] 호가창(Orderbook) 조회하여 매수 가격 결정
        //    단순 현재가(Ticker)가 아니라, 실제 살 수 있는 '매도 1호가'를 봅니다.
        auto orderbook = http_client_->getOrderBook(market);
        if (orderbook.empty()) {
            LOG_ERROR("호가 조회 실패: {}", market);
            return false;
        }
        
        // [중요] API 리턴 구조에 따른 예외 처리
        nlohmann::json units;
        if (orderbook.is_array() && !orderbook.empty()) {
            units = orderbook[0]["orderbook_units"];
        } else if (orderbook.contains("orderbook_units")) {
            units = orderbook["orderbook_units"];
        } else {
            LOG_ERROR("{} - 호가 유닛(units)을 찾을 수 없습니다: {}", market, orderbook.dump());
            return false;
        }

        double best_ask_price = calculateOptimalBuyPrice(market, signal.entry_price, orderbook); // 매도 1호가
        
        // 2. 투자 금액 및 수량 계산
        auto metrics = risk_manager_->getRiskMetrics();
        
            // ===== [단순화] executeSignals()에서 이미 position_size 보정됨 =====
            // 따라서 여기서는 최종 안전장치만 수행
        
            // [최종 체크 1] 가용자본 최소값 확인 (executeSignals에서도 했지만, 재확인)
        if (metrics.available_capital < RECOMMENDED_MIN_ENTER_KRW) {
            LOG_WARN("{} - 가용자본 부족: {:.0f} < {:.0f}원 (진입 불가)", 
                     market, metrics.available_capital, RECOMMENDED_MIN_ENTER_KRW);
            return false;
        }
        
            // [최종 체크 2] position_size 상한선 (executeSignals에서도 했지만, 재확인)
            double safe_position_size = signal.position_size;
            if (safe_position_size > 1.0) {
                LOG_WARN("{} - [최종 안전장치] position_size {:.4f} → 1.0 (상한선 적용)", 
                         market, safe_position_size);
                safe_position_size = 1.0;
            }

            double invest_amount = metrics.available_capital * safe_position_size;

        LOG_INFO("{} - [계산] 가용자본(100%): {:.0f}, 투자비중: {:.4f}%, 투자예정금액: {:.0f}원", 
                     market, metrics.available_capital, safe_position_size * 100.0, invest_amount);
        
        if (invest_amount < RECOMMENDED_MIN_ENTER_KRW) {
            // 이 상황은 이제 거의 발생하지 않아야 함 (위에서 차단함)
            LOG_WARN("{} - 진입액 부족 (금액: {:.0f}, 필요: {:.0f}원) [내부 오류]", 
                     market, invest_amount, RECOMMENDED_MIN_ENTER_KRW);
            return false;
        }
        
        // [🔧 중요] 보정된 position_size로 RiskManager에 재확인
        if (!risk_manager_->canEnterPosition(
            market,
            signal.entry_price,
            safe_position_size,
            signal.strategy_name
        )) {
            LOG_WARN("{} - 재검증 실패 (리스크 제한)", market);
            return false;
        }

        if (invest_amount > config_.max_order_krw) invest_amount = config_.max_order_krw;
        
        // 지정가 주문 수량 계산 (소수점 8자리까지)
        double quantity = invest_amount / best_ask_price;
        
        // 문자열 변환 (업비트는 소수점 처리에 민감하므로 포맷팅 주의)
        // [제안] 소수점 정밀도 제어 (sprintf 또는 stringstream 사용)
        char buffer[64];
        // 수량은 소수점 8자리까지, 불필요한 0 제거 로직 필요하면 추가
        std::snprintf(buffer, sizeof(buffer), "%.8f", quantity); 
        std::string vol_str(buffer);
        
        LOG_INFO("  주문 준비: 평단 {:.0f}, 수량 {}, 금액 {:.0f}", 
                 best_ask_price, vol_str, invest_amount);

        // 3. [안전] 실전 매수 주문 (지정가 Limit Order, 10초 미체결 시 재호가 반복)
        if (config_.mode == TradingMode::LIVE && !config_.dry_run) {
            auto order_result = executeLimitBuyOrder(
                market,
                best_ask_price,
                quantity,
                signal.max_retries,
                signal.retry_wait_ms
            );

            if (!order_result.success || order_result.executed_volume <= 0.0) {
                LOG_ERROR("❌ 매수 체결 실패: {}", order_result.error_message);
                return false;
            }

            double executed_volume = order_result.executed_volume;
            double avg_price = order_result.executed_price;

            LOG_INFO("🆗 실제 체결 확인: 수량 {:.8f}, 평단 {:.0f} (재시도: {})",
                     executed_volume, avg_price, order_result.retry_count);

            // 5. [동적 손절 계산] Candles 조회 후 동적 손절가 계산
            double dynamic_stop_loss = avg_price * 0.975; // 기본값: -2.5%
            try {
                auto candles_json = http_client_->getCandles(market, "60", 200);
                if (!candles_json.empty() && candles_json.is_array()) {
                    auto candles = analytics::TechnicalIndicators::jsonToCandles(candles_json);
                    if (!candles.empty()) {
                        dynamic_stop_loss = risk_manager_->calculateDynamicStopLoss(avg_price, candles);
                        LOG_INFO("📊 [LIVE] 동적 손절가 계산: {:.0f} ({:.2f}%)", 
                                 dynamic_stop_loss, (dynamic_stop_loss - avg_price) / avg_price * 100.0);
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("⚠️ [LIVE] 동적 손절 계산 실패, 기본값(-2.5%) 사용: {}", e.what());
            }

            // 6. [동적 익절가 계산] Signal의 take_profit을 사용
            double tp1 = signal.take_profit_1 > 0 ? signal.take_profit_1 : avg_price * 1.020;
            double tp2 = signal.take_profit_2 > 0 ? signal.take_profit_2 : avg_price * 1.030;
            
            LOG_INFO("📈 [LIVE] 익절가 적용: TP1={:.0f} ({:.2f}%), TP2={:.0f} ({:.2f}%)",
                     tp1, (tp1 - avg_price) / avg_price * 100.0,
                     tp2, (tp2 - avg_price) / avg_price * 100.0);
            
            // 7. RiskManager 등록 (실제 체결 데이터 기반)
            risk_manager_->enterPosition(
                market,
                avg_price,        // 실제 체결 평단
                executed_volume,  // 실제 체결 수량
                dynamic_stop_loss, // 동적 손절가
                tp1,              // [수정됨] Signal 기반 1차 익절가
                tp2,              // [수정됨] Signal 기반 2차 익절가
                signal.strategy_name
            );

            if (strategy_manager_) {
                auto strategy_ptr = strategy_manager_->getStrategy(signal.strategy_name);
                if (strategy_ptr) {
                    strategy_ptr->onSignalAccepted(signal, invest_amount);
                }
            }
            
            return true;
        } 
        else {
            // Paper Trading (모의투자) 모드 - [동적 손절 계산]
            double dynamic_stop_loss = best_ask_price * 0.975; // 기본값: -2.5%
            try {
                auto candles_json = http_client_->getCandles(market, "60", 200);
                if (!candles_json.empty() && candles_json.is_array()) {
                    auto candles = analytics::TechnicalIndicators::jsonToCandles(candles_json);
                    if (!candles.empty()) {
                        dynamic_stop_loss = risk_manager_->calculateDynamicStopLoss(best_ask_price, candles);
                        LOG_INFO("📊 [PAPER] 동적 손절가 계산: {:.0f} ({:.2f}%)", 
                                 dynamic_stop_loss, (dynamic_stop_loss - best_ask_price) / best_ask_price * 100.0);
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("⚠️ [PAPER] 동적 손절 계산 실패, 기본값(-2.5%) 사용: {}", e.what());
            }

            // [동적 익절가 계산] Signal의 take_profit을 사용
            double tp1_paper = signal.take_profit_1 > 0 ? signal.take_profit_1 : best_ask_price * 1.015;
            double tp2_paper = signal.take_profit_2 > 0 ? signal.take_profit_2 : best_ask_price * 1.03;
            
            LOG_INFO("📈 [PAPER] 익절가 적용: TP1={:.0f} ({:.2f}%), TP2={:.0f} ({:.2f}%)",
                     tp1_paper, (tp1_paper - best_ask_price) / best_ask_price * 100.0,
                     tp2_paper, (tp2_paper - best_ask_price) / best_ask_price * 100.0);
            
            risk_manager_->enterPosition(
                market, best_ask_price, quantity, 
                dynamic_stop_loss, tp1_paper, tp2_paper, 
                signal.strategy_name
            );

            if (strategy_manager_) {
                auto strategy_ptr = strategy_manager_->getStrategy(signal.strategy_name);
                if (strategy_ptr) {
                    strategy_ptr->onSignalAccepted(signal, invest_amount);
                }
            }
            return true;
        }

    } catch (const std::exception& e) {
        LOG_ERROR("매수 실행 중 예외 발생: {}", e.what());
        return false;
    }
}


// ===== 포지션 모니터링 =====

// TradingEngine.cpp

void TradingEngine::monitorPositions() {

    static int log_counter = 0;
    bool should_log = (log_counter++ % 10 == 0);

    // 1. 현재 관리 중인 포지션 목록 가져오기
    auto positions = risk_manager_->getAllPositions();
    
    // 2. 보유 중인 종목 및 활성화된 전략 마켓 수집 (Batch 조회를 위함)
    std::set<std::string> market_set;
    for (const auto& pos : positions) {
        market_set.insert(pos.market);
    }

    std::vector<std::shared_ptr<strategy::IStrategy>> strategies;
    if (strategy_manager_) {
        strategies = strategy_manager_->getStrategies();
        for (const auto& strategy : strategies) {
            for (const auto& market : strategy->getActiveMarkets()) {
                market_set.insert(market);
            }
        }
    }

    if (market_set.empty()) {
        return;
    }

    std::vector<std::string> markets;
    markets.reserve(market_set.size());
    for (const auto& market : market_set) {
        markets.push_back(market);
    }
    
    if (should_log) {
        LOG_INFO("===== 포지션 모니터링 ({}종목) =====", markets.size());
    }

    // 3. [핵심] 한 번의 HTTP 요청으로 모든 종목 현재가 조회 (Batch Processing)
    std::map<std::string, double> price_map;
    
    try {
        // MarketScanner에서 사용했던 getTickerBatch 재사용
        auto tickers = http_client_->getTickerBatch(markets);
        
        for (const auto& t : tickers) {
            std::string market_code = t["market"].get<std::string>();
            double trade_price = t["trade_price"].get<double>();
            price_map[market_code] = trade_price;
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR("포지션 시세 일괄 조회 실패: {}", e.what());
        return; // 시세 조회 실패 시 이번 틱은 건너뜀 (잘못된 매도 방지)
    }

    // 4. 메모리 상의 데이터를 기반으로 포지션 루프 (통신 없음, 매우 빠름)
    for (auto& pos : positions) {
        // 시세 맵에서 해당 종목 가격 찾기
        if (price_map.find(pos.market) == price_map.end()) {
            LOG_WARN("{} 시세 데이터 누락됨", pos.market);
            continue;
        }

        double current_price = price_map[pos.market];
        
        // RiskManager 상태 업데이트 (메모리 연산)
        risk_manager_->updatePosition(pos.market, current_price);
        
        // [✅ 추가된 핵심 부분] 전략에게 "업데이트 해!" 라고 명령 ===================
        std::shared_ptr<strategy::IStrategy> strategy;
        if (strategy_manager_) {
            // 해당 포지션을 담당하는 전략 찾기 (pos.strategy_name 사용)
            strategy = strategy_manager_->getStrategy(pos.strategy_name);
            
            if (strategy) {
                // 아까 IStrategy에 추가한 updateState 호출
                // 그리드 전략이라면 내부적으로 그물망 매매를 수행할 것이고,
                // 스캘핑 전략이라면 그냥 빈 함수라 아무 일도 안 일어남 (안전함)
                strategy->updateState(pos.market, current_price);
            }
        }

        // 갱신된 포지션 포인터 다시 가져오기 (수익률 등 계산된 값 확인)
        auto* updated_pos = risk_manager_->getPosition(pos.market);
        if (!updated_pos) continue;
        
        if (should_log) {
            LOG_INFO("  {} - 전략: {} - 진입: {:.0f}, 현재: {:.0f}, 손익: {:.0f} ({:+.2f}%)",
                     pos.market, updated_pos->strategy_name, updated_pos->entry_price, current_price,
                     updated_pos->unrealized_pnl, updated_pos->unrealized_pnl_pct * 100.0);
        }
        
        // --- 매도 로직 (전략 기반 청산 우선) ---

        // 전략별 청산 조건 (시간제한 포함)
        if (strategy) {
            long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            double holding_time_seconds = (now_ms - updated_pos->entry_time) / 1000.0;

            if (strategy->shouldExit(pos.market, updated_pos->entry_price, current_price, holding_time_seconds)) {
                LOG_INFO("⏱️ 전략 청산 조건 충족: {} (전략: {})", pos.market, updated_pos->strategy_name);
                executeSellOrder(pos.market, *updated_pos, "strategy_exit", current_price);
                continue;
            }
        }

        // 1차 익절 체크 (50% 청산)
        if (!updated_pos->half_closed && current_price >= updated_pos->take_profit_1) {
            LOG_INFO("💰 1차 익절 조건 도달! (수익률: {:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            executePartialSell(pos.market, *updated_pos, current_price);
            continue; // 부분 매도 후 다음 종목으로
        }
        
        // 전체 청산 체크 (손절 or 2차 익절)
        if (risk_manager_->shouldExitPosition(pos.market)) {
            std::string reason = "unknown";
            
            // 청산 사유 구체화
            if (current_price <= updated_pos->stop_loss) {
                reason = "stop_loss";
                LOG_INFO("📉 손절 조건 도달 (손실률: {:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            } else if (current_price >= updated_pos->take_profit_2) {
                reason = "take_profit";
                LOG_INFO("🚀 2차 익절(최종) 조건 도달 (수익률: {:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            } else {
                reason = "strategy_exit"; // 전략에서 청산 신호 보냄 (TS 등)
            }
            
            executeSellOrder(pos.market, *updated_pos, reason, current_price);
        }
    }

    if (!strategies.empty()) {
        for (const auto& strategy : strategies) {
            auto active_markets = strategy->getActiveMarkets();
            for (const auto& market : active_markets) {
                if (risk_manager_->getPosition(market)) {
                    continue;
                }

                auto price_it = price_map.find(market);
                if (price_it == price_map.end()) {
                    continue;
                }

                strategy->updateState(market, price_it->second);
            }
        }

        for (const auto& strategy : strategies) {
            auto order_requests = strategy->drainOrderRequests();
            for (const auto& request : order_requests) {
                autolife::strategy::OrderResult result;
                result.market = request.market;
                result.side = request.side;
                result.level_id = request.level_id;
                result.reason = request.reason;

                double order_amount = request.price * request.quantity;
                if (order_amount < EXCHANGE_MIN_ORDER_KRW) {
                    LOG_WARN("그리드 주문 금액 부족: {:.0f} < {:.0f}", order_amount, EXCHANGE_MIN_ORDER_KRW);
                    strategy->onOrderResult(result);
                    continue;
                }

                if (config_.mode == TradingMode::LIVE && !config_.dry_run) {
                    if (request.side == autolife::strategy::OrderSide::BUY) {
                        auto exec = executeLimitBuyOrder(
                            request.market,
                            request.price,
                            request.quantity,
                            0,
                            500
                        );
                        result.success = exec.success;
                        result.executed_price = exec.executed_price;
                        result.executed_volume = exec.executed_volume;
                    } else {
                        auto exec = executeLimitSellOrder(
                            request.market,
                            request.price,
                            request.quantity,
                            0,
                            500
                        );
                        result.success = exec.success;
                        result.executed_price = exec.executed_price;
                        result.executed_volume = exec.executed_volume;
                    }
                } else {
                    result.success = true;
                    result.executed_price = request.price;
                    result.executed_volume = request.quantity;
                }

                if (result.success && result.executed_volume > 0.0) {
                    risk_manager_->applyGridFill(
                        result.market,
                        result.side,
                        result.executed_price,
                        result.executed_volume
                    );
                }

                strategy->onOrderResult(result);
            }

            auto released_markets = strategy->drainReleasedMarkets();
            for (const auto& market : released_markets) {
                risk_manager_->releaseGridCapital(market);
            }
        }
    }
}

bool TradingEngine::executeSellOrder(
    const std::string& market,
    const risk::Position& position,
    const std::string& reason,
    double current_price
) {
    LOG_INFO("📉 전량 매도 실행: {} (이유: {})", market, reason);
    
    //double current_price = getCurrentPrice(market);
    if (current_price <= 0) {
        LOG_ERROR("현재가 조회 실패: {}", market);
        return false;
    }
    
    // 전량 매도
    double sell_quantity = std::floor(position.quantity * 0.9999 * 100000000.0) / 100000000.0;
    double invest_amount = sell_quantity * current_price;
    
    // 1. 최소 주문 금액 체크 (거래소 최소: 5,000 KRW)
    if (invest_amount < EXCHANGE_MIN_ORDER_KRW) {
        LOG_WARN("매도 금액 부족: {:.0f} < {:.0f} (거래소 최소)", invest_amount, EXCHANGE_MIN_ORDER_KRW);
        return false;
    }
    
    // 2. 문자열 변환 시 std::to_string 대신 정밀도를 고정한 stringstream 사용 (매우 중요)
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << sell_quantity;
        std::string quantity_str = ss.str();

    // 2-1. 호가창에서 최적 매도가 계산 (지정가 매도 위해)
    double sell_price = current_price;
    try {
        auto orderbook = http_client_->getOrderBook(market);
        sell_price = calculateOptimalSellPrice(market, current_price, orderbook);
        LOG_INFO("📊 매도 호가 최적화: {} 원 (주문: {} 원)", sell_price, current_price);
    } catch (const std::exception& e) {
        LOG_WARN("⚠️ 호가 조회 실패 (모의값 사용): {}", e.what());
        sell_price = current_price;
    }

    // 2. 실전 주문 실행 (지정가 재호가 반복)
    double executed_price = current_price;
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run) {
            LOG_WARN("🔶 DRY RUN: 매도 시뮬레이션 완료");
        } else {
            auto order_result = executeLimitSellOrder(
                market,
                sell_price,
                sell_quantity,
                0,
                500
            );

            if (!order_result.success || order_result.executed_volume <= 0.0) {
                LOG_ERROR("❌ 매도 체결 실패: {}", order_result.error_message);
                return false;
            }

            executed_price = order_result.executed_price;
            LOG_INFO("🆗 매도 체결 확인: 평단 {:.0f} (재시도: {})",
                     executed_price, order_result.retry_count);
        }
    }
    
    // 3. 수익금 계산
    double gross_pnl = (executed_price - position.entry_price) * sell_quantity;
    bool is_win = gross_pnl > 0;
    
    // 4. RiskManager 업데이트 (포지션 삭제)
    risk_manager_->exitPosition(market, executed_price, reason);
    
    // 5. [핵심 수정] StrategyManager를 통해 전략을 찾아 통계 업데이트 & 잠금 해제
    if (strategy_manager_ && !position.strategy_name.empty()) {
        // Position 구조체에 저장된 strategy_name("Advanced Scalping" 등)으로 전략 찾기
        auto strategy = strategy_manager_->getStrategy(position.strategy_name);
        
        if (strategy) {
            // [중요] market을 넘겨서 active_positions_에서 삭제하게 함
            strategy->updateStatistics(market, is_win, gross_pnl);
            LOG_INFO("📊 전략({}) 통계 업데이트 및 재진입 허용", position.strategy_name);
        } else if (position.strategy_name != "RECOVERED") {
            // RECOVERED 포지션은 무시하고, 다른 경우만 경고
            LOG_WARN("⚠️ 전략({})을 찾을 수 없어 통계 업데이트 실패", position.strategy_name);
        }
    }
    
    LOG_INFO("🗑️ 포지션 종료 완료: {} (손익: {:.0f} KRW)", market, gross_pnl);
    
    return true;
}


bool TradingEngine::executePartialSell(const std::string& market, const risk::Position& position, double current_price) {

    //double current_price = getCurrentPrice(market);

        if (current_price <= 0) {
        LOG_ERROR("현재가 조회 실패: {}", market);
        return false;
    }
    
    // 50% 수량 계산
    double sell_quantity = std::floor(position.quantity * 0.5 * 100000000.0) / 100000000.0;
    double invest_amount = sell_quantity * current_price;
    
    // 1. 최소 주문 금액 체크 및 대응 (거래소 최소: 5,000 KRW)
    if (invest_amount < EXCHANGE_MIN_ORDER_KRW) {
        LOG_WARN("⚠️ 부분 매도 금액 부족 ({:.0f}원, 최소: {:.0f}원)", invest_amount, EXCHANGE_MIN_ORDER_KRW);
        
        // [핵심] 여기서 전량 매도 함수를 호출하여 포지션을 완전히 정리해버립니다.
        // 그래야 다음 루프에서 다시 진입하지 않습니다.
        return executeSellOrder(market, position, "Partial sell amount too small - Force Exit", current_price);
    }
    
    LOG_INFO("✂️ 부분 매도 실행 (50%): {}", market);
    
    LOG_INFO("  진입가: {:.0f}, 청산가: {:.0f}, 부분매도: {:.8f}",
             position.entry_price, current_price, sell_quantity);
    
    // 2. 문자열 변환 시 std::to_string 대신 정밀도를 고정한 stringstream 사용 (매우 중요)
    std::stringstream ss;
    ss << std::fixed << std::setprecision(8) << sell_quantity;
    std::string quantity_str = ss.str();

    // 2-1. 호가창에서 최적 매도가 계산 (지정가 매도 위해)
    double sell_price = current_price;
    try {
        auto orderbook = http_client_->getOrderBook(market);
        sell_price = calculateOptimalSellPrice(market, current_price, orderbook);
    } catch (const std::exception&) {
        sell_price = current_price;
    }

    // 2. 실전 주문 실행 (지정가 재호가 반복)
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run) {
            LOG_WARN("🔶 DRY RUN: 부분 매도 시뮬레이션");
            risk_manager_->partialExit(market, current_price);
            return true;
        }

        auto order_result = executeLimitSellOrder(
            market,
            sell_price,
            sell_quantity,
            0,
            500
        );

        if (!order_result.success || order_result.executed_volume <= 0.0) {
            LOG_ERROR("❌ 부분 매도 체결 실패: {}", order_result.error_message);
            return false;
        }

        LOG_INFO("🆗 부분 매도 체결 확인: 평단 {:.0f} (재시도: {})",
                 order_result.executed_price, order_result.retry_count);
        risk_manager_->partialExit(market, order_result.executed_price);
        return true;
    }

    // Paper Trading
    risk_manager_->partialExit(market, current_price);
    return true;
}

// ===== 주문 헬퍼 =====

double TradingEngine::calculateOptimalBuyPrice(
    const std::string& market,
    double base_price,
    const nlohmann::json& orderbook
) {
    (void)market;
    nlohmann::json units;

    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0]["orderbook_units"];
    } else if (orderbook.contains("orderbook_units")) {
        units = orderbook["orderbook_units"];
    }

    if (units.is_array() && !units.empty()) {
        return units[0].value("ask_price", base_price);
    }

    return base_price;
}

double TradingEngine::calculateOptimalSellPrice(
    const std::string& market,
    double base_price,
    const nlohmann::json& orderbook
) {
    (void)market;
    nlohmann::json units;

    if (orderbook.is_array() && !orderbook.empty()) {
        units = orderbook[0]["orderbook_units"];
    } else if (orderbook.contains("orderbook_units")) {
        units = orderbook["orderbook_units"];
    }

    if (units.is_array() && !units.empty()) {
        return units[0].value("bid_price", base_price);
    }

    return base_price;
}

TradingEngine::OrderFillInfo TradingEngine::verifyOrderFill(
    const std::string& uuid,
    const std::string& market,
    double order_volume
) {
    OrderFillInfo info{};
    (void)market;
    (void)order_volume;

    auto toDouble = [](const nlohmann::json& value) -> double {
        try {
            if (value.is_string()) {
                return std::stod(value.get<std::string>());
            }
            if (value.is_number()) {
                return value.get<double>();
            }
        } catch (...) {
        }
        return 0.0;
    };

    try {
        auto check = http_client_->getOrder(uuid);
        std::string state = check.value("state", "");

        double total_funds = 0.0;
        double total_vol = 0.0;

        if (check.contains("trades") && check["trades"].is_array()) {
            for (const auto& trade : check["trades"]) {
                double trade_vol = toDouble(trade["volume"]);
                double trade_price = toDouble(trade["price"]);
                total_vol += trade_vol;
                total_funds += trade_vol * trade_price;
            }
        } else if (check.contains("executed_volume")) {
            total_vol = toDouble(check["executed_volume"]);
        }

        if (total_vol > 0.0 && total_funds > 0.0) {
            info.avg_price = total_funds / total_vol;
        } else if (check.contains("price")) {
            info.avg_price = toDouble(check["price"]);
        }

        info.filled_volume = total_vol;
        info.is_filled = (state == "done") && total_vol > 0.0;
        info.is_partially_filled = (!info.is_filled && total_vol > 0.0);
        info.fee = 0.0;
    } catch (const std::exception& e) {
        LOG_WARN("주문 체결 확인 실패: {}", e.what());
    }

    return info;
}

TradingEngine::LimitOrderResult TradingEngine::executeLimitBuyOrder(
    const std::string& market,
    double entry_price,
    double quantity,
    int max_retries,
    int retry_wait_ms
) {
    LimitOrderResult result{};
    result.success = false;
    result.retry_count = 0;
    result.executed_price = 0.0;
    result.executed_volume = 0.0;

    double remaining = quantity;
    double total_filled = 0.0;
    double total_funds = 0.0;

    (void)max_retries; // 무한 재시도 정책 (체결될 때까지)

    while (running_ && remaining > 0.00000001) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << remaining;
        std::string vol_str = ss.str();
        std::string price_str = std::to_string((long long)entry_price);

        nlohmann::json order_res;
        try {
            order_res = http_client_->placeOrder(market, "bid", vol_str, price_str, "limit");
        } catch (const std::exception& e) {
            result.error_message = e.what();
            return result;
        }

        if (!order_res.contains("uuid")) {
            result.error_message = "No UUID returned";
            return result;
        }

        std::string uuid = order_res["uuid"].get<std::string>();
        LOG_INFO("✅ 매수 주문 전송 (UUID: {}, 가격: {:.0f}, 수량: {})", uuid, entry_price, vol_str);

        // 10초 동안 체결 확인 (500ms * 20)
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto fill = verifyOrderFill(uuid, market, remaining);

            if (fill.filled_volume > 0.0) {
                total_filled += fill.filled_volume;
                total_funds += fill.avg_price * fill.filled_volume;
                remaining -= fill.filled_volume;
            }

            if (fill.is_filled || remaining <= 0.00000001) {
                break;
            }
        }

        if (remaining <= 0.00000001) {
            break;
        }

        // 미체결 잔량은 주문 취소 후 재호가
        try {
            http_client_->cancelOrder(uuid);
            LOG_WARN("⏳ 매수 미체결 (10초) → 주문 취소 및 재호가");
        } catch (const std::exception& e) {
            LOG_WARN("매수 주문 취소 실패: {}", e.what());
        }

        result.retry_count++;

        if (retry_wait_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_wait_ms));
        }

        try {
            auto orderbook = http_client_->getOrderBook(market);
            entry_price = calculateOptimalBuyPrice(market, entry_price, orderbook);
        } catch (const std::exception& e) {
            LOG_WARN("재호가를 위한 호가 조회 실패: {}", e.what());
        }
    }

    if (total_filled > 0.0) {
        result.success = true;
        result.executed_volume = total_filled;
        result.executed_price = total_funds / total_filled;
    } else {
        result.error_message = "No fills";
    }

    return result;
}

TradingEngine::LimitOrderResult TradingEngine::executeLimitSellOrder(
    const std::string& market,
    double exit_price,
    double quantity,
    int max_retries,
    int retry_wait_ms
) {
    LimitOrderResult result{};
    result.success = false;
    result.retry_count = 0;
    result.executed_price = 0.0;
    result.executed_volume = 0.0;

    double remaining = quantity;
    double total_filled = 0.0;
    double total_funds = 0.0;

    (void)max_retries; // 무한 재시도 정책 (체결될 때까지)

    while (running_ && remaining > 0.00000001) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << remaining;
        std::string vol_str = ss.str();
        std::string price_str = std::to_string((long long)exit_price);

        nlohmann::json order_res;
        try {
            order_res = http_client_->placeOrder(market, "ask", vol_str, price_str, "limit");
        } catch (const std::exception& e) {
            result.error_message = e.what();
            return result;
        }

        if (!order_res.contains("uuid")) {
            result.error_message = "No UUID returned";
            return result;
        }

        std::string uuid = order_res["uuid"].get<std::string>();
        LOG_INFO("✅ 매도 주문 전송 (UUID: {}, 가격: {:.0f}, 수량: {})", uuid, exit_price, vol_str);

        // 10초 동안 체결 확인 (500ms * 20)
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto fill = verifyOrderFill(uuid, market, remaining);

            if (fill.filled_volume > 0.0) {
                total_filled += fill.filled_volume;
                total_funds += fill.avg_price * fill.filled_volume;
                remaining -= fill.filled_volume;
            }

            if (fill.is_filled || remaining <= 0.00000001) {
                break;
            }
        }

        if (remaining <= 0.00000001) {
            break;
        }

        // 미체결 잔량은 주문 취소 후 재호가
        try {
            http_client_->cancelOrder(uuid);
            LOG_WARN("⏳ 매도 미체결 (10초) → 주문 취소 및 재호가");
        } catch (const std::exception& e) {
            LOG_WARN("매도 주문 취소 실패: {}", e.what());
        }

        result.retry_count++;

        if (retry_wait_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_wait_ms));
        }

        try {
            auto orderbook = http_client_->getOrderBook(market);
            exit_price = calculateOptimalSellPrice(market, exit_price, orderbook);
        } catch (const std::exception& e) {
            LOG_WARN("재호가를 위한 호가 조회 실패: {}", e.what());
        }
    }

    if (total_filled > 0.0) {
        result.success = true;
        result.executed_volume = total_filled;
        result.executed_price = total_funds / total_filled;
    } else {
        result.error_message = "No fills";
    }

    return result;
}


// ===== 메트릭 업데이트 =====

void TradingEngine::updateMetrics() {
    auto metrics = risk_manager_->getRiskMetrics();
    
    LOG_INFO("===== 성과 요약 =====");
    LOG_INFO("총 자본: {:.0f} KRW ({:+.2f}%)",
             metrics.total_capital + metrics.invested_capital,
             metrics.total_pnl_pct * 100);
    LOG_INFO("가용 자본: {:.0f}, 투자 중: {:.0f}",
             metrics.available_capital, metrics.invested_capital);
    LOG_INFO("실현 손익: {:.0f}, 미실현: {:.0f}",
             metrics.realized_pnl, metrics.unrealized_pnl);
    LOG_INFO("거래: {} (승: {}, 패: {}, 승률: {:.1f}%)",
             metrics.total_trades,
             metrics.winning_trades,
             metrics.losing_trades,
             metrics.win_rate * 100);
    LOG_INFO("포지션: {}/{}, Drawdown: {:.2f}%",
             metrics.active_positions,
             metrics.max_positions,
             metrics.current_drawdown * 100);
    LOG_INFO("========================");
}

// ===== 상태 조회 =====

risk::RiskManager::RiskMetrics TradingEngine::getMetrics() const {
    return risk_manager_->getRiskMetrics();
}

std::vector<risk::Position> TradingEngine::getPositions() const {
    return risk_manager_->getAllPositions();
}

std::vector<risk::TradeHistory> TradingEngine::getTradeHistory() const {
    return risk_manager_->getTradeHistory();
}

// ===== 수동 제어 =====

void TradingEngine::manualScan() {
    LOG_INFO("수동 스캔 실행");
    scanMarkets();
    generateSignals();
}

// void TradingEngine::manualClosePosition(const std::string& market) {
//     LOG_INFO("수동 청산: {}", market);
    
//     auto* pos = risk_manager_->getPosition(market);
//     if (!pos) {
//         LOG_WARN("포지션 없음: {}", market);
//         return;
//     }
    
//     executeSellOrder(market, *pos, "manual", current_price);
// }

// void TradingEngine::manualCloseAll() {
//     LOG_INFO("전체 포지션 수동 청산");
    
//     auto positions = risk_manager_->getAllPositions();
//     for (const auto& pos : positions) {
//         executeSellOrder(pos.market, pos, "manual_all", current_price);
//     }
// }

// ===== 헬퍼 함수 (수정) =====

double TradingEngine::getCurrentPrice(const std::string& market) {
    try {
        auto ticker = http_client_->getTicker(market);
        if (ticker.empty()) {
            return 0;
        }
        
        // 2. nlohmann/json 사용 시 안전한 타입 변환
        if (ticker.is_array() && !ticker.empty()) {
            return ticker[0].value("trade_price", 0.0);
        }

        if (ticker.contains("trade_price") && !ticker["trade_price"].is_null()) {
            return ticker.value("trade_price", 0.0);
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        LOG_ERROR("현재가 조회 실패: {} - {}", market, e.what());
        return 0;
    }
}

bool TradingEngine::hasEnoughBalance(double required_krw) {
    auto metrics = risk_manager_->getRiskMetrics();
    return metrics.available_capital >= required_krw;
}

void TradingEngine::logPerformance() {
    auto metrics = risk_manager_->getRiskMetrics();
    auto history = risk_manager_->getTradeHistory();
    
    long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    double runtime_hours = (now - start_time_) / (1000.0 * 60.0 * 60.0);
    
    LOG_INFO("========================================");
    LOG_INFO("🏁 최종 성과 보고서");
    LOG_INFO("========================================");
    LOG_INFO("실행 시간: {:.1f}시간", runtime_hours);
    LOG_INFO("총 스캔: {}, 신호: {}, 거래: {}",
             total_scans_, total_signals_, metrics.total_trades);
    LOG_INFO("");
    LOG_INFO("【 자산 변화 】");
    LOG_INFO("초기 자본: {:.0f} KRW", config_.initial_capital);
    LOG_INFO("최종 자본: {:.0f} KRW", metrics.total_capital);
    LOG_INFO("총 손익: {:.0f} KRW ({:+.2f}%)",
             metrics.total_pnl, metrics.total_pnl_pct * 100);
    LOG_INFO("");
    LOG_INFO("【 거래 성과 】");
    LOG_INFO("승률: {:.1f}% ({}/{})",
             metrics.win_rate * 100,
             metrics.winning_trades,
             metrics.total_trades);
    LOG_INFO("Profit Factor: {:.2f}", metrics.profit_factor);
    LOG_INFO("Sharpe Ratio: {:.2f}", metrics.sharpe_ratio);
    LOG_INFO("Max Drawdown: {:.2f}%", metrics.max_drawdown * 100);
    LOG_INFO("");
    LOG_INFO("【 거래 이력 (최근 10개) 】");
    
    // 거래 이력 출력
    if (!history.empty()) {
        int count = 0;
        for (auto it = history.rbegin(); it != history.rend() && count < 10; ++it, ++count) {
            std::string status_emoji = (it->profit_loss > 0) ? "✅" : "❌";
            LOG_INFO("  {} {} | 진입: {:.0f}, 청산: {:.0f} | {:+.2f}% | {}",
                     status_emoji, it->market,
                     it->entry_price,
                     it->exit_price,
                     it->profit_loss_pct * 100,
                     it->exit_reason);
        }
    } else {
        LOG_INFO("  거래 이력 없음");
    }
    
    LOG_INFO("");
    LOG_INFO("【 권고사항 】");
    
    // 성과 기반 권고사항 (금융공학적 근거)
    if (metrics.total_trades == 0) {
        LOG_INFO("  ⚠️ 거래가 없었습니다. 신호 필터 설정 또는 포지션 진입 조건을 검토하세요.");
    } else if (metrics.win_rate < 0.4) {
        LOG_INFO("  ⚠️ 승률({:.1f}%)이 낮습니다. 손절 전략을 개선하거나 신호 품질을 점검하세요.",
                 metrics.win_rate * 100);
    } else if (metrics.win_rate >= 0.6 && metrics.profit_factor > 1.5) {
        LOG_INFO("  ✅ 성과가 우수합니다(전략 승률: {:.1f}%, PF: {:.2f}). 포지션 규모 확대를 검토하세요.",
                 metrics.win_rate * 100, metrics.profit_factor);
    } else if (metrics.max_drawdown > 0.20) {
        LOG_INFO("  ⚠️ 최대손실({:.2f}%)이 과중합니다. 포지션 사이징을 축소하거나 리스크 관리를 강화하세요.",
                 metrics.max_drawdown * 100);
    } else {
        LOG_INFO("  📈 점진적 성과 증대를 기록 중입니다. 현재 설정 유지 또는 미세 조정을 권고합니다.");
    }
    
    LOG_INFO("========================================");
    
    // ===== [NEW] Prometheus 메트릭 정보 추가 =====
    LOG_INFO("");
    LOG_INFO("【 실시간 모니터링 메트릭 】");
    LOG_INFO("현재 필터값: {:.3f} (범위: 0.45~0.55)", dynamic_filter_value_);
    LOG_INFO("포지션 확대 배수: {:.2f}배", position_scale_multiplier_);
    LOG_INFO("누적 매수 주문: {}, 누적 매도 주문: {}", 
             prometheus_metrics_.total_buy_orders,
             prometheus_metrics_.total_sell_orders);
    LOG_INFO("");
    LOG_INFO("📊 Prometheus 메트릭 내보내기 가능 (포트 9090):");
    
    // Prometheus 메트릭 문자열 생성 및 로깅
    auto prom_metrics = exportPrometheusMetrics();
    LOG_INFO("메트릭 샘플: {}", prom_metrics.substr(0, 200) + "...");
}

void TradingEngine::syncAccountState() {
    LOG_INFO("🔄 계좌 상태 동기화 시작...");

    try {
        auto accounts = http_client_->getAccounts();
        bool krw_found = false;

        for (const auto& acc : accounts) {
            std::string currency = acc["currency"].get<std::string>();
            double balance = std::stod(acc["balance"].get<std::string>());
            double locked = std::stod(acc["locked"].get<std::string>());
            
            // 1. [수정] KRW(현금) 처리 로직 추가
            if (currency == "KRW") {
                double total_cash = balance + locked; // 사용가능 + 미체결 동결
                
                // RiskManager의 자본금을 실제 현금 잔고로 리셋!
                risk_manager_->resetCapital(total_cash);
                // (2) [✅ 추가] 엔진 설정상의 '초기 자본'도 실제 잔고로 변경!
                config_.initial_capital = total_cash;
                krw_found = true;
                LOG_INFO("💰 현금 잔고 동기화: {:.0f} KRW (가용: {:.0f})", total_cash, balance);
                continue; // 현금 처리는 끝났으니 다음으로
            }

            // 2. 보유 코인 처리 (기존 로직 유지)
            // 마켓 코드 생성 (예: BTC -> KRW-BTC)
            std::string market = "KRW-" + currency;
            
            double avg_buy_price = std::stod(acc["avg_buy_price"].get<std::string>());
            
            // 짜투리(Dust) 무시 (거래소 최소 주문금액 이하)
            if (balance * avg_buy_price < 5000) continue;

            // 이미 RiskManager에 있으면 패스
            if (risk_manager_->getPosition(market) != nullptr) continue;

            LOG_INFO("🔍 기존 보유 코인 발견: {} (수량: {:.8f}, 평단: {:.0f})", 
                     market, balance, avg_buy_price);

            //double current_price = getCurrentPrice(market); 
            // 1. [기존] 단순 비율 손절가
            double target_sl = avg_buy_price * 0.97;

            // 2. [추가] 5,100원 마지노선을 지키기 위한 단가 계산 
            // balance(수량)가 0일 경우를 대비해 아주 작은 값(1e-9)으로 안전장치
            double upbit_limit_sl = EXCHANGE_MIN_ORDER_KRW / (balance > 0 ? balance : 1e-9);

            // 3. [보정] 소량(더스트)일 때 업비트 최소금액 기준으로 손절을 과도하게 끌어올리지 않음
            // 만약 upbit_limit_sl이 진입가의 99% 이상으로 매우 진입가에 근접하면
            // 복구 시점에 손절을 원래 target_sl에 두고 경고를 남깁니다.
            double safe_stop_loss;
            if (upbit_limit_sl > avg_buy_price * 0.99) {
                LOG_WARN("{} 복구 포지션 소량 감지: 수량 {:.6f}, upbit_limit_sl {:.0f} → 손절 보정 보류 (target_sl 사용)",
                         market, balance, upbit_limit_sl);
                safe_stop_loss = target_sl;
            } else {
                // 일반적인 경우: 진입가 대비 너무 낮은 손절로 인해 주문이 최소금액 미만이 되지 않도록 상향 조정
                safe_stop_loss = std::max(target_sl, upbit_limit_sl);
            }

            // 포지션 복구
            risk_manager_->enterPosition(
                market,
                avg_buy_price,
                balance,
                safe_stop_loss,
                avg_buy_price * 1.010,
                avg_buy_price * 1.015,
                "RECOVERED"
            );
        }
        
        if (!krw_found) {
            LOG_WARN("⚠️ 계좌에 KRW가 없습니다! (자본금 0원으로 설정됨)");
            risk_manager_->resetCapital(0.0);
        }
        
        LOG_INFO("✅ 계좌 동기화 완료");

    } catch (const std::exception& e) {
        LOG_ERROR("❌ 계좌 동기화 실패: {}", e.what());
    }
}

// ===== [NEW] 동적 필터값 계산 (변동성 기반) =====

double TradingEngine::calculateDynamicFilterValue() {
    // 스캔된 모든 시장의 변동성을 기반으로 필터값 동적 조정
    // 변동성 범위: 0.0 ~ 1.0
    // → 필터값: 0.45 ~ 0.55 범위
    
    if (scanned_markets_.empty()) {
        LOG_WARN("스캔된 시장이 없어 필터값을 기본값으로 유지 (0.5)");
        return 0.5;  // 기본값
    }
    
    // 1. 모든 시장의 변동성 평균 계산
    double total_volatility = 0.0;
    for (const auto& metrics : scanned_markets_) {
        // MarketScanner에서 계산한 volatility 활용
        // volatility가 0~1 범위라고 가정
        total_volatility += metrics.volatility;
    }
    
    double avg_volatility = total_volatility / scanned_markets_.size();
    
    // 2. 변동성 → 필터값 매핑
    // 변동성 낮음 (0.0~0.3): 필터값 높음 (0.55, 충분히 신뢰할 수 있는 신호만)
    // 변동성 중간 (0.3~0.7): 필터값 중간 (0.50, 중립)
    // 변동성 높음 (0.7~1.0): 필터값 낮음 (0.45, 더 많은 기회 포착)
    
    double new_filter_value;
    if (avg_volatility < 0.3) {
        // 낮은 변동성 → 높은 필터값 (0.55)
        new_filter_value = 0.50 + (0.3 - avg_volatility) * 0.1667;  // 최대 0.55
    } else if (avg_volatility > 0.7) {
        // 높은 변동성 → 낮은 필터값 (0.45)
        new_filter_value = 0.50 - (avg_volatility - 0.7) * 0.1667;  // 최소 0.45
    } else {
        // 중간 변동성 → 기본값 (0.50)
        new_filter_value = 0.50;
    }
    
    // 3. 범위 클리핑
    new_filter_value = std::max(0.45, std::min(0.55, new_filter_value));
    
    // 4. 변경이 크면 로깅
    if (std::abs(new_filter_value - dynamic_filter_value_) > 0.01) {
        LOG_INFO("📊 필터값 동적 조정: {:.3f} → {:.3f} (평균 변동성: {:.3f})",
                 dynamic_filter_value_, new_filter_value, avg_volatility);
    }
    
    dynamic_filter_value_ = new_filter_value;
    return dynamic_filter_value_;
}

// ===== [NEW] 포지션 확대 배수 계산 (Win Rate & Profit Factor 기반) =====

double TradingEngine::calculatePositionScaleMultiplier() {
    // 기관급 기준:
    // Win Rate >= 60% AND Profit Factor >= 1.5 → 포지션 확대 허용
    // 
    // 포지션 배수 결정:
    // - WR < 45% || PF < 1.0: 0.5배 (위험 축소)
    // - 45% <= WR < 50% || 1.0 <= PF < 1.2: 0.75배 (보수)
    // - 50% <= WR < 60% || 1.2 <= PF < 1.5: 1.0배 (표준)
    // - WR >= 60% && PF >= 1.5: 1.5배~2.5배 (확대)
    
    auto metrics = risk_manager_->getRiskMetrics();
    
    // 거래 이력이 부족하면 표준 배수 유지
    if (metrics.total_trades < 20) {
        LOG_INFO("거래 데이터 부족 ({}/20) → 포지션 배수 1.0 유지", metrics.total_trades);
        return 1.0;
    }
    
    double win_rate = metrics.win_rate;
    double profit_factor = metrics.profit_factor;
    
    double new_multiplier;
    
    if (win_rate < 0.45 || profit_factor < 1.0) {
        // 성과 양호하지 않음 → 위험 축소
        new_multiplier = 0.5;
    } else if (win_rate < 0.50 || profit_factor < 1.2) {
        // 보수적 성과 → 조신 포지션
        new_multiplier = 0.75;
    } else if (win_rate < 0.60 || profit_factor < 1.5) {
        // 표준 성과 → 기본 포지션
        new_multiplier = 1.0;
    } else {
        // 기관급 성과 → 확대 가능
        // PF와 WR을 조합하여 배수 결정
        // WR 60%~75%: 1.5배~2.0배, PF 1.5~2.5: 추가 0.25배
        double wr_bonus = (win_rate - 0.60) * 10.0;  // 0~1.5
        double pf_bonus = std::min(0.5, (profit_factor - 1.5) * 0.5);  // 0~0.5
        new_multiplier = 1.5 + wr_bonus + pf_bonus;
        new_multiplier = std::min(2.5, new_multiplier);  // 최대 2.5배
    }
    
    // 로깅
    if (std::abs(new_multiplier - position_scale_multiplier_) > 0.01) {
        LOG_INFO("📈 포지션 확대 배수 조정: {:.2f}배 → {:.2f}배 "
                 "(WR: {:.1f}%, PF: {:.2f}, 거래: {})",
                 position_scale_multiplier_, new_multiplier,
                 win_rate * 100.0, profit_factor, metrics.total_trades);
    }
    
    position_scale_multiplier_ = new_multiplier;
    return new_multiplier;
}

// ===== [NEW] ML 기반 최적 필터값 학습 =====

void TradingEngine::learnOptimalFilterValue() {
    // historical P&L 데이터에서 필터값별 성능 분석
    // 알고리즘:
    // 1. 거래 이력에서 signal_filter 기반으로 거래 분류
    // 2. 각 필터값에 대해 성능 지표 계산 (Win Rate, Profit Factor, Sharpe Ratio)
    // 3. 최고 성과 필터값 추천
    
    auto history = risk_manager_->getTradeHistory();
    
    if (history.size() < 50) {
        LOG_INFO("학습 데이터 부족 ({}/50) → ML 학습 미실행", history.size());
        return;
    }
    
    // 필터값별 거래 분류 및 성능 계산
    std::map<double, std::vector<TradeHistory>> trades_by_filter;
    std::map<double, std::vector<double>> returns_by_filter;  // Sharpe Ratio 계산용
    
    // 필터값 범위 (0.45 ~ 0.55, 0.01 단위)
    for (double filter = 0.45; filter <= 0.55; filter += 0.01) {
        trades_by_filter[filter] = std::vector<TradeHistory>();
        returns_by_filter[filter] = std::vector<double>();
    }
    
    // 1. 거래 이력을 필터값별로 분류
    for (const auto& trade : history) {
        // signal_filter를 가장 가까운 0.01 단위로 반올림
        double rounded_filter = std::round(trade.signal_filter * 100.0) / 100.0;
        
        // 유효한 필터값 범위 확인
        if (rounded_filter >= 0.45 && rounded_filter <= 0.55) {
            trades_by_filter[rounded_filter].push_back(trade);
            returns_by_filter[rounded_filter].push_back(trade.profit_loss_pct);
        }
    }
    
    // 2. 각 필터값에 대한 성능 분석
    struct FilterPerformance {
        double filter_value;
        int trade_count;
        double win_rate;
        double avg_return;
        double profit_factor;
        double sharpe_ratio;
        double total_pnl;
        
        FilterPerformance()
            : filter_value(0), trade_count(0), win_rate(0)
            , avg_return(0), profit_factor(0), sharpe_ratio(0), total_pnl(0)
        {}
    };
    
    std::map<double, FilterPerformance> performances;
    double best_sharpe = -999.0;
    double best_filter = 0.5;
    
    for (auto& [filter_val, trades] : trades_by_filter) {
        if (trades.empty()) continue;
        
        FilterPerformance perf;
        perf.filter_value = filter_val;
        perf.trade_count = static_cast<int>(trades.size());
        
        // Win Rate 계산
        int winning_trades = 0;
        double total_profit = 0.0;
        double total_loss = 0.0;  // 손해액 절대값
        
        for (const auto& trade : trades) {
            if (trade.profit_loss > 0) {
                winning_trades++;
                total_profit += trade.profit_loss;
            } else {
                total_loss += std::abs(trade.profit_loss);  // 손해는 절대값으로
            }
        }
        
        perf.win_rate = static_cast<double>(winning_trades) / trades.size();
        perf.total_pnl = total_profit - total_loss;
        
        // Profit Factor 계산 (총 수익 / 총 손실)
        perf.profit_factor = (total_loss > 0) ? (total_profit / total_loss) : total_profit;
        
        // 평균 수익률
        perf.avg_return = perf.total_pnl / trades.size();
        
        // Sharpe Ratio 계산 (리스크 조정 수익률)
        const auto& returns = returns_by_filter[filter_val];
        if (returns.size() > 1) {
            double mean_return = 0.0;
            for (double ret : returns) {
                mean_return += ret;
            }
            mean_return /= returns.size();
            
            // 표준편차 계산
            double variance = 0.0;
            for (double ret : returns) {
                double diff = ret - mean_return;
                variance += diff * diff;
            }
            variance /= returns.size();
            double std_dev = std::sqrt(variance);
            
            // Sharpe Ratio = (평균 수익률 - 무위험률) / 표준편차
            // 무위험률 0으로 가정
            perf.sharpe_ratio = (std_dev > 0.0001) ? (mean_return / std_dev) : 0.0;
        }
        
        performances[filter_val] = perf;
        
        // 최적 필터값 선택 (Sharpe Ratio 기준)
        if (perf.sharpe_ratio > best_sharpe) {
            best_sharpe = perf.sharpe_ratio;
            best_filter = filter_val;
        }
        
        LOG_INFO("필터값 {:.2f}: 거래수={}, 승률={:.1f}%, PF={:.2f}, Sharpe={:.3f}, 총손익={:.0f}",
                 filter_val, perf.trade_count, perf.win_rate * 100.0, 
                 perf.profit_factor, perf.sharpe_ratio, perf.total_pnl);
    }
    
    // 3. 결과 분석 및 추천
    // 추가 조건: Win Rate >= 50% 및 Profit Factor >= 1.2 필터 (안정성)
    std::vector<double> qualified_filters;
    for (auto& [filter_val, perf] : performances) {
        if (perf.win_rate >= 0.50 && perf.profit_factor >= 1.2 && perf.trade_count >= 10) {
            qualified_filters.push_back(filter_val);
        }
    }
    
    if (!qualified_filters.empty()) {
        // 적격 필터 중에서 Sharpe Ratio 최고값 선택
        double best_qualified_sharpe = -999.0;
        for (double f : qualified_filters) {
            if (performances[f].sharpe_ratio > best_qualified_sharpe) {
                best_qualified_sharpe = performances[f].sharpe_ratio;
                best_filter = f;
            }
        }
        
        LOG_INFO("✨ ML 학습 완료 (적격 필터만 고려):");
        LOG_INFO("  추천 필터값: {:.2f} (Sharpe: {:.3f}, 승률: {:.1f}%, PF: {:.2f})",
                 best_filter, best_qualified_sharpe,
                 performances[best_filter].win_rate * 100.0,
                 performances[best_filter].profit_factor);
    } else {
        // 적격 필터가 없으면 전체에서 Sharpe 최고값
        LOG_WARN("✨ ML 학습 (적격 필터 없음, 전체에서 선택):");
        LOG_WARN("  추천 필터값: {:.2f} (Sharpe: {:.3f})", best_filter, best_sharpe);
    }
    
    // 필터 성능 이력 저장 (추세 분석용)
    filter_performance_history_[best_filter] = performances[best_filter].win_rate;
}

// ===== [NEW] Prometheus 메트릭 노출 =====

std::string TradingEngine::exportPrometheusMetrics() const {
    // Prometheus 형식의 메트릭 문자열 생성
    // Grafana와 연동하여 실시간 모니터링 지원
    
    auto metrics = risk_manager_->getRiskMetrics();
    auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    std::ostringstream oss;
    
    // 메타데이터 (한글 설명 추가)
    oss << "# HELP autolife_state AutoLife 거래 엔진 상태 정보\n";
    oss << "# TYPE autolife_state gauge\n";

    // 자본 관련 설명
    oss << "# HELP autolife_capital_total 총 자본 (KRW)\n";
    oss << "# TYPE autolife_capital_total gauge\n";
    oss << "# HELP autolife_capital_available 사용 가능한 현금(가용 자본, KRW)\n";
    oss << "# TYPE autolife_capital_available gauge\n";
    oss << "# HELP autolife_capital_invested 투자 중인 금액(포지션에 묶인 자금, KRW)\n";
    oss << "# TYPE autolife_capital_invested gauge\n";

    // 손익 관련 설명
    oss << "# HELP autolife_pnl_realized 실현 손익(누적, KRW)\n";
    oss << "# TYPE autolife_pnl_realized gauge\n";
    oss << "# HELP autolife_pnl_unrealized 미실현 손익(현재 포지션 기준, KRW)\n";
    oss << "# TYPE autolife_pnl_unrealized gauge\n";
    oss << "# HELP autolife_pnl_total 총 손익(실현+미실현, KRW)\n";
    oss << "# TYPE autolife_pnl_total gauge\n";
    oss << "# HELP autolife_pnl_total_pct 전체 포트폴리오 수익률(%)\n";
    oss << "# TYPE autolife_pnl_total_pct gauge\n";

    // 리스크 관련 설명
    oss << "# HELP autolife_drawdown_max 최대 누적 손실 비율(포트폴리오)\n";
    oss << "# TYPE autolife_drawdown_max gauge\n";
    oss << "# HELP autolife_drawdown_current 현재 손실 비율(포트폴리오)\n";
    oss << "# TYPE autolife_drawdown_current gauge\n";

    // 포지션 관련 설명
    oss << "# HELP autolife_positions_active 현재 보유 포지션 수\n";
    oss << "# TYPE autolife_positions_active gauge\n";
    oss << "# HELP autolife_positions_max 허용 최대 포지션 수\n";
    oss << "# TYPE autolife_positions_max gauge\n";

    // 거래 통계 설명
    oss << "# HELP autolife_trades_total 누적 거래 수\n";
    oss << "# TYPE autolife_trades_total counter\n";
    oss << "# HELP autolife_trades_winning 누적 수익 거래 수\n";
    oss << "# TYPE autolife_trades_winning counter\n";
    oss << "# HELP autolife_trades_losing 누적 손실 거래 수\n";
    oss << "# TYPE autolife_trades_losing counter\n";

    // 성과 지표 설명
    oss << "# HELP autolife_winrate 승률(0~1)\n";
    oss << "# TYPE autolife_winrate gauge\n";
    oss << "# HELP autolife_profit_factor 수익요인(Profit Factor)\n";
    oss << "# TYPE autolife_profit_factor gauge\n";
    oss << "# HELP autolife_sharpe_ratio 샤프지수(성과 측정)\n";
    oss << "# TYPE autolife_sharpe_ratio gauge\n";

    // 엔진 상태 설명
    oss << "# HELP autolife_engine_running 엔진 실행 상태(1=실행중,0=정지)\n";
    oss << "# TYPE autolife_engine_running gauge\n";
    oss << "# HELP autolife_engine_scans_total 수행된 스캔 횟수\n";
    oss << "# TYPE autolife_engine_scans_total counter\n";
    oss << "# HELP autolife_engine_signals_total 생성된 신호 총수\n";
    oss << "# TYPE autolife_engine_signals_total counter\n";

    // 동적 필터/스케일 설명
    oss << "# HELP autolife_filter_value_dynamic 동적 필터값 (0~1)\n";
    oss << "# TYPE autolife_filter_value_dynamic gauge\n";
    oss << "# HELP autolife_position_scale_multiplier 포지션 확대 배수\n";
    oss << "# TYPE autolife_position_scale_multiplier gauge\n";

    // 엔진 거래 메트릭 설명
    oss << "# HELP autolife_buy_orders_total 누적 매수 주문 수\n";
    oss << "# TYPE autolife_buy_orders_total counter\n";
    oss << "# HELP autolife_sell_orders_total 누적 매도 주문 수\n";
    oss << "# TYPE autolife_sell_orders_total counter\n";
    oss << "# HELP autolife_pnl_cumulative 누적 실현 손익(포지션 종료 후 합계, KRW)\n";
    oss << "# TYPE autolife_pnl_cumulative gauge\n";
    
    // 1. 자본 관련 메트릭
    oss << "autolife_capital_total{mode=\"" 
        << (config_.mode == TradingMode::LIVE ? "LIVE" : "PAPER") << "\"} "
        << metrics.total_capital << " " << timestamp_ms << "\n";
    
    oss << "autolife_capital_available{} " << metrics.available_capital << " " << timestamp_ms << "\n";
    oss << "autolife_capital_invested{} " << metrics.invested_capital << " " << timestamp_ms << "\n";
    
    // 2. 손익 관련 메트릭
    oss << "autolife_pnl_realized{} " << metrics.realized_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_unrealized{} " << metrics.unrealized_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_total{} " << metrics.total_pnl << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_total_pct{} " << metrics.total_pnl_pct << " " << timestamp_ms << "\n";
    
    // 3. 리스크 관련 메트릭
    oss << "autolife_drawdown_max{} " << metrics.max_drawdown << " " << timestamp_ms << "\n";
    oss << "autolife_drawdown_current{} " << metrics.current_drawdown << " " << timestamp_ms << "\n";
    
    // 4. 포지션 관련 메트릭
    oss << "autolife_positions_active{} " << metrics.active_positions << " " << timestamp_ms << "\n";
    oss << "autolife_positions_max{} " << config_.max_positions << " " << timestamp_ms << "\n";
    
    // 5. 거래 통계
    oss << "autolife_trades_total{} " << metrics.total_trades << " " << timestamp_ms << "\n";
    oss << "autolife_trades_winning{} " << metrics.winning_trades << " " << timestamp_ms << "\n";
    oss << "autolife_trades_losing{} " << metrics.losing_trades << " " << timestamp_ms << "\n";
    
    // 6. 거래 성과 지표
    oss << "autolife_winrate{} " << metrics.win_rate << " " << timestamp_ms << "\n";
    oss << "autolife_profit_factor{} " << metrics.profit_factor << " " << timestamp_ms << "\n";
    oss << "autolife_sharpe_ratio{} " << metrics.sharpe_ratio << " " << timestamp_ms << "\n";
    
    // 7. 엔진 상태 메트릭
    oss << "autolife_engine_running{} " << (running_ ? 1 : 0) << " " << timestamp_ms << "\n";
    oss << "autolife_engine_scans_total{} " << total_scans_ << " " << timestamp_ms << "\n";
    oss << "autolife_engine_signals_total{} " << total_signals_ << " " << timestamp_ms << "\n";
    
    // 8. [NEW] 동적 필터 및 포지션 확대 메트릭
    oss << "autolife_filter_value_dynamic{} " << dynamic_filter_value_ << " " << timestamp_ms << "\n";
    oss << "autolife_position_scale_multiplier{} " << position_scale_multiplier_ << " " << timestamp_ms << "\n";
    
    // 9. [NEW] 거래 엔진 메트릭
    oss << "autolife_buy_orders_total{} " << prometheus_metrics_.total_buy_orders << " " << timestamp_ms << "\n";
    oss << "autolife_sell_orders_total{} " << prometheus_metrics_.total_sell_orders << " " << timestamp_ms << "\n";
    oss << "autolife_pnl_cumulative{} " << prometheus_metrics_.cumulative_realized_pnl << " " << timestamp_ms << "\n";
    
    oss << "# End of AutoLife Metrics\n";
    
    return oss.str();
}

// [NEW] Prometheus HTTP 서버 구현
void TradingEngine::runPrometheusHttpServer(int port) {
    prometheus_server_port_ = port;
    prometheus_server_running_ = true;
    
    LOG_INFO("📊 Prometheus HTTP 서버 시작 (포트: {})", port);
    
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        LOG_ERROR("WSAStartup 실패");
        prometheus_server_running_ = false;
        return;
    }
    
    SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        LOG_ERROR("소켓 생성 실패");
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    // 포트 재사용 설정 (TIME_WAIT 상태에서도 포트 사용 가능)
    int reuse = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<char*>(&reuse), sizeof(reuse)) < 0) {
        LOG_WARN("SO_REUSEADDR 설정 실패");
    }
    
    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<u_short>(port));
    
    // Use inet_pton instead of deprecated inet_addr
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) != 1) {
        LOG_ERROR("inet_pton 실패");
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        LOG_ERROR("bind 실패 (포트: {})", port);
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    if (listen(listen_socket, 5) == SOCKET_ERROR) {
        LOG_ERROR("listen 실패");
        closesocket(listen_socket);
        prometheus_server_running_ = false;
        WSACleanup();
        return;
    }
    
    LOG_INFO("✅ Prometheus 메트릭 서버 준비 완료 (http://localhost:{}/metrics)", port);
    
    // 서버 루프
    while (prometheus_server_running_) {
        sockaddr_in client_addr = {};
        int client_addr_size = sizeof(client_addr);
        
        // 5초 타임아웃 설정
        timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket, &read_fds);
        
        int select_result = select(0, &read_fds, nullptr, nullptr, &timeout);
        if (select_result == 0) {
            // 타임아웃 - 다시 체크
            continue;
        }
        if (select_result == SOCKET_ERROR) {
            LOG_WARN("select 실패");
            break;
        }
        
        SOCKET client_socket = accept(listen_socket, 
                                      reinterpret_cast<sockaddr*>(&client_addr), 
                                      &client_addr_size);
        if (client_socket == INVALID_SOCKET) {
            LOG_WARN("accept 실패");
            continue;
        }
        
        // HTTP 요청 읽기
        char buffer[4096] = {0};
        int recv_result = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (recv_result > 0) {
            buffer[recv_result] = '\0';
            std::string request(buffer);
            
            // GET /metrics 확인
            if (request.find("GET /metrics") == 0) {
                // Prometheus 메트릭 생성
                std::string metrics = exportPrometheusMetrics();
                
                // HTTP 응답 작성
                std::ostringstream response;
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                         << "Content-Length: " << metrics.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << metrics;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            } 
            else if (request.find("GET /health") == 0) {
                // 헬스 체크 엔드포인트
                std::string health_response = "OK";
                std::ostringstream response;
                response << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/plain; charset=utf-8\r\n"
                         << "Content-Length: " <<health_response.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << health_response;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            }
            else {
                // 404 응답
                std::string error_response = "Not Found";
                std::ostringstream response;
                response << "HTTP/1.1 404 Not Found\r\n"
                         << "Content-Type: text/plain; charset=utf-8\r\n"
                         << "Content-Length: " << error_response.length() << "\r\n"
                         << "Connection: close\r\n"
                         << "\r\n"
                         << error_response;
                
                std::string response_str = response.str();
                send(client_socket, response_str.c_str(), static_cast<int>(response_str.length()), 0);
            }
        }
        
        closesocket(client_socket);
    }
    
    closesocket(listen_socket);
    WSACleanup();
    
    LOG_INFO("📊 Prometheus HTTP 서버 종료");
}

} // namespace engine
} // namespace autolife

