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

        virtual std::optional<std::string_view> prepare_request(Protocol protocol, const pw::Headers&, pw::Headers& outbound_headers, SJSON::JSObject&, const std::string&) const {
            if (protocol == PROTOCOL_CHAT_COMPLETIONS || protocol == PROTOCOL_RESPONSES) {
                outbound_headers["Authorization"] = "Bearer " + api_key;
            } else if (protocol == PROTOCOL_ANTHROPIC_MESSAGES) {
                outbound_headers["x-api-key"] = api_key;
            }

            return std::nullopt;
        }
    };
} // namespace providers
