#include "common/Logger.h"
#include "common/Config.h"
#include "network/UpbitHttpClient.h"
#include "engine/TradingEngine.h"
#include "backtest/BacktestEngine.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <chrono>
#include <sstream>
#include <vector>

using namespace autolife;

// 전역 엔진 인스턴스(Ctrl+C 종료용)
std::unique_ptr<engine::TradingEngine> g_engine;

void signalHandler(int signal) {
    if (signal == SIGINT) {
        LOG_INFO("\n종료 신호 수신 (Ctrl+C)");
        if (g_engine) {
            g_engine->stop();
        }
    }
}

// 콘솔 입력 헬퍼
static std::string readLine() {
    std::string input;
    std::getline(std::cin, input);

    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    input = input.substr(first, last - first + 1);
    return input;
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

static std::string toLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

struct CompanionCheckResult {
    bool applicable = false;
    std::vector<std::string> missing_tokens;
    std::vector<std::string> found_tokens;
};

static CompanionCheckResult checkHigherTfCompanions(const std::string& csv_path) {
    CompanionCheckResult out;
    std::filesystem::path primary(csv_path);
    if (!std::filesystem::exists(primary) || !primary.has_parent_path()) {
        return out;
    }

    const std::string stem_lower = toLowerCopy(primary.stem().string());
    const std::string prefix = "upbit_";
    const std::string pivot = "_1m_";
    if (!startsWith(stem_lower, prefix)) {
        return out;
    }

    const size_t market_begin = prefix.size();
    const size_t market_end = stem_lower.find(pivot, market_begin);
    if (market_end == std::string::npos || market_end <= market_begin) {
        return out;
    }
    out.applicable = true;

    const std::string market_token = stem_lower.substr(market_begin, market_end - market_begin);
    const std::filesystem::path parent = primary.parent_path();

    auto hasCompanion = [&](const std::string& token) {
        const std::string expected_prefix = "upbit_" + market_token + "_" + token + "_";
        for (const auto& entry : std::filesystem::directory_iterator(parent)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (toLowerCopy(entry.path().extension().string()) != ".csv") {
                continue;
            }
            const std::string candidate_stem = toLowerCopy(entry.path().stem().string());
            if (startsWith(candidate_stem, expected_prefix)) {
                return true;
            }
        }
        return false;
    };

    for (const auto& token : {"5m", "60m", "240m"}) {
        if (hasCompanion(token)) {
            out.found_tokens.push_back(token);
        } else {
            out.missing_tokens.push_back(token);
        }
    }

    return out;
}

static void printCompanionRequirementError(const std::string& csv_path, const CompanionCheckResult& check) {
    std::cout << "실거래 동등 MTF 모드 검증 실패: " << csv_path << "\n";
    if (!check.applicable) {
        std::cout << "  파일명 규칙이 맞지 않습니다. 예: upbit_KRW_BTC_1m_12000.csv\n";
        std::cout << "  companion(5m/60m/240m) 자동 매칭이 가능한 1m 파일을 지정하세요.\n";
        return;
    }

    if (!check.missing_tokens.empty()) {
        std::cout << "  누락된 companion TF: ";
        for (size_t i = 0; i < check.missing_tokens.size(); ++i) {
            if (i > 0) {
                std::cout << ", ";
            }
            std::cout << check.missing_tokens[i];
        }
        std::cout << "\n";
        std::cout << "  같은 폴더에 upbit_<market>_5m_*.csv / 60m / 240m 파일이 필요합니다.\n";
    }
}

static std::vector<std::string> listRealDataPrimaryCsvs(bool require_companions) {
    std::vector<std::string> out;
    const std::filesystem::path root("data/backtest_real");
    if (!std::filesystem::exists(root)) {
        return out;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = toLowerCopy(entry.path().extension().string());
        if (ext != ".csv") {
            continue;
        }
        const std::string name_lower = toLowerCopy(entry.path().filename().string());
        if (name_lower.find("_1m_") == std::string::npos) {
            continue;
        }

        const std::string path_str = entry.path().string();
        if (require_companions) {
            auto check = checkHigherTfCompanions(path_str);
            if (!check.applicable || !check.missing_tokens.empty()) {
                continue;
            }
        }
        out.push_back(path_str);
    }

    std::sort(out.begin(), out.end());
    return out;
}

// 백테스트용 모의 데이터 생성
static std::string generateSimulationCSV(int candle_count, double start_price) {
    std::filesystem::create_directories("data/backtest");
    const std::string filename = "data/backtest/auto_sim_" + std::to_string(candle_count) + ".csv";

    std::ofstream out(filename);
    if (!out.is_open()) {
        std::cout << "CSV 파일 생성 실패: " << filename << std::endl;
        return "";
    }

    out << "timestamp,open,high,low,close,volume\n";

    std::mt19937 rng(static_cast<unsigned int>(std::time(nullptr)));
    std::normal_distribution<double> price_change(0.0, 0.002);
    std::uniform_real_distribution<double> volume_dist(5.0, 150.0);
    std::uniform_real_distribution<double> wick_dist(0.001, 0.004);

    std::uniform_real_distribution<double> trend_dist(-0.0003, 0.0005);
    double trend_bias = trend_dist(rng);
    int trend_duration = 0;
    int trend_max = 100 + (rng() % 200);

    double price = start_price;
    long long timestamp = static_cast<long long>(std::time(nullptr)) - (candle_count * 60);

    for (int i = 0; i < candle_count; ++i) {
        if (++trend_duration > trend_max) {
            trend_bias = trend_dist(rng);
            trend_max = 100 + (rng() % 200);
            trend_duration = 0;
        }

        const double change = price_change(rng) + trend_bias;
        const double open = price;
        const double close = open * (1.0 + change);

        const double upper_wick = open * wick_dist(rng);
        const double lower_wick = open * wick_dist(rng);
        const double high = std::max(open, close) + upper_wick;
        const double low = std::min(open, close) - lower_wick;

        const double vol_base = volume_dist(rng);
        const double vol_mult = 1.0 + std::abs(change) * 50.0;
        const double volume = vol_base * vol_mult;

        out << timestamp << ","
            << std::fixed << std::setprecision(1)
            << open << "," << high << "," << low << "," << close << ","
            << std::setprecision(4) << volume << "\n";

        price = close;
        timestamp += 60;
    }

    out.close();
    return filename;
}

int main(int argc, char* argv[]) {
    try {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        Logger::getInstance().initialize("logs");

        std::cout << "\n";
        std::cout << "=============================================\n";
        std::cout << "       AutoLife Trading Bot v1.0\n";
        std::cout << "       자동 신호기반 트레이딩 시스템\n";
        std::cout << "=============================================\n\n";

        Config::getInstance().load("config/config.json");
        auto& config = Config::getInstance();

        if (argc > 1) {
            std::string arg1 = argv[1];
            if (arg1 == "--backtest" && argc > 2) {
                bool json_mode = false;
                std::vector<std::string> cli_enabled_strategies;
                double cli_initial_capital = -1.0;
                bool cli_require_higher_tf_companions = false;

                auto trim_copy = [](std::string s) {
                    const auto first = s.find_first_not_of(" \t\r\n");
                    if (first == std::string::npos) {
                        return std::string();
                    }
                    const auto last = s.find_last_not_of(" \t\r\n");
                    return s.substr(first, last - first + 1);
                };
                auto normalize_strategy_name = [&](std::string s) {
                    s = trim_copy(s);
                    std::transform(s.begin(), s.end(), s.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (s == "grid") {
                        return std::string("grid_trading");
                    }
                    return s;
                };

                for (int i = 3; i < argc; ++i) {
                    const std::string arg = argv[i];
                    if (arg == "--json") {
                        json_mode = true;
                        continue;
                    }
                    if (arg == "--require-higher-tf-companions") {
                        cli_require_higher_tf_companions = true;
                        continue;
                    }
                    if (arg == "--strategies" && i + 1 < argc) {
                        std::string csv = argv[++i];
                        size_t start = 0;
                        while (start <= csv.size()) {
                            const size_t comma = csv.find(',', start);
                            std::string token = (comma == std::string::npos)
                                ? csv.substr(start)
                                : csv.substr(start, comma - start);
                            token = normalize_strategy_name(token);
                            if (!token.empty()) {
                                cli_enabled_strategies.push_back(token);
                            }
                            if (comma == std::string::npos) {
                                break;
                            }
                            start = comma + 1;
                        }
                        continue;
                    }
                    if (arg == "--initial-capital" && i + 1 < argc) {
                        try {
                            cli_initial_capital = std::stod(argv[++i]);
                        } catch (...) {
                            std::cerr << "Invalid --initial-capital value. Ignored.\n";
                        }
                    }
                }

                if (cli_initial_capital > 0.0) {
                    config.setInitialCapital(cli_initial_capital);
                }
                if (!cli_enabled_strategies.empty()) {
                    config.setEnabledStrategies(cli_enabled_strategies);
                }

                std::cout << "백테스트 모드(CLI) 실행\n";
                const std::string cli_backtest_path = argv[2];
                if (!std::filesystem::exists(cli_backtest_path)) {
                    std::cerr << "백테스트 파일을 찾을 수 없습니다: " << cli_backtest_path << "\n";
                    return 1;
                }
                if (cli_require_higher_tf_companions) {
                    const auto check = checkHigherTfCompanions(cli_backtest_path);
                    if (!check.applicable || !check.missing_tokens.empty()) {
                        printCompanionRequirementError(cli_backtest_path, check);
                        return 1;
                    }
                }
                LOG_INFO("Starting Backtest Mode with file: {}", cli_backtest_path);

                backtest::BacktestEngine bt_engine;
                bt_engine.init(config);
                bt_engine.loadData(cli_backtest_path);
                bt_engine.run();

                auto result = bt_engine.getResult();
                if (json_mode) {
                    nlohmann::json j;
                    j["final_balance"] = result.final_balance;
                    j["total_profit"] = result.total_profit;
                    j["max_drawdown"] = result.max_drawdown;
                    j["total_trades"] = result.total_trades;
                    j["winning_trades"] = result.winning_trades;
                    j["losing_trades"] = result.losing_trades;
                    j["win_rate"] = result.win_rate;
                    j["avg_win_krw"] = result.avg_win_krw;
                    j["avg_loss_krw"] = result.avg_loss_krw;
                    j["profit_factor"] = result.profit_factor;
                    j["expectancy_krw"] = result.expectancy_krw;
                    j["strategy_summaries"] = nlohmann::json::array();
                    for (const auto& s : result.strategy_summaries) {
                        j["strategy_summaries"].push_back({
                            {"strategy_name", s.strategy_name},
                            {"total_trades", s.total_trades},
                            {"winning_trades", s.winning_trades},
                            {"losing_trades", s.losing_trades},
                            {"win_rate", s.win_rate},
                            {"total_profit", s.total_profit},
                            {"avg_win_krw", s.avg_win_krw},
                            {"avg_loss_krw", s.avg_loss_krw},
                            {"profit_factor", s.profit_factor}
                        });
                    }
                    j["pattern_summaries"] = nlohmann::json::array();
                    for (const auto& p : result.pattern_summaries) {
                        j["pattern_summaries"].push_back({
                            {"strategy_name", p.strategy_name},
                            {"regime", p.regime},
                            {"strength_bucket", p.strength_bucket},
                            {"expected_value_bucket", p.expected_value_bucket},
                            {"reward_risk_bucket", p.reward_risk_bucket},
                            {"total_trades", p.total_trades},
                            {"winning_trades", p.winning_trades},
                            {"losing_trades", p.losing_trades},
                            {"win_rate", p.win_rate},
                            {"total_profit", p.total_profit},
                            {"avg_profit_krw", p.avg_profit_krw},
                            {"profit_factor", p.profit_factor}
                        });
                    }
                    std::cout << j.dump() << "\n";
                    return 0;
                }

                std::cout << "\n백테스트 결과\n";
                std::cout << "---------------------------------------------\n";
                std::cout << "최종 잔고:   " << static_cast<long long>(result.final_balance) << " KRW\n";
                std::cout << "총 수익:     " << static_cast<long long>(result.total_profit) << " KRW\n";
                std::cout << "MDD:        " << (result.max_drawdown * 100.0) << "%\n";
                std::cout << "총 거래 수:  " << result.total_trades << "\n";
                std::cout << "승리 거래:   " << result.winning_trades << "\n";
                std::cout << "패배 거래:   " << result.losing_trades << "\n";
                std::cout << "승률:        " << std::fixed << std::setprecision(2) << (result.win_rate * 100.0) << "%\n";
                std::cout << "평균 이익:   " << static_cast<long long>(result.avg_win_krw) << " KRW\n";
                std::cout << "평균 손실:   " << static_cast<long long>(result.avg_loss_krw) << " KRW\n";
                std::cout << "Profit Factor: " << std::setprecision(3) << result.profit_factor << "\n";
                std::cout << "Expectancy:  " << static_cast<long long>(result.expectancy_krw) << " KRW/trade\n";
                if (!result.strategy_summaries.empty()) {
                    std::cout << "전략별 요약:\n";
                    for (const auto& s : result.strategy_summaries) {
                        std::cout << "  - " << s.strategy_name
                                  << " | trades=" << s.total_trades
                                  << " | win=" << std::fixed << std::setprecision(1) << (s.win_rate * 100.0) << "%"
                                  << " | pnl=" << static_cast<long long>(s.total_profit)
                                  << " | pf=" << std::setprecision(3) << s.profit_factor << "\n";
                    }
                }
                std::cout << "---------------------------------------------\n";
                return 0;
            }
        }

        std::cout << "모드를 선택하세요\n";
        std::cout << "  [1] 실거래 (Live Trading)\n";
        std::cout << "  [2] 백테스트 (Backtest)\n";
        std::cout << "선택: ";

        std::string mode_input = readLine();
        int mode_choice = 0;
        try { mode_choice = std::stoi(mode_input); } catch (...) {}

        if (mode_choice == 2) {
            std::cout << "\n[백테스트 설정]\n";

            double bt_capital = readDouble("초기 자본금(KRW)", 1000000.0);
            std::cout << "데이터 소스 [1=모의 생성, 2=기존 CSV 입력, 3=실데이터 목록 선택] [기본값: 3]: ";
            std::string source_input = readLine();
            int source_choice = 3;
            int bt_candles = 0;
            bool require_higher_tf_companions = false;
            try {
                if (!source_input.empty()) {
                    source_choice = std::stoi(source_input);
                }
            } catch (...) {
                source_choice = 3;
            }

            std::string csv_path;
            if (source_choice == 3) {
                require_higher_tf_companions = readYesNo(
                    "실거래 동등 MTF 모드로 실행할까요? (1m + 5m/60m/240m companion 강제)",
                    true
                );
                auto candidates = listRealDataPrimaryCsvs(require_higher_tf_companions);
                if (candidates.empty()) {
                    std::cout << "선택 가능한 실데이터 1m CSV가 없습니다.\n";
                    std::cout << "경로: data/backtest_real\n";
                    std::cout << "필요 파일 예: upbit_KRW_BTC_1m_12000.csv (+ 5m/60m/240m companion)\n";
                    return 1;
                }

                std::cout << "\n실데이터 후보 목록\n";
                for (size_t idx = 0; idx < candidates.size(); ++idx) {
                    std::cout << "  [" << (idx + 1) << "] " << candidates[idx] << "\n";
                }
                int selected = readInt("실데이터 번호 선택", 1);
                selected = std::clamp(selected, 1, static_cast<int>(candidates.size()));
                csv_path = candidates[static_cast<size_t>(selected - 1)];
                std::cout << "선택된 실데이터 CSV: " << csv_path << "\n\n";
            } else if (source_choice == 2) {
                std::string default_csv = "data/backtest_real/upbit_KRW_BTC_1m_12000.csv";
                std::cout << "백테스트 CSV 경로 [기본값: " << default_csv << "]: ";
                std::string input_csv = readLine();
                csv_path = input_csv.empty() ? default_csv : input_csv;

                if (!std::filesystem::exists(csv_path)) {
                    std::cout << "CSV 파일을 찾을 수 없습니다: " << csv_path << "\n";
                    return 1;
                }
                require_higher_tf_companions = readYesNo(
                    "실거래 동등 MTF 모드로 실행할까요? (1m + 5m/60m/240m companion 강제)",
                    true
                );
                std::cout << "실데이터 CSV 사용: " << csv_path << "\n\n";
            } else {
                bt_candles = readInt("시뮬레이션 캔들 수 (예: 500/1000/2000)", 2000);
                double bt_start_price = readDouble("시작 가격 (예: 50000000 = BTC 5천만원)", 50000000.0);

                std::cout << "\n모의 데이터 생성 중...\n";
                csv_path = generateSimulationCSV(bt_candles, bt_start_price);
                if (csv_path.empty()) {
                    std::cout << "데이터 생성 실패\n";
                    return 1;
                }
                std::cout << "생성 완료: " << csv_path << " (" << bt_candles << "개 캔들)\n\n";
            }

            if (require_higher_tf_companions) {
                const auto check = checkHigherTfCompanions(csv_path);
                if (!check.applicable || !check.missing_tokens.empty()) {
                    printCompanionRequirementError(csv_path, check);
                    return 1;
                }
                std::cout << "MTF companion 검증 통과: 5m/60m/240m\n\n";
            }

            config.setInitialCapital(bt_capital);

            std::cout << "백테스트 실행 중...\n\n";
            if (source_choice == 2) {
                LOG_INFO("Interactive Backtest: csv={}, capital={:.0f}", csv_path, bt_capital);
            } else {
                LOG_INFO("Interactive Backtest: {} candles, capital={:.0f}", bt_candles, bt_capital);
            }

            backtest::BacktestEngine bt_engine;
            bt_engine.init(config);
            bt_engine.loadData(csv_path);
            bt_engine.run();

            auto result = bt_engine.getResult();
            double profit_pct = (bt_capital > 0) ? (result.total_profit / bt_capital * 100.0) : 0.0;

            std::cout << "백테스트 결과\n";
            std::cout << "---------------------------------------------\n";
            std::cout << "초기 자본:   " << static_cast<long long>(bt_capital) << " KRW\n";
            std::cout << "최종 잔고:   " << static_cast<long long>(result.final_balance) << " KRW\n";
            std::cout << "총 수익:     " << static_cast<long long>(result.total_profit) << " KRW\n";
            std::cout << "수익률:      " << std::fixed << std::setprecision(2) << profit_pct << "%\n";
            std::cout << "MDD:         " << std::setprecision(3) << (result.max_drawdown * 100.0) << "%\n";
            std::cout << "총 거래 수:  " << result.total_trades << "\n";
            std::cout << "승리 거래:   " << result.winning_trades << "\n";
            std::cout << "패배 거래:   " << result.losing_trades << "\n";
            std::cout << "승률:        " << std::setprecision(2) << (result.win_rate * 100.0) << "%\n";
            std::cout << "평균 이익:   " << static_cast<long long>(result.avg_win_krw) << " KRW\n";
            std::cout << "평균 손실:   " << static_cast<long long>(result.avg_loss_krw) << " KRW\n";
            std::cout << "Profit Factor: " << std::setprecision(3) << result.profit_factor << "\n";
            std::cout << "Expectancy:  " << static_cast<long long>(result.expectancy_krw) << " KRW/trade\n";
            if (!result.strategy_summaries.empty()) {
                std::cout << "전략별 요약:\n";
                for (const auto& s : result.strategy_summaries) {
                    std::cout << "  - " << s.strategy_name
                              << " | trades=" << s.total_trades
                              << " | win=" << std::fixed << std::setprecision(1) << (s.win_rate * 100.0) << "%"
                              << " | pnl=" << static_cast<long long>(s.total_profit)
                              << " | pf=" << std::setprecision(3) << s.profit_factor << "\n";
                }
            }
            std::cout << "---------------------------------------------\n\n";

            std::cout << "엔터를 누르면 종료합니다.";
            std::cin.get();
            return 0;
        }

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

        g_engine = std::make_unique<engine::TradingEngine>(engine_config, http_client);
        std::signal(SIGINT, signalHandler);

        std::cout << "\n거래 엔진을 시작합니다.\n";
        std::cout << "중지하려면 Ctrl+C를 누르세요.\n\n";

        if (!g_engine->start()) {
            LOG_ERROR("엔진 시작 실패");
            std::cout << "엔진 시작에 실패했습니다.\n";
            std::cin.get();
            return 1;
        }

        if (engine_config.mode == engine::TradingMode::LIVE) {
            auto metrics = g_engine->getMetrics();
            std::cout << "초기화 완료\n";
            std::cout << "보유 자산: " << static_cast<long long>(metrics.total_capital) << " KRW\n\n";
        }

        while (g_engine->isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::cout << "\n프로그램이 종료됩니다.\n";
        LOG_INFO("Program terminated");
        return 0;
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: {}", e.what());
        std::cout << "\n오류가 발생했습니다: " << e.what() << std::endl;
        std::cout << "엔터를 누르면 종료합니다." << std::endl;
        std::cin.get();
        return 1;
    }
}
