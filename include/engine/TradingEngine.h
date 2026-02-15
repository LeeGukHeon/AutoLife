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
#include "engine/AdaptivePolicyController.h"
#include "engine/PerformanceStore.h"
#include "core/contracts/IExecutionPlane.h"
#include "core/contracts/IEventJournal.h"
#include "core/contracts/ILearningStateStore.h"
#include "core/contracts/IPolicyLearningPlane.h"
#include "core/contracts/IRiskCompliancePlane.h"
#include "core/orchestration/TradingCycleCoordinator.h"
#include "execution/OrderManager.h"

namespace autolife {
namespace engine {

// TradingMode and EngineConfig moved to EngineConfig.h

// Trading Engine - ?꾩껜 嫄곕옒 ?쒖뒪??
class TradingEngine {
public:
    TradingEngine(
        const EngineConfig& config,
        std::shared_ptr<network::UpbitHttpClient> http_client
    );
    
    ~TradingEngine();
    
    // ===== ?붿쭊 ?쒖뼱 =====
    
    bool start();
    void stop();
    bool isRunning() const { return running_; }
    
    // ===== 硫붿씤 猷⑦봽 =====
    
    void run();  // 硫붿씤 嫄곕옒 猷⑦봽 (釉붾줈??
    
    // ===== ?곹깭 議고쉶 =====
    
    risk::RiskManager::RiskMetrics getMetrics() const;
    std::vector<risk::Position> getPositions() const;
    std::vector<risk::TradeHistory> getTradeHistory() const;
    
    // ===== ?섎룞 ?쒖뼱 (?뚯뒪?몄슜) =====
    
    void manualScan();
    void manualClosePosition(const std::string& market);
    void manualCloseAll();
    
private:
    // ===== 硫붿씤 ?꾨줈?몄뒪 =====
    
    void scanMarkets();           // ?쒖옣 ?ㅼ틪
    void generateSignals();       // 留ㅻℓ ?좏샇 ?앹꽦
    void executeSignals();        // ?좏샇 ?ㅽ뻾
    void monitorPositions();      // ?ъ???紐⑤땲?곕쭅
    void updateMetrics();         // 硫뷀듃由??낅뜲?댄듃
    
    // ===== 二쇰Ц ?ㅽ뻾 (媛쒖꽑) =====
    
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
    
    // [??異붽?] 二쇰Ц ?ы띁 ?⑥닔??
    
    // LIMIT 二쇰Ц?쇰줈 留ㅼ닔 (?ъ떆??濡쒖쭅 ?ы븿)
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
        double entry_price,      // 吏?뺢?
        double quantity,         // 二쇰Ц ?섎웾
        int max_retries,         // 理쒕? ?ъ떆??
        int retry_wait_ms        // ?ъ떆??媛꾧꺽 (ms)
    );
    
    // LIMIT 二쇰Ц?쇰줈 留ㅻ룄
    LimitOrderResult executeLimitSellOrder(
        const std::string& market,
        double exit_price,
        double quantity,
        int max_retries,
        int retry_wait_ms
    );
    
    // ?쒖옣媛 二쇰Ц (?대갚??
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
    
    // LIMIT 二쇰Ц 理쒖쟻 媛寃?怨꾩궛 (?쒖옣 ?곹솴 諛섏쁺)
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
    
    // 二쇰Ц ?곹깭 寃利?諛?泥닿껐 ?뺤씤
    struct OrderFillInfo {
        bool is_filled;           // ?꾩쟾??泥닿껐?섏뿀?붽?
        bool is_partially_filled; // 遺遺?泥닿껐?섏뿀?붽?
        double filled_volume;     // 泥닿껐???섎웾
        double avg_price;         // ?됯퇏 泥닿껐媛
        double fee;               // ?섏닔猷?
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
    
    // ===== ?ы띁 ?⑥닔 =====
    
    double getCurrentPrice(const std::string& market);
    bool hasEnoughBalance(double required_krw);
    void logPerformance();
    // [異붽?] 怨꾩쥖 ?곹깭瑜?議고쉶?섏뿬 RiskManager? ?숆린??
    void syncAccountState();

    // ===== ?곹깭 ???蹂듦뎄 =====
    void loadState();
    void saveState();
    void runStatePersistence();
    void loadLearningState();
    void saveLearningState();
    void appendJournalEvent(
        core::JournalEventType type,
        const std::string& market,
        const std::string& entity_id,
        const nlohmann::json& payload
    );
    
    // ===== [NEW] ?숈쟻 ?꾪꽣 諛??ъ????뺣? 湲곕뒫 =====
    
    // ?쒖옣 蹂?숈꽦??湲곕컲?쇰줈 ?꾪꽣媛믪쓣 ?숈쟻 議곗젙 (0.45~0.55)
    // ?믪? 蹂?숈꽦 ????? ?꾪꽣媛?(??留롮? ?좏샇 ?ъ갑)
    // ??? 蹂?숈꽦 ???믪? ?꾪꽣媛?(?덉쭏 ?믪? ?좏샇留?
    double calculateDynamicFilterValue();
    
    // Win Rate >= 60%, Profit Factor >= 1.5?????ъ????뺣?
    // 湲곌?湲??깅뒫 湲곗? ?곸슜
    double calculatePositionScaleMultiplier();
    
    // Historical P&L ?곗씠?곗뿉??理쒖쟻 ?꾪꽣媛??숈뒿 (ML)
    // ?댁쟾 嫄곕옒 ?깃낵?먯꽌 理쒖쟻???꾪꽣 ?꾧퀎媛??먮룞 怨꾩궛
    void learnOptimalFilterValue();
    
    // Prometheus 硫뷀듃由??몄텧 (?ㅼ떆媛?紐⑤땲?곕쭅??
    // Grafana ?곕룞???꾪븳 硫뷀듃由??곗씠???앹꽦
    std::string exportPrometheusMetrics() const;
    
    // [NEW] Prometheus HTTP ?쒕쾭 (硫뷀듃由??몄텧)
    // /metrics ?붾뱶?ъ씤?몃줈 Prometheus ?뺤떇 硫뷀듃由??쒓났
    void runPrometheusHttpServer(int port = 8080);
    
    // ===== 硫ㅻ쾭 蹂??=====
    
    EngineConfig config_;
    
    std::shared_ptr<network::UpbitHttpClient> http_client_;
    std::unique_ptr<analytics::MarketScanner> scanner_;
    std::unique_ptr<strategy::StrategyManager> strategy_manager_;
    std::unique_ptr<AdaptivePolicyController> policy_controller_;
    std::unique_ptr<PerformanceStore> performance_store_;
    std::unique_ptr<risk::RiskManager> risk_manager_;
    std::unique_ptr<execution::OrderManager> order_manager_;
    std::unique_ptr<analytics::RegimeDetector> regime_detector_;
    std::shared_ptr<core::IPolicyLearningPlane> core_policy_plane_;
    std::shared_ptr<core::IRiskCompliancePlane> core_risk_plane_;
    std::shared_ptr<core::IExecutionPlane> core_execution_plane_;
    std::unique_ptr<core::TradingCycleCoordinator> core_cycle_;
    std::unique_ptr<core::IEventJournal> event_journal_;
    std::unique_ptr<core::ILearningStateStore> learning_state_store_;
    
    // ?ㅼ틪 寃곌낵
    std::vector<analytics::CoinMetrics> scanned_markets_;
    std::vector<strategy::Signal> pending_signals_;
    
    // ?ㅻ젅???쒖뼱
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> worker_thread_;
    std::unique_ptr<std::thread> state_persist_thread_;
    std::atomic<bool> state_persist_running_{false};
    mutable std::mutex mutex_;
    
    // [異붽?] ?ㅼ틪 諛??숆린????대컢
    std::chrono::steady_clock::time_point last_scan_time_;
    std::chrono::steady_clock::time_point last_account_sync_time_;
    
    // ===== [NEW] ?숈쟻 ?꾪꽣 諛??ъ????뺣? 硫ㅻ쾭 =====
    
    // ?꾩옱 ?숈쟻 ?꾪꽣媛?(0.45~0.55 踰붿쐞)
    // 珥덇린媛? 0.5 (以묐┰)
    double dynamic_filter_value_ = 0.5;
    
    // ?ъ????뺣? 諛곗닔 (湲곕낯 1.0, 理쒕? 2.5)
    // Win Rate ??60%, PF ??1.5?????먮룞 利앷?
    double position_scale_multiplier_ = 1.0;
    
    // ML 紐⑤뱢???꾪븳 理쒖쟻 ?꾪꽣媛??숈뒿 ?곗씠??
    // key: ?꾪꽣媛? value: ?대떦 ?꾪꽣媛믪뿉?쒖쓽 ?밸쪧
    std::map<double, double> filter_performance_history_;
    int scans_without_new_entry_ = 0;
    double market_hostility_ewma_ = 0.0;
    int hostile_pause_scans_remaining_ = 0;
    
    // Prometheus 硫뷀듃由??꾩쟻??(硫붾え由??⑥쑉)
    struct PrometheusMetrics {
        long long total_buy_orders = 0;
        long long total_sell_orders = 0;
        double cumulative_realized_pnl = 0.0;
        int active_strategies_count = 0;
        double last_update_timestamp = 0.0;
    } prometheus_metrics_;
    
    // [NEW] Prometheus HTTP ?쒕쾭 愿??
    int prometheus_server_port_ = 8080;  // HTTP ?쒕쾭 ?ы듃 (湲곕낯媛? 8080)
    std::unique_ptr<std::thread> prometheus_http_thread_;  // HTTP ?쒕쾭 ?ㅻ젅??
    std::atomic<bool> prometheus_server_running_ = false;  // HTTP ?쒕쾭 ?ㅽ뻾 ?곹깭

    std::map<std::string, std::string> recovered_strategy_map_;

    struct PersistedPosition {
        std::string market;
        std::string strategy_name;
        double entry_price = 0.0;
        double quantity = 0.0;
        long long entry_time = 0;
        double signal_filter = 0.5;
        double signal_strength = 0.0;
        analytics::MarketRegime market_regime = analytics::MarketRegime::UNKNOWN;
        double liquidity_score = 0.0;
        double volatility = 0.0;
        double expected_value = 0.0;
        double reward_risk_ratio = 0.0;
        // [Phase 4] ?먯젅/?듭젅/?몃젅?쇰쭅 ?곸냽??
        double stop_loss = 0.0;
        double take_profit_1 = 0.0;
        double take_profit_2 = 0.0;
        double breakeven_trigger = 0.0;
        double trailing_start = 0.0;
        bool half_closed = false;
    };

    std::vector<PersistedPosition> pending_reconcile_positions_;
    
    // ?듦퀎
    long long start_time_;
    int total_scans_;
    int total_signals_;
};

} // namespace engine
} // namespace autolife

