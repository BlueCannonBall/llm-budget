#pragma once

#include <stdint.h>
#include <string_view>

namespace providers {
    struct Provider {
        std::string_view name;
        std::string_view chat_completions_base_url;
        std::string_view anthropic_messages_base_url;
        std::string_view responses_base_url;
    };

    enum Protocol : unsigned {
        PROTOCOL_CHAT_COMPLETIONS = 1,
        PROTOCOL_ANTHROPIC_MESSAGES = 2,
        PROTOCOL_RESPONSES = 4,
    };

    enum Schedule {
        SCHEDULE_FLAT,
        SCHEDULE_DEEPSEEK_2026,
        SCHEDULE_WEEKDAY_UTC_DOUBLED,
    };

    // Integer picodollars per token preserve fractional nanodollar prices, e.g.
    // MiMo's $0.003625 / million cache reads = 3.625 nanodollars / token.
    struct Rates {
        uint64_t input;
        uint64_t cached_read;
        uint64_t output;
        uint64_t cached_write;
    };

    struct Model {
        std::string_view name;
        unsigned protocols;
        Rates rates;

        Schedule schedule = SCHEDULE_FLAT;

        uint32_t input_token_threshold = 0;
        Rates above_threshold_rates {};

        constexpr bool supports(Protocol protocol) const {
            return (protocols & (unsigned) protocol) != 0;
        }
    };
} // namespace providers
