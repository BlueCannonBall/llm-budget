#include "provider_registry.hpp"
#include <cassert>
#include <type_traits>

int main() {
    auto configured = providers::configure({
        {"deepseek", "test-deepseek"},
        {"openai", "test-openai"},
        {"opencode-go", SJSON::JSObject {
            {"api_key", "test-go"},
            {"plan", "go"},
        }},
    });

    providers::ResolvedModel empty;
    assert(!empty);
    assert(empty.provider == nullptr);
    assert(empty.model == nullptr);

    auto direct = providers::resolve(configured, "deepseek-flash");
    static_assert(std::is_same_v<decltype(direct), providers::ResolvedModel>);
    assert(direct);
    assert(direct.provider->definition().name == "deepseek");
    assert(direct.model->name == "deepseek-flash");

    for (std::string_view name : {
             "unknown/deepseek-flash",
             "deepseek/unknown-model",
             "opencode-go/",
             "gpt-4.1-mini",
         }) {
        auto unresolved = providers::resolve(configured, name);
        assert(!unresolved);
        assert(unresolved.provider == nullptr);
        assert(unresolved.model == nullptr);
    }

    for (std::string_view name : {
             "deepseek/deepseek-flash",
             "openai/gpt-4.1-mini",
             "opencode-go/gpt-5.6-luna",
         }) {
        auto selection = providers::resolve(configured, name);
        assert(selection);

        for (auto protocol : {
                 providers::PROTOCOL_CHAT_COMPLETIONS,
                 providers::PROTOCOL_RESPONSES,
                 providers::PROTOCOL_ANTHROPIC_MESSAGES,
             }) {
            if (!selection.model->supports(protocol)) {
                continue;
            }

            pw::Headers inbound_headers;
            pw::Headers outbound_headers;
            SJSON::JSObject body;
            auto error = selection.provider->prepare_request(
                protocol, inbound_headers, outbound_headers, body, "alice");
            static_assert(std::is_same_v<decltype(error), std::string_view>);
            assert(error.empty());
            assert(!body.contains("prompt_cache_options"));

            body["tools"] = SJSON::JSArray {
                SJSON::JSObject {{"type", "web_search"}},
            };
            error = selection.provider->prepare_request(
                protocol, inbound_headers, outbound_headers, body, "alice");
            assert(!error.empty());
        }
    }

    for (std::string_view name : {
             "deepseek/deepseek-flash",
             "openai/gpt-6.1-sol",
             "opencode-go/gpt-5.6-luna",
         }) {
        auto selection = providers::resolve(configured, name);
        assert(selection);
        pw::Headers inbound_headers;
        pw::Headers outbound_headers;

        SJSON::JSObject default_options {
            {"prompt_cache_options", SJSON::JSObject {{"mode", "implicit"}}},
            {"input", "plain text"},
        };
        assert(selection.provider->prepare_request(
            providers::PROTOCOL_RESPONSES, inbound_headers, outbound_headers, default_options, "alice").empty());
        assert(default_options.at("prompt_cache_options").object().at("ttl").string() == "30m");
        assert(default_options.at("prompt_cache_options").object().at("mode").string() == "implicit");
        assert(default_options.at("input").string() == "plain text");

        SJSON::JSObject legacy {
            {"prompt_cache_retention", "24h"},
            {"messages", SJSON::JSArray {}},
        };
        assert(selection.provider->prepare_request(
            providers::PROTOCOL_CHAT_COMPLETIONS, inbound_headers, outbound_headers, legacy, "alice").empty());
        assert(legacy.at("prompt_cache_retention").string() == "24h");
        assert(!legacy.contains("prompt_cache_options"));

        for (const auto* ttl : {"12h", "30m"}) {
            SJSON::JSObject marker {{"mode", "explicit"}, {"ttl", ttl}};
            SJSON::JSArray content {
                SJSON::JSObject {
                    {"type", "input_text"}, {"text", "stable"},
                    {"prompt_cache_breakpoint", marker},
                },
                SJSON::JSObject {
                    {"type", "input_text"}, {"text", "default lifetime"},
                    {"prompt_cache_breakpoint", SJSON::JSObject {{"mode", "explicit"}}},
                },
                SJSON::JSObject {{"type", "input_text"}, {"text", "dynamic"}},
            };
            SJSON::JSObject body {
                {"prompt_cache_options", SJSON::JSObject {
                    {"mode", "explicit"}, {"ttl", ttl}, {"prewarm", true},
                }},
                {"prompt_cache_retention", "24h"},
                {"input", SJSON::JSArray {
                    SJSON::JSObject {{"role", "developer"}, {"content", content}},
                    SJSON::JSObject {{"role", "user"}, {"content", content}},
                    SJSON::JSObject {
                        {"type", "function_call"}, {"arguments", "{\"ttl\":\"12h\"}"},
                        {"content", content},
                    },
                }},
            };
            auto error = selection.provider->prepare_request(
                providers::PROTOCOL_RESPONSES, inbound_headers, outbound_headers, body, "alice");
            assert(error.empty());
            const auto& options = body.at("prompt_cache_options").object();
            assert(options.at("ttl").string() == "30m");
            assert(options.at("mode").string() == "explicit");
            assert(options.at("prewarm").boolean());
            assert(body.at("prompt_cache_retention").string() == "24h");
            const auto& input = body.at("input").array();
            for (size_t i = 0; i < 2; ++i) {
                const auto& blocks = input[i].object().at("content").array();
                for (size_t j = 0; j < 2; ++j) {
                    const auto& breakpoint_object = blocks[j].object().at("prompt_cache_breakpoint").object();
                    assert(breakpoint_object.at("ttl").string() == "30m");
                    assert(breakpoint_object.at("mode").string() == "explicit");
                }
                assert(!blocks[2].object().contains("prompt_cache_breakpoint"));
            }
            assert(input[2].object().at("arguments").string() == "{\"ttl\":\"12h\"}");
            const auto& untouched = input[2].object().at("content").array();
            assert(untouched[0].object().at("prompt_cache_breakpoint").object().at("ttl").string() == ttl);

            // A breakpoint override must be normalized even without top-level options.
            body.erase("prompt_cache_options");
            body["input"] = SJSON::JSArray {
                SJSON::JSObject {{"role", "user"}, {"content", content}},
            };
            error = selection.provider->prepare_request(
                providers::PROTOCOL_RESPONSES, inbound_headers, outbound_headers, body, "alice");
            assert(error.empty());
            assert(!body.contains("prompt_cache_options"));
            const auto& blocks = body.at("input").array()[0].object().at("content").array();
            assert(blocks[0].object().at("prompt_cache_breakpoint").object().at("ttl").string() == "30m");
        }
    }
}
