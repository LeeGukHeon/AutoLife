#include "app/BacktestInteractiveHandler.h"

#include "app/BacktestReportFormatter.h"
#include "common/Logger.h"
#include "runtime/BacktestRuntime.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

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

    for (const auto& token : {"5m", "15m", "60m", "240m"}) {
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
        std::cout << "  companion(5m/15m/60m/240m) 자동 매칭이 가능한 1m 파일을 지정하세요.\n";
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
        std::cout << "  같은 폴더에 upbit_<market>_5m_*.csv / 15m / 60m / 240m 파일이 필요합니다.\n";
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

}  // namespace

int runInteractiveBacktest(Config& config) {
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
            "실거래 동등 MTF 모드로 실행할까요? (1m + 5m/15m/60m/240m companion 강제)",
            true
        );
        auto candidates = listRealDataPrimaryCsvs(require_higher_tf_companions);
        if (candidates.empty()) {
            std::cout << "선택 가능한 실데이터 1m CSV가 없습니다.\n";
            std::cout << "경로: data/backtest_real\n";
            std::cout << "필요 파일 예: upbit_KRW_BTC_1m_12000.csv (+ 5m/15m/60m/240m companion)\n";
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
            "실거래 동등 MTF 모드로 실행할까요? (1m + 5m/15m/60m/240m companion 강제)",
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
        std::cout << "MTF companion 검증 통과: 5m/15m/60m/240m\n\n";
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

    app::BacktestSummaryOptions summary_options;
    summary_options.include_initial_capital = true;
    summary_options.initial_capital_krw = bt_capital;
    summary_options.include_profit_rate = true;
    summary_options.include_trailing_blank_line = true;
    app::printBacktestResultSummary(result, summary_options, std::cout);

    std::cout << "엔터를 누르면 종료합니다.";
    std::cin.get();
    return 0;
}

}  // namespace autolife::app
