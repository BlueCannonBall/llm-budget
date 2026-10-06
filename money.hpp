#pragma once

#include <charconv>
#include <limits>
#include <stddef.h>
#include <stdexcept>
#include <stdint.h>
#include <string_view>

inline uint64_t parse_nanodollars(std::string_view dollars) {
    size_t decimal = dollars.find('.');
    std::string_view whole_text = dollars.substr(0, decimal);
    std::string_view fraction_text;
    if (decimal != std::string_view::npos) fraction_text = dollars.substr(decimal + 1);
    if (whole_text.empty() || (decimal != std::string_view::npos && fraction_text.empty()) || fraction_text.size() > 9) {
        throw std::invalid_argument("Dollar amount must be nonnegative with at most nine decimal places");
    }

    uint64_t whole = 0;
    uint64_t fraction = 0;
    auto [whole_end, whole_error] = std::from_chars(whole_text.data(), whole_text.data() + whole_text.size(), whole);
    if (whole_error != std::errc {} || whole_end != whole_text.data() + whole_text.size()) {
        throw std::invalid_argument("Invalid dollar amount");
    }

    if (!fraction_text.empty()) {
        auto [fraction_end, fraction_error] = std::from_chars(fraction_text.data(), fraction_text.data() + fraction_text.size(), fraction);
        if (fraction_error != std::errc {} || fraction_end != fraction_text.data() + fraction_text.size()) {
            throw std::invalid_argument("Invalid dollar amount");
        }
    }

    for (size_t i = fraction_text.size(); i < 9; ++i) fraction *= 10;

    constexpr uint64_t nanos_per_dollar = 1'000'000'000;
    constexpr uint64_t maximum = (uint64_t) std::numeric_limits<int64_t>::max();
    if (whole > (maximum - fraction) / nanos_per_dollar) {
        throw std::out_of_range("Dollar amount is too large");
    }

    return whole * nanos_per_dollar + fraction;
}
