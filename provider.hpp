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
        static std::string_view validate_tools(Protocol protocol, const SJSON::JSValue& tools) {
            if (!tools.is_array()) {
                return "tools must be an array";
            }

            for (const auto& tool : tools.array()) {
                if (!tool.is_object()) {
                    return "Each tool must be an object";
                }

                if (protocol == PROTOCOL_ANTHROPIC_MESSAGES) {
                    if (auto type_it = tool.object().find("type"); type_it != tool.object().end()) {
                        if (!type_it->second.is_string()) {
                            return "Tool type must be a string";
                        }
                        if (type_it->second.string() != "custom") {
                            return "Only client-executed tools are supported; "
                                   "hosted tool charges are not accounted for";
                        }
                    }

                    if (auto name_it = tool.object().find("name");
                        name_it == tool.object().end() || !name_it->second.is_string()) {
                        return "Client-executed Messages tools must have a string name";
                    }

                    if (auto schema_it = tool.object().find("input_schema");
                        schema_it == tool.object().end() || !schema_it->second.is_object()) {
                        return "Client-executed Messages tools must have an input_schema object";
                    }
                } else {
                    if (auto type_it = tool.object().find("type");
                        type_it != tool.object().end() && type_it->second.is_string()) {
                        if (type_it->second.string() != "function" &&
                            type_it->second.string() != "custom") {
                            return "Only client-executed function and custom tools are supported; "
                                   "hosted tool charges are not accounted for";
                        }
                    } else {
                        return "Each tool must have a string type";
                    }
                }
            }

            return {};
        }

        static std::string_view validate_request_tools(Protocol protocol, const SJSON::JSObject& body) {
            if (auto tools_it = body.find("tools"); tools_it != body.end()) {
                if (auto error = validate_tools(protocol, tools_it->second); !error.empty()) {
                    return error;
                }
            }

            if (body.contains("web_search_options")) {
                return "Hosted web search is not supported; tool charges are not accounted for";
            }

            if (auto servers_it = body.find("mcp_servers"); servers_it != body.end()) {
                if (!servers_it->second.is_array() || !servers_it->second.array().empty()) {
                    return "Hosted MCP servers are not supported; tool charges are not accounted for";
                }
            }

            if (protocol == PROTOCOL_RESPONSES) {
                if (auto input_it = body.find("input");
                    input_it != body.end() && input_it->second.is_array()) {
                    for (const auto& item : input_it->second.array()) {
                        if (!item.is_object()) {
                            continue;
                        }

                        if (auto type_it = item.object().find("type");
                            type_it == item.object().end() ||
                            !type_it->second.is_string() ||
                            type_it->second.string() != "additional_tools") {
                            continue;
                        }

                        if (auto tools_it = item.object().find("tools"); tools_it != item.object().end()) {
                            if (auto error = validate_tools(protocol, tools_it->second); !error.empty()) {
                                return error;
                            }
                        } else {
                            return "additional_tools input items must contain tools";
                        }
                    }
                }
            }

            return {};
        }

        static std::string_view normalize_service_tier(SJSON::JSObject& body) {
            if (auto service_tier_it = body.find("service_tier"); service_tier_it != body.end()) {
                if (!service_tier_it->second.is_string() || service_tier_it->second.string() != "default") {
                    return "Only standard service_tier=\"default\" is supported; other service-tier prices are not accounted for";
                }
            }
            body["service_tier"] = "default";
            return {};
        }

        static void normalize_messages_cache_control(SJSON::JSObject& object) {
            if (auto cache_control_it = object.find("cache_control"); cache_control_it != object.end() && cache_control_it->second.is_object()) {
                auto& cache_control = cache_control_it->second.object();
                if (auto type_it = cache_control.find("type"); type_it != cache_control.end() && type_it->second.is_string() && type_it->second.string() == "ephemeral") {
                    cache_control["ttl"] = "5m";
                }
            }
        }

        static void normalize_messages_content_cache_controls(SJSON::JSValue& content) {
            if (!content.is_array()) {
                return;
            }

            for (auto& block : content.array()) {
                if (!block.is_object()) {
                    continue;
                }

                normalize_messages_cache_control(block.object());
                if (auto content_it = block.object().find("content"); content_it != block.object().end()) {
                    normalize_messages_content_cache_controls(content_it->second);
                }
            }
        }

        static void normalize_messages_cache_controls(SJSON::JSObject& body) {
            normalize_messages_cache_control(body);
            for (const auto* field : {"system", "tools"}) {
                if (auto field_it = body.find(field); field_it != body.end()) {
                    normalize_messages_content_cache_controls(field_it->second);
                }
            }
            if (auto messages_it = body.find("messages"); messages_it != body.end() && messages_it->second.is_array()) {
                for (auto& message : messages_it->second.array()) {
                    if (!message.is_object()) {
                        continue;
                    }

                    if (auto content_it = message.object().find("content"); content_it != message.object().end()) {
                        normalize_messages_content_cache_controls(content_it->second);
                    }
                }
            }
        }

        static void normalize_responses_cache_controls(SJSON::JSObject& body) {
            // Match the catalog's 30-minute cache-write rates. Do not add
            // caching options or breakpoints where the caller omitted them.
            if (auto options_it = body.find("prompt_cache_options");
                options_it != body.end() && options_it->second.is_object()) {
                options_it->second.object()["ttl"] = "30m";
            }
            if (auto input_it = body.find("input");
                input_it != body.end() && input_it->second.is_array()) {
                for (auto& item : input_it->second.array()) {
                    if (!item.is_object()) {
                        continue;
                    }

                    auto& message = item.object();
                    if (auto role_it = message.find("role");
                        role_it == message.end() || !role_it->second.is_string()) {
                        continue;
                    }
                    if (auto content_it = message.find("content");
                        content_it != message.end() && content_it->second.is_array()) {
                        for (auto& block : content_it->second.array()) {
                            if (!block.is_object()) {
                                continue;
                            }

                            if (auto breakpoint_it = block.object().find("prompt_cache_breakpoint");
                                breakpoint_it != block.object().end() && breakpoint_it->second.is_object()) {
                                breakpoint_it->second.object()["ttl"] = "30m";
                            }
                        }
                    }
                }
            }
        }

        static std::string_view validate_responses_background(const SJSON::JSObject& body) {
            if (auto background_it = body.find("background"); background_it != body.end()) {
                if (!background_it->second.is_boolean()) {
                    return "background must be a boolean";
                }
                if (background_it->second.boolean()) {
                    return "Background Responses are not supported; later usage is not accounted for";
                }
            }
            return {};
        }

        void prepare_headers(Protocol protocol, const pw::Headers& inbound_headers, pw::Headers& outbound_headers) const {
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
        }

        static std::string_view normalize_chat_stream_options(SJSON::JSObject& body) {
            if (auto stream_it = body.find("stream"); stream_it != body.end() && stream_it->second.is_boolean() && stream_it->second.boolean()) {
                auto& options = body["stream_options"];
                if (options.is_null()) {
                    options = SJSON::JSObject {};
                } else if (!options.is_object()) {
                    return "stream_options must be an object";
                }
                options.object()["include_usage"] = true;
            }
            return {};
        }

        const ProviderDefinition* provider;
        std::string api_key;

        explicit Provider(const ProviderDefinition& provider):
            provider(&provider) {}

        static std::string read_api_key(SJSON::JSValue&& config) {
            if (config.is_string()) {
                return std::move(config.string());
            }

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
                if (model.name == name) {
                    return &model;
                }
            }

            return nullptr;
        }

        virtual std::optional<cost::Multiplier> multiplier(const Model&) const {
            if (!configured()) {
                return std::nullopt;
            }

            return cost::Multiplier {1, 1};
        }

        // An empty view indicates success; otherwise the view contains an error message.
        virtual std::string_view prepare_request(Protocol protocol, const pw::Headers& inbound_headers, pw::Headers& outbound_headers, SJSON::JSObject& body, const std::string&) const {
            if (auto error = validate_request_tools(protocol, body); !error.empty()) {
                return error;
            }

            if (protocol == PROTOCOL_CHAT_COMPLETIONS || protocol == PROTOCOL_RESPONSES) {
                if (auto error = normalize_service_tier(body); !error.empty()) {
                    return error;
                }
            }

            switch (protocol) {
            case PROTOCOL_ANTHROPIC_MESSAGES:
                normalize_messages_cache_controls(body);
                break;

            case PROTOCOL_RESPONSES:
                normalize_responses_cache_controls(body);
                if (auto error = validate_responses_background(body); !error.empty()) {
                    return error;
                }
                break;

            case PROTOCOL_CHAT_COMPLETIONS:
                break;
            }

            prepare_headers(protocol, inbound_headers, outbound_headers);
            if (protocol == PROTOCOL_CHAT_COMPLETIONS) {
                return normalize_chat_stream_options(body);
            }

            return {};
        }
    };
} // namespace providers
