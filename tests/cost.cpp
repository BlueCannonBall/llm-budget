#include "provider_config.hpp"
#include <cassert>
#include <chrono>
#include <limits>
#include <type_traits>
#include <vector>
#include <utility>

static providers::Configuration make_configuration(std::string_view plan = "go") {
    return providers::configure({
        {"deepseek", "test-deepseek"},
        {"openai", "test-openai"},
        {"opencode-go", SJSON::JSObject {{"api_key", "test-go"}, {"plan", plan}}},
    });
}

static const auto configuration = make_configuration();

static std::optional<std::uint64_t> calculate_cost(std::string_view service, std::string_view model,
    const cost::TokenUsage& usage, std::chrono::system_clock::time_point at,
    const providers::Configuration& configured = configuration) {
    const auto* provider = providers::find_provider(configured, service);
    if (!provider || !provider->configured()) return std::nullopt;

    const auto* selected_model = provider->find_model(model);
    if (!selected_model) return std::nullopt;

    auto multiplier = provider->multiplier(*selected_model);
    if (!multiplier) return std::nullopt;

    return cost::calculate(*selected_model, usage, at, *multiplier);
}

static void check_usage(const cost::UsageResult& usage,
    std::uint64_t hits, std::uint64_t misses, std::uint64_t output, std::uint64_t creation = 0) {
    assert(usage);
    assert(usage->cache_hit_tokens == hits);
    assert(usage->cache_miss_tokens == misses);
    assert(usage->output_tokens == output);
    assert(usage->cache_creation_tokens == creation);
}

static void check_cost(std::string_view service, std::string_view model,
    const cost::TokenUsage& usage, std::chrono::system_clock::time_point at,
    std::uint64_t expected, const providers::Configuration& configured = configuration) {
    auto amount = calculate_cost(service, model, usage, at, configured);
    assert(amount && *amount == expected);
}

int main() {
    using namespace std::chrono;
    cost::TokenUsage usage {0, 35, 15, 0};
    auto off_peak = sys_days {2026y / September / 27} + 12h; // Sunday
    auto peak = sys_days {2026y / September / 28} + 1h; // Monday 01:00 UTC

    auto amount = calculate_cost("deepseek", "deepseek-flash", usage, off_peak);
    static_assert(std::is_same_v<decltype(amount), std::optional<std::uint64_t>>);
    assert(amount && *amount == 14250);
    amount = calculate_cost("deepseek", "deepseek-flash", usage, peak);
    assert(amount && *amount == 28500);
    assert(*calculate_cost("deepseek", "deepseek-flash", usage, peak - 1s) == 14250);
    assert(*calculate_cost("deepseek", "deepseek-flash", usage, peak + 3h) == 14250);
    assert(*calculate_cost("deepseek", "deepseek-flash", usage, peak + 5h) == 28500);
    assert(*calculate_cost("deepseek", "deepseek-flash", usage, peak + 9h) == 14250);
    // A weekday national holiday is off-peak despite falling in the peak UTC hours.
    assert(*calculate_cost("deepseek", "deepseek-flash", usage, sys_days {2026y / September / 25} + 1h) == 14250);
    assert(!calculate_cost("deepseek", "deepseek-flash", usage, sys_days {2027y / January / 4} + 1h));

    usage = {10, 25, 16, 0};
    amount = calculate_cost("deepseek", "deepseek-v4-pro", usage, off_peak);
    assert(amount && *amount == 48400);
    amount = calculate_cost("deepseek", "deepseek-v4-pro", usage, peak);
    assert(amount && *amount == 96800);

    assert(!calculate_cost("anthropic", "deepseek-v4-pro", usage, peak));
    assert(!calculate_cost("deepseek", "unknown-model", usage, peak));
    usage = {0, 9007199254740991ULL, 9007199254740991ULL, 0};
    assert(!calculate_cost("deepseek", "deepseek-v4-pro", usage, peak));

    // Check individual products, accumulation, and the peak-price multiplier.
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    assert(!calculate_cost("deepseek", "deepseek-flash", {maximum, 0, 0, 0}, off_peak));
    assert(!calculate_cost("deepseek", "deepseek-flash", {0, maximum, 0, 0}, off_peak));
    assert(!calculate_cost("deepseek", "deepseek-flash", {0, 0, maximum, 0}, off_peak));
    assert(!calculate_cost("deepseek", "deepseek-flash", {maximum / 3, 1, 0, 0}, off_peak));
    assert(calculate_cost("deepseek", "deepseek-flash", {0, 0, maximum / 600, 0}, off_peak));
    assert(!calculate_cost("deepseek", "deepseek-flash", {0, 0, maximum / 600, 0}, peak));
    amount = calculate_cost("deepseek", "deepseek-flash", {0, 0, 0, 0}, peak);
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
    assert(*calculate_cost("deepseek", "deepseek-v4-pro", *chat, off_peak) == 48400);
    assert(*calculate_cost("deepseek", "deepseek-v4-pro", *anthropic, off_peak) == 48400);

    // Cache creation is disjoint from ordinary input in the Messages format.
    anthropic_usage["input_tokens"] = 20;
    anthropic_usage["cache_creation_input_tokens"] = 5;
    check_usage(cost::from_anthropic_usage(anthropic_usage), 10, 25, 16, 5);
    check_usage(cost::from_anthropic_usage({{"input_tokens", 25}, {"output_tokens", 16}}), 0, 25, 16);
    check_usage(cost::from_anthropic_usage({{"input_tokens", 0}, {"output_tokens", 0}}), 0, 0, 0);
    check_usage(cost::from_chat_completions_usage({{"prompt_tokens", 0}, {"prompt_cache_hit_tokens", 0}, {"prompt_cache_miss_tokens", 0}, {"completion_tokens", 0}}), 0, 0, 0);

    // Responses includes cache reads in input and reasoning in output.
    SJSON::JSObject responses_usage {
        {"input_tokens", 35}, {"input_tokens_details", SJSON::JSObject {{"cached_tokens", 10}}},
        {"output_tokens", 16}, {"output_tokens_details", SJSON::JSObject {{"reasoning_tokens", 12}}},
    };
    auto responses_tokens = cost::from_responses_usage(responses_usage);
    check_usage(responses_tokens, 10, 25, 16);
    check_cost("openai", "gpt-4.1-mini", *responses_tokens, off_peak, 36600);
    check_usage(cost::from_responses_usage({{"input_tokens", 35}, {"output_tokens", 16}}), 0, 35, 16);
    check_usage(cost::from_responses_usage({{"input_tokens", 35}, {"input_tokens_details", SJSON::JSObject {{"cached_tokens", 35}}}}), 35, 0, 0);
    assert(!cost::from_responses_usage({{"input_tokens", 35}, {"input_tokens_details", SJSON::JSObject {{"cached_tokens", 36}}}}));
    assert(!cost::from_responses_usage({{"input_tokens_details", "invalid"}}));

    auto response_writes = cost::from_responses_usage({
        {"input_tokens", 35}, {"input_tokens_details", SJSON::JSObject {{"cached_tokens", 10}, {"cache_write_tokens", 5}}},
        {"output_tokens", 16},
    });
    check_usage(response_writes, 10, 25, 16, 5);
    check_cost("openai", "gpt-6.1-sol", *response_writes, off_peak, 213500);
    check_usage(cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 10}, {"cache_write_tokens", 5}}},
        {"completion_tokens", 16}}), 10, 25, 16, 5);
    assert(!cost::from_responses_usage({{"input_tokens", 35}, {"input_tokens_details", SJSON::JSObject {{"cached_tokens", 10}, {"cache_write_tokens", 26}}}}));
    assert(!cost::from_chat_completions_usage({{"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 10}, {"cache_write_tokens", 26}}}}));

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
    for (const auto& value : invalid_counts) {
        auto invalid = responses_usage;
        invalid["input_tokens"] = value;
        assert(!cost::from_responses_usage(invalid));
        invalid = responses_usage;
        invalid["output_tokens"] = value;
        assert(!cost::from_responses_usage(invalid));
        invalid = responses_usage;
        invalid["input_tokens_details"] = SJSON::JSObject {{"cached_tokens", value}};
        assert(!cost::from_responses_usage(invalid));
        invalid["input_tokens_details"] = SJSON::JSObject {{"cache_write_tokens", value}};
        if (value.is_null()) {
            check_usage(cost::from_responses_usage(invalid), 0, 35, 16);
            check_usage(cost::from_chat_completions_usage({{"prompt_tokens", 35},
                {"prompt_tokens_details", SJSON::JSObject {{"cache_write_tokens", value}}}}), 0, 35, 0);
        } else {
            assert(!cost::from_responses_usage(invalid));
            assert(!cost::from_chat_completions_usage({{"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSObject {{"cache_write_tokens", value}}}}));
        }
    }
    for (const auto& field : {"prompt_tokens", "prompt_cache_hit_tokens", "prompt_cache_miss_tokens", "completion_tokens"}) {
        for (const auto& value : invalid_counts) {
            auto invalid = chat_usage;
            invalid[field] = value;
            auto rejected = cost::from_chat_completions_usage(invalid);
            assert(!rejected && rejected.error().field == field);
        }
        auto missing = chat_usage;
        missing.erase(field);
        check_usage(cost::from_chat_completions_usage(missing), 10, 25,
            std::string_view(field) == "completion_tokens" ? 0 : 16);
    }
    auto conflicting_cache = cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_cache_hit_tokens", 0},
        {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 10}}}});
    assert(!conflicting_cache);
    assert(conflicting_cache.error().field == "prompt_cache_hit_tokens");
    assert(conflicting_cache.error().reason == "conflicts_with_prompt_tokens_details.cached_tokens");
    auto null_writes = cost::from_responses_usage({
        {"input_tokens", 35}, {"input_tokens_details", SJSON::JSObject {{"cached_tokens", 10}, {"cache_write_tokens", SJSON::JSNull {}}}}});
    check_usage(null_writes, 10, 25, 0);
    auto go_flash_usage = cost::from_chat_completions_usage({
        {"prompt_tokens", 211852}, {"completion_tokens", 139},
        {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 2688}, {"cache_write_tokens", SJSON::JSNull {}}}}});
    check_usage(go_flash_usage, 2688, 209164, 139);
    auto oversized_cache = cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 36}}}});
    assert(!oversized_cache);
    assert(oversized_cache.error().field == "prompt_tokens_details.cached_tokens");
    assert(oversized_cache.error().reason == "cache_reads_exceed_prompt_tokens");
    for (const auto& field : {"input_tokens", "cache_read_input_tokens", "cache_creation_input_tokens", "output_tokens"}) {
        for (const auto& value : invalid_counts) {
            auto invalid = anthropic_usage;
            invalid[field] = value;
            auto rejected = cost::from_anthropic_usage(invalid);
            assert(!rejected && rejected.error().field == field);
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

    // Ordinary OpenAI prompts must not become free just because DeepSeek's
    // explicit cache-miss field is absent.
    check_usage(cost::from_chat_completions_usage({{"prompt_tokens", 35}, {"completion_tokens", 16}}), 0, 35, 16);
    check_usage(cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 10}}},
        {"completion_tokens", 16}}), 10, 25, 16);
    check_usage(cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_cache_hit_tokens", 10}}), 10, 25, 0);
    check_usage(cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_cache_miss_tokens", 25}}), 10, 25, 0);
    check_usage(cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSObject {}}}), 0, 35, 0);
    check_usage(cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSNull {}}}), 0, 35, 0);
    assert(!cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 36}}}}));
    assert(!cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_cache_hit_tokens", 10},
        {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 9}}}}));
    assert(!cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_cache_miss_tokens", 24},
        {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", 10}}}}));
    for (const auto& invalid : invalid_counts) {
        assert(!cost::from_chat_completions_usage({
            {"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSObject {{"cached_tokens", invalid}}}}));
        if (!invalid.is_null()) {
            assert(!cost::from_chat_completions_usage({
                {"prompt_tokens", 35}, {"prompt_tokens_details", invalid}}));
        }
    }
    assert(!cost::from_chat_completions_usage({
        {"prompt_tokens", 35}, {"prompt_tokens_details", SJSON::JSArray {}}}));

    // Direct pricing treats cache writes as ordinary input.
    check_cost("deepseek", "deepseek-v4-pro", {10, 25, 16, 5}, off_peak, 48400);

    // Identical token prices, different included allowances: $10/$15 versus $10/$60.
    check_cost("opencode-go", "glm-5.3", {100, 1000, 100, 0}, off_peak, 1244000);
    check_cost("opencode-go", "glm-5.2", {100, 1000, 100, 0}, off_peak, 311000);

    // Plus allowances are model-specific, not a uniform multiple of Go.
    auto plus_configuration = make_configuration("go-plus");
    check_cost("opencode-go", "glm-5.3", {100, 1000, 100, 0}, off_peak, 622000, plus_configuration);
    check_cost("opencode-go", "glm-5.2", {100, 1000, 100, 0}, off_peak, 414667, plus_configuration);

    check_cost("deepseek", "deepseek-flash", {0, 35, 15, 0}, off_peak, 14250, plus_configuration);

    assert(!calculate_cost("opencode-go", "glm-5.3", {100, 1000, 100, 0}, off_peak, providers::Configuration {}));

    check_cost("opencode-go", "deepseek-v4.1-flash", {0, 35, 15, 0}, off_peak, 2375);
    check_cost("opencode-go", "deepseek-v4-flash", {0, 35, 15, 0}, off_peak, 4750);
    check_cost("opencode-go", "deepseek-v4-flash-vision-exp", {0, 35, 15, 0}, off_peak, 9500);

    // Cache writes use their own rate without also charging the input rate.
    check_cost("opencode-go", "minimax-m2.7", {100, 205, 1000, 5}, off_peak, 211313);
    check_cost("opencode-go", "minimax-m2.7", {0, 10, 0, 10}, off_peak, 625);
    check_cost("opencode-go", "minimax-m3", {0, 10, 0, 10}, off_peak, 500);

    // Fractional nanodollar rates survive until the final subscription calculation.
    check_cost("opencode-go", "mimo-v2.6-pro", {1, 0, 0, 0}, off_peak, 3);
    check_cost("opencode-go", "mimo-v2.6-pro", {2, 0, 0, 0}, off_peak, 5);
    check_cost("opencode-go", "mimo-v2.6-flash", {1, 0, 0, 0}, off_peak, 1);
    check_cost("opencode-go", "mimo-v2.6-flash", {6, 0, 0, 0}, off_peak, 3);
    check_cost("opencode-go", "mimo-v2.6-pro", {1, 1, 1, 0}, off_peak, 873);

    // Go uses UTC weekdays, independently of direct DeepSeek's China holidays.
    auto go_peak = sys_days {2026y / October / 1} + 1h;
    check_cost("deepseek", "deepseek-flash", {0, 35, 15, 0}, go_peak, 14250);
    check_cost("opencode-go", "deepseek-v4-flash", {0, 35, 15, 0}, go_peak, 9500);

    for (auto time : std::initializer_list<sys_seconds> {peak - 1s, peak + 3h, peak + 9h, off_peak}) {
        check_cost("opencode-go", "deepseek-v4-flash", {0, 35, 15, 0}, time, 4750);
    }

    for (auto time : {peak, peak + 5h, sys_days {2027y / January / 4} + 1h}) {
        check_cost("opencode-go", "deepseek-v4-flash", {0, 35, 15, 0}, time, 9500);
    }

    auto saturday = sys_days {2026y / October / 3} + 1h;
    check_cost("opencode-go", "deepseek-v4-flash", {0, 35, 15, 0}, saturday, 4750);

    auto friday_late = sys_days {2026y / October / 2} + 23h;
    check_cost("opencode-go", "deepseek-v4-flash", {0, 35, 15, 0}, friday_late, 4750);

    // Context thresholds count all input, not output, and select whole-request rates.
    check_cost("opencode-go", "qwen3.7-plus", {255999, 1, 10, 1}, off_peak, 1709410);
    check_cost("opencode-go", "qwen3.7-plus", {255999, 2, 10, 1}, off_peak, 5128430);
    check_cost("opencode-go", "qwen3.7-plus", {0, 256000, 1000000, 0}, off_peak, 283733334);

    check_cost("opencode-go", "grok-4.7", {200000, 0, 1, 0}, off_peak, 66670667);
    check_cost("opencode-go", "grok-4.7", {200000, 1, 1, 0}, off_peak, 133344000);
    check_cost("opencode-go", "gpt-6-luna", {272000, 0, 1, 0}, off_peak, 1813667);
    check_cost("opencode-go", "gpt-6-luna", {272000, 1, 1, 1}, off_peak, 3627334);

    // Haiku's 100K boundary includes reads and writes, but never output tokens.
    check_cost("opencode-go", "claude-haiku-5-5", {99999, 1, 10, 1}, off_peak, 670077);
    check_cost("opencode-go", "claude-haiku-5-5", {99999, 2, 10, 2}, off_peak, 3350800);
    check_cost("opencode-go", "claude-haiku-5-5", {99999, 1, 1000000, 1}, off_peak, 334000077);
    check_cost("opencode-go", "claude-haiku-5-5", {100, 205, 1000, 5}, off_peak, 347750);
    check_cost("opencode-go", "claude-haiku-5-5", {99999, 1, 10, 1}, off_peak, 670077, plus_configuration);
    check_cost("opencode-go", "claude-haiku-5-5", {99999, 2, 10, 2}, off_peak, 3350800, plus_configuration);

    // Space Bunny is now paid, with $30/$120 allowances; do not route the retired free ID.
    check_cost("opencode-go", "space-bunny", {100, 205, 1000, 5}, off_peak, 211250);
    check_cost("opencode-go", "space-bunny", {100, 205, 1000, 5}, off_peak, 211250, plus_configuration);
    assert(!providers::resolve(configuration, "opencode-go/space-bunny-free"));
    check_cost("opencode-go", "longcat-2.5-preview-free", {maximum, 0, maximum, 0}, peak, 0);

    check_cost("opencode-go", "glm-5.3", {0, 0, 0, 0}, peak, 0);
    check_cost("deepseek", "deepseek-flash", {maximum / 3, 0, 0, 0}, off_peak, maximum);

    // Large intermediate sums must not overflow when the final cost still fits.
    check_cost("opencode-go", "mimo-v2.6-pro", {maximum / 4, 0, 0, 0}, off_peak, 11144907877866187433ULL);
    assert(!calculate_cost("opencode-go", "glm-5.3", {0, maximum, maximum, 0}, off_peak));
    assert(!calculate_cost("opencode-go", "mimo-v2.6-pro", {maximum, 1, 0, 0}, off_peak));
    assert(!calculate_cost("deepseek", "deepseek-flash", {0, 1, 0, 2}, off_peak));
    assert(!calculate_cost("opencode-go", "longcat-2.5-preview-free", {0, 1, 0, 2}, off_peak));
    assert(!calculate_cost("unknown", "glm-5.3", {0, 0, 0, 0}, off_peak));
    assert(!calculate_cost("opencode-go", "unknown", {0, 0, 0, 0}, off_peak));

    check_cost("openai", "gpt-4.1-mini", {10, 25, 16, 0}, peak, 36600);
    check_cost("openai", "gpt-4o-mini", {10, 25, 16, 0}, peak, 14100);

    // At the boundary, use short-context rates; beyond it reprice the full request.
    check_cost("openai", "gpt-6.1-sol", {272000, 0, 1, 0}, off_peak, 27210000);
    check_cost("openai", "gpt-6.1-sol", {272000, 1, 1, 1}, off_peak, 54420000);
    check_cost("openai", "gpt-5.5-pro", {0, 272000, 1, 0}, off_peak, 8160180000);
    check_cost("openai", "gpt-5.5-pro", {0, 272001, 1, 0}, off_peak, 16320330000);
    check_cost("openai", "gpt-5.3-codex", {10, 25, 16, 0}, off_peak, 269500);

    // The same upstream name can be different products; never silently choose
    // a subscription or provider for an unqualified model.
    auto direct = providers::resolve(configuration, "deepseek-v4-pro");
    auto direct_qualified = providers::resolve(configuration, "deepseek/deepseek-v4-pro");
    auto go = providers::resolve(configuration, "opencode-go/deepseek-v4-pro");
    assert(direct && direct_qualified && go);
    assert(direct->provider->definition().name == "deepseek" && direct_qualified->provider->definition().name == "deepseek");
    assert(go->provider->definition().name == "opencode-go");
    assert(direct->model->name == "deepseek-v4-pro" && direct_qualified->model->name == "deepseek-v4-pro");
    assert(go->model->name == "deepseek-v4-pro");
    assert(direct->model->supports(providers::PROTOCOL_CHAT_COMPLETIONS) && direct->model->supports(providers::PROTOCOL_ANTHROPIC_MESSAGES));

    auto openai = providers::resolve(configuration, "openai/gpt-4.1-mini");
    assert(openai && openai->model->supports(providers::PROTOCOL_CHAT_COMPLETIONS) && !openai->model->supports(providers::PROTOCOL_ANTHROPIC_MESSAGES));

    assert(!providers::resolve(configuration, "glm-5.3"));
    assert(!providers::resolve(configuration, "gpt-4.1-mini"));
    assert(!providers::resolve(configuration, "opencode-go/"));
    assert(!providers::resolve(configuration, "opencode-go/glm-5.3/extra"));
    assert(!providers::resolve(configuration, "deepseek/glm-5.3"));
    assert(!providers::resolve(configuration, "unknown/deepseek-v4-pro"));

    // Reordering configured providers must not change routing or pricing.
    auto reordered = make_configuration();
    std::swap(reordered.front(), reordered.back());
    auto reordered_direct = providers::resolve(reordered, "deepseek-v4-pro");
    assert(reordered_direct && reordered_direct->provider->definition().name == "deepseek");
    check_cost("deepseek", "deepseek-v4-pro", {0, 35, 15, 0}, off_peak, 52800, reordered);
    check_cost("opencode-go", "deepseek-v4-pro", {0, 35, 15, 0}, off_peak, 35200, reordered);
    check_cost("openai", "gpt-4.1-mini", {10, 25, 16, 0}, off_peak, 36600, reordered);
}
