#pragma once

#include "SJSON/src/value.hpp"
#include "provider_types.hpp"
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
        // Cache writes are included in cache_miss_tokens, not added to total input.
        uint64_t cache_creation_tokens;
    };

    // The first unusable usage field, reported by the parsers below.
    struct UsageError {
        std::string_view field;
        std::string_view reason;
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
        // Reads one token counter. An absent counter, or a malformed one, yields
        // nothing and records the first malformed field; the caller keeps the
        // counters it could read.
        inline std::optional<uint64_t> token_count(const SJSON::JSObject& usage, std::string_view field, UsageError* warning = nullptr, std::string_view path = {}) {
            if (path.empty()) path = field;

            auto value = usage.find(std::string(field));
            if (value == usage.end()) return std::nullopt;
            // Optional cache-write metadata can explicitly report no count.
            if (value->second.is_null() && field == "cache_write_tokens") return 0;

            std::string_view defect;
            if (!value->second.is_number()) {
                defect = "expected_number";
            } else {
                double count = value->second.number();
                if (!std::isfinite(count)) {
                    defect = "nonfinite_count";
                } else if (count < 0) {
                    defect = "negative_count";
                } else if (count > 9007199254740991.0) {
                    defect = "count_exceeds_exact_integer_range";
                } else if (std::trunc(count) != count) {
                    defect = "fractional_count";
                } else {
                    return (uint64_t) count;
                }
            }

            if (warning && warning->field.empty()) *warning = {path, defect};
            return std::nullopt;
        }

        // National days off in China's published 2026 holiday calendar. Weekend dates
        // need no separate entry. Make-up working weekends remain off-peak.
        // https://www.gov.cn/zhengce/zhengceku/202511/content_7047091.htm
        inline bool is_chinese_public_holiday_2026(std::chrono::year_month_day day) {
            using namespace std::chrono;

            auto holiday = [day](unsigned month, unsigned first, unsigned last) {
                return day.month() == std::chrono::month {month} && unsigned(day.day()) >= first && unsigned(day.day()) <= last;
            };

            return holiday(1, 1, 3) || holiday(2, 15, 23) || holiday(4, 4, 6) || holiday(5, 1, 5) || holiday(6, 19, 21) || holiday(9, 25, 27) || holiday(10, 1, 7);
        }

        inline std::optional<bool> is_deepseek_peak(std::chrono::sys_seconds time) {
            using namespace std::chrono;
            // Peak hours are Mon-Fri, 01:00-04:00 and 06:00-10:00 UTC,
            // excluding Chinese public holidays (determined in Beijing time).
            sys_days beijing_day = floor<days>(time + 8h);
            year_month_day date {beijing_day};
            if (date.year() != 2026y) return std::nullopt; // No holiday calendar for other years.

            int hour_utc = duration_cast<hours>(time - floor<days>(time)).count();
            return weekday {beijing_day}.iso_encoding() <= 5 && !is_chinese_public_holiday_2026(date) && ((hour_utc >= 1 && hour_utc < 4) || (hour_utc >= 6 && hour_utc < 10));
        }

        inline bool is_weekday_utc_peak(std::chrono::sys_seconds time) {
            using namespace std::chrono;
            auto day = floor<days>(time);
            int hour = duration_cast<hours>(time - day).count();

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

    // prompt_tokens is the inclusive input total. DeepSeek reports explicit hit
    // and miss counts; OpenAI nests the same counts under prompt_tokens_details.
    inline TokenUsage from_chat_completions_usage(const SJSON::JSObject& usage, UsageError* warning = nullptr) {
        auto input = detail::token_count(usage, "prompt_tokens", warning);
        auto reads = detail::token_count(usage, "prompt_cache_hit_tokens", warning);
        auto misses = detail::token_count(usage, "prompt_cache_miss_tokens", warning);
        uint64_t output = detail::token_count(usage, "completion_tokens", warning).value_or(0);
        uint64_t writes = 0;

        if (auto details = usage.find("prompt_tokens_details"); details != usage.end() && details->second.is_object()) {
            const SJSON::JSObject& fields = details->second.object();
            if (!reads) reads = detail::token_count(fields, "cached_tokens", warning, "prompt_tokens_details.cached_tokens");
            writes = detail::token_count(fields, "cache_write_tokens", warning, "prompt_tokens_details.cache_write_tokens").value_or(0);
        }

        uint64_t reads_value = reads.value_or(0);
        uint64_t misses_value = misses.value_or(0);
        if (input) {
            // The inclusive total decides the uncached remainder.
            if (misses) {
                misses_value = *misses;
                reads_value = *input > misses_value ? *input - misses_value : 0;
            } else {
                if (reads_value > *input) reads_value = 0;
                misses_value = *input - reads_value;
            }
        }

        if (writes > misses_value) writes = misses_value;
        return TokenUsage {reads_value, misses_value, output, writes};
    }

    // input_tokens is the inclusive input total, and input_tokens_details holds
    // cache reads and writes that are subsets of it.
    inline TokenUsage from_responses_usage(const SJSON::JSObject& usage, UsageError* warning = nullptr) {
        auto input = detail::token_count(usage, "input_tokens", warning);
        uint64_t output = detail::token_count(usage, "output_tokens", warning).value_or(0);
        uint64_t reads = 0;
        uint64_t writes = 0;

        if (auto details = usage.find("input_tokens_details"); details != usage.end() && details->second.is_object()) {
            const SJSON::JSObject& fields = details->second.object();
            reads = detail::token_count(fields, "cached_tokens", warning, "input_tokens_details.cached_tokens").value_or(0);
            writes = detail::token_count(fields, "cache_write_tokens", warning, "input_tokens_details.cache_write_tokens").value_or(0);
        }

        uint64_t uncached = writes;
        if (input) {
            if (reads > *input) reads = 0;
            uncached = *input - reads;
        }

        if (writes > uncached) writes = uncached;
        return TokenUsage {reads, uncached, output, writes};
    }

    // Messages reports disjoint counters: input_tokens excludes the cache counts,
    // while cache_creation_input_tokens is the cache-write count. An unreadable
    // field removes only its own charge.
    inline TokenUsage from_anthropic_usage(const SJSON::JSObject& usage, UsageError* warning = nullptr) {
        uint64_t input = detail::token_count(usage, "input_tokens", warning).value_or(0);
        uint64_t reads = detail::token_count(usage, "cache_read_input_tokens", warning).value_or(0);
        uint64_t writes = detail::token_count(usage, "cache_creation_input_tokens", warning).value_or(0);
        uint64_t output = detail::token_count(usage, "output_tokens", warning).value_or(0);
        return TokenUsage {reads, input + writes, output, writes};
    }

    // Apply the exact multiplier, then round picodollars up to nanodollars once.
    // Invalid usage, unavailable schedules and overflow return no estimate.
    inline std::optional<uint64_t> calculate(const providers::Model& model, const TokenUsage& usage, std::chrono::system_clock::time_point time, Multiplier multiplier) {
        if (multiplier.denominator == 0 || usage.cache_creation_tokens > usage.cache_miss_tokens || usage.cache_hit_tokens > std::numeric_limits<uint64_t>::max() - usage.cache_miss_tokens) {
            return std::nullopt;
        }

        auto seconds = std::chrono::floor<std::chrono::seconds>(time);

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
            auto scheduled = detail::is_deepseek_peak(seconds);
            if (!scheduled) return std::nullopt;

            peak = *scheduled;
            break;
        }

        case providers::SCHEDULE_WEEKDAY_UTC_DOUBLED:
            peak = detail::is_weekday_utc_peak(seconds);
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
