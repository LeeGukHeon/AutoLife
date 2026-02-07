#pragma once

#include "common/Types.h"
#include "network/UpbitHttpClient.h"
#include "analytics/MarketScanner.h"
#include "strategy/StrategyManager.h"
#include "risk/RiskManager.h"
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

namespace autolife {
namespace engine {

// 거래 모드
enum class TradingMode {
    LIVE,           // 실전 거래
    PAPER,          // 모의 거래 (주문 실행 안함)
    BACKTEST        // 백테스트
};

// 엔진 설정
struct EngineConfig {
    TradingMode mode;
    double initial_capital;
    
    // 스캔 설정
    int scan_interval_seconds;
    long long min_volume_krw;  // ✅ int → long long 변경
    
    // 리스크 설정
    int max_positions;
    int max_daily_trades;
    double max_drawdown;
    
    // 실전 거래 안전 설정 (새로 추가)
    double max_daily_loss_krw = 50000.0;    // 일일 최대 손실 5만원
    double max_order_krw = 500000.0;        // 단일 주문 최대 50만원
    double min_order_krw = 5000.0;          // 단일 주문 최소 5천원
    bool dry_run = true;                    // 실전모드라도 실제 주문 안하고 로그만

    // 전략 설정
    std::vector<std::string> enabled_strategies;
    
    EngineConfig()
        : mode(TradingMode::PAPER)
        , initial_capital(1000000)
        , scan_interval_seconds(60)
        , min_volume_krw(5000000000LL)  // ✅ LL 추가
        , max_positions(5)
        , max_daily_trades(20)
        , max_drawdown(0.10)
    {}
};

// Trading Engine - 전체 거래 시스템
class TradingEngine {
public:
    TradingEngine(
        const EngineConfig& config,
        std::shared_ptr<network::UpbitHttpClient> http_client
    );
    
    ~TradingEngine();
    
    // ===== 엔진 제어 =====
    
    bool start();
    void stop();
    bool isRunning() const { return running_; }
    
    // ===== 메인 루프 =====
    
    void run();  // 메인 거래 루프 (블로킹)
    
    // ===== 상태 조회 =====
    
    risk::RiskManager::RiskMetrics getMetrics() const;
    std::vector<risk::Position> getPositions() const;
    std::vector<risk::TradeHistory> getTradeHistory() const;
    
    // ===== 수동 제어 (테스트용) =====
    
    void manualScan();
    void manualClosePosition(const std::string& market);
    void manualCloseAll();
    
private:
    // ===== 메인 프로세스 =====
    
    void scanMarkets();           // 시장 스캔
    void generateSignals();       // 매매 신호 생성
    void executeSignals();        // 신호 실행
    void monitorPositions();      // 포지션 모니터링
    void updateMetrics();         // 메트릭 업데이트
    
    // ===== 주문 실행 =====
    
    bool executeBuyOrder(
        const std::string& market,
        const strategy::Signal& signal
    );
    
    bool executeSellOrder(
        const std::string& market,
        const risk::Position& position,
        const std::string& reason
    );
    
    bool executePartialSell(
        const std::string& market,
        const risk::Position& position
    );
    
    // ===== 헬퍼 함수 =====
    
    double getCurrentPrice(const std::string& market);
    bool hasEnoughBalance(double required_krw);
    void logPerformance();
    // [추가] 계좌 상태를 조회하여 RiskManager와 동기화
    void syncAccountState();
    // ===== 멤버 변수 =====
    
    EngineConfig config_;
    
    std::shared_ptr<network::UpbitHttpClient> http_client_;
    std::unique_ptr<analytics::MarketScanner> scanner_;
    std::unique_ptr<strategy::StrategyManager> strategy_manager_;
    std::unique_ptr<risk::RiskManager> risk_manager_;
    
    // 스캔 결과
    std::vector<analytics::CoinMetrics> scanned_markets_;
    std::vector<strategy::Signal> pending_signals_;
    
    // 스레드 제어
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> worker_thread_;
    mutable std::mutex mutex_;
    
    // 통계
    long long start_time_;
    int total_scans_;
    int total_signals_;
};

} // namespace engine
} // namespace autolife
