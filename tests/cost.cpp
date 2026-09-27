#include "cost.hpp"
#include <cassert>

int main() {
    SJSON::JSObject usage {
        {"prompt_tokens", 35},
        {"prompt_cache_hit_tokens", 0},
        {"prompt_cache_miss_tokens", 35},
        {"completion_tokens", 15},
        {"completion_tokens_details", SJSON::JSObject {{"reasoning_tokens", 13}}},
    };
    auto cost = deepseek_cost_range(usage, "deepseek-flash");
    assert(cost && cost->off_peak_nano_usd == 14250 && cost->peak_nano_usd == 28500);

    usage["completion_tokens"] = 16;
    cost = deepseek_cost_range(usage, "deepseek-flash");
    assert(cost && cost->off_peak_nano_usd == 14850 && cost->peak_nano_usd == 29700);

    usage["prompt_cache_hit_tokens"] = 10;
    usage["prompt_cache_miss_tokens"] = 25;
    cost = deepseek_cost_range(usage, "deepseek-v4-pro");
    assert(cost && cost->off_peak_nano_usd == 48400 && cost->peak_nano_usd == 96800);

    assert(!deepseek_cost_range(usage, "unknown-model"));
    usage["prompt_cache_miss_tokens"] = 24;
    assert(!deepseek_cost_range(usage, "deepseek-flash"));
    usage["prompt_cache_miss_tokens"] = 25.5;
    assert(!deepseek_cost_range(usage, "deepseek-flash"));
    usage["prompt_cache_miss_tokens"] = 9007199254740991.0;
    usage["prompt_tokens"] = 9007199254740991.0;
    usage["prompt_cache_hit_tokens"] = 0;
    usage["completion_tokens"] = 9007199254740991.0;
    assert(!deepseek_cost_range(usage, "deepseek-v4-pro"));
}
