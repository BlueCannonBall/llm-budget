#pragma once

#include "../configured_provider.hpp"

namespace providers {
    class DeepSeekProvider final : public ConfiguredProvider {
    protected:
        inline static constexpr Provider provider_definition {
            .name = "deepseek",
            .chat_completions_base_url = "https://api.deepseek.com",
            .anthropic_messages_base_url = "https://api.deepseek.com/anthropic",
            .responses_base_url = "https://api.deepseek.com",
        };

        // Rates checked October 8, 2026. Direct Chinese holiday rules are independent of Go.
        // https://api-docs.deepseek.com/quick_start/pricing/
        // https://api-docs.deepseek.com/news/news260910/
        // https://api-docs.deepseek.com/news/news260813/
        // Canonical current IDs; retired Flash aliases are intentionally omitted.
        // https://api-docs.deepseek.com/guides/responses_api
        inline static constexpr Model model_catalog[] {
            {
                .name = "deepseek-flash",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_ANTHROPIC_MESSAGES | PROTOCOL_RESPONSES,
                .rates = {.input = 150'000, .cached_read = 3'000, .output = 600'000, .cached_write = 150'000},
                .schedule = SCHEDULE_DEEPSEEK_2026,
            },

            {
                .name = "deepseek-v4-pro",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_ANTHROPIC_MESSAGES | PROTOCOL_RESPONSES,
                .rates = {.input = 660'000, .cached_read = 22'000, .output = 1'980'000, .cached_write = 660'000},
                .schedule = SCHEDULE_DEEPSEEK_2026,
            },
        };

    public:
        explicit DeepSeekProvider(SJSON::JSValue&& config = {}):
            ConfiguredProvider(provider_definition) {
            api_key = read_api_key(std::move(config));
        }

        std::span<const Model> models() const override {
            return model_catalog;
        }

        std::optional<std::string_view> prepare_request(Protocol protocol, const pw::Headers& inbound_headers, pw::Headers& outbound_headers, SJSON::JSObject& body, const std::string& user_name) const override {
            ConfiguredProvider::prepare_request(protocol, inbound_headers, outbound_headers, body, user_name);

            if (protocol == PROTOCOL_CHAT_COMPLETIONS) {
                body["user_id"] = user_name;
            } else if (protocol == PROTOCOL_RESPONSES) {
                body["user"] = user_name;
            } else if (protocol == PROTOCOL_ANTHROPIC_MESSAGES) {
                auto& metadata = body["metadata"];
                if (metadata.is_null()) metadata = SJSON::JSObject {};
                if (!metadata.is_object()) return "Invalid metadata specified";

                metadata.object()["user_id"] = user_name;
            }

            return std::nullopt;
        }
    };
} // namespace providers
