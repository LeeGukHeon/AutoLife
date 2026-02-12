#pragma once

#include <string>
#include <vector>

namespace autolife {
namespace engine {

// 거래 모드
enum class TradingMode {
    LIVE,           // 실전 거래
    PAPER,          // 모의 거래 (주문 실행 안함)
    BACKTEST        // 백테스트
};

// 엔진 설정
// static constexpr double EXCHANGE_MIN_ORDER_KRW = 5000.0;  // See Config.h
struct EngineConfig {
    TradingMode mode;
    double initial_capital;
    
    // 스캔 설정
    int scan_interval_seconds;
    long long min_volume_krw;
    
    // 리스크 설정
    int max_positions;
    int max_daily_trades;
    double max_drawdown;
    double max_exposure_pct = 0.85;        // 총 자본 대비 허용 노출 비율 (기=85%)
    double max_daily_loss_pct = 0.05;      // 일 손실 한도 (포트폴리오 비율)
    double risk_per_trade_pct = 0.005;     // 트레이드당 위험 비율 (포트폴리오 비율)
    double max_slippage_pct = 0.003;       // 허용 최대 슬리피지 (0.3%)
    
    // 실전 거래 안전 설정
    double max_daily_loss_krw = 50000.0;    // 일일 최대 손실 5만원
    double max_order_krw = 500000.0;        // 단일 주문 최대 50만원
    double min_order_krw = 5000.0;          // 단일 주문 최소 5천원 (거래소 기준)
    bool dry_run = false;                    // 실전모드라도 실제 주문 안하고 로그만

    // 전략 설정
    std::vector<std::string> enabled_strategies;
    
    EngineConfig()
        : mode(TradingMode::PAPER)
        , initial_capital(50000)
        , scan_interval_seconds(60)
        , min_volume_krw(5000000000LL)
        , max_positions(10)
        , max_daily_trades(50)
        , max_drawdown(0.10)
    {}
};

} // namespace engine
} // namespace autolife
