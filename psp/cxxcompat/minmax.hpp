#pragma once

// Allegrex GCC exposes the project's i32 typedef as long while many original
// constants are int.  libstdc++'s same-type min/max templates reject that pair.
// This PSP-only bridge keeps engine source unchanged and promotes mixed integral
// operands to their common type.
#include <type_traits>

namespace std
{
template <typename A, typename B,
          typename R = typename common_type<A, B>::type,
          typename enable_if<!is_same<A, B>::value, int>::type = 0>
constexpr R min(A a, B b)
{
    return a < b ? static_cast<R>(a) : static_cast<R>(b);
}

template <typename A, typename B,
          typename R = typename common_type<A, B>::type,
          typename enable_if<!is_same<A, B>::value, int>::type = 0>
constexpr R max(A a, B b)
{
    return a > b ? static_cast<R>(a) : static_cast<R>(b);
}
} // namespace std
