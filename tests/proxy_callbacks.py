"""Build the server first, then run python3 tests/proxy_callbacks.py.

Exercise the real proxy against a local HTTP upstream. GNU ld --wrap redirects
only the fetch destination; parsing, streaming, routing, storage and limits use
production code. No real provider credentials or external traffic are used.
"""

import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import pathlib
import shutil
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time


root = pathlib.Path(__file__).resolve().parents[1]
seen = []


class Upstream(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        seen.append((self.headers["X-Test-Original-URL"], dict(self.headers), body))
        if self.path.endswith("/responses"):
            usage = {"input_tokens": 300, "input_tokens_details": {"cached_tokens": 100},
                     "output_tokens": 1000, "output_tokens_details": {"reasoning_tokens": 900}}
            if body["model"] == "gpt-6.1-sol":
                usage["input_tokens_details"]["cache_write_tokens"] = 50
            response = {"id": "resp_local", "object": "response", "status": "completed",
                        "output": [{"type": "message", "content": [{"type": "output_text", "text": "local response"}]}],
                        "usage": usage}
            if body.get("stream"):
                terminal = body.get("metadata", {}).get("terminal", "completed")
                response["status"] = terminal
                event = {"type": "response." + terminal, "response": response}
                # Initial null usage and output-item usage are not request totals.
                prefix = ('event: response.created\ndata: {"type":"response.created","response":{"usage":null}}\n\n'
                          'event: response.output_item.done\ndata: {"usage":{"input_tokens":999999}}\n\n'
                          'event: response.output_text.delta\ndata: {"delta":"local response"}\n\n')
                event_name = "" if body.get("metadata", {}).get("unnamed") else "event: " + event["type"] + "\n"
                payload = (prefix + event_name + 'data: ' + json.dumps(event) + '\n\n').encode()
            else:
                payload = json.dumps(response).encode()
        elif self.path.endswith("/messages"):
            usage = {"input_tokens": 200, "cache_read_input_tokens": 100,
                     "cache_creation_input_tokens": 5, "output_tokens": 1000}
            if body.get("stream"):
                initial = dict(usage, output_tokens=1)
                payload = (
                    'event: message_start\ndata: ' + json.dumps({"message": {"usage": initial}}) + '\n\n'
                    'event: message_delta\ndata: ' + json.dumps({"usage": {"output_tokens": 1000}}) + '\n\n'
                    'event: message_stop\ndata: {}\n\n'
                ).encode()
            else:
                payload = json.dumps({"usage": usage}).encode()
        else:
            # OpenAI-compatible cache reporting: no DeepSeek-specific fields.
            usage = {"prompt_tokens": 300, "prompt_tokens_details": {"cached_tokens": 100},
                     "completion_tokens": 1000000}
            if body.get("stream"):
                events = body.get("metadata", {}).get("usage_events", [usage])
                payload = (''.join('data: ' + json.dumps({"usage": event}) + '\n\n' for event in events)
                           + 'data: [DONE]\n\n').encode()
            else:
                usage = body.get("metadata", {}).get("usage_events", [usage])[-1]
                payload = json.dumps({"usage": usage}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream" if body.get("stream") else "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        # Fragment lifecycle events and JSON across transport reads.
        for offset in range(0, len(payload), 7):
            self.wfile.write(payload[offset:offset + 7])
            self.wfile.flush()


with socket.socket() as available_port:
    available_port.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    available_port.bind(("127.0.0.1", 8787))

with tempfile.TemporaryDirectory(prefix="llm-budget-proxy-tests-") as temporary:
    directory = pathlib.Path(temporary)
    upstream = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
    thread = threading.Thread(target=upstream.serve_forever, daemon=True)
    thread.start()
    try:
        symbols = subprocess.check_output(["nm", "-u", str(root / "obj/main_0.o")], text=True)
        symbol, = [line.split()[-1] for line in symbols.splitlines() if "_ZN2pw5fetch" in line]
        wrapper = directory / "transport.cpp"
        wrapper.write_text('''#include "Polyweb/polyweb.hpp"
#include <utility>
pn::Status real_fetch(std::string, pn::StringView, pw::Response&, pn::StringView,
    pw::Headers, const pw::ClientConfig&, std::string) asm("__real_''' + symbol + '''");
pn::Status local_fetch(std::string, pn::StringView, pw::Response&, pn::StringView,
    pw::Headers, const pw::ClientConfig&, std::string) asm("__wrap_''' + symbol + '''");
pn::Status local_fetch(std::string method, pn::StringView url, pw::Response& response,
    pn::StringView body, pw::Headers headers, const pw::ClientConfig& config, std::string version) {
    pw::URLInfo parsed;
    if (auto result = parsed.parse(url); !result) return result;
    headers["X-Test-Original-URL"] = std::string(url);
    std::string local = "http://127.0.0.1:''' + str(upstream.server_port) + '''" + std::string(parsed.path);
    return real_fetch(std::move(method), local, response, body, std::move(headers), config, std::move(version));
}
''')
        binary = directory / "proxy"
        subprocess.run([
            "g++", "-std=c++23", "-O2", "-pthread", "-I" + str(root), str(wrapper),
            *(str(path) for path in sorted((root / "obj").glob("*.o"))),
            str(root / "spdlog/build/libspdlog.a"), "-lssl", "-lcrypto", "-lsqlite3",
            "-Wl,--wrap=" + symbol, "-o", str(binary),
        ], check=True)
        shutil.copytree(root / "web", directory / "web")
        (directory / "keys.json").write_text(json.dumps({
            "deepseek": "test-deepseek",
            "opencode-go": {"api_key": "test-go", "plan": "go"},
            "openai": "test-openai",
        }))
        keys = []
        for name in ("alice", "bob"):
            result = subprocess.run([str(binary), "user", "add", name, "--five-hour-limit", "10", "--weekly-limit", "10"],
                                    cwd=directory, text=True, capture_output=True, check=True)
            keys.append(result.stdout.split("API key: ")[1].strip())
        db = sqlite3.connect(directory / "llm-budget.db")
        server = subprocess.Popen([str(binary)], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            for _ in range(100):
                try:
                    with socket.create_connection(("127.0.0.1", 8787), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise AssertionError("server did not start")

            def request(model, route="/chat/completions", stream=False, user=0, metadata=None):
                connection = http.client.HTTPConnection("127.0.0.1", 8787, timeout=5)
                headers = {"Content-Type": "application/json", "Authorization": "Bearer " + keys[user],
                           "x-opencode-session": "conversation-1", "session-id": "codex-conversation-1", "thread-id": "codex-thread-1"}
                body = {"model": model, "stream": stream}
                body["input" if route == "/responses" else "messages"] = "local prompt" if route == "/responses" else []
                if metadata is not None:
                    body["metadata"] = metadata
                connection.request("POST", route, json.dumps(body), headers)
                response = connection.getresponse()
                result = response.status, response.read()
                connection.close()
                return result

            def last_row():
                for _ in range(100):
                    row = db.execute(
                        "SELECT cost_nanodollars, state FROM requests ORDER BY id DESC LIMIT 1"
                    ).fetchone()

                    if row and row[1] == "completed":
                        return row[0]

                    time.sleep(0.01)

                raise AssertionError(row)

            assert request("deepseek/deepseek-flash")[0] == 200
            direct = last_row()
            assert direct in (600030300, 1200060600), direct
            assert seen[-1][0] == "https://api.deepseek.com/chat/completions"
            assert seen[-1][2]["model"] == "deepseek-flash" and seen[-1][2]["user_id"] == "alice"

            assert request("opencode-go/deepseek-v4-flash", stream=True, user=1)[0] == 200
            go = last_row()
            assert go in (200010100, 400020200), go
            assert go < direct
            assert seen[-1][0] == "https://opencode.ai/zen/go/v1/chat/completions"
            assert seen[-1][1]["Authorization"] == "Bearer test-go"
            assert seen[-1][1]["User-Agent"] == "llm-budget/1.0"
            assert seen[-1][1]["x-opencode-session"] == "conversation-1"
            assert seen[-1][2]["model"] == "deepseek-v4-flash"

            assert request("opencode-go/glm-5.3-flash")[0] == 200
            flat = last_row()
            assert flat == 83338834, flat

            assert request("opencode-go/minimax-m2.7", "/v1/messages", stream=True)[0] == 200
            messages = last_row()
            # Input, reads, writes and final cumulative output; no delta double counting.
            assert messages == 211313, messages
            assert seen[-1][0] == "https://opencode.ai/zen/go/v1/messages"
            assert seen[-1][1]["x-api-key"] == "test-go"
            assert "Authorization" not in seen[-1][1]

            assert request("openai/gpt-4.1-mini")[0] == 200
            openai = last_row()
            assert openai == 1600090000, openai
            assert seen[-1][0] == "https://api.openai.com/v1/chat/completions"
            assert seen[-1][1]["Authorization"] == "Bearer test-openai"

            status, payload = request("openai/gpt-4.1-mini", "/responses")
            assert status == 200 and json.loads(payload)["output"][0]["content"][0]["text"] == "local response"
            assert last_row() == 1690000
            assert seen[-1][0] == "https://api.openai.com/v1/responses"
            assert seen[-1][1]["Authorization"] == "Bearer test-openai"
            assert seen[-1][2]["model"] == "gpt-4.1-mini" and seen[-1][2]["input"] == "local prompt"

            for terminal, unnamed in (("completed", False), ("incomplete", False), ("failed", True)):
                status, payload = request("openai/gpt-4.1-mini", "/responses", stream=True,
                                          metadata={"terminal": terminal, "unnamed": unnamed})
                assert status == 200 and b"local response" in payload
                assert ('"type": "response.' + terminal + '"').encode() in payload
                assert last_row() == 1690000, "Terminal usage must not double-count reasoning or event snapshots"

            assert request("opencode-go/gpt-5.6-luna", "/responses", stream=True)[0] == 200
            assert last_row() == 828000
            assert seen[-1][0] == "https://opencode.ai/zen/go/v1/responses"
            assert seen[-1][1]["Authorization"] == "Bearer test-go"
            assert seen[-1][1]["x-opencode-session"] == "conversation-1"
            assert seen[-1][1]["session-id"] == "codex-conversation-1"
            assert seen[-1][1]["thread-id"] == "codex-thread-1"
            assert seen[-1][2]["model"] == "gpt-5.6-luna"

            assert request("openai/gpt-6.1-sol", "/responses", stream=True)[0] == 200
            assert last_row() == 10435000, "Cache writes must replace ordinary input pricing, not add another input charge"
            assert request("deepseek/deepseek-flash", "/responses", stream=True)[0] == 200
            assert last_row() in (630300, 1260600)
            assert seen[-1][0] == "https://api.deepseek.com/responses"
            assert seen[-1][1]["Authorization"] == "Bearer test-deepseek"
            assert seen[-1][2]["user"] == "alice"

            count = db.execute("SELECT COUNT(*) FROM requests").fetchone()[0]
            calls = len(seen)
            assert request("opencode-go/minimax-m2.7")[0] == 400
            assert request("opencode-go/gpt-5.6-luna")[0] == 400
            assert request("not-a-model")[0] == 400
            assert request("openai/gpt-5.5-pro")[0] == 400
            assert request("opencode-go/glm-5.3-flash", "/responses")[0] == 400
            assert len(seen) == calls
            assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == count

            # The allocated subscription cost leaves room even when nominal usage would not.
            db.execute(
                "UPDATE users SET five_hour_limit_nanodollars=?, weekly_limit_nanodollars=? WHERE id=2",
                (go + 1, go + 1),
            )
            db.commit()

            assert request("opencode-go/glm-5.3-flash", user=1)[0] == 200
            assert last_row() == 83338834
            assert request("opencode-go/glm-5.3-flash", user=1)[0] == 429
            assert len(seen) == calls + 1

            # A rejected later snapshot must preserve the prior charge, while
            # diagnostics never disclose arbitrary provider values.
            private_value = "private-provider-value-must-not-be-logged"
            valid_usage = {"prompt_tokens": 300, "completion_tokens": 1000,
                           "prompt_tokens_details": {"cached_tokens": 100}}
            invalid_usage = dict(valid_usage, prompt_cache_hit_tokens=None,
                                 private_provider_payload={"value": private_value})
            invalid_usage["prompt_tokens_details"] = {"cached_tokens": 100,
                                                      "cache_write_tokens": private_value}
            assert request("openai/gpt-4.1-mini", stream=True,
                           metadata={"usage_events": [valid_usage, invalid_usage]})[0] == 200
            assert last_row() == 1690000
            assert request("openai/gpt-4.1-mini",
                           metadata={"usage_events": [invalid_usage]})[0] == 200
            assert last_row() is None

            # Go's optional null write count must not discard cached reads or
            # prevent the final snapshot from correcting an uncached estimate.
            reported_usage = {"completion_tokens": 139, "prompt_tokens": 211852,
                              "prompt_tokens_details": {"cache_write_tokens": None, "cached_tokens": 2688},
                              "total_tokens": 211991}
            provisional_usage = {"prompt_tokens": 211852, "completion_tokens": 139}
            nominal = 209164 * 150000 + 2688 * 3000 + 139 * 600000
            expected_costs = ((nominal + 5999) // 6000, (nominal * 2 + 5999) // 6000)
            assert request("opencode-go/deepseek-v4.1-flash", stream=True,
                           metadata={"usage_events": [provisional_usage, reported_usage]})[0] == 200
            assert last_row() in expected_costs
            assert request("opencode-go/deepseek-v4.1-flash",
                           metadata={"usage_events": [reported_usage]})[0] == 200
            assert last_row() in expected_costs
        finally:
            server.terminate()
            output, _ = server.communicate(timeout=5)
            db.close()

        assert private_value not in output
        diagnostics = [line for line in output.splitlines() if "field=prompt_cache_hit_tokens" in line]
        assert len(diagnostics) == 2
        for line in diagnostics:
            assert "reason=expected_number" in line
            snapshot = json.loads(line.split(" usage=", 1)[1])
            assert snapshot["prompt_cache_hit_tokens"] == {"type": "Null"}
            assert snapshot["prompt_tokens_details"]["cache_write_tokens"] == {"type": "String"}
            assert "private_provider_payload" not in snapshot
            assert "user=alice id=1" in line and "request=" in line
            assert "service=openai" in line and "model=gpt-4.1-mini" in line
            assert "protocol=chat_completions" in line

        # Replace one configured provider with a catalog whose pricing can be
        # unavailable. The production routes, transport and database remain real.
        pricing_main = directory / "pricing_main.cpp"
        pricing_main.write_text('''#include "provider_config.hpp"
namespace providers {
    class PricingFixtureProvider final : public ConfiguredProvider {
    protected:
        inline static constexpr Provider provider_definition {
            .name = "pricing-fixture",
            .chat_completions_base_url = "http://127.0.0.1",
            .anthropic_messages_base_url = "http://127.0.0.1",
            .responses_base_url = "http://127.0.0.1",
        };
        inline static constexpr Model model_catalog[] {
            {
                .name = "unpriced",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_ANTHROPIC_MESSAGES | PROTOCOL_RESPONSES,
                .rates = {.input = 150'000, .cached_read = 3'000, .output = 600'000, .cached_write = 150'000},
            },
            {
                .name = "free",
                .protocols = PROTOCOL_CHAT_COMPLETIONS | PROTOCOL_ANTHROPIC_MESSAGES | PROTOCOL_RESPONSES,
                .rates = {.input = 150'000, .cached_read = 3'000, .output = 600'000, .cached_write = 150'000},
            },
        };

    public:
        PricingFixtureProvider():
            ConfiguredProvider(provider_definition) {
            api_key = "local-pricing-fixture";
        }

        std::span<const Model> models() const override {
            return model_catalog;
        }

        std::optional<cost::Multiplier> multiplier(const Model& model) const override {
            if (model.name == "free") return cost::Multiplier {0, 1};

            return std::nullopt;
        }
    };

    Configuration configure_with_pricing_fixture(SJSON::JSObject keys) {
        auto configured = configure(std::move(keys));
        configured.front() = std::make_unique<PricingFixtureProvider>();
        for (auto& provider : configured) {
            if (provider->definition().name == "openai") provider = std::make_unique<OpenAIProvider>();
        }
        return configured;
    }
}
#define configure(...) configure_with_pricing_fixture(__VA_ARGS__)
#include "main.cpp"
''')
        pricing_binary = directory / "pricing_proxy"
        subprocess.run([
            "g++", "-std=c++23", "-O2", "-pthread", "-I" + str(root),
            "-I" + str(root / "spdlog/include"), str(pricing_main), str(wrapper),
            *(str(path) for path in sorted((root / "obj").glob("*.o")) if path.name != "main_0.o"),
            str(root / "spdlog/build/libspdlog.a"), "-lssl", "-lcrypto", "-lsqlite3",
            "-Wl,--wrap=" + symbol, "-o", str(pricing_binary),
        ], check=True)
        db = sqlite3.connect(directory / "llm-budget.db")
        server = subprocess.Popen([str(pricing_binary)], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            for _ in range(100):
                try:
                    with socket.create_connection(("127.0.0.1", 8787), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise AssertionError("pricing fixture did not start")

            count = db.execute("SELECT COUNT(*) FROM requests").fetchone()[0]
            calls = len(seen)
            for route in ("/chat/completions", "/responses"):
                assert request("openai/gpt-4.1-mini", route)[0] == 503
            assert len(seen) == calls
            assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == count
            for route in ("/chat/completions", "/v1/messages", "/responses"):
                status, body = request("pricing-fixture/unpriced", route)
                assert status == 503, (status, body)
                assert len(seen) == calls, "Unpriced request reached upstream"
                assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == count

            for route in ("/chat/completions", "/v1/messages", "/responses"):
                assert request("pricing-fixture/free", route)[0] == 200
                assert last_row() == 0, "A resolved zero multiplier must remain valid"
            assert len(seen) == calls + 3
            assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == count + 3
        finally:
            server.terminate()
            server.communicate(timeout=5)
            db.close()
    finally:
        upstream.shutdown()
        upstream.server_close()
        thread.join(timeout=5)

print("PASS: all three proxy protocols, Responses JSON/terminal SSE accounting, budget limits, unavailable providers/pricing without dispatch or rows, and valid zero multipliers")
