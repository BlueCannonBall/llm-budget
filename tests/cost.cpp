#include "cost.hpp"
#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono;
    SJSON::JSObject usage {
        {"prompt_tokens", 35},
        {"prompt_cache_hit_tokens", 0},
        {"prompt_cache_miss_tokens", 35},
        {"completion_tokens", 15},
        {"completion_tokens_details", SJSON::JSObject {{"reasoning_tokens", 13}}},
    };
    auto off_peak = sys_days {2026y / September / 27} + 12h; // Sunday
    auto peak = sys_days {2026y / September / 28} + 1h; // Monday 01:00 UTC

    auto amount = cost::calculate("deepseek", "deepseek-flash", usage, off_peak);
    assert(amount && amount->currency == "USD" && amount->nano_units == 14250);
    amount = cost::calculate("deepseek", "deepseek-flash", usage, peak);
    assert(amount && amount->nano_units == 28500);
    assert(cost::calculate("deepseek", "deepseek-flash", usage, peak - 1s)->nano_units == 14250);
    assert(cost::calculate("deepseek", "deepseek-flash", usage, peak + 3h)->nano_units == 14250);
    assert(cost::calculate("deepseek", "deepseek-flash", usage, peak + 5h)->nano_units == 28500);
    assert(cost::calculate("deepseek", "deepseek-flash", usage, peak + 9h)->nano_units == 14250);
    // A weekday national holiday is off-peak despite falling in the peak UTC hours.
    assert(cost::calculate("deepseek", "deepseek-flash", usage, sys_days {2026y / September / 25} + 1h)->nano_units == 14250);
    assert(!cost::calculate("deepseek", "deepseek-flash", usage, sys_days {2027y / January / 4} + 1h));
    assert(!cost::calculate("deepseek", "deepseek-flash", usage, sys_days {2026y / September / 9}));

    usage["completion_tokens"] = 16;
    usage["prompt_cache_hit_tokens"] = 10;
    usage["prompt_cache_miss_tokens"] = 25;
    amount = cost::calculate("deepseek", "deepseek-v4-pro", usage, off_peak);
    assert(amount && amount->nano_units == 48400);
    amount = cost::calculate("deepseek", "deepseek-v4-pro", usage, peak);
    assert(amount && amount->nano_units == 96800);

    assert(!cost::calculate("anthropic", "deepseek-v4-pro", usage, peak));
    assert(!cost::calculate("deepseek", "unknown-model", usage, peak));
    usage["prompt_cache_miss_tokens"] = 24;
    assert(!cost::calculate("deepseek", "deepseek-flash", usage, peak));
    usage["prompt_cache_miss_tokens"] = 25.5;
    assert(!cost::calculate("deepseek", "deepseek-flash", usage, peak));
    usage["prompt_cache_miss_tokens"] = 9007199254740991.0;
    usage["prompt_tokens"] = 9007199254740991.0;
    usage["prompt_cache_hit_tokens"] = 0;
    usage["completion_tokens"] = 9007199254740991.0;
    assert(!cost::calculate("deepseek", "deepseek-v4-pro", usage, peak));
}
