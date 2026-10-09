#pragma once

#include "../configured_provider.hpp"
#include <stdexcept>
#include <stdint.h>

namespace providers {
    class OpenCodeGoProvider final : public ConfiguredProvider {
    protected:
        inline static constexpr Provider provider_definition {
            .name = "opencode-go",
            .chat_completions_base_url = "https://opencode.ai/zen/go/v1",
            .anthropic_messages_base_url = "https://opencode.ai/zen/go",
            .responses_base_url = "https://opencode.ai/zen/go/v1",
        };

        // Rates and thresholds: https://opencode.ai/v2/docs/console/go
        // Checked October 8, 2026. K means 1,000. A '-' cache-write rate uses input pricing.
        // Permit all proxy protocols; Go decides which combinations it supports.
        inline static constexpr unsigned permitted_protocols =
            PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_ANTHROPIC_MESSAGES | PROTOCOL_RESPONSES;

        inline static constexpr Model model_catalog[] {
            {
                .name = "glm-5.3-flash",
                .protocols = permitted_protocols,
                .rates = {.input = 150'000, .cached_read = 30'000, .output = 500'000, .cached_write = 150'000},
            },

            {
                .name = "glm-5.3",
                .protocols = permitted_protocols,
                .rates = {.input = 1'400'000, .cached_read = 260'000, .output = 4'400'000, .cached_write = 1'400'000},
            },

            {
                .name = "glm-5.2",
                .protocols = permitted_protocols,
                .rates = {.input = 1'400'000, .cached_read = 260'000, .output = 4'400'000, .cached_write = 1'400'000},
            },

            {
                .name = "kimi-k3",
                .protocols = permitted_protocols,
                .rates = {.input = 3'000'000, .cached_read = 300'000, .output = 15'000'000, .cached_write = 3'000'000},
            },

            {
                .name = "kimi-k2.7-code",
                .protocols = permitted_protocols,
                .rates = {.input = 950'000, .cached_read = 190'000, .output = 4'000'000, .cached_write = 950'000},
            },

            {
                .name = "kimi-k2.6",
                .protocols = permitted_protocols,
                .rates = {.input = 950'000, .cached_read = 160'000, .output = 4'000'000, .cached_write = 950'000},
            },

            {
                .name = "longcat-2.0",
                .protocols = permitted_protocols,
                .rates = {.input = 300'000, .cached_read = 6'000, .output = 1'200'000, .cached_write = 300'000},
            },

            {
                .name = "longcat-2.5-preview-free",
                .protocols = permitted_protocols,
                .rates = {.input = 0, .cached_read = 0, .output = 0, .cached_write = 0},
            },

            {
                .name = "mimo-v2.6-flash",
                .protocols = permitted_protocols,
                .rates = {.input = 140'000, .cached_read = 2'800, .output = 280'000, .cached_write = 140'000},
            },

            {
                .name = "mimo-v2.6-pro",
                .protocols = permitted_protocols,
                .rates = {.input = 435'000, .cached_read = 3'625, .output = 870'000, .cached_write = 435'000},
            },

            {
                .name = "mimo-v2.5",
                .protocols = permitted_protocols,
                .rates = {.input = 140'000, .cached_read = 2'800, .output = 280'000, .cached_write = 140'000},
            },

            {
                .name = "mimo-v2.5-pro",
                .protocols = permitted_protocols,
                .rates = {.input = 435'000, .cached_read = 3'625, .output = 870'000, .cached_write = 435'000},
            },

            {
                .name = "minimax-m3",
                .protocols = permitted_protocols,
                .rates = {.input = 300'000, .cached_read = 60'000, .output = 1'200'000, .cached_write = 300'000},
            },

            {
                .name = "minimax-m2.7",
                .protocols = permitted_protocols,
                .rates = {.input = 300'000, .cached_read = 60'000, .output = 1'200'000, .cached_write = 375'000},
            },

            {
                .name = "muse-spark-1.3-contributor",
                .protocols = permitted_protocols,
                .rates = {.input = 100'000, .cached_read = 2'000, .output = 200'000, .cached_write = 100'000},
            },

            {
                .name = "muse-spark-1.2-contributor",
                .protocols = permitted_protocols,
                .rates = {.input = 100'000, .cached_read = 2'000, .output = 200'000, .cached_write = 100'000},
            },

            {
                .name = "qwen3.8-max",
                .protocols = permitted_protocols,
                .rates = {.input = 2'000'000, .cached_read = 250'000, .output = 6'000'000, .cached_write = 2'500'000},
            },

            {
                .name = "qwen3.8-flash",
                .protocols = permitted_protocols,
                .rates = {.input = 150'000, .cached_read = 16'000, .output = 470'000, .cached_write = 200'000},
            },

            {
                .name = "qwen3.7-plus",
                .protocols = permitted_protocols,
                .rates = {.input = 400'000, .cached_read = 40'000, .output = 1'600'000, .cached_write = 500'000},
                .input_token_threshold = 256'000,
                .above_threshold_rates = {.input = 1'200'000, .cached_read = 120'000, .output = 4'800'000, .cached_write = 1'500'000},
            },

            {
                .name = "deepseek-v4.1-flash",
                .protocols = permitted_protocols,
                .rates = {.input = 150'000, .cached_read = 3'000, .output = 600'000, .cached_write = 150'000},
                .schedule = SCHEDULE_WEEKDAY_UTC_DOUBLED,
            },

            {
                .name = "deepseek-v4-pro",
                .protocols = permitted_protocols,
                .rates = {.input = 660'000, .cached_read = 22'000, .output = 1'980'000, .cached_write = 660'000},
                .schedule = SCHEDULE_WEEKDAY_UTC_DOUBLED,
            },

            {
                .name = "deepseek-v4-flash",
                .protocols = permitted_protocols,
                .rates = {.input = 150'000, .cached_read = 3'000, .output = 600'000, .cached_write = 150'000},
                .schedule = SCHEDULE_WEEKDAY_UTC_DOUBLED,
            },

            {
                .name = "deepseek-v4-flash-vision-exp",
                .protocols = permitted_protocols,
                .rates = {.input = 150'000, .cached_read = 3'000, .output = 600'000, .cached_write = 150'000},
                .schedule = SCHEDULE_WEEKDAY_UTC_DOUBLED,
            },

            {
                .name = "hy4-preview",
                .protocols = permitted_protocols,
                .rates = {.input = 834'000, .cached_read = 42'000, .output = 2'501'000, .cached_write = 834'000},
            },

            {
                .name = "hy3",
                .protocols = permitted_protocols,
                .rates = {.input = 140'000, .cached_read = 35'000, .output = 580'000, .cached_write = 140'000},
            },

            {
                .name = "space-bunny",
                .protocols = permitted_protocols,
                .rates = {.input = 150'000, .cached_read = 30'000, .output = 600'000, .cached_write = 150'000},
            },

            {
                .name = "grok-4.7",
                .protocols = permitted_protocols,
                .rates = {.input = 2'000'000, .cached_read = 500'000, .output = 6'000'000, .cached_write = 2'000'000},
                .input_token_threshold = 200'000,
                .above_threshold_rates = {.input = 4'000'000, .cached_read = 1'000'000, .output = 12'000'000, .cached_write = 4'000'000},
            },

            {
                .name = "grok-4.6",
                .protocols = permitted_protocols,
                .rates = {.input = 2'000'000, .cached_read = 500'000, .output = 6'000'000, .cached_write = 2'000'000},
                .input_token_threshold = 200'000,
                .above_threshold_rates = {.input = 4'000'000, .cached_read = 1'000'000, .output = 12'000'000, .cached_write = 4'000'000},
            },

            {
                .name = "claude-haiku-5-5",
                .protocols = permitted_protocols,
                .rates = {.input = 100'000, .cached_read = 10'000, .output = 500'000, .cached_write = 125'000},
                .input_token_threshold = 100'000,
                .above_threshold_rates = {.input = 500'000, .cached_read = 50'000, .output = 2'500'000, .cached_write = 625'000},
            },

            {
                .name = "gpt-6-luna",
                .protocols = permitted_protocols,
                .rates = {.input = 100'000, .cached_read = 10'000, .output = 500'000, .cached_write = 125'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 200'000, .cached_read = 20'000, .output = 750'000, .cached_write = 250'000},
            },

            {
                .name = "gpt-5.6-luna",
                .protocols = permitted_protocols,
                .rates = {.input = 200'000, .cached_read = 20'000, .output = 1'200'000, .cached_write = 250'000},
                .input_token_threshold = 272'000,
                .above_threshold_rates = {.input = 400'000, .cached_read = 40'000, .output = 1'800'000, .cached_write = 500'000},
            },
        };

        enum GoPlan {
            GO_PLAN_GO,
            GO_PLAN_GO_PLUS,
        };

        struct GoAllowance {
            std::string_view model;
            uint16_t go_monthly_usd;
            uint16_t go_plus_monthly_usd;
        };

        // Separate plan allowances from token rates. Free models have no cap.
        // Both columns: https://opencode.ai/v2/docs/console/go
        inline static constexpr GoAllowance go_allowances[] {
            {.model = "glm-5.3-flash", .go_monthly_usd = 60, .go_plus_monthly_usd = 180},
            {.model = "glm-5.3", .go_monthly_usd = 15, .go_plus_monthly_usd = 120},
            {.model = "glm-5.2", .go_monthly_usd = 60, .go_plus_monthly_usd = 180},
            {.model = "kimi-k3", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "kimi-k2.7-code", .go_monthly_usd = 60, .go_plus_monthly_usd = 180},
            {.model = "kimi-k2.6", .go_monthly_usd = 60, .go_plus_monthly_usd = 240},
            {.model = "longcat-2.0", .go_monthly_usd = 60, .go_plus_monthly_usd = 240},
            {.model = "longcat-2.5-preview-free", .go_monthly_usd = 0, .go_plus_monthly_usd = 0},
            {.model = "mimo-v2.6-flash", .go_monthly_usd = 60, .go_plus_monthly_usd = 120},
            {.model = "mimo-v2.6-pro", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "mimo-v2.5", .go_monthly_usd = 60, .go_plus_monthly_usd = 120},
            {.model = "mimo-v2.5-pro", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "minimax-m3", .go_monthly_usd = 60, .go_plus_monthly_usd = 180},
            {.model = "minimax-m2.7", .go_monthly_usd = 60, .go_plus_monthly_usd = 240},
            {.model = "muse-spark-1.3-contributor", .go_monthly_usd = 60, .go_plus_monthly_usd = 120},
            {.model = "muse-spark-1.2-contributor", .go_monthly_usd = 60, .go_plus_monthly_usd = 120},
            {.model = "qwen3.8-max", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "qwen3.8-flash", .go_monthly_usd = 30, .go_plus_monthly_usd = 90},
            {.model = "qwen3.7-plus", .go_monthly_usd = 60, .go_plus_monthly_usd = 180},
            {.model = "deepseek-v4.1-flash", .go_monthly_usd = 60, .go_plus_monthly_usd = 120},
            {.model = "deepseek-v4-pro", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "deepseek-v4-flash", .go_monthly_usd = 30, .go_plus_monthly_usd = 120},
            {.model = "deepseek-v4-flash-vision-exp", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "hy4-preview", .go_monthly_usd = 30, .go_plus_monthly_usd = 120},
            {.model = "hy3", .go_monthly_usd = 60, .go_plus_monthly_usd = 240},
            {.model = "space-bunny", .go_monthly_usd = 30, .go_plus_monthly_usd = 120},
            {.model = "grok-4.7", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "grok-4.6", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "claude-haiku-5-5", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "gpt-6-luna", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
            {.model = "gpt-5.6-luna", .go_monthly_usd = 15, .go_plus_monthly_usd = 60},
        };

        GoPlan go_plan = GO_PLAN_GO;

    public:
        OpenCodeGoProvider():
            ConfiguredProvider(provider_definition) {}

        explicit OpenCodeGoProvider(SJSON::JSValue&& config):
            ConfiguredProvider(provider_definition) {
            if (!config.is_object()) {
                throw std::invalid_argument("opencode-go must contain api_key and plan");
            }

            auto& object = config.object();
            auto api_key_it = object.find("api_key");
            auto plan_it = object.find("plan");
            if (api_key_it == object.end() || !api_key_it->second.is_string() || api_key_it->second.string().empty() || plan_it == object.end() || !plan_it->second.is_string()) {
                throw std::invalid_argument("opencode-go requires a nonempty api_key and string plan");
            }

            if (plan_it->second.string() == "go") {
                go_plan = GO_PLAN_GO;
            } else if (plan_it->second.string() == "go-plus") {
                go_plan = GO_PLAN_GO_PLUS;
            } else {
                throw std::invalid_argument("opencode-go plan must be go or go-plus");
            }

            api_key = std::move(api_key_it->second.string());
        }

        std::span<const Model> models() const override {
            return model_catalog;
        }

        std::optional<cost::Multiplier> multiplier(const Model& model) const override {
            if (!configured()) return std::nullopt;

            for (const auto& allowance : go_allowances) {
                if (allowance.model != model.name) continue;

                uint64_t monthly_usd;
                uint64_t plan_usd;
                if (go_plan == GO_PLAN_GO) {
                    monthly_usd = allowance.go_monthly_usd;
                    plan_usd = 10;
                } else {
                    monthly_usd = allowance.go_plus_monthly_usd;
                    plan_usd = 40;
                }
                if (!monthly_usd) return cost::Multiplier {0, 1};

                return cost::Multiplier {plan_usd, monthly_usd};
            }

            return std::nullopt;
        }

        std::optional<std::string_view> prepare_request(Protocol protocol, const pw::Headers& inbound_headers, pw::Headers& outbound_headers, SJSON::JSObject& body, const std::string& user_name) const override {
            ConfiguredProvider::prepare_request(protocol, inbound_headers, outbound_headers, body, user_name);

            auto user_agent = inbound_headers.find("User-Agent");
            if (user_agent != inbound_headers.end() && !user_agent->second.empty()) {
                outbound_headers["User-Agent"] = user_agent->second;
            } else {
                outbound_headers["User-Agent"] = "llm-budget/1.0";
            }

            for (const auto* header : {"x-opencode-session", "X-Claude-Code-Session-Id", "session-id", "thread-id"}) {
                if (auto session = inbound_headers.find(header); session != inbound_headers.end()) {
                    outbound_headers[session->first] = session->second;
                }
            }

            return std::nullopt;
        }
    };
} // namespace providers
