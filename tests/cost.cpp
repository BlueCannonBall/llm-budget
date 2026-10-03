#include "cost.hpp"
#include <cassert>
#include <chrono>
#include <limits>
#include <type_traits>
#include <vector>

static void check_usage(const std::optional<cost::TokenUsage>& usage,
    std::uint64_t hits, std::uint64_t misses, std::uint64_t output, std::uint64_t creation = 0) {
    assert(usage);
    assert(usage->cache_hit_tokens == hits);
    assert(usage->cache_miss_tokens == misses);
    assert(usage->output_tokens == output);
    assert(usage->cache_creation_tokens == creation);
}

int main() {
    using namespace std::chrono;
    cost::TokenUsage usage {0, 35, 15, 0};
    auto off_peak = sys_days {2026y / September / 27} + 12h; // Sunday
    auto peak = sys_days {2026y / September / 28} + 1h; // Monday 01:00 UTC

    auto amount = cost::calculate("deepseek", "deepseek-flash", usage, off_peak);
    static_assert(std::is_same_v<decltype(amount), std::optional<std::uint64_t>>);
    assert(amount && *amount == 14250);
    amount = cost::calculate("deepseek", "deepseek-flash", usage, peak);
    assert(amount && *amount == 28500);
    assert(*cost::calculate("deepseek", "deepseek-flash", usage, peak - 1s) == 14250);
    assert(*cost::calculate("deepseek", "deepseek-flash", usage, peak + 3h) == 14250);
    assert(*cost::calculate("deepseek", "deepseek-flash", usage, peak + 5h) == 28500);
    assert(*cost::calculate("deepseek", "deepseek-flash", usage, peak + 9h) == 14250);
    // A weekday national holiday is off-peak despite falling in the peak UTC hours.
    assert(*cost::calculate("deepseek", "deepseek-flash", usage, sys_days {2026y / September / 25} + 1h) == 14250);
    assert(!cost::calculate("deepseek", "deepseek-flash", usage, sys_days {2027y / January / 4} + 1h));
    assert(!cost::calculate("deepseek", "deepseek-flash", usage, sys_days {2026y / September / 9}));

    usage = {10, 25, 16, 0};
    amount = cost::calculate("deepseek", "deepseek-v4-pro", usage, off_peak);
    assert(amount && *amount == 48400);
    amount = cost::calculate("deepseek", "deepseek-v4-pro", usage, peak);
    assert(amount && *amount == 96800);

    assert(!cost::calculate("anthropic", "deepseek-v4-pro", usage, peak));
    assert(!cost::calculate("deepseek", "unknown-model", usage, peak));
    usage = {0, 9007199254740991ULL, 9007199254740991ULL, 0};
    assert(!cost::calculate("deepseek", "deepseek-v4-pro", usage, peak));

    // Check individual products, accumulation, and the peak-price multiplier.
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    assert(!cost::calculate("deepseek", "deepseek-flash", {maximum, 0, 0, 0}, off_peak));
    assert(!cost::calculate("deepseek", "deepseek-flash", {0, maximum, 0, 0}, off_peak));
    assert(!cost::calculate("deepseek", "deepseek-flash", {0, 0, maximum, 0}, off_peak));
    assert(!cost::calculate("deepseek", "deepseek-flash", {maximum / 3, 1, 0, 0}, off_peak));
    assert(cost::calculate("deepseek", "deepseek-flash", {0, 0, maximum / 600, 0}, off_peak));
    assert(!cost::calculate("deepseek", "deepseek-flash", {0, 0, maximum / 600, 0}, peak));
    amount = cost::calculate("deepseek", "deepseek-flash", {0, 0, 0, 0}, peak);
    assert(amount && *amount == 0);

    SJSON::JSObject chat_usage {
        {"prompt_tokens", 35},
        {"prompt_cache_hit_tokens", 10},
        {"prompt_cache_miss_tokens", 25},
        {"completion_tokens", 16},
        {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 10}}},
        {"completion_tokens_details", SJSON::JSObject {{"reasoning_tokens", 13}}},
        {"total_tokens", 51},
    };
    SJSON::JSObject anthropic_usage {
        {"input_tokens", 25},
        {"cache_read_input_tokens", 10},
        {"cache_creation_input_tokens", 0},
        {"output_tokens", 16},
    };
    auto chat = cost::from_chat_completions_usage(chat_usage);
    auto anthropic = cost::from_anthropic_usage(anthropic_usage);
    check_usage(chat, 10, 25, 16);
    check_usage(anthropic, 10, 25, 16);
    assert(*cost::calculate("deepseek", "deepseek-v4-pro", *chat, off_peak) == 48400);
    assert(*cost::calculate("deepseek", "deepseek-v4-pro", *anthropic, off_peak) == 48400);

    // Cache creation is disjoint from ordinary input in the Messages format.
    anthropic_usage["input_tokens"] = 20;
    anthropic_usage["cache_creation_input_tokens"] = 5;
    check_usage(cost::from_anthropic_usage(anthropic_usage), 10, 25, 16, 5);
    check_usage(cost::from_anthropic_usage({{"input_tokens", 25}, {"output_tokens", 16}}), 0, 25, 16);
    check_usage(cost::from_anthropic_usage({{"input_tokens", 0}, {"output_tokens", 0}}), 0, 0, 0);
    check_usage(cost::from_chat_completions_usage({{"prompt_tokens", 0}, {"prompt_cache_hit_tokens", 0}, {"prompt_cache_miss_tokens", 0}, {"completion_tokens", 0}}), 0, 0, 0);

    // Raw initial usage, output-only deltas, and empty snapshots are accepted.
    check_usage(cost::from_anthropic_usage({{"input_tokens", 25}, {"cache_read_input_tokens", 10}, {"output_tokens", 1}}), 10, 25, 1);
    check_usage(cost::from_anthropic_usage({{"output_tokens", 16}}), 0, 0, 16);
    check_usage(cost::from_anthropic_usage({{"input_tokens", 25}}), 0, 25, 0);
    check_usage(cost::from_anthropic_usage({{"cache_read_input_tokens", 10}}), 10, 0, 0);
    check_usage(cost::from_anthropic_usage({{"cache_creation_input_tokens", 5}}), 0, 5, 0, 5);
    check_usage(cost::from_anthropic_usage({}), 0, 0, 0);
    check_usage(cost::from_chat_completions_usage({{"completion_tokens", 16}}), 0, 0, 16);
    check_usage(cost::from_chat_completions_usage({{"prompt_cache_hit_tokens", 10}}), 10, 0, 0);
    check_usage(cost::from_chat_completions_usage({{"prompt_cache_miss_tokens", 25}}), 0, 25, 0);
    check_usage(cost::from_chat_completions_usage({}), 0, 0, 0);

    // Cache hit rate is cache reads over total input; writes count against it.
    assert(cost::cache_hit_rate({0, 0, 0, 0}) == 0.0);
    assert(cost::cache_hit_rate({0, 35, 15, 0}) == 0.0);
    assert(cost::cache_hit_rate({1, 3, 0, 0}) == 0.25);
    assert(cost::cache_hit_rate({6, 10, 0, 0}) == 0.375);
    assert(cost::cache_hit_rate(*cost::from_anthropic_usage({{"input_tokens", 20}, {"cache_creation_input_tokens", 5}, {"cache_read_input_tokens", 15}})) == 0.375);

    auto inconsistent = chat_usage;
    inconsistent["prompt_cache_miss_tokens"] = 24;
    assert(!cost::from_chat_completions_usage(inconsistent));
    inconsistent["prompt_cache_hit_tokens"] = 36;
    assert(!cost::from_chat_completions_usage(inconsistent));

    const std::vector<SJSON::JSValue> invalid_counts {
        -1, 1.5, 9007199254740992.0, "16", true, SJSON::JSNull {},
        std::numeric_limits<SJSON::JSNumber>::infinity(),
        std::numeric_limits<SJSON::JSNumber>::quiet_NaN(),
    };
    for (const auto& field : {"prompt_tokens", "prompt_cache_hit_tokens", "prompt_cache_miss_tokens", "completion_tokens"}) {
        for (const auto& value : invalid_counts) {
            auto invalid = chat_usage;
            invalid[field] = value;
            assert(!cost::from_chat_completions_usage(invalid));
        }
        auto missing = chat_usage;
        missing.erase(field);
        check_usage(cost::from_chat_completions_usage(missing),
            std::string_view(field) == "prompt_cache_hit_tokens" ? 0 : 10,
            std::string_view(field) == "prompt_cache_miss_tokens" ? 0 : 25,
            std::string_view(field) == "completion_tokens" ? 0 : 16);
    }
    for (const auto& field : {"input_tokens", "cache_read_input_tokens", "cache_creation_input_tokens", "output_tokens"}) {
        for (const auto& value : invalid_counts) {
            auto invalid = anthropic_usage;
            invalid[field] = value;
            assert(!cost::from_anthropic_usage(invalid));
        }
        auto missing = anthropic_usage;
        missing.erase(field);
        check_usage(cost::from_anthropic_usage(missing),
            std::string_view(field) == "cache_read_input_tokens" ? 0 : 10,
            (std::string_view(field) == "input_tokens" ? 0 : 20)
                + (std::string_view(field) == "cache_creation_input_tokens" ? 0 : 5),
            std::string_view(field) == "output_tokens" ? 0 : 16,
            std::string_view(field) == "cache_creation_input_tokens" ? 0 : 5);
    }
    check_usage(cost::from_anthropic_usage({{"input_tokens", 9007199254740991.0}, {"output_tokens", 0}}), 0, 9007199254740991ULL, 0);
}
