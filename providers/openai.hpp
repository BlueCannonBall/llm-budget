#pragma once

#include "../configured_provider.hpp"

namespace providers {
    class OpenAIProvider final : public ConfiguredProvider {
    protected:
        inline static constexpr Provider provider_definition {
            .name = "openai",
            .chat_completions_base_url = "https://api.openai.com/v1",
            .anthropic_messages_base_url = "",
        };

        // Standard text-token pricing (no tools, batch or priority fees).
        // https://developers.openai.com/api/docs/pricing
        // https://developers.openai.com/api/docs/models/gpt-4.1-mini
        // https://developers.openai.com/api/docs/models/gpt-4o-mini
        inline static constexpr Model model_catalog[] {
            {
                .name = "gpt-4.1-mini",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 400'000, .cached_read = 100'000, .output = 1'600'000, .cached_write = 400'000},
            },

            {
                .name = "gpt-4o-mini",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 150'000, .cached_read = 75'000, .output = 600'000, .cached_write = 150'000},
            },
        };

    public:
        explicit OpenAIProvider(SJSON::JSValue&& config = {}):
            ConfiguredProvider(provider_definition) {
            api_key = read_api_key(std::move(config));
        }

        std::span<const Model> models() const override {
            return model_catalog;
        }
    };
} // namespace providers
