#pragma once

#include "../configured_provider.hpp"

namespace providers {
    class OpenAIProvider final : public ConfiguredProvider {
    protected:
        static std::optional<std::string_view> validate_tools(const SJSON::JSValue& tools) {
            if (!tools.is_array()) return "tools must be an array";

            for (const auto& tool : tools.array()) {
                if (!tool.is_object()) return "Each tool must be an object";

                auto type = tool.object().find("type");
                if (type == tool.object().end() || !type->second.is_string()) return "Each tool must have a string type";
                if (type->second.string() != "function" && type->second.string() != "custom") {
                    return "Only client-executed function and custom tools are supported; hosted tool charges are not accounted for";
                }
            }

            return std::nullopt;
        }

        inline static constexpr Provider provider_definition {
            .name = "openai",
            .chat_completions_base_url = "https://api.openai.com/v1",
            .anthropic_messages_base_url = "",
            .responses_base_url = "https://api.openai.com/v1",
        };

        // Standard text-token rates, checked October 8, 2026. No tool or service-tier fees.
        // https://developers.openai.com/api/docs/pricing
        // Protocol support: https://developers.openai.com/api/docs/models/<model-id>
        // Models without discounted cache pricing use the ordinary input rate.
        inline static constexpr Model model_catalog[] {
            {
                .name = "gpt-6-astra",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 10'000'000, .cached_read = 1'000'000, .output = 50'000'000, .cached_write = 12'500'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 20'000'000, .cached_read = 2'000'000, .output = 75'000'000, .cached_write = 25'000'000},
            },

            {
                .name = "gpt-6.1-sol",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 2'000'000, .cached_read = 100'000, .output = 10'000'000, .cached_write = 2'500'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 4'000'000, .cached_read = 200'000, .output = 15'000'000, .cached_write = 5'000'000},
            },

            {
                .name = "gpt-6-sol",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 2'000'000, .cached_read = 200'000, .output = 10'000'000, .cached_write = 2'500'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 4'000'000, .cached_read = 400'000, .output = 15'000'000, .cached_write = 5'000'000},
            },

            {
                .name = "gpt-6-luna",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 100'000, .cached_read = 10'000, .output = 500'000, .cached_write = 125'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 200'000, .cached_read = 20'000, .output = 750'000, .cached_write = 250'000},
            },

            {
                .name = "gpt-5.6-sol",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 4'000'000, .cached_read = 400'000, .output = 20'000'000, .cached_write = 5'000'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 8'000'000, .cached_read = 800'000, .output = 30'000'000, .cached_write = 10'000'000},
            },

            {
                .name = "gpt-5.6-terra",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 2'000'000, .cached_read = 200'000, .output = 12'000'000, .cached_write = 2'500'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 4'000'000, .cached_read = 400'000, .output = 18'000'000, .cached_write = 5'000'000},
            },

            {
                .name = "gpt-5.6-luna",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 200'000, .cached_read = 20'000, .output = 1'200'000, .cached_write = 250'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 400'000, .cached_read = 40'000, .output = 1'800'000, .cached_write = 500'000},
            },

            {
                .name = "gpt-5.5",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 5'000'000, .cached_read = 500'000, .output = 30'000'000, .cached_write = 5'000'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 10'000'000, .cached_read = 1'000'000, .output = 45'000'000, .cached_write = 10'000'000},
            },

            {
                .name = "gpt-5.5-pro",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 30'000'000, .cached_read = 30'000'000, .output = 180'000'000, .cached_write = 30'000'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 60'000'000, .cached_read = 60'000'000, .output = 270'000'000, .cached_write = 60'000'000},
            },

            {
                .name = "gpt-5.4",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 2'500'000, .cached_read = 250'000, .output = 15'000'000, .cached_write = 2'500'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 5'000'000, .cached_read = 500'000, .output = 22'500'000, .cached_write = 5'000'000},
            },

            {
                .name = "gpt-5.4-mini",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 750'000, .cached_read = 75'000, .output = 4'500'000, .cached_write = 750'000},
            },

            {
                .name = "gpt-5.4-nano",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 200'000, .cached_read = 20'000, .output = 1'250'000, .cached_write = 200'000},
            },

            {
                .name = "gpt-5.4-pro",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 30'000'000, .cached_read = 30'000'000, .output = 180'000'000, .cached_write = 30'000'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 60'000'000, .cached_read = 60'000'000, .output = 270'000'000, .cached_write = 60'000'000},
            },

            {
                .name = "gpt-5.3-codex",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 1'750'000, .cached_read = 175'000, .output = 14'000'000, .cached_write = 1'750'000},
            },

            {
                .name = "gpt-5.2",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 1'750'000, .cached_read = 175'000, .output = 14'000'000, .cached_write = 1'750'000},
            },

            {
                .name = "gpt-5.2-pro",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 21'000'000, .cached_read = 21'000'000, .output = 168'000'000, .cached_write = 21'000'000},
            },

            {
                .name = "gpt-5.2-codex",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 1'750'000, .cached_read = 175'000, .output = 14'000'000, .cached_write = 1'750'000},
            },

            {
                .name = "gpt-5.1",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 1'250'000, .cached_read = 125'000, .output = 10'000'000, .cached_write = 1'250'000},
            },

            {
                .name = "gpt-5.1-codex",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 1'250'000, .cached_read = 125'000, .output = 10'000'000, .cached_write = 1'250'000},
            },

            {
                .name = "gpt-5.1-codex-max",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 1'250'000, .cached_read = 125'000, .output = 10'000'000, .cached_write = 1'250'000},
            },

            {
                .name = "gpt-5.1-codex-mini",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 250'000, .cached_read = 25'000, .output = 2'000'000, .cached_write = 250'000},
            },

            {
                .name = "gpt-5",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 1'250'000, .cached_read = 125'000, .output = 10'000'000, .cached_write = 1'250'000},
            },

            {
                .name = "gpt-5-mini",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 250'000, .cached_read = 25'000, .output = 2'000'000, .cached_write = 250'000},
            },

            {
                .name = "gpt-5-nano",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 50'000, .cached_read = 5'000, .output = 400'000, .cached_write = 50'000},
            },

            {
                .name = "gpt-5-pro",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 15'000'000, .cached_read = 15'000'000, .output = 120'000'000, .cached_write = 15'000'000},
            },

            {
                .name = "gpt-5-codex",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 1'250'000, .cached_read = 125'000, .output = 10'000'000, .cached_write = 1'250'000},
            },

            {
                .name = "gpt-4.1",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 2'000'000, .cached_read = 500'000, .output = 8'000'000, .cached_write = 2'000'000},
            },

            {
                .name = "gpt-4.1-mini",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 400'000, .cached_read = 100'000, .output = 1'600'000, .cached_write = 400'000},
            },

            {
                .name = "gpt-4.1-nano",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 100'000, .cached_read = 25'000, .output = 400'000, .cached_write = 100'000},
            },

            {
                .name = "gpt-4o",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 2'500'000, .cached_read = 1'250'000, .output = 10'000'000, .cached_write = 2'500'000},
            },

            {
                .name = "gpt-4o-mini",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 150'000, .cached_read = 75'000, .output = 600'000, .cached_write = 150'000},
            },

            {
                .name = "o3",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 2'000'000, .cached_read = 500'000, .output = 8'000'000, .cached_write = 2'000'000},
            },

            {
                .name = "o3-pro",
                .protocols = PROTOCOL_RESPONSES,
                .rates = {.input = 20'000'000, .cached_read = 20'000'000, .output = 80'000'000, .cached_write = 20'000'000},
            },

            {
                .name = "o4-mini",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 1'100'000, .cached_read = 275'000, .output = 4'400'000, .cached_write = 1'100'000},
            },

            {
                .name = "o3-mini",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_RESPONSES,
                .rates = {.input = 1'100'000, .cached_read = 550'000, .output = 4'400'000, .cached_write = 1'100'000},
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

        std::optional<std::string_view> prepare_request(Protocol protocol, const pw::Headers& inbound_headers, pw::Headers& outbound_headers, SJSON::JSObject& body, const std::string& user_name) const override {
            if (protocol == PROTOCOL_RESPONSES) {
                if (auto background = body.find("background"); background != body.end()) {
                    if (!background->second.is_boolean()) return "background must be a boolean";
                    if (background->second.boolean()) return "Background Responses are not supported; later usage is not accounted for";
                }
            }

            if (auto tools = body.find("tools"); tools != body.end()) {
                if (auto error = validate_tools(tools->second)) return error;
            }

            if (protocol == PROTOCOL_RESPONSES) {
                if (auto input = body.find("input"); input != body.end() && input->second.is_array()) {
                    for (const auto& item : input->second.array()) {
                        if (!item.is_object()) continue;

                        auto type = item.object().find("type");
                        if (type == item.object().end() || !type->second.is_string() || type->second.string() != "additional_tools") continue;

                        auto tools = item.object().find("tools");
                        if (tools == item.object().end()) return "additional_tools input items must contain tools";
                        if (auto error = validate_tools(tools->second)) return error;
                    }
                }
            }

            return ConfiguredProvider::prepare_request(protocol, inbound_headers, outbound_headers, body, user_name);
        }
    };
} // namespace providers
