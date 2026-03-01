#include "common/Config.h"
#include "common/Logger.h"
#include "runtime/LiveTradingRuntime.h"
#include "app/BacktestCliHandler.h"
#include "app/BacktestInteractiveHandler.h"
#include "app/LiveModeHandler.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

using namespace autolife;

// Global engine instance for Ctrl+C shutdown.
std::unique_ptr<engine::TradingEngine> g_engine;

namespace {

std::string trimCopy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowerCopy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string parseConfigPathArg(int argc, char* argv[]) {
    const std::string default_path = "config/config.json";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config-path" && i + 1 < argc) {
            return trimCopy(std::string(argv[++i]));
        }
        const std::string prefix = "--config-path=";
        if (arg.rfind(prefix, 0) == 0 && arg.size() > prefix.size()) {
            return trimCopy(arg.substr(prefix.size()));
        }
    }
    return default_path;
}

struct RuntimeBundleProvenance {
    std::string resolved_bundle_path;
    std::string prob_model_backend = "unknown";
    std::string lgbm_model_sha256;
    std::string warning;
};

RuntimeBundleProvenance loadRuntimeBundleProvenance(
    const engine::EngineConfig& engine_config,
    const std::string& config_path_raw) {
    RuntimeBundleProvenance out{};
    std::filesystem::path bundle_path =
        std::filesystem::path(engine_config.probabilistic_runtime_bundle_path);
    if (bundle_path.empty()) {
        out.warning = "bundle_path_missing";
        return out;
    }

    if (!bundle_path.is_absolute()) {
        const std::filesystem::path cwd_candidate = std::filesystem::absolute(bundle_path);
        if (std::filesystem::exists(cwd_candidate)) {
            bundle_path = cwd_candidate;
        } else {
            std::filesystem::path config_path = std::filesystem::path(config_path_raw);
            if (!config_path.empty()) {
                if (!config_path.is_absolute()) {
                    config_path = std::filesystem::absolute(config_path);
                }
                bundle_path = config_path.parent_path() / bundle_path;
            } else {
                bundle_path = cwd_candidate;
            }
        }
    }
    out.resolved_bundle_path = bundle_path.lexically_normal().string();

    try {
        std::ifstream fp(bundle_path);
        if (!fp.is_open()) {
            out.warning = "bundle_open_failed";
            return out;
        }

        nlohmann::json root;
        fp >> root;
        if (!root.is_object()) {
            out.warning = "bundle_json_invalid";
            return out;
        }

        out.prob_model_backend =
            lowerCopy(trimCopy(root.value("prob_model_backend", std::string("sgd"))));
        if (out.prob_model_backend != "sgd" && out.prob_model_backend != "lgbm") {
            out.prob_model_backend = "sgd";
        }
        if (out.prob_model_backend == "lgbm") {
            out.lgbm_model_sha256 =
                lowerCopy(trimCopy(root.value("lgbm_model_sha256", std::string{})));
        }
    } catch (const std::exception& e) {
        out.warning = std::string("bundle_read_exception:") + e.what();
    } catch (...) {
        out.warning = "bundle_read_exception_unknown";
    }

    return out;
}

}  // namespace

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

        const std::string config_path = parseConfigPathArg(argc, argv);
        Config::getInstance().load(config_path);
        auto& config = Config::getInstance();
        const auto bundle_provenance =
            loadRuntimeBundleProvenance(config.getEngineConfig(), config_path);
        LOG_INFO("Loaded config path: {}", config_path);
        LOG_INFO(
            "Bundle path: {}",
            bundle_provenance.resolved_bundle_path.empty()
                ? config.getEngineConfig().probabilistic_runtime_bundle_path
                : bundle_provenance.resolved_bundle_path
        );
        LOG_INFO("prob_model_backend: {}", bundle_provenance.prob_model_backend);
        if (bundle_provenance.prob_model_backend == "lgbm") {
            LOG_INFO("lgbm_model_sha256: {}", bundle_provenance.lgbm_model_sha256);
        }
        if (!bundle_provenance.warning.empty()) {
            LOG_WARN("Runtime bundle provenance warning: {}", bundle_provenance.warning);
        }

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
