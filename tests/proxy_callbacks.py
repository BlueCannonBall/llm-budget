"""Run with python3 tests/proxy_callbacks.py; requires a C++23 compiler.

Compile the actual JSON/SSE callbacks and request-isolation block from main.cpp
with recording storage stubs. No upstream traffic or real database is used.
"""

import pathlib
import re
import subprocess
import tempfile


root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "main.cpp").read_text()
sse_callbacks = re.findall(r"pw::SSEParser sse_parser\(.*?\n                        \}\);", source, re.S)
json_callbacks = re.findall(r'SJSON::Parse json_parser;\n                        json_parser.listen\("usage",.*?\n                        \}\);', source, re.S)
assert len(sse_callbacks) == len(json_callbacks) == 2
service_definition = re.search(r"struct Service \{.*?\n\};", source, re.S).group()
service_lookup = re.search(r"std::optional<Service> model_to_service\(.*?\n\}", source, re.S).group()
record_cost_definition = re.search(r"void record_request_cost\(.*?\n\}", source, re.S).group()
metadata_block = re.search(r'if \(service->name == "deepseek"\) \{\n                    auto& metadata.*?\n                \}', source, re.S).group()
upstream_urls = re.findall(r'pw::fetch\("POST", (service\.\w+ \+ "[^"]+")', source)
assert len(upstream_urls) == 2

cpp = r'''
#include "Polyweb/sse.hpp"
#include "SJSON/src/sjson.hpp"
#include "cost.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#define SPDLOG_WARN(...) ((void)0)
struct User { std::string name; };
using request_id_t = std::int64_t;
std::vector<std::uint64_t> recorded;
std::vector<cost::TokenUsage> printed;
void update_request(std::uint64_t, std::uint64_t amount) { recorded.push_back(amount); }
void print_cost(std::uint64_t, const User&, std::uint64_t, std::string_view, std::string_view, const cost::TokenUsage& usage) { printed.push_back(usage); }
int make_basic_resp(int status, const std::string&) { return status; }
'''
cpp += service_definition + "\n" + service_lookup + "\n"
cpp += record_cost_definition + "\n"
cpp += "int isolate_metadata(SJSON::JSObject& req_body) {\n"
cpp += 'std::optional<User> user = User {"alice"}; auto service = model_to_service("deepseek-v4-pro");\n'
cpp += metadata_block + "\nreturn 200;\n}\n"
for name, expression in zip(("chat_url", "anthropic_url"), upstream_urls):
    cpp += f"std::string {name}(const Service& service) {{ return {expression}; }}\n"

context = r'''
    using namespace std::chrono;
    User user {"alice"};
    std::string model = "deepseek-v4-pro";
    Service service = *model_to_service(model);
    const auto now = sys_days {2026y / September / 27} + 12h;
    std::optional<std::uint64_t> request_id = 1;
    recorded.clear();
'''
for protocol, callback in zip(("chat", "anthropic"), sse_callbacks):
    cpp += f"std::vector<std::uint64_t> {protocol}_sse(const std::string& input, std::size_t width) {{\n"
    cpp += context + callback + r'''
    for (std::size_t offset = 0; offset < input.size(); offset += width) {
        assert(sse_parser(input.substr(offset, width)));
    }
    return recorded;
}
'''
for protocol, callback in zip(("chat", "anthropic"), json_callbacks):
    cpp += f"std::vector<std::uint64_t> {protocol}_json(const std::string& input, std::size_t width) {{\n"
    cpp += context + callback + r'''
    for (std::size_t offset = 0; offset < input.size(); offset += width) {
        json_parser.recv(input.substr(offset, width));
    }
    return recorded;
}
'''
cpp += r'''
std::string event(std::string name, std::string data) {
    return "event: " + name + "\r\ndata: " + data + "\r\n\r\n";
}
int main() {
    for (const auto& model : {"deepseek-flash", "deepseek-v4-pro"}) {
        auto service = model_to_service(model);
        assert(service && service->name == "deepseek");
        assert(chat_url(*service) == "https://api.deepseek.com/chat/completions");
        assert(anthropic_url(*service) == "https://api.deepseek.com/anthropic/v1/messages");
    }
    assert(!model_to_service("unknown"));
    for (SJSON::JSObject body : {SJSON::JSObject {}, SJSON::JSObject {{"metadata", SJSON::JSNull {}}}}) {
        assert(isolate_metadata(body) == 200);
        assert(body.at("metadata").object().at("user_id").string() == "alice");
    }
    SJSON::JSObject spoofed {{"metadata", SJSON::JSObject {{"user_id", "another-user"}, {"note", "keep"}}}};
    assert(isolate_metadata(spoofed) == 200);
    assert(spoofed.at("metadata").object().at("user_id").string() == "alice");
    assert(spoofed.at("metadata").object().at("note").string() == "keep");
    for (const SJSON::JSValue& value : {SJSON::JSValue(1), SJSON::JSValue("invalid"), SJSON::JSValue(SJSON::JSArray {})}) {
        SJSON::JSObject invalid {{"metadata", value}};
        assert(isolate_metadata(invalid) == 400);
    }

    const std::string chat_usage = R"({"prompt_tokens":35,"prompt_cache_hit_tokens":10,"prompt_cache_miss_tokens":25,"completion_tokens":16,"completion_tokens_details":{"reasoning_tokens":13}})";
    const std::string anthropic_usage = R"({"input_tokens":25,"cache_read_input_tokens":10,"cache_creation_input_tokens":0,"output_tokens":16})";
    const std::string chat_stream = ": keep-alive\r\n\r\n"
        + event("message", R"({"choices":[{"delta":{"content":"Hello"}}],"usage":null})")
        + event("message", "{\"choices\":[{\"finish_reason\":\"stop\"}],\"usage\":" + chat_usage + "}")
        + "data: [DONE]\r\n\r\n";
    const auto start = event("message_start", R"({"type":"message_start","message":{"usage":{"input_tokens":25,"cache_read_input_tokens":10,"cache_creation_input_tokens":0,"output_tokens":1}}})");
    const auto delta = event("message_delta", R"({"type":"message_delta","usage":{"output_tokens":16}})");
    const auto stop = event("message_stop", R"({"type":"message_stop"})");
    const auto full_delta = event("message_delta", "{\"usage\":" + anthropic_usage + "}");
    for (std::size_t width : {1, 2, 7, 64, 4096}) {
        auto chat_s = chat_sse(chat_stream, width);
        assert(chat_s.size() == 1 && chat_s.back() == 48400);
        assert(printed.back().cache_hit_tokens == 10 && printed.back().cache_miss_tokens == 25 && printed.back().cache_creation_tokens == 0);
        auto chat_j = chat_json("{\"usage\":" + chat_usage + "}", width);
        assert(chat_j.size() == 1 && chat_j.back() == 48400);
        auto anthropic_j = anthropic_json("{\"usage\":" + anthropic_usage + "}", width);
        assert(anthropic_j.size() == 1 && anthropic_j.back() == 48400);
        auto anthropic_s = anthropic_sse(start + event("ping", "{}") + event("future_event", "{}") + delta + stop, width);
        assert(anthropic_s.size() == 2 && anthropic_s.front() == 18700 && anthropic_s.back() == 48400);
        auto repeated_input = anthropic_sse(start + full_delta + stop, width);
        assert(repeated_input.back() == 48400);
        auto repeated_output = anthropic_sse(start + delta + delta + stop, width);
        assert(repeated_output.size() == 3 && repeated_output.back() == 48400);
        auto changed_input = anthropic_sse(start + event("message_delta", R"({"usage":{"input_tokens":30,"output_tokens":16}})") + stop, width);
        assert(changed_input.back() == 51700);
        auto created = anthropic_sse(start + event("message_delta", R"({"usage":{"cache_creation_input_tokens":5,"output_tokens":16}})")
            + event("message_delta", R"({"usage":{"input_tokens":20}})") + stop, width);
        assert(created.size() == 3 && created[1] == 51700 && created.back() == 48400);
        assert(printed.back().cache_hit_tokens == 10 && printed.back().cache_miss_tokens == 25 && printed.back().cache_creation_tokens == 5);
        auto zero_cache = anthropic_sse(start + event("message_delta", R"({"usage":{"cache_read_input_tokens":0,"output_tokens":16}})") + stop, width);
        assert(zero_cache.back() == 48180);
        auto zero_output = anthropic_sse(start + event("message_delta", R"({"usage":{"output_tokens":0}})") + stop, width);
        assert(zero_output.back() == 16720);
        auto invalid_delta = anthropic_sse(start + event("message_delta", R"({"usage":{"output_tokens":-1}})") + delta + stop, width);
        assert(invalid_delta.size() == 2 && invalid_delta.back() == 48400);
    }
    const auto huge_start = event("message_start", R"({"message":{"usage":{"input_tokens":9000000000000000,"output_tokens":0}}})");
    const auto huge_delta = event("message_delta", R"({"usage":{"output_tokens":9000000000000000}})");
    auto overflow = anthropic_sse(huge_start + huge_delta + stop, 1);
    assert(overflow.size() == 1 && overflow.front() == 5940000000000000000ULL);
    std::cout << "PASS: upstream URLs, trusted Messages isolation, JSON/SSE accounting, cumulative updates, zeros, invalid counts and overflow\n";
}
'''

with tempfile.TemporaryDirectory(prefix="llm-budget-proxy-tests-") as temporary:
    directory = pathlib.Path(temporary)
    cpp_path = directory / "callbacks.cpp"
    cpp_path.write_text(cpp)
    executable = directory / "callbacks"
    subprocess.run([
        "g++", "-std=c++23", "-O2", "-Wall", "-Wextra", "-I" + str(root),
        str(cpp_path), str(root / "SJSON/src/value.cpp"),
        str(root / "SJSON/src/token.cpp"), str(root / "SJSON/src/sjson.cpp"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
