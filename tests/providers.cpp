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

            body["tools"] = SJSON::JSArray {
                SJSON::JSObject {{"type", "web_search"}},
            };
            error = selection.provider->prepare_request(
                protocol, inbound_headers, outbound_headers, body, "alice");
            assert(!error.empty());
        }
    }
}
