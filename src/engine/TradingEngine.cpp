#include "engine/TradingEngine.h"
#include "common/Logger.h"
#include "strategy/ScalpingStrategy.h"
#include "strategy/MomentumStrategy.h" 
#include "strategy/BreakoutStrategy.h"
#include "strategy/MeanReversionStrategy.h"
#include "strategy/GridTradingStrategy.h"
#include "analytics/TechnicalIndicators.h"
#include <chrono>
#include <thread>
#include <algorithm>

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
    
    // 리스크 설정
    risk_manager_->setMaxPositions(config.max_positions);
    risk_manager_->setMaxDailyTrades(config.max_daily_trades);
    risk_manager_->setMaxDrawdown(config.max_drawdown);
    
    // ✅ 전략 등록
    auto scalping = std::make_shared<strategy::ScalpingStrategy>(http_client);
    strategy_manager_->registerStrategy(scalping);
    LOG_INFO("스캘핑 전략 등록 완료");
    
    auto momentum = std::make_shared<strategy::MomentumStrategy>(http_client); 
    strategy_manager_->registerStrategy(momentum);                              
    LOG_INFO("모멘텀 전략 등록 완료");                                            

    auto breakout = std::make_shared<strategy::BreakoutStrategy>(http_client);
    strategy_manager_->registerStrategy(breakout);
    LOG_INFO("돌파 전략 등록 완료");

    auto mean_reversion = std::make_shared<strategy::MeanReversionStrategy>(http_client);
    strategy_manager_->registerStrategy(mean_reversion);
    LOG_INFO("평균회귀 전략 등록 완료");

    auto grid_trading = std::make_shared<strategy::GridTradingStrategy>(http_client);
    strategy_manager_->registerStrategy(grid_trading);
    LOG_INFO("그리드 트레이딩 전략 등록 완료");
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
                current_price
            );
            
            if (signals.empty()) {
                continue;
            }
            
            // 신호 필터링 (강도 0.6 이상)
            auto filtered = strategy_manager_->filterSignals(signals, 0.6); //0.6 -> 0.3 임시 완화
            
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
    
    // 강도 순으로 정렬
    std::sort(pending_signals_.begin(), pending_signals_.end(),
        [](const strategy::Signal& a, const strategy::Signal& b) {
            return a.strength > b.strength;
        });
    
    int executed = 0;
    
    for (const auto& signal : pending_signals_) {
        // 매수 신호만 처리
        if (signal.type != strategy::SignalType::BUY && 
            signal.type != strategy::SignalType::STRONG_BUY) {
            continue;
        }
        
        // 리스크 체크
        if (!risk_manager_->canEnterPosition(
            signal.market,
            signal.entry_price,
            signal.position_size,
            signal.strategy_name
        )) {
            LOG_WARN("{} 진입 불가 (리스크 제한)", signal.market);
            continue;
        }
        
        // 주문 실행
        if (executeBuyOrder(signal.market, signal)) {
            executed++;
        }
    }
    
    LOG_INFO("{}개 신호 실행 완료", executed);
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

        auto ask_units = orderbook[0]["orderbook_units"];
        double best_ask_price = ask_units[0]["ask_price"].get<double>(); // 매도 1호가
        
        // 2. 투자 금액 및 수량 계산
        auto metrics = risk_manager_->getRiskMetrics();
       // [✅ 최후의 근본적 보정 로직 추가]
        // 업비트 최소 주문 금액(5,000원)을 맞추기 위한 최종 방어선
        double min_required_ratio = (config_.min_order_krw + 600.0) / metrics.available_capital;

        // 함수 시작 부분
        auto modified_signal = signal; // 복사본 생성

        // 보정 로직 (이제 에러 안 남)
        if (modified_signal.position_size > 0 && modified_signal.position_size < min_required_ratio) {
            LOG_INFO("{} - [엔진 레벨 보정] 기존 비중 {:.4f} -> 보정 비중 {:.4f}", 
                     market, modified_signal.position_size, min_required_ratio);
            modified_signal.position_size = min_required_ratio;
        }

        double invest_amount = metrics.available_capital * modified_signal.position_size;

        LOG_INFO("{} - [계산] 가용자본: {:.0f}, 비중: {:.4f}, 투자예정: {:.0f}", 
                 market, metrics.available_capital, modified_signal.position_size, invest_amount);
        
        if (invest_amount < config_.min_order_krw) {
            // 이제 이 블록은 웬만하면 타지 않게 됩니다.
            LOG_WARN("{} - 최소 주문금액 미달 (금액: {:.0f}, 최소: {:.0f})", 
                     market, invest_amount, config_.min_order_krw);
            return false;
        }

        if (invest_amount > config_.max_order_krw) invest_amount = config_.max_order_krw;
        
        // 지정가 주문 수량 계산 (소수점 8자리까지)
        double quantity = invest_amount / best_ask_price;
        
        // 문자열 변환 (업비트는 소수점 처리에 민감하므로 포맷팅 주의)
        std::string price_str = std::to_string((long long)best_ask_price); // 원화는 정수
        // [제안] 소수점 정밀도 제어 (sprintf 또는 stringstream 사용)
        char buffer[64];
        // 수량은 소수점 8자리까지, 불필요한 0 제거 로직 필요하면 추가
        std::snprintf(buffer, sizeof(buffer), "%.8f", quantity); 
        std::string vol_str(buffer);
        
        LOG_INFO("  주문 준비: 평단 {:.0f}, 수량 {}, 금액 {:.0f}", 
                 best_ask_price, vol_str, invest_amount);

        // 3. [안전] 실전 매수 주문 (지정가 Limit Order)
        if (config_.mode == TradingMode::LIVE && !config_.dry_run) {
            
            // 지정가 매수 주문 전송
            auto order_res = http_client_->placeOrder(
                market, 
                "bid",      // 매수
                vol_str,    // 수량
                price_str,  // 가격 (지정가)
                "limit"     // 지정가 주문
            );
            
            if (!order_res.contains("uuid")) {
                LOG_ERROR("주문 요청 실패: {}", order_res.dump());
                return false;
            }
            
            std::string uuid = order_res["uuid"].get<std::string>();
            LOG_INFO("✅ 주문 전송 완료 (UUID: {})", uuid);
            
            // 4. [검증] 체결 확인 (Fill Verification)
            //    주문이 서버에 도달했어도, '체결'이 되었는지는 확인해야 함.
            //    약 1초 대기 후 상태 조회
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            auto check_res = http_client_->getOrder(uuid);
            std::string state = check_res["state"].get<std::string>();
            
            // done(체결됨) 또는 cancel(취소됨) 상태 확인
            // wait(미체결) 상태라면? -> 스캘핑이므로 즉시 취소하거나 시장가로 긁어야 함
            // 여기서는 간단하게 '미체결 시 취소' 전략 사용
            
            double executed_volume = 0.0;
            double paid_fee = 0.0;
            double avg_price = best_ask_price; // 기본값

            if (state == "wait") {
                LOG_WARN("⏳ 주문 미체결 (1초 경과) -> 주문 취소 시도");
                http_client_->cancelOrder(uuid);
                return false; // 진입 실패 처리
            } else if (state == "done" || state == "cancel") {
                // 부분 체결이라도 되었는지 확인
                if (check_res.contains("trades") && !check_res["trades"].empty()) {
                    // 실제 체결된 평균단가와 수량 다시 계산
                    double total_funds = 0.0;
                    double total_vol = 0.0;
                    
                    for (const auto& trade : check_res["trades"]) {
                        double trade_vol = std::stod(trade["volume"].get<std::string>());
                        double trade_price = std::stod(trade["price"].get<std::string>());
                        total_vol += trade_vol;
                        total_funds += trade_vol * trade_price;
                    }
                    
                    if (total_vol > 0) {
                        executed_volume = total_vol;
                        avg_price = total_funds / total_vol;
                        LOG_INFO("🆗 실제 체결 확인: 수량 {:.8f}, 평단 {:.0f}", executed_volume, avg_price);
                    }
                }
            }
            
            if (executed_volume <= 0) {
                LOG_WARN("❌ 체결 수량 0 (진입 실패)");
                return false;
            }

            // 5. RiskManager 등록 (실제 체결 데이터 기반)
            risk_manager_->enterPosition(
                market,
                avg_price,        // 실제 체결 평단
                executed_volume,  // 실제 체결 수량
                avg_price * 0.98, // SL -2% (예시)
                avg_price * 1.020,// TP 1.5%
                avg_price * 1.030, // TP 3.0%
                signal.strategy_name
            );
            
            return true;
        } 
        else {
            // Paper Trading (모의투자) 모드
            risk_manager_->enterPosition(
                market, best_ask_price, quantity, 
                best_ask_price * 0.98, best_ask_price * 1.015, best_ask_price * 1.03, 
                signal.strategy_name
            );
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
    
    if (positions.empty()) {
        return;
    }
    
    // 2. 보유 중인 종목들의 마켓 코드 수집 (Batch 조회를 위함)
    std::vector<std::string> markets;
    markets.reserve(positions.size());
    for (const auto& pos : positions) {
        markets.push_back(pos.market);
    }
    
    if (should_log) {
        LOG_INFO("===== 포지션 모니터링 ({}종목) =====", positions.size());
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
        if (strategy_manager_) {
            // 해당 포지션을 담당하는 전략 찾기 (pos.strategy_name 사용)
            auto strategy = strategy_manager_->getStrategy(pos.strategy_name);
            
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
            LOG_INFO("  {} - 진입: {:.0f}, 현재: {:.0f}, 손익: {:.0f} ({:+.2f}%)",
                     pos.market, updated_pos->entry_price, current_price,
                     updated_pos->unrealized_pnl, updated_pos->unrealized_pnl_pct * 100.0);
        }
        
        // --- 매도 로직 (기존과 동일) ---

        // 1차 익절 체크 (50% 청산)
        if (!updated_pos->half_closed && current_price >= updated_pos->take_profit_1) {
            LOG_INFO("💰 1차 익절 조건 도달! (수익률: {:+.2f}%)", updated_pos->unrealized_pnl_pct * 100.0);
            executePartialSell(pos.market, *updated_pos, current_price);
            continue; // 부분 매도 후 다음 종목으로
        }
        
        // 전체 청산 체크 (손절 or 2차 익절 or 전략적 청산)
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
    
    // 1. 최소 주문 금액 체크
    if (invest_amount < config_.min_order_krw) {
        LOG_WARN("매도 금액 부족: {:.0f} < {:.0f} (잔여 가치 부족)", invest_amount, config_.min_order_krw);
        return false;
    }
    
    // 2. 문자열 변환 시 std::to_string 대신 정밀도를 고정한 stringstream 사용 (매우 중요)
        std::stringstream ss;
        ss << std::fixed << std::setprecision(8) << sell_quantity;
        std::string quantity_str = ss.str();

    // 2. 실전 주문 실행
    bool order_success = true;
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run) {
            LOG_WARN("🔶 DRY RUN: 매도 시뮬레이션 완료");
        } else {
            try {
                // [수정] 업비트 시장가 매도: ord_type = "market"
                // 가격 필드("")는 비워둠
                auto order = http_client_->placeOrder(
                    market, 
                    "ask", 
                    quantity_str, 
                    "", 
                    "market" // [중요] 시장가 매도
                );
                
                LOG_INFO("✅ 매도 주문 접수 완료: {}", order["uuid"].get<std::string>());
                
            } catch (const std::exception& e) {
                LOG_ERROR("❌ 매도 API 호출 실패: {}", e.what());
                order_success = false;
            }
        }
    }
    
    if (!order_success) return false;
    
    // 3. 수익금 계산
    double gross_pnl = (current_price - position.entry_price) * sell_quantity;
    bool is_win = gross_pnl > 0;
    
    // 4. RiskManager 업데이트 (포지션 삭제)
    risk_manager_->exitPosition(market, current_price, reason);
    
    // 5. [핵심 수정] StrategyManager를 통해 전략을 찾아 통계 업데이트 & 잠금 해제
    if (strategy_manager_) {
        // Position 구조체에 저장된 strategy_name("Advanced Scalping" 등)으로 전략 찾기
        // (StrategyManager에 getStrategy 함수가 추가되어 있어야 함)
        auto strategy = strategy_manager_->getStrategy(position.strategy_name);
        
        if (strategy) {
            // [중요] market을 넘겨서 active_positions_에서 삭제하게 함
            strategy->updateStatistics(market, is_win, gross_pnl);
            LOG_INFO("📊 전략({}) 통계 업데이트 및 재진입 허용", position.strategy_name);
        } else {
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
    
    // 1. 최소 주문 금액 체크 및 대응
    if (invest_amount < config_.min_order_krw) {
        LOG_WARN("⚠️ 부분 매도 금액 부족 ({:.0f}원). 전량 매도로 전환합니다.", invest_amount);
        
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

    // 2. 실전 주문 실행
    bool order_success = true;
    if (config_.mode == TradingMode::LIVE) {
        if (config_.dry_run) {
            LOG_WARN("🔶 DRY RUN: 부분 매도 시뮬레이션");
        } else {
            try {
                // [수정] 시장가 매도
                auto order = http_client_->placeOrder(
                    market, 
                    "ask", 
                    quantity_str, 
                    "", 
                    "market" // [중요] 시장가 매도
                );
                
                LOG_INFO("✅ 부분 매도 성공: {}", order["uuid"].get<std::string>());
                
            } catch (const std::exception& e) {
                LOG_ERROR("❌ 부분 매도 실패: {}", e.what());
                order_success = false;
            }
        }
    }
    
    if (!order_success) {
        return false;
    }
    
    // 3. RiskManager 업데이트 (부분 청산 반영)
    risk_manager_->partialExit(market, current_price);
    
    return true;
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
        if (ticker.contains("trade_price") && !ticker["trade_price"].is_null()) {
            // value()를 사용하여 타입이 모호해도 double로 강제 변환 시도
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
    LOG_INFO("최종 성과 보고서");
    LOG_INFO("========================================");
    LOG_INFO("실행 시간: {:.1f}시간", runtime_hours);
    LOG_INFO("총 스캔: {}, 신호: {}, 거래: {}",
             total_scans_, total_signals_, metrics.total_trades);
    LOG_INFO("");
    LOG_INFO("초기 자본: {:.0f} KRW", config_.initial_capital);
    LOG_INFO("최종 자본: {:.0f} KRW", metrics.total_capital);
    LOG_INFO("총 손익: {:.0f} KRW ({:+.2f}%)",
             metrics.total_pnl, metrics.total_pnl_pct * 100);
    LOG_INFO("");
    LOG_INFO("승률: {:.1f}% ({}/{})",
             metrics.win_rate * 100,
             metrics.winning_trades,
             metrics.total_trades);
    LOG_INFO("Profit Factor: {:.2f}", metrics.profit_factor);
    LOG_INFO("Sharpe Ratio: {:.2f}", metrics.sharpe_ratio);
    LOG_INFO("Max Drawdown: {:.2f}%", metrics.max_drawdown * 100);
    LOG_INFO("");
    
    // 거래 이력 출력
    if (!history.empty()) {
        LOG_INFO("거래 이력 (최근 10개):");
        int count = 0;
        for (auto it = history.rbegin(); it != history.rend() && count < 10; ++it, ++count) {
            LOG_INFO("  {} | 진입: {:.0f}, 청산: {:.0f} | {:+.2f}% | {}",
                     it->market,
                     it->entry_price,
                     it->exit_price,
                     it->profit_loss_pct * 100,
                     it->exit_reason);
        }
    }
    
    LOG_INFO("========================================");
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
            
            // 짜투리(Dust) 무시
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
            double upbit_limit_sl = 5100.0 / (balance > 0 ? balance : 1e-9);

            // 3. [보정] 둘 중 높은 가격을 손절가로 채택
            double safe_stop_loss = std::max(target_sl, upbit_limit_sl);

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

} // namespace engine
} // namespace autolife
