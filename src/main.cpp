#include "common/Logger.h"
#include "common/Config.h"
#include "network/UpbitHttpClient.h"
#include "engine/TradingEngine.h"
#include "backtest/BacktestEngine.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <Windows.h>
#include <string>
#include <algorithm>
#include <random>
#include <fstream>
#include <filesystem>
#include <ctime>

using namespace autolife;

// ===== 전역 엔진 (Ctrl+C 처리용) =====
std::unique_ptr<engine::TradingEngine> g_engine;

void signalHandler(int signal) {
    if (signal == SIGINT) {
        LOG_INFO("\n종료 신호 수신 (Ctrl+C)");
        if (g_engine) {
            g_engine->stop();
        }
    }
}

// ===== 유틸리티: 콘솔 입력 =====

static std::string readLine() {
    std::string input;
    std::getline(std::cin, input);
    // Trim whitespace
    input.erase(0, input.find_first_not_of(" \t\r\n"));
    input.erase(input.find_last_not_of(" \t\r\n") + 1);
    return input;
}

// 숫자 입력 (정수) - 기본값 포함
static int readInt(const std::string& prompt, int default_val) {
    std::cout << prompt << " [기본값: " << default_val << "]: ";
    std::string input = readLine();
    if (input.empty()) return default_val;
    try { return std::stoi(input); }
    catch (...) {
        std::cout << "  ⚠️ 잘못된 입력, 기본값 " << default_val << " 사용\n";
        return default_val;
    }
}

// 숫자 입력 (실수) - 기본값 포함
static double readDouble(const std::string& prompt, double default_val) {
    std::cout << prompt << " [기본값: " << default_val << "]: ";
    std::string input = readLine();
    if (input.empty()) return default_val;
    try { return std::stod(input); }
    catch (...) {
        std::cout << "  ⚠️ 잘못된 입력, 기본값 " << default_val << " 사용\n";
        return default_val;
    }
}

// Y/N 입력
static bool readYesNo(const std::string& prompt, bool default_val) {
    std::string def_str = default_val ? "Y" : "N";
    std::cout << prompt << " (Y/N) [기본값: " << def_str << "]: ";
    std::string input = readLine();
    if (input.empty()) return default_val;
    char c = static_cast<char>(std::toupper(static_cast<unsigned char>(input[0])));
    return (c == 'Y');
}

// ===== 백테스트용 시뮬레이션 데이터 생성 =====
static std::string generateSimulationCSV(int candle_count, double start_price) {
    // 저장 경로
    std::filesystem::create_directories("data/backtest");
    std::string filename = "data/backtest/auto_sim_" + std::to_string(candle_count) + ".csv";

    std::ofstream out(filename);
    if (!out.is_open()) {
        std::cout << "❌ CSV 파일 생성 실패: " << filename << std::endl;
        return "";
    }

    // 헤더
    out << "timestamp,open,high,low,close,volume\n";

    // 랜덤 엔진
    std::mt19937 rng(static_cast<unsigned int>(std::time(nullptr)));
    std::normal_distribution<double> price_change(0.0, 0.002);  // 평균 0%, 표준편차 0.2%
    std::uniform_real_distribution<double> volume_dist(5.0, 150.0);
    std::uniform_real_distribution<double> wick_dist(0.001, 0.004);  // 꼬리 크기

    // 트렌드 생성 (장기 추세)
    std::uniform_real_distribution<double> trend_dist(-0.0003, 0.0005);
    double trend_bias = trend_dist(rng);
    int trend_duration = 0;
    int trend_max = 100 + (rng() % 200);  // 100~300 캔들마다 추세 변경

    double price = start_price;
    long long timestamp = static_cast<long long>(std::time(nullptr)) - (candle_count * 60);

    for (int i = 0; i < candle_count; ++i) {
        // 추세 변경
        if (++trend_duration > trend_max) {
            trend_bias = trend_dist(rng);
            trend_max = 100 + (rng() % 200);
            trend_duration = 0;
        }

        double change = price_change(rng) + trend_bias;
        double open = price;
        double close = open * (1.0 + change);

        // 고가/저가 (꼬리)
        double upper_wick = open * wick_dist(rng);
        double lower_wick = open * wick_dist(rng);
        double high = std::max(open, close) + upper_wick;
        double low = std::min(open, close) - lower_wick;

        // 거래량 (가격 변동이 클수록 거래량 증가)
        double vol_base = volume_dist(rng);
        double vol_mult = 1.0 + std::abs(change) * 50.0;  // 변동성 비례
        double volume = vol_base * vol_mult;

        out << timestamp << ","
            << std::fixed << std::setprecision(1)
            << open << "," << high << "," << low << "," << close << ","
            << std::setprecision(4) << volume << "\n";

        price = close;
        timestamp += 60;  // 1분봉
    }

    out.close();
    return filename;
}

// ===== 인터랙티브 메뉴 =====
int main(int argc, char* argv[]) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        Logger::getInstance().initialize("logs");

        std::cout << "\n";
        std::cout << "  ╔══════════════════════════════════════╗\n";
        std::cout << "  ║     AutoLife Trading Bot v1.0        ║\n";
        std::cout << "  ║     자동 암호화폐 트레이딩 시스템    ║\n";
        std::cout << "  ╚══════════════════════════════════════╝\n";
        std::cout << "\n";

        // 설정 로드 (API 키 등)
        Config::getInstance().load("config/config.json");
        auto& config = Config::getInstance();

        // ===== CLI 인수로 빠른 실행 지원 (기존 호환) =====
        if (argc > 1) {
            std::string arg1 = argv[1];
            if (arg1 == "--backtest" && argc > 2) {
                std::cout << "🔄 백테스트 모드 (CLI)...\n";
                LOG_INFO("Starting Backtest Mode with file: {}", argv[2]);
                backtest::BacktestEngine bt_engine;
                bt_engine.init(config);
                bt_engine.loadData(argv[2]);
                bt_engine.run();
                auto result = bt_engine.getResult();
                std::cout << "\n📊 백테스트 결과\n";
                std::cout << "────────────────────────────────────────\n";
                std::cout << "최종 잔고: " << (long long)result.final_balance << " KRW\n";
                std::cout << "총 수익금: " << (long long)result.total_profit << " KRW\n";
                std::cout << "MDD:       " << (result.max_drawdown * 100.0) << "%\n";
                std::cout << "총 거래수: " << result.total_trades << "\n";
                std::cout << "승리 거래: " << result.winning_trades << "\n";
                std::cout << "────────────────────────────────────────\n";
                return 0;
            }
        }

        // ===== 모드 선택 =====
        std::cout << "  모드를 선택하세요:\n";
        std::cout << "  ┌─────────────────────────────────────┐\n";
        std::cout << "  │  [1] 🔴 실전 거래 (Live Trading)    │\n";
        std::cout << "  │  [2] 📊 백테스트 (Backtest)         │\n";
        std::cout << "  └─────────────────────────────────────┘\n";
        std::cout << "  선택: ";

        std::string mode_input = readLine();
        int mode_choice = 0;
        try { mode_choice = std::stoi(mode_input); } catch (...) {}

        // ============================================================
        //  [2] 백테스트 모드
        // ============================================================
        if (mode_choice == 2) {
            std::cout << "\n";
            std::cout << "  ╔══════════════════════════════════════╗\n";
            std::cout << "  ║        📊 백테스트 설정              ║\n";
            std::cout << "  ╚══════════════════════════════════════╝\n\n";

            double bt_capital = readDouble("  💰 초기 자본금 (KRW, 숫자만 입력)", 1000000.0);
            int bt_candles = readInt("  📈 시뮬레이션 캔들 수 (500/1000/2000 등)", 2000);
            double bt_start_price = readDouble("  💲 시작 가격 (예: 50000000 = BTC 5천만원)", 50000000.0);

            std::cout << "\n  ⏳ 시뮬레이션 데이터 생성 중...\n";
            std::string csv_path = generateSimulationCSV(bt_candles, bt_start_price);
            if (csv_path.empty()) {
                std::cout << "  ❌ 데이터 생성 실패\n";
                return 1;
            }
            std::cout << "  ✅ " << csv_path << " 생성 완료 (" << bt_candles << "개 캔들)\n\n";

            // Config에 초기 자본 설정
            config.setInitialCapital(bt_capital);

            std::cout << "  🔄 백테스트 실행 중...\n\n";
            LOG_INFO("Interactive Backtest: {} candles, capital={:.0f}", bt_candles, bt_capital);

            backtest::BacktestEngine bt_engine;
            bt_engine.init(config);
            bt_engine.loadData(csv_path);
            bt_engine.run();

            auto result = bt_engine.getResult();
            double profit_pct = (bt_capital > 0) ? (result.total_profit / bt_capital * 100.0) : 0.0;

            std::cout << "\n";
            std::cout << "  ╔══════════════════════════════════════╗\n";
            std::cout << "  ║        📊 백테스트 결과              ║\n";
            std::cout << "  ╚══════════════════════════════════════╝\n\n";
            std::cout << "  초기 자본:   " << (long long)bt_capital << " KRW\n";
            std::cout << "  최종 잔고:   " << (long long)result.final_balance << " KRW\n";
            std::cout << "  총 수익금:   " << (long long)result.total_profit << " KRW";
            if (result.total_profit >= 0) std::cout << " 📈";
            else std::cout << " 📉";
            std::cout << "\n";
            std::cout << "  수익률:      " << std::fixed << std::setprecision(2) << profit_pct << "%\n";
            std::cout << "  MDD:         " << std::setprecision(3) << (result.max_drawdown * 100.0) << "%\n";
            std::cout << "  총 거래수:   " << result.total_trades << "\n";
            std::cout << "  승리 거래:   " << result.winning_trades << "\n";
            std::cout << "  ────────────────────────────────────────\n\n";

            std::cout << "  엔터를 눌러 종료...";
            std::cin.get();
            return 0;
        }

        // ============================================================
        //  [1] 실전 거래 모드
        // ============================================================
        std::cout << "\n";
        std::cout << "  ╔══════════════════════════════════════╗\n";
        std::cout << "  ║     🔴 실전 거래 설정                ║\n";
        std::cout << "  ╚══════════════════════════════════════╝\n\n";

        // API 키 확인
        std::string access_key = config.getAccessKey();
        std::string secret_key = config.getSecretKey();

        if (access_key.empty() || secret_key.empty() ||
            access_key == "YOUR_ACCESS_KEY" || secret_key == "YOUR_SECRET_KEY") {
            std::cout << "  ⚠️  API 키가 설정되지 않았습니다\n";
            std::cout << "  config/config.json 파일에서 api.access_key, api.secret_key를 설정하세요\n\n";
            std::cout << "  엔터를 눌러 종료...";
            std::cin.get();
            return 1;
        }

        // 파라미터 입력
        bool dry_run = readYesNo("  🔒 Dry Run 모드? (실제 주문 없이 시뮬레이션만)", true);
        int max_positions = readInt("  📦 동시 보유 최대 종목 수", 5);
        int max_daily_trades = readInt("  🔄 일일 최대 거래 횟수", 50);

        std::cout << "\n  ── 리스크 관리 ──\n";
        std::cout << "  ℹ️  아래 항목은 모두 \"얼마까지 잃어도 되는지\" 설정입니다.\n\n";

        double max_drawdown_pct = readDouble(
            "  📉 [전체] 총 누적 최대 손실 한도 (%)\n"
            "        봇 시작 후 전체 기간 동안 최고점 대비 허용 하락폭\n"
            "        예: 15 → 잔고 100만원에서 85만원이 되면 모든 거래 중단", 15.0);
        double max_daily_loss_pct = readDouble(
            "  🚨 [하루] 일일 손실 한도 - 비율 (%)\n"
            "        하루 안에 잃을 수 있는 최대 비율 (매일 자정에 초기화)\n"
            "        예: 5 → 잔고 100만원이면 하루 5만원 손실 시 당일 거래 중단", 5.0);
        double max_daily_loss_krw = readDouble(
            "  💸 [하루] 일일 손실 한도 - 금액 (KRW)\n"
            "        위 비율과 별도로, 절대 금액 기준 추가 안전장치\n"
            "        예: 50000 → 하루 5만원 손실 시 당일 거래 중단", 50000.0);
        double max_exposure_pct = readDouble(
            "  📊 최대 투자 비율 (%)\n"
            "        전체 자본 중 동시에 코인에 투자할 수 있는 최대 비율\n"
            "        예: 85 → 잔고 100만원 중 최대 85만원까지만 투자", 85.0);
        double risk_per_trade = readDouble(
            "  ⚖️ 건당 투자 비율 (%)\n"
            "        한 번 거래할 때 전체 자본 대비 투자 비율\n"
            "        예: 0.5 → 잔고 100만원이면 건당 5,000원 투입", 0.5);

        std::cout << "\n  ── 주문 한도 ──\n";
        double max_order_krw = readDouble("  💰 단일 주문 최대 금액 (KRW)", 500000.0);
        double min_order_krw = readDouble("  💰 단일 주문 최소 금액 (KRW)", 5000.0);
        double max_slippage = readDouble("  📏 허용 슬리피지 (%, 예: 0.3 = 0.3%)", 0.3);

        int scan_interval = readInt("  ⏱️ 시장 스캔 주기 (초)", 60);

        // EngineConfig 구성
        engine::EngineConfig engine_config;
        engine_config.mode = engine::TradingMode::LIVE;
        engine_config.dry_run = dry_run;
        engine_config.initial_capital = 0;  // syncAccountState에서 실잔고로 덮어쓰기
        engine_config.max_positions = max_positions;
        engine_config.max_daily_trades = max_daily_trades;
        engine_config.max_drawdown = max_drawdown_pct / 100.0;
        engine_config.max_daily_loss_pct = max_daily_loss_pct / 100.0;
        engine_config.max_daily_loss_krw = max_daily_loss_krw;
        engine_config.max_exposure_pct = max_exposure_pct / 100.0;
        engine_config.risk_per_trade_pct = risk_per_trade / 100.0;
        engine_config.max_order_krw = max_order_krw;
        engine_config.min_order_krw = min_order_krw;
        engine_config.max_slippage_pct = max_slippage / 100.0;
        engine_config.scan_interval_seconds = scan_interval;

        // 전략: config.json에서 읽거나 모든 전략 활성화
        auto cfg_strategies = config.getEngineConfig().enabled_strategies;
        if (!cfg_strategies.empty()) {
            engine_config.enabled_strategies = cfg_strategies;
        }
        // empty면 TradingEngine이 모든 전략 등록

        // 설정 요약 출력
        std::cout << "\n";
        std::cout << "  ╔══════════════════════════════════════╗\n";
        std::cout << "  ║        📋 설정 확인                  ║\n";
        std::cout << "  ╚══════════════════════════════════════╝\n\n";
        std::cout << "  모드:          " << (dry_run ? "🟢 DRY RUN (주문 미실행)" : "🔴 LIVE (실제 주문)") << "\n";
        std::cout << "  동시 포지션:   " << max_positions << "개\n";
        std::cout << "  일일 거래:     최대 " << max_daily_trades << "회\n";
        std::cout << "  최대 하락폭:   " << max_drawdown_pct << "% (전체 누적)\n";
        std::cout << "  일일 손실:     " << max_daily_loss_pct << "% / " << (long long)max_daily_loss_krw << "원 (하루)\n";
        std::cout << "  투자 비율:     최대 " << max_exposure_pct << "% / 건당 " << risk_per_trade << "%\n";
        std::cout << "  주문 한도:     " << (long long)min_order_krw << " ~ " << (long long)max_order_krw << " KRW\n";
        std::cout << "  슬리피지:      " << max_slippage << "%\n";
        std::cout << "  스캔 주기:     " << scan_interval << "초\n";
        std::cout << "  ────────────────────────────────────────\n\n";

        if (!readYesNo("  위 설정으로 시작하시겠습니까?", true)) {
            std::cout << "  취소되었습니다.\n";
            return 0;
        }

        // HTTP 클라이언트 생성
        LOG_INFO("========================================");
        LOG_INFO("AutoLife Trading Bot v1.0 - Live Mode");
        LOG_INFO("========================================");

        auto http_client = std::make_shared<network::UpbitHttpClient>(access_key, secret_key);

        // 연결 테스트
        std::cout << "\n  📡 업비트 API 연결 테스트...\n";
        auto all_markets = http_client->getMarkets();
        int krw_count = 0;
        if (all_markets.is_array()) {
            for (const auto& market : all_markets) {
                if (market.contains("market")) {
                    std::string mname = market["market"].get<std::string>();
                    if (mname.substr(0, 3) == "KRW") krw_count++;
                }
            }
        }
        std::cout << "  ✅ 연결 성공! KRW 마켓: " << krw_count << "개\n";
        LOG_INFO("KRW 마켓: {}개", krw_count);

        // 엔진 생성
        g_engine = std::make_unique<engine::TradingEngine>(engine_config, http_client);
        std::signal(SIGINT, signalHandler);

        std::cout << "\n  🚀 거래 엔진 시작!\n";
        std::cout << "  ⏸️  중지: Ctrl+C\n";
        std::cout << "  ════════════════════════════════════════\n\n";

        if (!g_engine->start()) {
            LOG_ERROR("엔진 시작 실패");
            std::cout << "  ❌ 엔진 시작 실패\n";
            std::cin.get();
            return 1;
        }

        // 실전 잔고 확인
        if (engine_config.mode == engine::TradingMode::LIVE) {
            auto metrics = g_engine->getMetrics();
            std::cout << "  💰 실제 계좌 연동 완료\n";
            std::cout << "     보유 현금: " << (long long)metrics.total_capital << " KRW\n\n";
        }

        // 메인 루프
        while (g_engine->isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "\n  ════════════════════════════════════════\n";
        std::cout << "  프로그램 종료\n";
        std::cout << "  ════════════════════════════════════════\n";
        LOG_INFO("프로그램 종료");

        return 0;

    } catch (const std::exception& e) {
        LOG_ERROR("치명적 오류: {}", e.what());
        std::cout << "\n  ❌ 오류 발생: " << e.what() << std::endl;
        std::cout << "  엔터를 눌러 종료..." << std::endl;
        std::cin.get();
        return 1;
    }

    return 0;
}
