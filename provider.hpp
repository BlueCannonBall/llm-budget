#pragma once

#include "Polyweb/polyweb.hpp"
#include "SJSON/src/value.hpp"
#include "cost.hpp"
#include "provider_types.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace providers {
    class Provider {
    protected:
        static void normalize_cache_control(SJSON::JSObject& object) {
            if (auto cache_control_it = object.find("cache_control"); cache_control_it != object.end() && cache_control_it->second.is_object()) {
                auto& cache_control = cache_control_it->second.object();
                if (auto type_it = cache_control.find("type"); type_it != cache_control.end() && type_it->second.is_string() && type_it->second.string() == "ephemeral") {
                    cache_control["ttl"] = "5m";
                }
            }
        }

        static void normalize_content_cache_controls(SJSON::JSValue& content) {
            if (!content.is_array()) return;

            for (auto& block : content.array()) {
                if (!block.is_object()) continue;

                normalize_cache_control(block.object());
                if (auto content_it = block.object().find("content"); content_it != block.object().end()) {
                    normalize_content_cache_controls(content_it->second);
                }
            }
        }

        const ProviderDefinition* provider;
        std::string api_key;

        explicit Provider(const ProviderDefinition& provider):
            provider(&provider) {}

        static std::string read_api_key(SJSON::JSValue&& config) {
            if (config.is_string()) return std::move(config.string());

            return {};
        }

    public:
        virtual ~Provider() = default;

        const ProviderDefinition& definition() const {
            return *provider;
        }

        bool configured() const {
            return !api_key.empty();
        }

        // Catalog records and provider metadata have static storage duration.
        virtual std::span<const Model> models() const = 0;

        const Model* find_model(std::string_view name) const {
            for (const auto& model : models()) {
                if (model.name == name) return &model;
            }

            return nullptr;
        }

        virtual std::optional<cost::Multiplier> multiplier(const Model&) const {
            if (!configured()) return std::nullopt;

            return cost::Multiplier {1, 1};
        }

        virtual std::optional<std::string_view> prepare_request(Protocol protocol, const pw::Headers& inbound_headers, pw::Headers& outbound_headers, SJSON::JSObject& body, const std::string&) const {
            if (protocol == PROTOCOL_CHAT_COMPLETIONS || protocol == PROTOCOL_RESPONSES) {
                if (auto service_tier_it = body.find("service_tier"); service_tier_it != body.end()) {
                    if (!service_tier_it->second.is_string() || service_tier_it->second.string() != "default") {
                        return "Only standard service_tier=\"default\" is supported; other service-tier prices are not accounted for";
                    }
                }
                body["service_tier"] = "default";
            }

            if (protocol == PROTOCOL_ANTHROPIC_MESSAGES) {
                normalize_cache_control(body);
                for (const auto* field : {"system", "tools"}) {
                    if (auto field_it = body.find(field); field_it != body.end()) normalize_content_cache_controls(field_it->second);
                }
                if (auto messages_it = body.find("messages"); messages_it != body.end() && messages_it->second.is_array()) {
                    for (auto& message : messages_it->second.array()) {
                        if (!message.is_object()) continue;

                        if (auto content_it = message.object().find("content"); content_it != message.object().end()) {
                            normalize_content_cache_controls(content_it->second);
                        }
                    }
                }
            }

            if (protocol == PROTOCOL_RESPONSES) {
                if (auto background_it = body.find("background"); background_it != body.end()) {
                    if (!background_it->second.is_boolean()) return "background must be a boolean";
                    if (background_it->second.boolean()) return "Background Responses are not supported; later usage is not accounted for";
                }
            }

            if (protocol == PROTOCOL_CHAT_COMPLETIONS || protocol == PROTOCOL_RESPONSES) {
                outbound_headers["Authorization"] = "Bearer " + api_key;
            } else if (protocol == PROTOCOL_ANTHROPIC_MESSAGES) {
                outbound_headers["x-api-key"] = api_key;
                outbound_headers["anthropic-version"] = "2023-06-01";
                for (const auto* header : {"anthropic-version", "anthropic-beta"}) {
                    if (auto header_it = inbound_headers.find(header); header_it != inbound_headers.end()) {
                        outbound_headers[header] = header_it->second;
                    }
                }
            }

            if (protocol == PROTOCOL_CHAT_COMPLETIONS) {
                if (auto stream_it = body.find("stream"); stream_it != body.end() && stream_it->second.is_boolean() && stream_it->second.boolean()) {
                    auto& options = body["stream_options"];
                    if (options.is_null()) {
                        options = SJSON::JSObject {};
                    } else if (!options.is_object()) {
                        return "stream_options must be an object";
                    }
                    options.object()["include_usage"] = true;
                }
            }

            return std::nullopt;
        }
    };
} // namespace providers
