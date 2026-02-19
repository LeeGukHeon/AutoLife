cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    elseif(EXISTS "D:/MyApps/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "D:/MyApps/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    endif()
endif()

project(AutoLifeTradingSystem VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/O2)
    endif()
endif()

find_package(CURL CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)

include_directories(${CMAKE_SOURCE_DIR}/include)

option(AUTOLIFE_BUILD_TOOL_BINARIES "Build optional operational tool binaries" OFF)
option(AUTOLIFE_BUILD_GATE_TESTS "Build core gate validation tests" OFF)
option(AUTOLIFE_BUILD_EXTRA_TESTS "Build extra diagnostic tests" OFF)

message(STATUS "AUTOLIFE_BUILD_TOOL_BINARIES=${AUTOLIFE_BUILD_TOOL_BINARIES}")
message(STATUS "AUTOLIFE_BUILD_GATE_TESTS=${AUTOLIFE_BUILD_GATE_TESTS}")
message(STATUS "AUTOLIFE_BUILD_EXTRA_TESTS=${AUTOLIFE_BUILD_EXTRA_TESTS}")

# Source files (shared library objects, excluding main entrypoint)
# Keep this list as the single compilation source of truth
set(LIB_SOURCES
    src/common/Logger.cpp
    src/common/Config.cpp
    src/common/PathUtils.cpp
    src/common/SignalPolicyShared.cpp
    src/common/RuntimeDiagnosticsShared.cpp
    src/common/StrategyEdgeStatsShared.cpp
    src/network/JwtGenerator.cpp
    src/network/UpbitHttpClient.cpp
    src/network/UpbitMyOrderWebSocketClient.cpp
    src/execution/RateLimiter.cpp
    src/analytics/MarketScanner.cpp
    src/analytics/TechnicalIndicators.cpp
    src/analytics/OrderbookAnalyzer.cpp
    src/strategy/StrategyManager.cpp
    src/strategy/FoundationRiskPipeline.cpp
    src/strategy/FoundationAdaptiveStrategy.cpp
    src/risk/RiskManager.cpp
    src/runtime/LiveTradingRuntime.cpp
    src/engine/AdaptivePolicyController.cpp
    src/engine/PerformanceStore.cpp
    src/execution/ExecutionPlaneAdapter.cpp
    src/engine/PolicyLearningPlaneAdapter.cpp
    src/risk/UpbitComplianceAdapter.cpp
    src/execution/OrderLifecycleStateMachine.cpp
    src/engine/TradingCycleCoordinator.cpp
    src/state/EventJournalJsonl.cpp
    src/state/LearningStateStoreJson.cpp
    src/backtest/DataHistory.cpp
    src/runtime/BacktestRuntime.cpp
    src/execution/OrderManager.cpp
    src/execution/OrderStateMapper.cpp
    src/analytics/RegimeDetector.cpp
)

add_executable(
    AutoLifeTrading
    src/main.cpp
    src/app/BacktestCliHandler.cpp
    src/app/BacktestInteractiveHandler.cpp
    src/app/BacktestReportFormatter.cpp
    src/app/LiveModeHandler.cpp
    ${LIB_SOURCES}
)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT AutoLifeTrading)

target_link_libraries(AutoLifeTrading PRIVATE
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Boost::system
    Boost::thread
)

target_compile_definitions(AutoLifeTrading PRIVATE
    _WIN32_WINNT=0x0A00
    NOMINMAX
)

if(WIN32)
    target_link_libraries(AutoLifeTrading PRIVATE ws2_32 wsock32)
endif()

if(AUTOLIFE_BUILD_TOOL_BINARIES)
    add_executable(
        AutoLifeCancelOrder
        src/tools/CancelOrder.cpp
        src/common/Config.cpp
        src/common/PathUtils.cpp
        src/common/Logger.cpp
        src/network/JwtGenerator.cpp
        src/network/UpbitHttpClient.cpp
        src/execution/RateLimiter.cpp
    )
    add_executable(
        AutoLifeLiveExecutionProbe
        src/tools/LiveExecutionProbe.cpp
        src/common/Config.cpp
        src/common/PathUtils.cpp
        src/common/Logger.cpp
        src/network/JwtGenerator.cpp
        src/network/UpbitHttpClient.cpp
        src/network/UpbitMyOrderWebSocketClient.cpp
        src/execution/RateLimiter.cpp
        src/execution/OrderManager.cpp
        src/execution/OrderStateMapper.cpp
        src/execution/OrderLifecycleStateMachine.cpp
    )
    add_executable(
        AutoLifeLiveQuotationProbe
        src/tools/LiveQuotationProbe.cpp
        src/common/Config.cpp
        src/common/PathUtils.cpp
        src/common/Logger.cpp
        src/network/JwtGenerator.cpp
        src/network/UpbitHttpClient.cpp
        src/execution/RateLimiter.cpp
    )

    target_compile_definitions(AutoLifeLiveExecutionProbe PRIVATE
        _WIN32_WINNT=0x0A00
        NOMINMAX
    )
    target_compile_definitions(AutoLifeLiveQuotationProbe PRIVATE
        _WIN32_WINNT=0x0A00
        NOMINMAX
    )

    target_link_libraries(AutoLifeCancelOrder PRIVATE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        spdlog::spdlog
    )
    target_link_libraries(AutoLifeLiveExecutionProbe PRIVATE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Boost::system
        Boost::thread
    )
    target_link_libraries(AutoLifeLiveQuotationProbe PRIVATE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        spdlog::spdlog
    )
    if(WIN32)
        target_link_libraries(AutoLifeCancelOrder PRIVATE ws2_32 wsock32)
        target_link_libraries(AutoLifeLiveExecutionProbe PRIVATE ws2_32 wsock32)
        target_link_libraries(AutoLifeLiveQuotationProbe PRIVATE ws2_32 wsock32)
    endif()
endif()

if(AUTOLIFE_BUILD_GATE_TESTS)
    # [Test] Config Unit Test
    add_executable(AutoLifeTest tests/TestConfig.cpp ${LIB_SOURCES})
    target_link_libraries(AutoLifeTest PRIVATE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Boost::system
        Boost::thread
    )
    if(WIN32)
        target_link_libraries(AutoLifeTest PRIVATE ws2_32 wsock32)
    endif()
    target_compile_definitions(AutoLifeTest PRIVATE
        _WIN32_WINNT=0x0A00
        NOMINMAX
    )

    add_executable(
        AutoLifeStateTest
        tests/TestOrderStateMapper.cpp
        src/execution/OrderStateMapper.cpp
        src/execution/OrderLifecycleStateMachine.cpp
    )
    target_link_libraries(AutoLifeStateTest PRIVATE nlohmann_json::nlohmann_json)

    add_executable(
        AutoLifeEventJournalTest
        tests/TestEventJournal.cpp
        src/state/EventJournalJsonl.cpp
    )
    target_link_libraries(AutoLifeEventJournalTest PRIVATE nlohmann_json::nlohmann_json)

    add_executable(
        AutoLifeExecutionStateTest
        tests/TestExecutionStateMachine.cpp
        src/execution/OrderLifecycleStateMachine.cpp
    )
    target_link_libraries(AutoLifeExecutionStateTest PRIVATE nlohmann_json::nlohmann_json)
endif()

if(AUTOLIFE_BUILD_EXTRA_TESTS)
    add_executable(
        AutoLifeRateLimiterComplianceTest
        tests/TestRateLimiterComplianceTelemetry.cpp
        src/execution/RateLimiter.cpp
        src/common/PathUtils.cpp
        src/common/Logger.cpp
    )
    target_link_libraries(
        AutoLifeRateLimiterComplianceTest
        PRIVATE
            nlohmann_json::nlohmann_json
            spdlog::spdlog
    )
    target_compile_definitions(AutoLifeRateLimiterComplianceTest PRIVATE
        _WIN32_WINNT=0x0A00
        NOMINMAX
    )
endif()

# ========================================
# Post-build asset copy
# ========================================

# Copy config directory
add_custom_command(TARGET AutoLifeTrading POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/config
    $<TARGET_FILE_DIR:AutoLifeTrading>/config
    COMMENT "Copying config files to build directory"
)

# Create logs directory
add_custom_command(TARGET AutoLifeTrading POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:AutoLifeTrading>/logs
    COMMENT "Creating logs directory"
)

# Copy README (optional)
if(EXISTS ${CMAKE_SOURCE_DIR}/README.md)
    add_custom_command(TARGET AutoLifeTrading POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/README.md
        $<TARGET_FILE_DIR:AutoLifeTrading>/README.md
        COMMENT "Copying README"
    )
endif()


