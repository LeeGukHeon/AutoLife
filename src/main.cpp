#include "common/Config.h"
#include "common/Logger.h"
#include "runtime/LiveTradingRuntime.h"
#include "app/BacktestCliHandler.h"
#include "app/BacktestInteractiveHandler.h"
#include "app/LiveModeHandler.h"

#include <Windows.h>

#include <csignal>
#include <iostream>
#include <memory>
#include <string>

using namespace autolife;

// Global engine instance for Ctrl+C shutdown.
std::unique_ptr<engine::TradingEngine> g_engine;

void signalHandler(int signal) {
    if (signal == SIGINT) {
        LOG_INFO("\n종료 신호 수신 (Ctrl+C)");
        if (g_engine) {
            g_engine->stop();
        }
    }
}

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

        {
            int cli_backtest_exit_code = 0;
            if (app::tryRunCliBacktest(argc, argv, config, cli_backtest_exit_code)) {
                return cli_backtest_exit_code;
            }
        }

        std::cout << "모드를 선택하세요\n";
        std::cout << "  [1] 실거래 (Live Trading)\n";
        std::cout << "  [2] 백테스트 (Backtest)\n";
        std::cout << "선택: ";

        std::string mode_input = readLine();
        int mode_choice = 0;
        try {
            mode_choice = std::stoi(mode_input);
        } catch (...) {
        }

        if (mode_choice != 1 && mode_choice != 2) {
            if (mode_input.empty()) {
                std::cout << "\n입력이 없어 안전 종료합니다.\n";
                std::cout << "실거래를 시작하려면 반드시 [1]을 명시적으로 입력하세요.\n";
                return 0;
            }
            std::cout << "\n잘못된 입력입니다. [1] 또는 [2]를 선택하세요.\n";
            return 1;
        }

        if (mode_choice == 2) {
            return app::runInteractiveBacktest(config);
        }

        return app::runInteractiveLiveMode(config, g_engine, signalHandler);
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: {}", e.what());
        std::cout << "\n오류가 발생했습니다: " << e.what() << std::endl;
        std::cout << "엔터를 누르면 종료합니다." << std::endl;
        std::cin.get();
        return 1;
    }
}
