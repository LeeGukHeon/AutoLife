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
#include <map>

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

// Trading Engine - ?„ì²´ ê±°ë˜ ?œìŠ¤??
class TradingEngine {
public:
    TradingEngine(
        const EngineConfig& config,
        std::shared_ptr<network::UpbitHttpClient> http_client
    );
    
    ~TradingEngine();
    
    // ===== ?”ì§„ ?œì–´ =====
    
    bool start();
    void stop();
    bool isRunning() const { return running_; }
    
    // ===== ë©”ì¸ ë£¨í”„ =====
    
    void run();  // ë©”ì¸ ê±°ë˜ ë£¨í”„ (ë¸”ë¡œ??
    
    // ===== ?íƒœ ì¡°íšŒ =====
    
    risk::RiskManager::RiskMetrics getMetrics() const;
    std::vector<risk::Position> getPositions() const;
    std::vector<risk::TradeHistory> getTradeHistory() const;
    
    // ===== ?˜ë™ ?œì–´ (?ŒìŠ¤?¸ìš©) =====
    
    void manualScan();
    void manualClosePosition(const std::string& market);
    void manualCloseAll();
    
private:
    // ===== ë©”ì¸ ?„ë¡œ?¸ìŠ¤ =====
    
    void scanMarkets();           // ?œì¥ ?¤ìº”
    void generateSignals();       // ë§¤ë§¤ ? í˜¸ ?ì„±
    void executeSignals();        // ? í˜¸ ?¤í–‰
    void monitorPositions();      // ?¬ì???ëª¨ë‹ˆ?°ë§
    void updateMetrics();         // ë©”íŠ¸ë¦??…ë°?´íŠ¸
    
    // ===== ì£¼ë¬¸ ?¤í–‰ (ê°œì„ ) =====
    
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
    
    // [??ì¶”ê?] ì£¼ë¬¸ ?¬í¼ ?¨ìˆ˜??
    
    // LIMIT ì£¼ë¬¸?¼ë¡œ ë§¤ìˆ˜ (?¬ì‹œ??ë¡œì§ ?¬í•¨)
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
        double entry_price,      // ì§€?•ê?
        double quantity,         // ì£¼ë¬¸ ?˜ëŸ‰
        int max_retries,         // ìµœë? ?¬ì‹œ??
        int retry_wait_ms        // ?¬ì‹œ??ê°„ê²© (ms)
    );
    
    // LIMIT ì£¼ë¬¸?¼ë¡œ ë§¤ë„
    LimitOrderResult executeLimitSellOrder(
        const std::string& market,
        double exit_price,
        double quantity,
        int max_retries,
        int retry_wait_ms
    );
    
    // ?œì¥ê°€ ì£¼ë¬¸ (?´ë°±??
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
    
    // LIMIT ì£¼ë¬¸ ìµœì  ê°€ê²?ê³„ì‚° (?œì¥ ?í™© ë°˜ì˜)
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
    
    // ì£¼ë¬¸ ?íƒœ ê²€ì¦?ë°?ì²´ê²° ?•ì¸
    struct OrderFillInfo {
        bool is_filled;           // ?„ì „??ì²´ê²°?˜ì—ˆ?”ê?
        bool is_partially_filled; // ë¶€ë¶?ì²´ê²°?˜ì—ˆ?”ê?
        double filled_volume;     // ì²´ê²°???˜ëŸ‰
        double avg_price;         // ?‰ê·  ì²´ê²°ê°€
        double fee;               // ?˜ìˆ˜ë£?
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
    
    // ===== ?¬í¼ ?¨ìˆ˜ =====
    
    double getCurrentPrice(const std::string& market);
    bool hasEnoughBalance(double required_krw);
    bool isLiveRealOrderEnabled() const {
        return config_.mode == TradingMode::LIVE && !config_.dry_run && config_.allow_live_orders;
    }
    void logPerformance();
    // [ì¶”ê?] ê³„ì¢Œ ?íƒœë¥?ì¡°íšŒ?˜ì—¬ RiskManager?€ ?™ê¸°??
    void syncAccountState();

    // ===== ?íƒœ ?€??ë³µêµ¬ =====
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
    
    // ===== [NEW] ?™ì  ?„í„° ë°??¬ì????•ë? ê¸°ëŠ¥ =====
    
    // ?œì¥ ë³€?™ì„±??ê¸°ë°˜?¼ë¡œ ?„í„°ê°’ì„ ?™ì  ì¡°ì • (0.45~0.55)
    // ?’ì? ë³€?™ì„± ????? ?„í„°ê°?(??ë§ì? ? í˜¸ ?¬ì°©)
    // ??? ë³€?™ì„± ???’ì? ?„í„°ê°?(?ˆì§ˆ ?’ì? ? í˜¸ë§?
    double calculateDynamicFilterValue();
    
    // Win Rate >= 60%, Profit Factor >= 1.5?????¬ì????•ë?
    // ê¸°ê?ê¸??±ëŠ¥ ê¸°ì? ?ìš©
    double calculatePositionScaleMultiplier();
    
    // Historical P&L ?°ì´?°ì—??ìµœì  ?„í„°ê°??™ìŠµ (ML)
    // ?´ì „ ê±°ë˜ ?±ê³¼?ì„œ ìµœì ???„í„° ?„ê³„ê°??ë™ ê³„ì‚°
    void learnOptimalFilterValue();
    
    // Prometheus ë©”íŠ¸ë¦??¸ì¶œ (?¤ì‹œê°?ëª¨ë‹ˆ?°ë§??
    // Grafana ?°ë™???„í•œ ë©”íŠ¸ë¦??°ì´???ì„±
    std::string exportPrometheusMetrics() const;
    
    // [NEW] Prometheus HTTP ?œë²„ (ë©”íŠ¸ë¦??¸ì¶œ)
    // /metrics ?”ë“œ?¬ì¸?¸ë¡œ Prometheus ?•ì‹ ë©”íŠ¸ë¦??œê³µ
    void runPrometheusHttpServer(int port = 8080);
    void recordLiveSignalReject(const std::string& reason, long long count = 1);
    void flushLiveSignalFunnelTaxonomyReport(
        const std::map<std::string, int>& last_scan_rejection_counts,
        const std::map<std::string, int>& last_scan_hint_adjustment_counts
    ) const;
    void captureLiveMtfDatasetSnapshotIfDue();
    
    // ===== ë©¤ë²„ ë³€??=====
    
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
    
    // ?¤ìº” ê²°ê³¼
    std::vector<analytics::CoinMetrics> scanned_markets_;
    std::vector<strategy::Signal> pending_signals_;
    
    // ?¤ë ˆ???œì–´
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> worker_thread_;
    std::unique_ptr<std::thread> state_persist_thread_;
    std::atomic<bool> state_persist_running_{false};
    mutable std::mutex mutex_;
    
    // [ì¶”ê?] ?¤ìº” ë°??™ê¸°???€?´ë°
    std::chrono::steady_clock::time_point last_scan_time_;
    std::chrono::steady_clock::time_point last_account_sync_time_;
    
    // ===== [NEW] ?™ì  ?„í„° ë°??¬ì????•ë? ë©¤ë²„ =====
    
    // ?„ì¬ ?™ì  ?„í„°ê°?(0.45~0.55 ë²”ìœ„)
    // ì´ˆê¸°ê°? 0.5 (ì¤‘ë¦½)
    double dynamic_filter_value_ = 0.5;
    
    // ?¬ì????•ë? ë°°ìˆ˜ (ê¸°ë³¸ 1.0, ìµœë? 2.5)
    // Win Rate ??60%, PF ??1.5?????ë™ ì¦ê?
    double position_scale_multiplier_ = 1.0;
    
    // ML ëª¨ë“ˆ???„í•œ ìµœì  ?„í„°ê°??™ìŠµ ?°ì´??
    // key: ?„í„°ê°? value: ?´ë‹¹ ?„í„°ê°’ì—?œì˜ ?¹ë¥ 
    std::map<double, double> filter_performance_history_;
    int scans_without_new_entry_ = 0;
    double market_hostility_ewma_ = 0.0;
    int hostile_pause_scans_remaining_ = 0;
    
    // Prometheus ë©”íŠ¸ë¦??„ì ??(ë©”ëª¨ë¦??¨ìœ¨)
    struct PrometheusMetrics {
        long long total_buy_orders = 0;
        long long total_sell_orders = 0;
        double cumulative_realized_pnl = 0.0;
        int active_strategies_count = 0;
        double last_update_timestamp = 0.0;
    } prometheus_metrics_;

    struct LiveSignalFunnelTelemetry {
        long long scan_count = 0;
        long long markets_considered = 0;
        long long skipped_due_to_open_position = 0;
        long long skipped_due_to_active_order = 0;
        long long no_candle_data = 0;
        long long no_signal_generated = 0;
        long long filtered_out_by_manager = 0;
        long long no_best_signal = 0;
        long long markets_with_selected_candidate = 0;
        long long generated_signal_candidates = 0;
        long long selected_signal_candidates = 0;
        long long selection_call_count = 0;
        long long selection_scored_candidate_count = 0;
        long long selection_hint_adjusted_candidate_count = 0;
        std::map<std::string, long long> selection_hint_adjustment_counts;
        std::map<std::string, long long> rejection_reason_counts;
    } live_signal_funnel_;

    // Counts open-position skips once per market holding episode to avoid
    // per-scan overcount distortion in bottleneck diagnostics.
    std::map<std::string, bool> open_position_skip_latch_;
    std::chrono::steady_clock::time_point last_live_mtf_capture_time_{};
    std::map<std::string, long long> live_mtf_capture_last_timestamp_by_file_;
    std::map<std::string, int> pre_cat_recovery_hysteresis_hold_by_key_;
    std::map<std::string, int> pre_cat_recovery_bridge_activation_by_key_;
    std::map<std::string, int> pre_cat_no_soft_quality_relief_activation_by_key_;
    std::map<std::string, int> pre_cat_candidate_rr_failsafe_activation_by_key_;
    std::map<std::string, int> pre_cat_pressure_rebound_relief_activation_by_key_;
    std::map<std::string, int> pre_cat_negative_history_quarantine_hold_by_key_;
    
    // [NEW] Prometheus HTTP ?œë²„ ê´€??
    int prometheus_server_port_ = 8080;  // HTTP ?œë²„ ?¬íŠ¸ (ê¸°ë³¸ê°? 8080)
    std::unique_ptr<std::thread> prometheus_http_thread_;  // HTTP ?œë²„ ?¤ë ˆ??
    std::atomic<bool> prometheus_server_running_ = false;  // HTTP ?œë²„ ?¤í–‰ ?íƒœ

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
        // [Phase 4] ?ì ˆ/?µì ˆ/?¸ë ˆ?¼ë§ ?ì†??
        double stop_loss = 0.0;
        double take_profit_1 = 0.0;
        double take_profit_2 = 0.0;
        double breakeven_trigger = 0.0;
        double trailing_start = 0.0;
        bool half_closed = false;
    };

    std::vector<PersistedPosition> pending_reconcile_positions_;
    
    // ?µê³„
    long long start_time_;
    int total_scans_;
    int total_signals_;
};

} // namespace engine
} // namespace autolife


