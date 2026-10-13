#pragma once

#include "Polyweb/polyweb.hpp"
#include "SJSON/src/value.hpp"
#include "cost.hpp"
#include "providers.hpp"
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace providers {
    class ConfiguredProvider {
    protected:
        const Provider* provider;
        std::string api_key;

        explicit ConfiguredProvider(const Provider& provider):
            provider(&provider) {}

        static std::string read_api_key(SJSON::JSValue&& config) {
            if (config.is_string()) return std::move(config.string());

            return {};
        }

    public:
        virtual ~ConfiguredProvider() = default;

        const Provider& definition() const {
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
