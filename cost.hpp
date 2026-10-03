#pragma once

#include "SJSON/src/value.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cost {
    // Validated counts, independent of the API's JSON field names.
    // Output includes reasoning tokens; do not count them separately.
    struct TokenUsage {
        std::uint64_t cache_hit_tokens;
        std::uint64_t cache_miss_tokens;
        std::uint64_t output_tokens;
        // Tokens written to cache, a subset of cache_miss_tokens that pricing
        // charges at the miss rate. Chat Completions reports no separate count.
        std::uint64_t cache_creation_tokens;
    };

    // Share of input tokens served from cache: cache reads over total input
    // (reads, uncached input, and cache writes). Zero when no input is reported.
    inline double cache_hit_rate(const TokenUsage& usage) {
        double reads = static_cast<double>(usage.cache_hit_tokens);
        double total = reads + static_cast<double>(usage.cache_miss_tokens);
        return total > 0 ? reads / total : 0.0;
    }

    namespace detail {
        inline std::optional<std::uint64_t> token_count(const SJSON::JSObject& usage, std::string_view field) {
            auto it = usage.find(std::string(field));
            if (it == usage.end()) return 0;
            if (!it->second.is_number()) return std::nullopt;
            auto count = it->second.number();
            if (!std::isfinite(count) || count < 0 || count > 9007199254740991.0 || std::trunc(count) != count) {
                return std::nullopt;
            }
            return static_cast<std::uint64_t>(count);
        }

        struct Prices {
            std::uint64_t cache_hit;
            std::uint64_t cache_miss;
            std::uint64_t completion;
        };

        // The published rates apply to these models from their launch/pricing-change dates.
        // Sources: https://api-docs.deepseek.com/quick_start/pricing/
        // https://api-docs.deepseek.com/news/news260910/
        // https://api-docs.deepseek.com/news/news260813/
        // USD per token, in billionths of a dollar; checked September 27, 2026.
        inline std::optional<Prices> deepseek_off_peak_prices(std::string_view model, std::chrono::sys_seconds at) {
            using namespace std::chrono;
            if (model == "deepseek-flash" && at >= sys_days {2026y / September / 10} + 4h) {
                return Prices {3, 150, 600};
            }
            if (model == "deepseek-v4-pro" && at >= sys_days {2026y / August / 16} + 16h) {
                return Prices {22, 660, 1980};
            }
            return std::nullopt;
        }

        // National days off in China's published 2026 holiday calendar. Weekend dates
        // need no separate entry. Make-up working weekends remain off-peak.
        // https://www.gov.cn/zhengce/zhengceku/202511/content_7047091.htm
        inline bool chinese_public_holiday_2026(std::chrono::year_month_day day) {
            using namespace std::chrono;
            auto holiday = [day](unsigned month, unsigned first, unsigned last) {
                return day.month() == std::chrono::month {month}
                    && unsigned(day.day()) >= first && unsigned(day.day()) <= last;
            };
            return holiday(1, 1, 3) || holiday(2, 15, 23) || holiday(4, 4, 6)
                || holiday(5, 1, 5) || holiday(6, 19, 21) || holiday(9, 25, 27)
                || holiday(10, 1, 7);
        }

        inline std::optional<bool> deepseek_peak(std::chrono::sys_seconds at) {
            using namespace std::chrono;
            // Peak hours are Mon-Fri, 01:00-04:00 and 06:00-10:00 UTC,
            // excluding Chinese public holidays (determined in Beijing time).
            sys_days beijing_day = floor<days>(at + 8h);
            year_month_day date {beijing_day};
            if (date.year() != 2026y) return std::nullopt; // No holiday calendar for other years.
            int hour_utc = duration_cast<hours>(at - floor<days>(at)).count();
            return weekday {beijing_day}.iso_encoding() <= 5
                && !chinese_public_holiday_2026(date)
                && ((hour_utc >= 1 && hour_utc < 4) || (hour_utc >= 6 && hour_utc < 10));
        }

        inline std::optional<std::uint64_t> deepseek(std::string_view model, const TokenUsage& usage, std::chrono::sys_seconds at) {
            auto prices = deepseek_off_peak_prices(model, at);
            auto peak = deepseek_peak(at);
            if (!prices || !peak) return std::nullopt;

            std::uint64_t total = 0;
            auto add_tokens = [&total](std::uint64_t tokens, std::uint64_t rate) {
                if (tokens > (std::numeric_limits<std::uint64_t>::max() - total) / rate) return false;
                total += tokens * rate;
                return true;
            };
            if (!add_tokens(usage.cache_hit_tokens, prices->cache_hit)
                || !add_tokens(usage.cache_miss_tokens, prices->cache_miss)
                || !add_tokens(usage.output_tokens, prices->completion)) {
                return std::nullopt;
            }
            if (*peak) {
                if (total > std::numeric_limits<std::uint64_t>::max() / 2) return std::nullopt;
                total *= 2;
            }
            return total;
        }
    } // namespace detail

    // Convert only this snapshot's reported counts; missing fields become zero.
    // DeepSeek Chat Completions: prompt_tokens includes both cache buckets, and
    // there is no separate cache-write count.
    inline std::optional<TokenUsage> from_chat_completions_usage(const SJSON::JSObject& usage) {
        auto input = detail::token_count(usage, "prompt_tokens");
        auto hits = detail::token_count(usage, "prompt_cache_hit_tokens");
        auto misses = detail::token_count(usage, "prompt_cache_miss_tokens");
        auto output = detail::token_count(usage, "completion_tokens");
        if (!input || !hits || !misses || !output) return std::nullopt;
        if (usage.contains("prompt_tokens")) {
            if (usage.contains("prompt_cache_hit_tokens") && *hits > *input) return std::nullopt;
            if (usage.contains("prompt_cache_miss_tokens") && *misses > *input) return std::nullopt;
            if (usage.contains("prompt_cache_hit_tokens") && usage.contains("prompt_cache_miss_tokens")
                && *misses != *input - *hits) {
                return std::nullopt;
            }
        }
        return TokenUsage {*hits, *misses, *output, 0};
    }

    // Accepts raw usage from any lifecycle point; missing fields become zero.
    // No merging or accumulation is performed, and delta counts remain cumulative.
    // Messages input_tokens excludes both cache buckets. For DeepSeek pricing,
    // newly cached input belongs in the ordinary cache-miss bucket; the write
    // count is kept alongside for reporting.
    inline std::optional<TokenUsage> from_anthropic_usage(const SJSON::JSObject& usage) {
        auto input = detail::token_count(usage, "input_tokens");
        auto hits = detail::token_count(usage, "cache_read_input_tokens");
        auto created = detail::token_count(usage, "cache_creation_input_tokens");
        auto output = detail::token_count(usage, "output_tokens");
        if (!input || !hits || !created || !output
            || *created > std::numeric_limits<std::uint64_t>::max() - *input) {
            return std::nullopt;
        }
        return TokenUsage {*hits, *input + *created, *output, *created};
    }

    // Time is UTC. Add other services in this dispatch without changing callers.
    // Returns USD nanodollars: 1'000'000'000 = $1. Zero is a valid cost.
    // Empty result means the model, usage, or pricing period cannot be priced reliably.
    inline std::optional<std::uint64_t> calculate(std::string_view service, std::string_view model,
        const TokenUsage& usage, std::chrono::system_clock::time_point at = std::chrono::system_clock::now()) {
        if (service == "deepseek") {
            return detail::deepseek(model, usage, std::chrono::floor<std::chrono::seconds>(at));
        }
        return std::nullopt;
    }
} // namespace cost
