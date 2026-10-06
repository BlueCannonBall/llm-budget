#pragma once

#include "SJSON/src/value.hpp"
#include "providers.hpp"
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <stdint.h>
#include <string_view>

namespace cost {
    // Output includes reasoning tokens; do not count them separately.
    struct TokenUsage {
        uint64_t cache_hit_tokens;
        uint64_t cache_miss_tokens;
        uint64_t output_tokens;
        // Cache writes are included in cache_miss_tokens; Chat Completions reports zero.
        uint64_t cache_creation_tokens;
    };

    struct Multiplier {
        uint64_t numerator;
        uint64_t denominator;
    };

    // Input cache-read fraction, including cache writes in total input.
    inline double cache_hit_rate(const TokenUsage& usage) {
        double reads = (double) usage.cache_hit_tokens;
        double total = reads + (double) usage.cache_miss_tokens;
        return total > 0 ? reads / total : 0.0;
    }

    namespace detail {
        inline const SJSON::JSValue* field_value(const SJSON::JSObject& usage, std::string_view field) {
            // JSObject has no transparent lookup; avoid temporary string allocation.
            for (const auto& [name, value] : usage) {
                if (name == field) return &value;
            }
            return nullptr;
        }

        inline std::optional<uint64_t> token_count(const SJSON::JSObject& usage, std::string_view field) {
            auto value = field_value(usage, field);
            if (!value) return 0;
            if (!value->is_number()) return std::nullopt;

            double count = value->number();
            if (!std::isfinite(count) || count < 0 || count > 9007199254740991.0 || std::trunc(count) != count) {
                return std::nullopt;
            }

            return (uint64_t) count;
        }

        // National days off in China's published 2026 holiday calendar. Weekend dates
        // need no separate entry. Make-up working weekends remain off-peak.
        // https://www.gov.cn/zhengce/zhengceku/202511/content_7047091.htm
        inline bool chinese_public_holiday_2026(std::chrono::year_month_day day) {
            using namespace std::chrono;

            auto holiday = [day](unsigned month, unsigned first, unsigned last) {
                return day.month() == std::chrono::month {month} && unsigned(day.day()) >= first && unsigned(day.day()) <= last;
            };

            return holiday(1, 1, 3) || holiday(2, 15, 23) || holiday(4, 4, 6) || holiday(5, 1, 5) || holiday(6, 19, 21) || holiday(9, 25, 27) || holiday(10, 1, 7);
        }

        inline std::optional<bool> deepseek_peak(std::chrono::sys_seconds at) {
            using namespace std::chrono;
            // Peak hours are Mon-Fri, 01:00-04:00 and 06:00-10:00 UTC,
            // excluding Chinese public holidays (determined in Beijing time).
            sys_days beijing_day = floor<days>(at + 8h);
            year_month_day date {beijing_day};
            if (date.year() != 2026y) return std::nullopt; // No holiday calendar for other years.

            int hour_utc = duration_cast<hours>(at - floor<days>(at)).count();
            return weekday {beijing_day}.iso_encoding() <= 5 && !chinese_public_holiday_2026(date) && ((hour_utc >= 1 && hour_utc < 4) || (hour_utc >= 6 && hour_utc < 10));
        }

        inline bool weekday_utc_peak(std::chrono::sys_seconds at) {
            using namespace std::chrono;
            auto day = floor<days>(at);
            int hour = duration_cast<hours>(at - day).count();

            return weekday {day}.iso_encoding() <= 5 && ((hour >= 1 && hour < 4) || (hour >= 6 && hour < 10));
        }

        typedef unsigned __int128 uint128_t;

        inline bool add_product(uint128_t& total, uint64_t tokens, uint64_t rate) {
            uint128_t product = (uint128_t) tokens * rate;
            if (product > ~(uint128_t) 0 - total) return false;

            total += product;
            return true;
        }

        inline std::optional<uint64_t> ceil_ratio(uint128_t numerator, uint128_t denominator) {
            if (denominator == 0) return std::nullopt;

            uint128_t result = numerator / denominator + (numerator % denominator != 0);
            if (result > std::numeric_limits<uint64_t>::max()) return std::nullopt;

            return (uint64_t) result;
        }
    } // namespace detail

    // prompt_tokens includes both cache buckets. Nested cached_tokens and explicit
    // cache counts must agree when both are present.
    inline std::optional<TokenUsage> from_chat_completions_usage(const SJSON::JSObject& usage) {
        auto input = detail::token_count(usage, "prompt_tokens");
        auto hits = detail::token_count(usage, "prompt_cache_hit_tokens");
        auto misses = detail::token_count(usage, "prompt_cache_miss_tokens");
        auto output = detail::token_count(usage, "completion_tokens");
        if (!input || !hits || !misses || !output) return std::nullopt;

        bool have_hits = detail::field_value(usage, "prompt_cache_hit_tokens") != nullptr;
        bool have_misses = detail::field_value(usage, "prompt_cache_miss_tokens") != nullptr;
        if (auto details = detail::field_value(usage, "prompt_tokens_details"); details && !details->is_null()) {
            if (!details->is_object()) return std::nullopt;

            auto cached = detail::token_count(details->object(), "cached_tokens");
            if (!cached) return std::nullopt;

            if (detail::field_value(details->object(), "cached_tokens")) {
                if (have_hits && *hits != *cached) return std::nullopt;
                hits = cached;
                have_hits = true;
            }
        }

        if (detail::field_value(usage, "prompt_tokens")) {
            if (*hits > *input || *misses > *input) return std::nullopt;

            if (!have_hits && have_misses) hits = *input - *misses;
            if (have_misses && *misses != *input - *hits) return std::nullopt;
            misses = *input - *hits;
        }

        return TokenUsage {*hits, *misses, *output, 0};
    }

    // Missing fields become zero. Input excludes cache reads and writes; retain
    // writes in cache_miss_tokens and separate them when applying rates.
    inline std::optional<TokenUsage> from_anthropic_usage(const SJSON::JSObject& usage) {
        auto input = detail::token_count(usage, "input_tokens");
        auto hits = detail::token_count(usage, "cache_read_input_tokens");
        auto created = detail::token_count(usage, "cache_creation_input_tokens");
        auto output = detail::token_count(usage, "output_tokens");
        if (!input || !hits || !created || !output || *created > std::numeric_limits<uint64_t>::max() - *input) {
            return std::nullopt;
        }

        return TokenUsage {*hits, *input + *created, *output, *created};
    }

    // Apply the exact multiplier, then round picodollars up to nanodollars once.
    // Invalid usage, unavailable schedules and overflow return no estimate.
    inline std::optional<uint64_t> calculate(const providers::Model& model, const TokenUsage& usage, std::chrono::system_clock::time_point at, Multiplier multiplier) {
        if (multiplier.denominator == 0 || usage.cache_creation_tokens > usage.cache_miss_tokens || usage.cache_hit_tokens > std::numeric_limits<uint64_t>::max() - usage.cache_miss_tokens) {
            return std::nullopt;
        }

        auto seconds = std::chrono::floor<std::chrono::seconds>(at);

        uint64_t input = usage.cache_hit_tokens + usage.cache_miss_tokens;
        const providers::Rates* rates = &model.rates;
        if (model.input_token_threshold && input > model.input_token_threshold) {
            rates = &model.above_threshold_rates;
        }

        detail::uint128_t total = 0;
        if (!detail::add_product(total, usage.cache_miss_tokens - usage.cache_creation_tokens, rates->input) || !detail::add_product(total, usage.cache_hit_tokens, rates->cached_read) || !detail::add_product(total, usage.output_tokens, rates->output) || !detail::add_product(total, usage.cache_creation_tokens, rates->cached_write)) {
            return std::nullopt;
        }

        bool peak = false;
        switch (model.schedule) {
        case providers::SCHEDULE_FLAT:
            break;

        case providers::SCHEDULE_DEEPSEEK_2026: {
            auto scheduled = detail::deepseek_peak(seconds);
            if (!scheduled) return std::nullopt;

            peak = *scheduled;
            break;
        }

        case providers::SCHEDULE_WEEKDAY_UTC_DOUBLED:
            peak = detail::weekday_utc_peak(seconds);
            break;
        }

        if (peak) {
            if (total > ~(detail::uint128_t) 0 / 2) return std::nullopt;

            total *= 2;
        }

        if (multiplier.numerator == 0) return 0;
        if (multiplier.numerator != 1 && total > ~(detail::uint128_t) 0 / multiplier.numerator) return std::nullopt;

        return detail::ceil_ratio(total * multiplier.numerator, (detail::uint128_t) 1000 * multiplier.denominator);
    }
} // namespace cost
