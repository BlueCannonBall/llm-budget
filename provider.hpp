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
            auto entry = object.find("cache_control");
            if (entry == object.end() || !entry->second.is_object()) return;

            auto& cache_control = entry->second.object();
            auto type = cache_control.find("type");
            if (type != cache_control.end() && type->second.is_string() && type->second.string() == "ephemeral") {
                cache_control["ttl"] = "5m";
            }
        }

        static void normalize_content_cache_controls(SJSON::JSValue& content) {
            if (!content.is_array()) return;

            for (auto& block : content.array()) {
                if (!block.is_object()) continue;

                normalize_cache_control(block.object());
                if (auto nested = block.object().find("content"); nested != block.object().end()) {
                    normalize_content_cache_controls(nested->second);
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
                if (auto tier = body.find("service_tier"); tier != body.end()) {
                    if (!tier->second.is_string() || tier->second.string() != "default") {
                        return "Only standard service_tier=\"default\" is supported; other service-tier prices are not accounted for";
                    }
                }
                body["service_tier"] = "default";
            }

            if (protocol == PROTOCOL_ANTHROPIC_MESSAGES) {
                normalize_cache_control(body);
                for (const auto* field : {"system", "tools"}) {
                    if (auto value = body.find(field); value != body.end()) normalize_content_cache_controls(value->second);
                }
                if (auto messages = body.find("messages"); messages != body.end() && messages->second.is_array()) {
                    for (auto& message : messages->second.array()) {
                        if (!message.is_object()) continue;

                        if (auto content = message.object().find("content"); content != message.object().end()) {
                            normalize_content_cache_controls(content->second);
                        }
                    }
                }
            }

            if (protocol == PROTOCOL_RESPONSES) {
                if (auto background = body.find("background"); background != body.end()) {
                    if (!background->second.is_boolean()) return "background must be a boolean";
                    if (background->second.boolean()) return "Background Responses are not supported; later usage is not accounted for";
                }
            }

            if (protocol == PROTOCOL_CHAT_COMPLETIONS || protocol == PROTOCOL_RESPONSES) {
                outbound_headers["Authorization"] = "Bearer " + api_key;
            } else if (protocol == PROTOCOL_ANTHROPIC_MESSAGES) {
                outbound_headers["x-api-key"] = api_key;
                outbound_headers["anthropic-version"] = "2023-06-01";
                for (const auto* header : {"anthropic-version", "anthropic-beta"}) {
                    if (auto supplied = inbound_headers.find(header); supplied != inbound_headers.end()) {
                        outbound_headers[header] = supplied->second;
                    }
                }
            }

            if (protocol == PROTOCOL_CHAT_COMPLETIONS) {
                auto stream = body.find("stream");
                if (stream != body.end() && stream->second.is_boolean() && stream->second.boolean()) {
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
