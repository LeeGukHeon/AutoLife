#include "common/Config.h"
#include "common/Logger.h"
#include <iostream>
#include <cassert>
#include <filesystem>

// Simple manual test runner
int main() {
    using namespace autolife;

    // Setup logger (console only)
    spdlog::set_level(spdlog::level::debug);

    std::cout << "[TEST] Starting Config Test..." << std::endl;

    // 1. Get Instance
    Config& config = Config::getInstance();
    
    // 2. Load Config (Assumes config/config.json exists relative to CWD)
    // We might need to point to the right path if verifying defaults vs loaded
    // For now, let's just check defaults or loaded values if file exists.
    
    // Check Defaults (before load or if load fails/empty)
    // Actually Config loads on demand or we call load().
    // main.cpp calls load. We should call load.
    // Assuming we run this from build/Release where config/ exists.
    
    std::string config_path = "config/config.json";
    if (std::filesystem::exists(config_path)) {
        std::cout << "[TEST] Found config.json, loading..." << std::endl;
        config.load(config_path);
    } else {
        std::cout << "[TEST] config.json not found, constructing default..." << std::endl;
        // Default constructor was already called for singleton
    }

    // 3. Verify Centralized Constants
    std::cout << "Fee Rate: " << config.getFeeRate() << std::endl;
    std::cout << "Min Order: " << config.getMinOrderKrw() << std::endl;
    std::cout << "Slippage: " << config.getMaxSlippagePct() << std::endl;

    // Assertions (Adjust based on what you expect in config.json or defaults)
    // Upbit default fee is 0.05% -> 0.0005
    assert(std::abs(config.getFeeRate() - 0.0005) < 1e-9);
    assert(config.getMinOrderKrw() >= 5000.0);

    // 4. Verify Engine Config (foundation-first runtime path)
    const auto engine_cfg = config.getEngineConfig();
    std::cout << "Enabled Strategies: " << engine_cfg.enabled_strategies.size() << std::endl;
    std::cout << "Min Expected Edge: " << engine_cfg.min_expected_edge_pct << std::endl;
    std::cout << "Min Reward/Risk: " << engine_cfg.min_reward_risk << std::endl;

    // Assertions
    assert(engine_cfg.max_positions > 0);
    assert(engine_cfg.min_expected_edge_pct >= 0.0);
    assert(engine_cfg.min_reward_risk >= 1.0);

    std::cout << "[TEST] Config Test PASSED!" << std::endl;

    return 0;
}
