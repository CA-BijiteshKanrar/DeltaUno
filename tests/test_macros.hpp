#pragma once

#include <cmath>
#include <cstdlib>
#include <iostream>

// Release-safe test assertions that NEVER compile out under -DNDEBUG
#define DU_EXPECT(expr)                                                                         \
    do {                                                                                        \
        if (!(expr)) {                                                                          \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << #expr << "\n";  \
            std::exit(1);                                                                       \
        }                                                                                       \
    } while (0)

#define DU_EXPECT_NEAR(lhs, rhs, eps)                                                           \
    do {                                                                                        \
        const double _du_l = static_cast<double>(lhs);                                          \
        const double _du_r = static_cast<double>(rhs);                                          \
        if (std::abs(_du_l - _du_r) > static_cast<double>(eps)) {                               \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> |" << #lhs << " ("    \
                      << _du_l << ") - " << #rhs << " (" << _du_r << ")| > " << (eps) << "\n";  \
            std::exit(1);                                                                       \
        }                                                                                       \
    } while (0)
