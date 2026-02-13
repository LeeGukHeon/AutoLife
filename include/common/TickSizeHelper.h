#pragma once
// ===================================================================
// [Phase 3] 업비트 KRW 마켓 호가 단위 (Tick Size) 헬퍼
// 
// 업비트 원화 마켓에서는 가격 구간에 따라 호가 단위가 다르며,
// 잘못된 호가 단위로 주문하면 에러 발생 (invalid_parameter).
//
// 참고: https://docs.upbit.com/docs/market-info-trade-price-detail
// ===================================================================

#include <cmath>

namespace autolife {
namespace common {

// 업비트 KRW 마켓 호가 단위 테이블 (2024.10 기준)
// 
// | 가격 구간           | 호가 단위 |
// |---------------------|-----------|
// | 2,000,000 이상      | 1,000원   |
// | 1,000,000~2,000,000 | 500원     |
// | 500,000~1,000,000   | 100원     |
// | 100,000~500,000     | 50원      |
// | 10,000~100,000      | 10원      |
// | 1,000~10,000        | 1원       |
// | 100~1,000           | 1원       |
// | 10~100              | 0.01원    |
// | 1~10                | 0.001원   |

inline double getTickSize(double price) {
    if (price >= 2000000.0) return 1000.0;
    if (price >= 1000000.0) return 500.0;
    if (price >= 500000.0)  return 100.0;
    if (price >= 100000.0)  return 50.0;
    if (price >= 10000.0)   return 10.0;
    if (price >= 1000.0)    return 1.0;
    if (price >= 100.0)     return 1.0;
    if (price >= 10.0)      return 0.1;
    if (price >= 1.0)       return 0.01;
    return 0.0001;
}

// 매수용: 호가 단위로 올림 (체결 확률 높이기)
inline double roundUpToTickSize(double price) {
    double tick = getTickSize(price);
    if (tick < 0.0001) return price;
    return std::ceil(price / tick) * tick;
}

// 매도용: 호가 단위로 내림 (체결 확률 높이기)
inline double roundDownToTickSize(double price) {
    double tick = getTickSize(price);
    if (tick < 0.0001) return price;
    return std::floor(price / tick) * tick;
}

// 가장 가까운 호가로 반올림 (중립)
inline double roundToTickSize(double price) {
    double tick = getTickSize(price);
    if (tick < 0.0001) return price;
    return std::round(price / tick) * tick;
}

// 호가 단위에 맞는 가격 문자열 생성 (주문 전송용)
// 정수 호가이면 정수 문자열, 소수 호가이면 소수점 포함
inline std::string priceToString(double price) {
    double tick = getTickSize(price);
    
    if (tick >= 1.0) {
        // 정수 호가: "136523000"
        return std::to_string(static_cast<long long>(price));
    }
    
    // 소수 호가: 소수점 자릿수 결정
    int decimals = 0;
    double t = tick;
    while (t < 1.0 && decimals < 8) {
        t *= 10.0;
        decimals++;
    }
    
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, price);
    return std::string(buf);
}

} // namespace common
} // namespace autolife
