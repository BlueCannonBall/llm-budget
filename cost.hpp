#pragma once

#include "SJSON/src/value.hpp"
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

// DeepSeek prices published at https://api-docs.deepseek.com/quick_start/pricing/
// on September 27, 2026. Units are billionths of a USD per token.
// Check the published prices again before relying on this estimate for a budget.
struct DeepSeekPrices {
    std::uint64_t cache_hit;
    std::uint64_t cache_miss;
    std::uint64_t completion;
};

struct DeepSeekCostRange {
    std::uint64_t off_peak_nano_usd;
    std::uint64_t peak_nano_usd;
};

inline std::optional<DeepSeekPrices> deepseek_off_peak_prices(std::string_view model) {
    if (model == "deepseek-flash") return DeepSeekPrices {3, 150, 600};
    if (model == "deepseek-v4-pro") return DeepSeekPrices {22, 660, 1980};
    return std::nullopt;
}

inline std::optional<std::uint64_t> deepseek_token_count(const SJSON::JSObject& usage, std::string_view field) {
    auto it = usage.find(std::string(field));
    if (it == usage.end() || !it->second.is_number()) return std::nullopt;
    double count = it->second.number();
    // SJSON stores JSON numbers as doubles. Above 2^53 - 1, integer counts
    // cannot be represented exactly and must not be used for accounting.
    if (!std::isfinite(count) || count < 0 || count > 9007199254740991.0 || std::trunc(count) != count) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(count);
}

inline std::optional<DeepSeekCostRange> deepseek_cost_range(const SJSON::JSObject& usage, std::string_view model) {
    auto prices = deepseek_off_peak_prices(model);
    auto hits = deepseek_token_count(usage, "prompt_cache_hit_tokens");
    auto misses = deepseek_token_count(usage, "prompt_cache_miss_tokens");
    auto completions = deepseek_token_count(usage, "completion_tokens");
    auto prompts = deepseek_token_count(usage, "prompt_tokens");
    if (!prices || !hits || !misses || !completions || !prompts || *hits > *prompts || *misses != *prompts - *hits) {
        return std::nullopt;
    }

    std::uint64_t total = 0;
    auto add_tokens = [&total](std::uint64_t tokens, std::uint64_t rate) {
        if (tokens > (std::numeric_limits<std::uint64_t>::max() - total) / rate) return false;
        total += tokens * rate;
        return true;
    };
    if (!add_tokens(*hits, prices->cache_hit) || !add_tokens(*misses, prices->cache_miss) || !add_tokens(*completions, prices->completion)) {
        return std::nullopt;
    }
    if (total > std::numeric_limits<std::uint64_t>::max() / 2) return std::nullopt;
    // Published peak prices are twice the off-peak prices for these models.
    return DeepSeekCostRange {total, total * 2};
}
