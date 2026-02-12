#pragma once

#include "common/Types.h"
#include "network/UpbitHttpClient.h"
#include "analytics/MarketScanner.h"
#include "analytics/RegimeDetector.h"
#include "strategy/StrategyManager.h"
#include "risk/RiskManager.h"
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>

#include "engine/EngineConfig.h"
#include "execution/OrderManager.h"

namespace autolife {
namespace engine {

// TradingMode and EngineConfig moved to EngineConfig.h

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
    
    // ===== 주문 실행 (개선) =====
    
    bool executeBuyOrder(
        const std::string& market,
        const strategy::Signal& signal
    );
    
    bool executeSellOrder(
        const std::string& market,
        const risk::Position& position,
        const std::string& reason,
        double current_price
    );
    
    bool executePartialSell(
        const std::string& market,
        const risk::Position& position,
        double current_price
    );
    
    // [✅ 추가] 주문 헬퍼 함수들
    
    // LIMIT 주문으로 매수 (재시도 로직 포함)
    struct LimitOrderResult {
        bool success;
        std::string order_uuid;
        double executed_price;
        double executed_volume;
        int retry_count;
        std::string error_message;
    };
    
    LimitOrderResult executeLimitBuyOrder(
        const std::string& market,
        double entry_price,      // 지정가
        double quantity,         // 주문 수량
        int max_retries,         // 최대 재시도
        int retry_wait_ms        // 재시도 간격 (ms)
    );
    
    // LIMIT 주문으로 매도
    LimitOrderResult executeLimitSellOrder(
        const std::string& market,
        double exit_price,
        double quantity,
        int max_retries,
        int retry_wait_ms
    );
    
    // 시장가 주문 (폴백용)
    bool executeMarketBuyOrder(
        const std::string& market,
        double quantity,
        double& out_avg_price,
        double& out_volume
    );
    
    bool executeMarketSellOrder(
        const std::string& market,
        double quantity
    );
    
    // LIMIT 주문 최적 가격 계산 (시장 상황 반영)
    double calculateOptimalBuyPrice(
        const std::string& market,
        double base_price,
        const nlohmann::json& orderbook
    );
    
    double calculateOptimalSellPrice(
        const std::string& market,
        double base_price,
        const nlohmann::json& orderbook
    );
    
    // 주문 상태 검증 및 체결 확인
    struct OrderFillInfo {
        bool is_filled;           // 완전히 체결되었는가
        bool is_partially_filled; // 부분 체결되었는가
        double filled_volume;     // 체결된 수량
        double avg_price;         // 평균 체결가
        double fee;               // 수수료
    };
    
    OrderFillInfo verifyOrderFill(
        const std::string& uuid,
        const std::string& market,
        double order_volume
    );

    double estimateOrderbookVWAPPrice(
        const nlohmann::json& orderbook,
        double target_volume,
        bool is_buy
    ) const;

    double estimateOrderbookSlippagePct(
        const nlohmann::json& orderbook,
        double target_volume,
        bool is_buy,
        double reference_price
    ) const;
    
    // ===== 헬퍼 함수 =====
    
    double getCurrentPrice(const std::string& market);
    bool hasEnoughBalance(double required_krw);
    void logPerformance();
    // [추가] 계좌 상태를 조회하여 RiskManager와 동기화
    void syncAccountState();

    // ===== 상태 저장/복구 =====
    void loadState();
    void saveState();
    void runStatePersistence();
    
    // ===== [NEW] 동적 필터 및 포지션 확대 기능 =====
    
    // 시장 변동성을 기반으로 필터값을 동적 조정 (0.45~0.55)
    // 높은 변동성 → 낮은 필터값 (더 많은 신호 포착)
    // 낮은 변동성 → 높은 필터값 (품질 높은 신호만)
    double calculateDynamicFilterValue();
    
    // Win Rate >= 60%, Profit Factor >= 1.5일 때 포지션 확대
    // 기관급 성능 기준 적용
    double calculatePositionScaleMultiplier();
    
    // Historical P&L 데이터에서 최적 필터값 학습 (ML)
    // 이전 거래 성과에서 최적의 필터 임계값 자동 계산
    void learnOptimalFilterValue();
    
    // Prometheus 메트릭 노출 (실시간 모니터링용)
    // Grafana 연동을 위한 메트릭 데이터 생성
    std::string exportPrometheusMetrics() const;
    
    // [NEW] Prometheus HTTP 서버 (메트릭 노출)
    // /metrics 엔드포인트로 Prometheus 형식 메트릭 제공
    void runPrometheusHttpServer(int port = 8080);
    
    // ===== 멤버 변수 =====
    
    EngineConfig config_;
    
    std::shared_ptr<network::UpbitHttpClient> http_client_;
    std::unique_ptr<analytics::MarketScanner> scanner_;
    std::unique_ptr<strategy::StrategyManager> strategy_manager_;
    std::unique_ptr<risk::RiskManager> risk_manager_;
    std::unique_ptr<execution::OrderManager> order_manager_;
    std::unique_ptr<analytics::RegimeDetector> regime_detector_;
    
    // 스캔 결과
    std::vector<analytics::CoinMetrics> scanned_markets_;
    std::vector<strategy::Signal> pending_signals_;
    
    // 스레드 제어
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> worker_thread_;
    std::unique_ptr<std::thread> state_persist_thread_;
    std::atomic<bool> state_persist_running_{false};
    mutable std::mutex mutex_;
    
    // [추가] 스캔 및 동기화 타이밍
    std::chrono::steady_clock::time_point last_scan_time_;
    std::chrono::steady_clock::time_point last_account_sync_time_;
    
    // ===== [NEW] 동적 필터 및 포지션 확대 멤버 =====
    
    // 현재 동적 필터값 (0.45~0.55 범위)
    // 초기값: 0.5 (중립)
    double dynamic_filter_value_ = 0.5;
    
    // 포지션 확대 배수 (기본 1.0, 최대 2.5)
    // Win Rate ≥ 60%, PF ≥ 1.5일 때 자동 증가
    double position_scale_multiplier_ = 1.0;
    
    // ML 모듈을 위한 최적 필터값 학습 데이터
    // key: 필터값, value: 해당 필터값에서의 승률
    std::map<double, double> filter_performance_history_;
    
    // Prometheus 메트릭 누적용 (메모리 효율)
    struct PrometheusMetrics {
        long long total_buy_orders = 0;
        long long total_sell_orders = 0;
        double cumulative_realized_pnl = 0.0;
        int active_strategies_count = 0;
        double last_update_timestamp = 0.0;
    } prometheus_metrics_;
    
    // [NEW] Prometheus HTTP 서버 관련
    int prometheus_server_port_ = 8080;  // HTTP 서버 포트 (기본값: 8080)
    std::unique_ptr<std::thread> prometheus_http_thread_;  // HTTP 서버 스레드
    std::atomic<bool> prometheus_server_running_ = false;  // HTTP 서버 실행 상태

    std::map<std::string, std::string> recovered_strategy_map_;

    struct PersistedPosition {
        std::string market;
        std::string strategy_name;
        double entry_price = 0.0;
        double quantity = 0.0;
        long long entry_time = 0;
        double signal_filter = 0.5;
        double signal_strength = 0.0;
    };

    std::vector<PersistedPosition> pending_reconcile_positions_;
    
    // 통계
    long long start_time_;
    int total_scans_;
    int total_signals_;
};

} // namespace engine
} // namespace autolife
