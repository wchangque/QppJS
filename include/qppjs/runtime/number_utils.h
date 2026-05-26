#pragma once

#include <cmath>
#include <cstdint>

namespace qppjs {

// ToInt32 — IEEE 754 double → int32_t（规范 7.1.7）
// 快路径：整数且在 [-2^31, 2^31) 范围内直接转换。
inline int32_t to_int32_bits(double d) {
    if (d >= -2147483648.0 && d < 2147483648.0 && d == std::trunc(d))
        return static_cast<int32_t>(static_cast<int64_t>(d));
    if (!std::isfinite(d) || d == 0.0) return 0;
    double t = std::trunc(d);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return static_cast<int32_t>(static_cast<uint32_t>(m));
}

// ToUint32 — IEEE 754 double → uint32_t（规范 7.1.8）
inline uint32_t to_uint32_bits(double d) {
    if (d >= 0.0 && d < 4294967296.0 && d == std::trunc(d))
        return static_cast<uint32_t>(d);
    if (!std::isfinite(d) || d == 0.0) return 0;
    double t = std::trunc(d);
    double m = std::fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return static_cast<uint32_t>(m);
}

}  // namespace qppjs
