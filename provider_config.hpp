#pragma once

#include "configured_provider.hpp"
#include "providers/deepseek.hpp"
#include "providers/openai.hpp"
#include "providers/opencode_go.hpp"
#include <array>
#include <memory>
#include <optional>
#include <stddef.h>
#include <string>
#include <string_view>
#include <utility>

namespace providers {
    using Configuration = std::array<std::unique_ptr<ConfiguredProvider>, 3>;

    struct ResolvedModel {
        const ConfiguredProvider* provider;
        const Model* model;
    };

    namespace detail {
        template <typename T>
        std::unique_ptr<ConfiguredProvider> make_provider(SJSON::JSObject& keys, std::string_view name) {
            if (auto key = keys.find(std::string(name)); key != keys.end()) {
                return std::make_unique<T>(std::move(key->second));
            }

            return std::make_unique<T>();
        }
    } // namespace detail

    inline Configuration configure(SJSON::JSObject keys) {
        return {
            detail::make_provider<DeepSeekProvider>(keys, "deepseek"),
            detail::make_provider<OpenCodeGoProvider>(keys, "opencode-go"),
            detail::make_provider<OpenAIProvider>(keys, "openai"),
        };
    }

    inline const ConfiguredProvider* find_provider(const Configuration& configured, std::string_view name) {
        for (const auto& provider : configured) {
            if (provider && provider->definition().name == name) return provider.get();
        }

        return nullptr;
    }

    inline std::optional<ResolvedModel> resolve(const Configuration& configured, std::string_view name) {
        std::string_view provider_name = "deepseek";
        std::string_view model_name = name;
        if (size_t slash = name.find('/'); slash != std::string_view::npos) {
            provider_name = name.substr(0, slash);
            model_name = name.substr(slash + 1);
        }

        const ConfiguredProvider* provider = find_provider(configured, provider_name);
        if (!provider) return std::nullopt;

        const Model* model = provider->find_model(model_name);
        if (!model) return std::nullopt;

        return ResolvedModel {provider, model};
    }
} // namespace providers
