#include "app/LiveModeHandler.h"

#include "common/Logger.h"
#include "network/UpbitHttpClient.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cctype>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace autolife::app {

namespace {

static std::string readLine() {
    std::string input;
    std::getline(std::cin, input);

    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

static int readInt(const std::string& prompt, int default_val) {
    std::cout << prompt << " [기본값: " << default_val << "]: ";
    std::string input = readLine();
    if (input.empty()) {
        return default_val;
    }

    try {
        return std::stoi(input);
    } catch (...) {
        std::cout << "  입력값이 올바르지 않아 기본값 " << default_val << "을 사용합니다.\n";
        return default_val;
    }
}

static double readDouble(const std::string& prompt, double default_val) {
    std::cout << prompt << " [기본값: " << default_val << "]: ";
    std::string input = readLine();
    if (input.empty()) {
        return default_val;
    }

    try {
        return std::stod(input);
    } catch (...) {
        std::cout << "  입력값이 올바르지 않아 기본값 " << default_val << "을 사용합니다.\n";
        return default_val;
    }
}

static bool readYesNo(const std::string& prompt, bool default_val) {
    const std::string default_text = default_val ? "Y" : "N";
    std::cout << prompt << " (Y/N) [기본값: " << default_text << "]: ";

    std::string input = readLine();
    if (input.empty()) {
        return default_val;
    }

    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(input[0])));
    return (c == 'Y');
}

}  // namespace

int runInteractiveLiveMode(
    Config& config,
    std::unique_ptr<engine::TradingEngine>& engine_instance,
    void (*signal_handler)(int)
) {
    std::cout << "\n[실거래 설정]\n";

    std::string access_key = config.getAccessKey();
    std::string secret_key = config.getSecretKey();
    auto cfg_engine = config.getEngineConfig();
    if (access_key.empty() || secret_key.empty() ||
        access_key == "YOUR_ACCESS_KEY" || secret_key == "YOUR_SECRET_KEY") {
        std::cout << "API 키가 설정되지 않았습니다.\n";
        std::cout << "`config/config.json` 또는 환경변수(`UPBIT_ACCESS_KEY`, `UPBIT_SECRET_KEY`)를 설정하세요.\n\n";
        std::cout << "엔터를 누르면 종료합니다.";
        std::cin.get();
        return 1;
    }

    bool dry_run = readYesNo("Dry Run 모드로 실행할까요? (실주문 없음)", cfg_engine.dry_run);
    bool allow_live_orders = false;
    if (!dry_run) {
        allow_live_orders = readYesNo(
            "실주문 제출을 허용할까요? (매우 주의)",
            cfg_engine.allow_live_orders
        );
    }
    int max_positions = readInt("동시 보유 최대 종목 수", cfg_engine.max_positions);
    int max_daily_trades = readInt("일일 최대 거래 횟수", cfg_engine.max_daily_trades);
    bool advanced_mode = readYesNo("고급 설정 모드로 세부 파라미터를 직접 조정할까요?", false);
    int live_profile = 2;
    std::string live_profile_name = "BALANCED";

    double max_drawdown_pct = cfg_engine.max_drawdown * 100.0;
    double max_daily_loss_pct = cfg_engine.max_daily_loss_pct * 100.0;
    double max_daily_loss_krw = cfg_engine.max_daily_loss_krw;
    double max_exposure_pct = cfg_engine.max_exposure_pct * 100.0;
    double risk_per_trade = cfg_engine.risk_per_trade_pct * 100.0;

    double max_order_krw = cfg_engine.max_order_krw;
    double min_order_krw = cfg_engine.min_order_krw;
    int max_new_orders_per_scan = cfg_engine.max_new_orders_per_scan;
    double max_slippage = cfg_engine.max_slippage_pct * 100.0;
    int scan_interval = cfg_engine.scan_interval_seconds;

    double min_expected_edge = cfg_engine.min_expected_edge_pct * 100.0;
    double min_reward_risk = cfg_engine.min_reward_risk;
    double min_rr_weak = cfg_engine.min_rr_weak_signal;
    double min_rr_strong = cfg_engine.min_rr_strong_signal;
    int min_ev_trades = cfg_engine.min_strategy_trades_for_ev;
    double min_ev_krw = cfg_engine.min_strategy_expectancy_krw;
    double min_ev_pf = cfg_engine.min_strategy_profit_factor;
    bool avoid_high_volatility = cfg_engine.avoid_high_volatility;
    bool avoid_trending_down = cfg_engine.avoid_trending_down;

    if (advanced_mode) {
        std::cout << "\n[리스크 설정]\n";
        max_drawdown_pct = readDouble("전체 기간 최대 손실 허용(%)", max_drawdown_pct);
        max_daily_loss_pct = readDouble("일일 손실 허용(%)", max_daily_loss_pct);
        max_daily_loss_krw = readDouble("일일 손실 허용(KRW)", max_daily_loss_krw);
        max_exposure_pct = readDouble("최대 투자 비중(%)", max_exposure_pct);
        risk_per_trade = readDouble("거래당 투자 비중(%)", risk_per_trade);

        std::cout << "\n[주문 제한]\n";
        max_order_krw = readDouble("1회 주문 최대 금액(KRW)", max_order_krw);
        min_order_krw = readDouble("1회 주문 최소 금액(KRW)", min_order_krw);
        max_new_orders_per_scan = readInt("스캔당 신규 주문 최대 개수", max_new_orders_per_scan);
        max_slippage = readDouble("허용 슬리피지(%)", max_slippage);
        scan_interval = readInt("시장 스캔 주기(초)", scan_interval);

        std::cout << "\n[진입 품질 게이트]\n";
        min_expected_edge = readDouble("최소 순기대엣지(%)", min_expected_edge);
        min_reward_risk = readDouble("최소 손익비(TP/SL)", min_reward_risk);
        min_rr_weak = readDouble("약한 신호 최소 RR", min_rr_weak);
        min_rr_strong = readDouble("강한 신호 최소 RR", min_rr_strong);
        min_ev_trades = readInt("전략 EV 계산 최소 거래수", min_ev_trades);
        min_ev_krw = readDouble("전략 최소 기대값(KRW/trade)", min_ev_krw);
        min_ev_pf = readDouble("전략 최소 Profit Factor", min_ev_pf);
        avoid_high_volatility = readYesNo("고변동 구간(HIGH_VOLATILITY) 진입 차단", avoid_high_volatility);
        avoid_trending_down = readYesNo("하락추세(TRENDING_DOWN) 진입 차단", avoid_trending_down);
    } else {
        std::cout << "\n[간단 설정]\n";
        live_profile = std::clamp(readInt("운영 프로파일 [1=SAFE, 2=BALANCED, 3=ACTIVE]", 2), 1, 3);
        if (live_profile == 1) {
            live_profile_name = "SAFE";
            max_drawdown_pct = std::min(max_drawdown_pct, 12.0);
            max_daily_loss_pct = std::min(max_daily_loss_pct, 3.0);
            max_exposure_pct = std::min(max_exposure_pct, 70.0);
            risk_per_trade = std::min(risk_per_trade, 0.35);
            max_new_orders_per_scan = 1;
            min_expected_edge = std::max(min_expected_edge, 0.14);
            min_reward_risk = std::max(min_reward_risk, 1.35);
            min_rr_weak = std::max(min_rr_weak, 2.0);
            min_rr_strong = std::max(min_rr_strong, 1.3);
            min_ev_trades = std::max(min_ev_trades, 40);
            min_ev_pf = std::max(min_ev_pf, 1.00);
            avoid_high_volatility = true;
            avoid_trending_down = true;
        } else if (live_profile == 3) {
            live_profile_name = "ACTIVE";
            max_drawdown_pct = std::max(max_drawdown_pct, 15.0);
            max_daily_loss_pct = std::max(max_daily_loss_pct, 4.0);
            max_exposure_pct = std::min(95.0, std::max(max_exposure_pct, 85.0));
            risk_per_trade = std::min(1.20, std::max(risk_per_trade, 0.55));
            max_new_orders_per_scan = std::max(max_new_orders_per_scan, 3);
            min_expected_edge = std::max(0.02, min_expected_edge * 0.80);
            min_reward_risk = std::max(1.00, min_reward_risk - 0.10);
            min_rr_weak = std::max(1.20, min_rr_weak - 0.40);
            min_rr_strong = std::max(0.90, min_rr_strong - 0.20);
            min_ev_trades = std::max(5, std::min(min_ev_trades, 20));
            min_ev_pf = std::max(0.85, std::min(min_ev_pf, 0.95));
            avoid_high_volatility = false;
            avoid_trending_down = false;
        } else {
            live_profile_name = "BALANCED";
        }
        scan_interval = readInt("시장 스캔 주기(초)", scan_interval);
    }

    engine::EngineConfig engine_config;
    engine_config.mode = engine::TradingMode::LIVE;
    engine_config.dry_run = dry_run;
    engine_config.allow_live_orders = allow_live_orders;
    engine_config.initial_capital = 0;
    engine_config.max_positions = max_positions;
    engine_config.max_daily_trades = max_daily_trades;
    engine_config.max_drawdown = max_drawdown_pct / 100.0;
    engine_config.max_daily_loss_pct = max_daily_loss_pct / 100.0;
    engine_config.max_daily_loss_krw = max_daily_loss_krw;
    engine_config.max_exposure_pct = max_exposure_pct / 100.0;
    engine_config.risk_per_trade_pct = risk_per_trade / 100.0;
    engine_config.max_order_krw = max_order_krw;
    engine_config.min_order_krw = min_order_krw;
    engine_config.max_new_orders_per_scan = std::max(1, max_new_orders_per_scan);
    engine_config.max_slippage_pct = max_slippage / 100.0;
    engine_config.scan_interval_seconds = scan_interval;
    engine_config.min_expected_edge_pct = min_expected_edge / 100.0;
    engine_config.min_reward_risk = std::max(0.1, min_reward_risk);
    engine_config.min_rr_weak_signal = std::max(0.5, min_rr_weak);
    engine_config.min_rr_strong_signal = std::max(0.5, min_rr_strong);
    if (engine_config.min_rr_strong_signal > engine_config.min_rr_weak_signal) {
        std::swap(engine_config.min_rr_strong_signal, engine_config.min_rr_weak_signal);
    }
    engine_config.min_strategy_trades_for_ev = std::max(1, min_ev_trades);
    engine_config.min_strategy_expectancy_krw = min_ev_krw;
    engine_config.min_strategy_profit_factor = std::max(0.1, min_ev_pf);
    engine_config.avoid_high_volatility = avoid_high_volatility;
    engine_config.avoid_trending_down = avoid_trending_down;

    auto cfg_strategies = config.getEngineConfig().enabled_strategies;
    if (!cfg_strategies.empty()) {
        engine_config.enabled_strategies = cfg_strategies;
    }

    std::cout << "\n[설정 요약]\n";
    std::cout << "모드:            " << (dry_run ? "DRY RUN" : "LIVE") << "\n";
    std::cout << "실주문 제출:     " << (engine_config.allow_live_orders ? "허용" : "차단(시뮬레이션)") << "\n";
    std::cout << "설정 방식:       "
              << (advanced_mode ? "ADVANCED(직접입력)" : (std::string("SIMPLE(") + live_profile_name + ")"))
              << "\n";
    std::cout << "동시 보유:       " << max_positions << "개\n";
    std::cout << "일일 거래 횟수:  최대 " << max_daily_trades << "회\n";
    std::cout << "최대 누적 손실:  " << max_drawdown_pct << "%\n";
    std::cout << "일일 손실 제한:  " << max_daily_loss_pct << "% / "
              << static_cast<long long>(max_daily_loss_krw) << " KRW\n";
    std::cout << "최대 노출 비중:  " << max_exposure_pct << "%\n";
    std::cout << "거래당 비중:     " << risk_per_trade << "%\n";
    std::cout << "주문 금액 범위:  " << static_cast<long long>(min_order_krw)
              << " ~ " << static_cast<long long>(max_order_krw) << " KRW\n";
    std::cout << "스캔당 신규주문: 최대 " << engine_config.max_new_orders_per_scan << "건\n";
    std::cout << "허용 슬리피지:   " << max_slippage << "%\n";
    std::cout << "최소 순기대엣지: " << min_expected_edge << "%\n";
    std::cout << "최소 손익비:     " << engine_config.min_reward_risk << "\n";
    std::cout << "약한 신호 RR:    " << engine_config.min_rr_weak_signal << "\n";
    std::cout << "강한 신호 RR:    " << engine_config.min_rr_strong_signal << "\n";
    std::cout << "EV 최소 거래수:  " << engine_config.min_strategy_trades_for_ev << "\n";
    std::cout << "EV 기대값 하한:  " << engine_config.min_strategy_expectancy_krw << " KRW/trade\n";
    std::cout << "EV PF 하한:      " << engine_config.min_strategy_profit_factor << "\n";
    std::cout << "고변동 차단:     " << (engine_config.avoid_high_volatility ? "ON" : "OFF") << "\n";
    std::cout << "하락추세 차단:   " << (engine_config.avoid_trending_down ? "ON" : "OFF") << "\n";
    std::cout << "스캔 주기:       " << scan_interval << "초\n\n";
    if (!advanced_mode) {
        std::cout << "참고: 세부 임계치는 내부 적응형 정책이 실시간 보정합니다.\n\n";
    }

    if (!readYesNo("이 설정으로 시작할까요?", true)) {
        std::cout << "취소했습니다.\n";
        return 0;
    }

    LOG_INFO("========================================");
    LOG_INFO("AutoLife Trading Bot v1.0 - Live Mode");
    LOG_INFO("========================================");

    auto http_client = std::make_shared<network::UpbitHttpClient>(access_key, secret_key);

    std::cout << "\n업비트 API 연결 테스트 중...\n";
    auto all_markets = http_client->getMarkets();
    int krw_count = 0;
    if (all_markets.is_array()) {
        for (const auto& market : all_markets) {
            if (market.contains("market")) {
                const std::string mname = market["market"].get<std::string>();
                if (mname.rfind("KRW", 0) == 0) {
                    ++krw_count;
                }
            }
        }
    }

    std::cout << "연결 성공: KRW 마켓 " << krw_count << "개\n";
    LOG_INFO("KRW markets: {}", krw_count);

    engine_instance = std::make_unique<engine::TradingEngine>(engine_config, http_client);
    if (signal_handler != nullptr) {
        std::signal(SIGINT, signal_handler);
    }

    std::cout << "\n거래 엔진을 시작합니다.\n";
    std::cout << "중지하려면 Ctrl+C를 누르세요.\n\n";

    if (!engine_instance->start()) {
        LOG_ERROR("엔진 시작 실패");
        std::cout << "엔진 시작에 실패했습니다.\n";
        std::cin.get();
        return 1;
    }

    if (engine_config.mode == engine::TradingMode::LIVE) {
        auto metrics = engine_instance->getMetrics();
        std::cout << "초기화 완료\n";
        std::cout << "보유 자산: " << static_cast<long long>(metrics.total_capital) << " KRW\n\n";
    }

    while (engine_instance->isRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\n프로그램이 종료됩니다.\n";
    LOG_INFO("Program terminated");
    return 0;
}

}  // namespace autolife::app
