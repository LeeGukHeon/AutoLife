cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE "D:/MyApps/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

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

# 소스 파일 명시
set(SOURCES
    src/main.cpp
    src/common/Logger.cpp
    src/common/Config.cpp
    src/common/PathUtils.cpp
    src/network/JwtGenerator.cpp
    src/network/UpbitHttpClient.cpp
    src/execution/RateLimiter.cpp
    src/analytics/MarketScanner.cpp
    src/analytics/TechnicalIndicators.cpp
    src/analytics/OrderbookAnalyzer.cpp
    src/strategy/StrategyManager.cpp
    src/strategy/ScalpingStrategy.cpp
    src/strategy/MomentumStrategy.cpp
    src/strategy/BreakoutStrategy.cpp
    src/strategy/MeanReversionStrategy.cpp
    src/strategy/GridTradingStrategy.cpp
    src/risk/RiskManager.cpp
    src/engine/TradingEngine.cpp
)

add_executable(AutoLifeTrading ${SOURCES})

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

# ========================================
# 빌드 후 리소스 파일 자동 복사
# ========================================

# config 폴더 복사
add_custom_command(TARGET AutoLifeTrading POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/config
    $<TARGET_FILE_DIR:AutoLifeTrading>/config
    COMMENT "Copying config files to build directory"
)

# logs 폴더 생성
add_custom_command(TARGET AutoLifeTrading POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:AutoLifeTrading>/logs
    COMMENT "Creating logs directory"
)

# README 복사 (선택)
if(EXISTS ${CMAKE_SOURCE_DIR}/README.md)
    add_custom_command(TARGET AutoLifeTrading POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/README.md
        $<TARGET_FILE_DIR:AutoLifeTrading>/README.md
        COMMENT "Copying README"
    )
endif()