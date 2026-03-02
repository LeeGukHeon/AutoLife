#pragma once

#include "common/Types.h"
#include "network/UpbitHttpClient.h"
#include "analytics/MarketScanner.h"
#include "analytics/RegimeDetector.h"
#include "analytics/ProbabilisticRuntimeModel.h"
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
#include "execution/IExecutionPlane.h"
#include "state/IEventJournal.h"
#include "state/ILearningStateStore.h"
#include "engine/IPolicyLearningPlane.h"
#include "risk/IRiskCompliancePlane.h"
#include "engine/TradingCycleCoordinator.h"
#include "execution/OrderManager.h"

namespace autolife {
namespace engine {

// TradingMode and EngineConfig moved to EngineConfig.h

// Trading Engine - ?袁⑷퍥 椰꾧퀡????뽯뮞??
class TradingEngine {
public:
    TradingEngine(
        const EngineConfig& config,
        std::shared_ptr<network::UpbitHttpClient> http_client
    );
    
    ~TradingEngine();
    
    // ===== ?遺우춭 ??뽯선 =====
    
    bool start();
    void stop();
    bool isRunning() const { return running_; }
    
    // ===== 筌롫뗄???룐뫂遊?=====
    
    void run();  // 筌롫뗄??椰꾧퀡???룐뫂遊?(?됰뗀以??
    
    // ===== ?怨밴묶 鈺곌퀬??=====
    
    risk::RiskManager::RiskMetrics getMetrics() const;
    std::vector<risk::Position> getPositions() const;
    std::vector<risk::TradeHistory> getTradeHistory() const;
    
    // ===== ??롫짗 ??뽯선 (???뮞?紐꾩뒠) =====
    
    void manualScan();
    void manualClosePosition(const std::string& market);
    void manualCloseAll();
    
private:
    // ===== 筌롫뗄???袁⑥쨮?紐꾨뮞 =====
    
    void scanMarkets();           // ??뽰삢 ??쇳떔
    void generateSignals();       // 筌띲끇???醫륁깈 ??밴쉐
    void executeSignals();        // ?醫륁깈 ??쎈뻬
    void monitorPositions();      // ?????筌뤴뫀??怨뺤춦
    void updateMetrics();         // 筌롫??껆뵳???낅쑓??꾨뱜
    
    // ===== 雅뚯눖揆 ??쎈뻬 (揶쏆뮇苑? =====
    
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
    
    // [???곕떽?] 雅뚯눖揆 ??????λ땾??
    
    // LIMIT 雅뚯눖揆??곗쨮 筌띲끉??(?????嚥≪뮇彛???釉?
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
        double entry_price,      // 筌왖?類?
        double quantity,         // 雅뚯눖揆 ??롮쎗
        int max_retries,         // 筌ㅼ뮆? ?????
        int retry_wait_ms        // ?????揶쏄쑨爰?(ms)
    );
    
    // LIMIT 雅뚯눖揆??곗쨮 筌띲끇猷?
    LimitOrderResult executeLimitSellOrder(
        const std::string& market,
        double exit_price,
        double quantity,
        int max_retries,
        int retry_wait_ms
    );
    
    // ??뽰삢揶쎛 雅뚯눖揆 (??媛??
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
    
    // LIMIT 雅뚯눖揆 筌ㅼ뮇??揶쎛野??④쑴沅?(??뽰삢 ?怨뱀넺 獄쏆꼷??
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
    
    // 雅뚯눖揆 ?怨밴묶 野꺜筌?獄?筌ｋ떯猿??類ㅼ뵥
    struct OrderFillInfo {
        bool is_filled;           // ?袁⑹읈??筌ｋ떯猿??뤿??遺?
        bool is_partially_filled; // ?봔??筌ｋ떯猿??뤿??遺?
        double filled_volume;     // 筌ｋ떯猿????롮쎗
        double avg_price;         // ???뇧 筌ｋ떯猿먨첎?
        double fee;               // ??뤿땾??
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
    
    // ===== ??????λ땾 =====
    
    double getCurrentPrice(const std::string& market);
    bool hasEnoughBalance(double required_krw);
    bool isLiveRealOrderEnabled() const {
        return config_.mode == TradingMode::LIVE && !config_.dry_run && config_.allow_live_orders;
    }
    bool isLivePaperFixedCapitalMode() const {
        return config_.mode == TradingMode::LIVE &&
               !config_.allow_live_orders &&
               config_.live_paper_use_fixed_initial_capital &&
               config_.live_paper_fixed_initial_capital_krw > 0.0;
    }
    void logPerformance();
    // [?곕떽?] ?④쑴伊??怨밴묶??鈺곌퀬???뤿연 RiskManager?? ??녿┛??
    void syncAccountState();

    // ===== ?怨밴묶 ????癰귣벀??=====
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
    
    // ===== [NEW] ??덉읅 ?袁り숲 獄???????類? 疫꿸퀡??=====
    
    // ??뽰삢 癰궰??덇쉐??疫꿸퀡而??곗쨮 ?袁り숲揶쏅?????덉읅 鈺곌퀣??(0.45~0.55)
    // ?誘? 癰궰??덇쉐 ????? ?袁り숲揶?(??筌띾‘? ?醫륁깈 ??媛?
    // ??? 癰궰??덇쉐 ???誘? ?袁り숲揶?(??됱춳 ?誘? ?醫륁깈筌?
    
    // Win Rate >= 60%, Profit Factor >= 1.5??????????類?
    // 疫꿸퀗?疫??源낅뮟 疫꿸퀣? ?怨몄뒠
    
    // Historical P&L ?怨쀬뵠?怨쀫퓠??筌ㅼ뮇???袁り숲揶???덈뮸 (ML)
    // ??곸읈 椰꾧퀡???源껊궢?癒?퐣 筌ㅼ뮇????袁り숲 ?袁㏉롥첎??癒?짗 ?④쑴沅?
    
    // Prometheus 筌롫??껆뵳??紐꾪뀱 (??쇰뻻揶?筌뤴뫀??怨뺤춦??
    // Grafana ?怨뺣짗???袁る립 筌롫??껆뵳??怨쀬뵠????밴쉐
    std::string exportPrometheusMetrics() const;
    
    // [NEW] Prometheus HTTP ??뺤쒔 (筌롫??껆뵳??紐꾪뀱)
    // /metrics ?遺얜굡????紐껋쨮 Prometheus ?類ㅻ뻼 筌롫??껆뵳???볥궗
    void runPrometheusHttpServer(int port = 8080);
    void recordLiveSignalReject(const std::string& reason, long long count = 1);
    void flushLiveSignalFunnelTaxonomyReport(
        const std::map<std::string, int>& last_scan_rejection_counts,
        const std::map<std::string, int>& last_scan_hint_adjustment_counts
    ) const;
    void captureLiveMtfDatasetSnapshotIfDue();
    
    // ===== 筌롢끇苡?癰궰??=====
    
    EngineConfig config_;
    
    std::shared_ptr<network::UpbitHttpClient> http_client_;
    std::unique_ptr<analytics::MarketScanner> scanner_;
    std::unique_ptr<strategy::StrategyManager> strategy_manager_;
    std::unique_ptr<AdaptivePolicyController> policy_controller_;
    std::unique_ptr<PerformanceStore> performance_store_;
    std::unique_ptr<risk::RiskManager> risk_manager_;
    std::unique_ptr<execution::OrderManager> order_manager_;
    std::unique_ptr<analytics::RegimeDetector> regime_detector_;
    analytics::ProbabilisticRuntimeModel probabilistic_runtime_model_;
    bool probabilistic_runtime_model_loaded_ = false;
    std::shared_ptr<core::IPolicyLearningPlane> core_policy_plane_;
    std::shared_ptr<core::IRiskCompliancePlane> core_risk_plane_;
    std::shared_ptr<core::IExecutionPlane> core_execution_plane_;
    std::unique_ptr<core::TradingCycleCoordinator> core_cycle_;
    std::unique_ptr<core::IEventJournal> event_journal_;
    std::unique_ptr<core::ILearningStateStore> learning_state_store_;
    
    // ??쇳떔 野껉퀗??
    std::vector<analytics::CoinMetrics> scanned_markets_;
    std::vector<strategy::Signal> pending_signals_;
    
    // ??살쟿????뽯선
    std::atomic<bool> running_;
    std::unique_ptr<std::thread> worker_thread_;
    std::unique_ptr<std::thread> state_persist_thread_;
    std::atomic<bool> state_persist_running_{false};
    mutable std::mutex mutex_;
    
    // [?곕떽?] ??쇳떔 獄???녿┛??????而?
    std::chrono::steady_clock::time_point last_scan_time_;
    std::chrono::steady_clock::time_point last_account_sync_time_;
    
    // ===== [NEW] ??덉읅 ?袁り숲 獄???????類? 筌롢끇苡?=====
    
    // ?袁⑹삺 ??덉읅 ?袁り숲揶?(0.45~0.55 甕곕뗄??
    // ?λ뜃由겼첎? 0.5 (餓λ쵎??
    
    // ??????類? 獄쏄퀣??(疫꿸퀡??1.0, 筌ㅼ뮆? 2.5)
    // Win Rate ??60%, PF ??1.5?????癒?짗 筌앹빓?
    
    // ML 筌뤴뫀諭???袁る립 筌ㅼ뮇???袁り숲揶???덈뮸 ?怨쀬뵠??
    // key: ?袁り숲揶? value: ?????袁り숲揶쏅?肉??뽰벥 ?諛몄ぇ
    double market_hostility_ewma_ = 0.0;
    
    // Prometheus 筌롫??껆뵳??袁⑹읅??(筌롫뗀?덄뵳???μ몛)
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
        long long no_best_signal = 0;
        long long reject_regime_entry_disabled_count = 0;
        std::map<std::string, long long> reject_regime_entry_disabled_by_regime;
        bool regime_entry_disable_enabled = false;
        std::map<std::string, bool> regime_entry_disable;
        long long shadow_count_total = 0;
        std::map<std::string, long long> shadow_count_by_regime;
        std::map<std::string, long long> shadow_count_by_market;
        long long shadow_would_pass_execution_guard_count = 0;
        long long shadow_edge_neg_count = 0;
        long long shadow_edge_pos_count = 0;
        std::string liq_vol_gate_mode = "static";
        double liq_vol_gate_quantile_q = 0.0;
        int liq_vol_gate_window_minutes = 0;
        int liq_vol_gate_min_samples_required = 0;
        std::string liq_vol_gate_low_conf_action = "hold";
        long long liq_vol_gate_observation_count = 0;
        long long liq_vol_gate_pass_count = 0;
        long long liq_vol_gate_fail_count = 0;
        long long liq_vol_gate_low_conf_triggered_count = 0;
        double liq_vol_gate_observed_sum = 0.0;
        double liq_vol_gate_threshold_sum = 0.0;
        struct LiqVolGateSample {
            std::string market;
            double observed = 0.0;
            double threshold_dynamic = 0.0;
            int history_count = 0;
            bool pass = false;
            bool low_conf = false;
        };
        std::vector<LiqVolGateSample> liq_vol_gate_samples;
        std::string structure_gate_mode = "static";
        double structure_gate_relax_delta = 0.0;
        long long structure_gate_observation_count = 0;
        long long structure_gate_fail_count_total = 0;
        std::map<std::string, long long> structure_gate_fail_count_by_regime;
        std::map<std::string, long long> structure_gate_pass_count_by_regime;
        double structure_gate_observed_score_sum = 0.0;
        double structure_gate_threshold_before_sum = 0.0;
        double structure_gate_threshold_after_sum = 0.0;
        struct StructureGateSample {
            std::string market;
            std::string regime;
            double observed_score = 0.0;
            double threshold_before = 0.0;
            double threshold_after = 0.0;
            bool pass = false;
            bool relax_applied = false;
        };
        std::vector<StructureGateSample> structure_gate_samples;
        std::string bear_rebound_guard_mode = "static";
        double bear_rebound_guard_quantile_q = 0.0;
        int bear_rebound_guard_window_minutes = 0;
        int bear_rebound_guard_min_samples_required = 0;
        std::string bear_rebound_guard_low_conf_action = "hold";
        long long bear_rebound_guard_observation_count = 0;
        long long bear_rebound_guard_pass_count = 0;
        long long bear_rebound_guard_fail_count = 0;
        long long bear_rebound_guard_low_conf_triggered_count = 0;
        std::map<std::string, long long> bear_rebound_guard_fail_count_by_regime;
        std::map<std::string, long long> bear_rebound_guard_pass_count_by_regime;
        double bear_rebound_guard_observed_sum = 0.0;
        double bear_rebound_guard_threshold_sum = 0.0;
        struct BearReboundGuardSample {
            std::string market;
            std::string regime;
            double observed = 0.0;
            double threshold_dynamic = 0.0;
            int history_count = 0;
            bool pass = false;
            bool low_conf = false;
        };
        std::vector<BearReboundGuardSample> bear_rebound_guard_samples;
        std::string strategy_exit_mode_effective = "enforce";
        long long strategy_exit_triggered_count = 0;
        long long strategy_exit_observe_only_suppressed_count = 0;
        long long strategy_exit_executed_count = 0;
        long long strategy_exit_clamp_applied_count = 0;
        int be_after_partial_tp_delay_sec = 0;
        long long be_move_attempt_count = 0;
        long long be_move_applied_count = 0;
        long long be_move_skipped_due_to_delay_count = 0;
        long long stop_loss_after_partial_tp_count = 0;
        long long stop_loss_before_partial_tp_count = 0;
        std::map<std::string, long long> strategy_exit_triggered_by_market;
        std::map<std::string, long long> strategy_exit_triggered_by_regime;
        struct StrategyExitTriggerSample {
            long long ts_ms = 0;
            std::string market;
            std::string regime;
            double unrealized_pnl_at_trigger = 0.0;
            double holding_time_seconds = 0.0;
            std::string reason_code;
        };
        std::vector<StrategyExitTriggerSample> strategy_exit_trigger_samples;
        struct StrategyExitClampSample {
            long long ts_ms = 0;
            std::string market;
            std::string regime;
            double stop_loss_price = 0.0;
            double exit_price_before_clamp = 0.0;
            double exit_price_after_clamp = 0.0;
            double pnl_before_clamp = 0.0;
            double pnl_after_clamp = 0.0;
            std::string reason_code;
        };
        std::vector<StrategyExitClampSample> strategy_exit_clamp_samples;
        long long markets_with_selected_candidate = 0;
        long long generated_signal_candidates = 0;
        long long selected_signal_candidates = 0;
        long long selection_call_count = 0;
        long long selection_scored_candidate_count = 0;
        long long selection_hint_adjusted_candidate_count = 0;
        std::string gate_system_version_effective = "vnext";
        long long gate_vnext_s0_snapshots_valid = 0;
        long long gate_vnext_s1_selected_topk = 0;
        long long gate_vnext_s2_sized_count = 0;
        long long gate_vnext_s3_exec_gate_pass = 0;
        long long gate_vnext_drop_ev_negative_count = 0;
        std::string quality_sort_mode = "vnext_margin_rank";
        int quality_topk_effective = 5;
        long long candidates_before_topk = 0;
        long long candidates_after_topk = 0;
        long long selected_after_topk_count = 0;
        long long quality_selected_candidates = 0;
        long long quality_margin_valid_count = 0;
        double quality_margin_mean = 0.0;
        double quality_margin_std = 0.0;
        double quality_margin_p10 = 0.0;
        double quality_margin_p50 = 0.0;
        double quality_margin_p90 = 0.0;
        std::map<std::string, long long> selection_hint_adjustment_counts;
        std::map<std::string, long long> rejection_reason_counts;
    } live_signal_funnel_;

    // Counts open-position skips once per market holding episode to avoid
    // per-scan overcount distortion in bottleneck diagnostics.
    std::map<std::string, bool> open_position_skip_latch_;
    std::map<std::string, double> recent_best_ask_by_market_;
    std::map<std::string, long long> recent_best_ask_timestamp_by_market_;
    std::map<std::string, long long> pending_be_after_partial_due_ms_;
    int live_warmup_scans_completed_ = 0;
    bool live_warmup_done_ = false;
    std::chrono::steady_clock::time_point last_live_mtf_capture_time_{};
    std::map<std::string, long long> live_mtf_capture_last_timestamp_by_file_;
    
    // [NEW] Prometheus HTTP ??뺤쒔 ?온??
    int prometheus_server_port_ = 8080;  // HTTP ??뺤쒔 ????(疫꿸퀡??첎? 8080)
    std::unique_ptr<std::thread> prometheus_http_thread_;  // HTTP ??뺤쒔 ??살쟿??
    std::atomic<bool> prometheus_server_running_ = false;  // HTTP ??뺤쒔 ??쎈뻬 ?怨밴묶

    std::map<std::string, std::string> recovered_strategy_map_;
    std::map<std::string, double> pending_buy_reservation_by_order_id_;

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
        std::string entry_archetype = "UNSPECIFIED";
        bool probabilistic_runtime_applied = false;
        double probabilistic_h1_calibrated = 0.5;
        double probabilistic_h5_calibrated = 0.5;
        double probabilistic_h5_threshold = 0.6;
        double probabilistic_h5_margin = 0.0;
        // [Phase 4] ?癒?쟿/???쟿/?紐껋쟿??곗춦 ?怨몃꺗??
        double stop_loss = 0.0;
        double take_profit_1 = 0.0;
        double take_profit_2 = 0.0;
        double breakeven_trigger = 0.0;
        double trailing_start = 0.0;
        bool half_closed = false;
    };

    std::vector<PersistedPosition> pending_reconcile_positions_;

    struct PendingSignalMetadata {
        double signal_filter = 0.5;
        double signal_strength = 0.0;
        analytics::MarketRegime market_regime = analytics::MarketRegime::UNKNOWN;
        double liquidity_score = 0.0;
        double volatility = 0.0;
        double expected_value = 0.0;
        double reward_risk_ratio = 0.0;
        std::string entry_archetype = "UNSPECIFIED";
        bool probabilistic_runtime_applied = false;
        double probabilistic_h1_calibrated = 0.5;
        double probabilistic_h5_calibrated = 0.5;
        double probabilistic_h5_threshold = 0.6;
        double probabilistic_h5_margin = 0.0;
    };
    std::map<std::string, PendingSignalMetadata> pending_signal_metadata_by_market_;
    
    // ????
    long long start_time_;
    int total_scans_;
    int total_signals_;
};

} // namespace engine
} // namespace autolife


