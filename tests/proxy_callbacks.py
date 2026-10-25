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
import sys
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
        failure = body.get("metadata", {}).get("transport_failure")
        if failure:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            if failure == "headers":
                self.send_header("Content-Length", "invalid")
            else:
                self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            if failure == "body":
                chunk = b'data: {}\n\n'
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b'\r\ninvalid\r\n')
                self.wfile.flush()
            self.close_connection = True
            return
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
            usage = body.get("metadata", {}).get("message_usage", usage)
            if body.get("stream"):
                initial = dict(usage, output_tokens=1)
                payload = (
                    'event: message_start\ndata: ' + json.dumps({"message": {"usage": initial}}) + '\n\n'
                    'event: message_delta\ndata: ' + json.dumps({"usage": {"output_tokens": usage["output_tokens"]}}) + '\n\n'
                    'event: message_stop\ndata: {}\n\n'
                ).encode()
            else:
                payload = json.dumps({"usage": usage}).encode()
        else:
            if body.get("stream"):
                assert body.get("stream_options", {}).get("include_usage") is True
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
        content_type = body.get("metadata", {}).get("test_content_type", "text/event-stream" if body.get("stream") else "application/json")
        if content_type is not None:
            self.send_header("Content-Type", content_type)
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
        # Drain logs to a file so a full pipe cannot stall the server mid-test.
        server_output = tempfile.TemporaryFile(mode="w+t", dir=directory)
        server = subprocess.Popen(
            [str(binary)],
            cwd=directory,
            stdout=server_output,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            for _ in range(100):
                try:
                    with socket.create_connection(("127.0.0.1", 8787), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise AssertionError("server did not start")

            def request(model, route="/chat/completions", stream=False, user=0, metadata=None, extra_headers=None, body_fields=None):
                connection = http.client.HTTPConnection("127.0.0.1", 8787, timeout=5)
                headers = {"Content-Type": "application/json", "Authorization": "Bearer " + keys[user],
                           "x-opencode-session": "conversation-1", "session-id": "codex-conversation-1", "thread-id": "codex-thread-1"}
                if extra_headers is not None:
                    headers.update(extra_headers)
                body = {"model": model, "stream": stream}
                if body_fields is not None:
                    body.update(body_fields)
                body.setdefault("input" if route == "/responses" else "messages", "local prompt" if route == "/responses" else [])
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

            def test_client_tools():
                client_tools = [
                    {
                        "type": "function",
                        "name": "local_function",
                        "parameters": {"type": "object"},
                    },
                    {"type": "custom", "name": "local_custom"},
                    {
                        "type": "namespace",
                        "name": "local_group",
                        "description": "Local tools grouped by namespace",
                        "tools": [
                            {
                                "type": "function",
                                "name": "nested_function",
                                "parameters": {"type": "object"},
                            },
                        ],
                    },
                ]
                image_input = [
                    {
                        "role": "user",
                        "content": [
                            {"type": "input_text", "text": "Describe this image"},
                            {
                                "type": "input_image",
                                "image_url": "data:image/png;base64,local-test-image",
                            },
                        ],
                    },
                ]

                # The shared policy rejects hosted/unknown tools before admission,
                # for every provider and protocol, with both response transports.
                for model in (
                    "openai/gpt-6.1-sol",
                    "opencode-go/gpt-5.6-luna",
                    "deepseek/deepseek-v4-pro",
                ):
                    routes = ("/chat/completions", "/responses")
                    if not model.startswith("openai/"):
                        routes += ("/v1/messages",)

                    for route in routes:
                        blocked_fields = [
                            {
                                "tools": [
                                    {"type": tool_type, "name": "local", "input_schema": {}},
                                ],
                            }
                            for tool_type in (
                                "web_search",
                                "web_search_20250305",
                                "web_fetch_20250910",
                                "image_generation",
                                "file_search",
                                "code_interpreter",
                                "computer_20250124",
                                "bash_20250124",
                                "mcp",
                                "tool_search",
                                "unknown_future_tool",
                            )
                        ]
                        blocked_fields += [
                            {"tools": value}
                            for value in (None, {}, "invalid", [None], [{}], [{"type": 1}])
                        ]
                        blocked_fields += [
                            {
                                "tools": [
                                    {
                                        "type": "namespace",
                                        "name": "hosted_group",
                                        "tools": [{"type": hosted_type}],
                                    },
                                ],
                            }
                            for hosted_type in ("web_search", "code_interpreter", "unknown_future_tool")
                        ]
                        blocked_fields += [
                            {"tools": [{"type": "namespace", "name": "missing_tools"}]},
                            {"tools": [{"type": "namespace", "name": "bad_tools", "tools": None}]},
                            {"tools": [{"type": "namespace", "name": "empty_items", "tools": [None]}]},
                        ]
                        blocked_fields += [
                            {"web_search_options": value}
                            for value in ({}, None, False)
                        ]
                        blocked_fields += [
                            {"mcp_servers": value}
                            for value in (
                                [{"type": "url", "url": "https://example.invalid"}],
                                None,
                                {},
                            )
                        ]

                        if route == "/responses":
                            blocked_fields += [
                                {
                                    "input": [
                                        {
                                            "type": "additional_tools",
                                            "tools": [{"type": "web_search"}],
                                        },
                                    ],
                                },
                                {"input": [{"type": "additional_tools"}]},
                                {"input": [{"type": "additional_tools", "tools": None}]},
                                {
                                    "input": [
                                        {
                                            "type": "additional_tools",
                                            "tools": [
                                                {
                                                    "type": "namespace",
                                                    "name": "hosted_group",
                                                    "tools": [{"type": "web_search"}],
                                                },
                                            ],
                                        },
                                    ],
                                },
                            ]
                        elif route == "/v1/messages":
                            blocked_fields += [
                                {"tools": [tool]}
                                for tool in (
                                    {"name": "local"},
                                    {"name": "local", "input_schema": None},
                                    {"name": 1, "input_schema": {}},
                                )
                            ]

                        for stream in (False, True):
                            for fields in blocked_fields:
                                calls_before = len(seen)
                                rows_before = db.execute(
                                    "SELECT COUNT(*) FROM requests"
                                ).fetchone()[0]
                                status, payload = request(
                                    model,
                                    route,
                                    stream=stream,
                                    body_fields=fields,
                                )

                                assert status == 400, (model, route, fields, status, payload)
                                assert len(seen) == calls_before
                                rows_after = db.execute(
                                    "SELECT COUNT(*) FROM requests"
                                ).fetchone()[0]
                                assert rows_after == rows_before

                            if route == "/v1/messages":
                                tools = [
                                    {"name": "local", "input_schema": {"type": "object"}},
                                    {
                                        "type": "custom",
                                        "name": "explicit_local",
                                        "input_schema": {},
                                    },
                                ]
                            else:
                                tools = client_tools

                            fields = {"tools": tools, "mcp_servers": []}
                            if route == "/responses":
                                fields["input"] = image_input + [
                                    {"type": "additional_tools", "tools": tools},
                                ]
                            else:
                                fields["metadata"] = {
                                    "usage_events": [
                                        {"prompt_tokens": 1, "completion_tokens": 1},
                                    ],
                                }

                            status, payload = request(
                                model,
                                route,
                                stream=stream,
                                body_fields=fields,
                            )
                            assert status == 200, (model, route, status, payload)
                            assert last_row() is not None
                            assert seen[-1][2]["tools"] == tools
                            assert seen[-1][2]["mcp_servers"] == []
                            if route == "/responses":
                                assert seen[-1][2]["input"] == fields["input"]

                db.execute("DELETE FROM requests")
                db.commit()
                print(
                    "PASS: shared hosted-tool rejection and client-tool forwarding "
                    "across providers, protocols and JSON/SSE"
                )

            test_client_tools()
            if "--tools-only" in sys.argv:
                raise SystemExit(0)

            assert request("deepseek/deepseek-flash")[0] == 200
            direct = last_row()
            assert direct in (600030300, 1200060600), direct
            assert seen[-1][0] == "https://api.deepseek.com/chat/completions"
            assert seen[-1][2]["model"] == "deepseek-flash" and seen[-1][2]["user_id"] == "alice"

            assert request("opencode-go/deepseek-v4.1-flash", stream=True, user=1)[0] == 200
            go = last_row()
            # $10 Go subscription / $80 Flash allowance, rounded up once.
            assert go in (75003788, 150007575), go
            assert go < direct
            assert seen[-1][0] == "https://opencode.ai/zen/go/v1/chat/completions"
            assert seen[-1][1]["Authorization"] == "Bearer test-go"
            assert seen[-1][1]["User-Agent"] == "llm-budget/1.0"
            assert seen[-1][1]["x-opencode-session"] == "conversation-1"
            assert seen[-1][2]["model"] == "deepseek-v4.1-flash"

            for options in (None, {}, {"include_usage": False, "other_option": "preserved"}):
                assert request("opencode-go/deepseek-v4.1-flash", stream=True,
                               body_fields={"stream_options": options})[0] == 200
                last_row()
                assert seen[-1][2]["stream_options"] == dict(options or {}, include_usage=True)

            for options in (False, "invalid", [], 1):
                calls_before = len(seen)
                rows_before = db.execute("SELECT COUNT(*) FROM requests").fetchone()[0]
                assert request("opencode-go/deepseek-v4.1-flash", stream=True,
                               body_fields={"stream_options": options})[0] == 400
                assert len(seen) == calls_before
                assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == rows_before

            assert request("opencode-go/deepseek-v4.1-flash",
                           body_fields={"stream_options": {"include_usage": False}})[0] == 200
            last_row()
            assert seen[-1][2]["stream_options"] == {"include_usage": False}

            assert request("opencode-go/glm-5.3-flash")[0] == 200
            flat = last_row()
            assert flat == 83338834, flat

            assert request("opencode-go/minimax-m3", "/v1/messages", stream=True)[0] == 200
            messages = last_row()
            # Input, reads, writes and final cumulative output; no delta double counting.
            assert messages == 211250, messages
            assert seen[-1][0] == "https://opencode.ai/zen/go/v1/messages"
            assert seen[-1][1]["x-api-key"] == "test-go"
            assert "Authorization" not in seen[-1][1]

            for stream in (False, True):
                assert request("opencode-go/claude-haiku-5-5", "/v1/messages", stream=stream)[0] == 200
                assert last_row() == 347750
                assert seen[-1][0] == "https://opencode.ai/zen/go/v1/messages"
                assert seen[-1][1]["x-api-key"] == "test-go"
                assert seen[-1][1]["anthropic-version"] == "2023-06-01"
                assert "anthropic-beta" not in seen[-1][1]
                assert seen[-1][1]["x-opencode-session"] == "conversation-1"
                assert seen[-1][2]["model"] == "claude-haiku-5-5"

            for headers in (
                {"AnThRoPiC-VeRsIoN": "2099-01-01", "AnThRoPiC-BeTa": "test-beta-1,test-beta-2"},
                {"anthropic-beta": "test-beta-1"},
                {"anthropic-version": "", "anthropic-beta": ""},
            ):
                assert request("opencode-go/claude-haiku-5-5", "/v1/messages", extra_headers=headers)[0] == 200
                assert last_row() == 347750
                supplied = {name.lower(): value for name, value in headers.items()}
                assert seen[-1][1]["anthropic-version"] == supplied.get("anthropic-version", "2023-06-01")
                assert seen[-1][1]["anthropic-beta"] == supplied["anthropic-beta"]

            for writes, expected in ((1, 670077), (2, 3350800)):
                usage = {"input_tokens": 0, "cache_read_input_tokens": 99999,
                         "cache_creation_input_tokens": writes, "output_tokens": 10}
                assert request("opencode-go/claude-haiku-5-5", "/v1/messages", stream=True,
                               metadata={"message_usage": usage})[0] == 200
                assert last_row() == expected

            assert request("opencode-go/beam-free")[0] == 200
            assert last_row() == 0
            calls_before_retired_model = len(seen)
            assert request("opencode-go/space-bunny")[0] == 400
            assert request("opencode-go/space-bunny-free")[0] == 400
            assert len(seen) == calls_before_retired_model

            assert request("openai/gpt-4.1-mini")[0] == 200
            openai = last_row()
            assert openai == 1600090000, openai
            assert seen[-1][0] == "https://api.openai.com/v1/chat/completions"
            assert seen[-1][1]["Authorization"] == "Bearer test-openai"

            assert seen[-1][2]["service_tier"] == "default"
            for model in ("openai/gpt-6.1-sol", "opencode-go/gpt-5.6-luna", "deepseek/deepseek-flash"):
                for route in ("/chat/completions", "/responses"):
                    for tier in ("auto", "fast", "priority", "ultrafast", "flex", None, 1):
                        calls_before = len(seen)
                        rows_before = db.execute("SELECT COUNT(*) FROM requests").fetchone()[0]
                        assert request(model, route, body_fields={"service_tier": tier})[0] == 400
                        assert len(seen) == calls_before
                        assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == rows_before
                for stream in (False, True):
                    for background in (True, "true", None, 1, [], {}):
                        calls_before = len(seen)
                        rows_before = db.execute("SELECT COUNT(*) FROM requests").fetchone()[0]
                        status, payload = request(model, "/responses", stream=stream,
                                                  body_fields={"background": background})
                        assert status == 400 and b"background" in payload.lower(), (model, background, status, payload)
                        assert len(seen) == calls_before
                        assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == rows_before
                    assert request(model, "/responses", stream=stream, body_fields={"background": False})[0] == 200
                    assert last_row() is not None
                    assert seen[-1][2]["background"] is False
                    assert seen[-1][2]["service_tier"] == "default"

            for route in ("/chat/completions", "/responses"):
                blocked_fields = [
                    {"service_tier": tier}
                    for tier in ("auto", "fast", "priority", "ultrafast", "flex", None, 1)
                ]
                blocked_fields += [
                    {"tools": [{"type": tool_type}]}
                    for tool_type in ("image_generation", "web_search", "web_search_preview",
                                      "file_search", "code_interpreter", "shell", "mcp", "tool_search")
                ]
                blocked_fields += [{"tools": value} for value in (None, {}, "invalid", [None], [{}], [{"type": 1}])]
                if route == "/responses":
                    blocked_fields += [
                        {"input": [{"type": "additional_tools", "tools": [{"type": "web_search"}]}]},
                        {"input": [{"type": "additional_tools"}]},
                        {"input": [{"type": "additional_tools", "tools": None}]},
                    ]
                for fields in blocked_fields:
                    calls_before = len(seen)
                    rows_before = db.execute("SELECT COUNT(*) FROM requests").fetchone()[0]
                    status, payload = request("openai/gpt-4.1-mini", route, body_fields=fields)
                    assert status == 400, (route, fields, status, payload)
                    assert len(seen) == calls_before
                    assert db.execute("SELECT COUNT(*) FROM requests").fetchone()[0] == rows_before

            client_tools = [{"type": "function", "name": "local_function", "parameters": {"type": "object"}},
                            {"type": "custom", "name": "local_custom"}]
            image_input = [{"role": "user", "content": [
                {"type": "input_text", "text": "Describe this image"},
                {"type": "input_image", "image_url": "data:image/png;base64,local-test-image"},
            ]}]
            for stream in (False, True):
                fields = {"service_tier": "default", "background": False, "tools": client_tools,
                          "input": image_input + [{"type": "additional_tools", "tools": client_tools}]}
                assert request("openai/gpt-6.1-sol", "/responses", stream=stream, body_fields=fields)[0] == 200
                assert last_row() == 10435000
                assert seen[-1][2]["service_tier"] == "default"
                assert seen[-1][2]["tools"] == client_tools
                assert seen[-1][2]["input"] == fields["input"]

            assert request("opencode-go/claude-haiku-5-5", "/v1/messages",
                           body_fields={"cache_control": {"type": "ephemeral", "ttl": "1h"}})[0] == 200
            assert last_row() == 347750
            assert seen[-1][2]["cache_control"]["ttl"] == "5m"
            cache_marker = {"type": "ephemeral", "ttl": "1h"}
            fields = {
                "system": [{"type": "text", "text": "system", "cache_control": cache_marker}],
                "tools": [{"name": "local", "input_schema": {"type": "object"}, "cache_control": cache_marker}],
                "messages": [{"role": "user", "content": [
                    {"type": "tool_result", "content": [{"type": "text", "text": "nested", "cache_control": cache_marker}]},
                    {"type": "tool_use", "input": {"cache_control": cache_marker}, "cache_control": {"type": "ephemeral"}},
                    {"type": "text", "text": "no marker"},
                ]}],
            }
            for model in ("opencode-go/claude-haiku-5-5", "deepseek/deepseek-flash"):
                assert request(model, "/v1/messages", body_fields=fields)[0] == 200
                assert last_row() is not None
                forwarded = seen[-1][2]
                assert forwarded["system"][0]["cache_control"]["ttl"] == "5m"
                assert forwarded["tools"][0]["cache_control"]["ttl"] == "5m"
                content = forwarded["messages"][0]["content"]
                assert content[0]["content"][0]["cache_control"]["ttl"] == "5m"
                assert content[1]["cache_control"]["ttl"] == "5m"
                assert content[1]["input"]["cache_control"]["ttl"] == "1h"
                assert "cache_control" not in content[2]
                assert "cache_control" not in forwarded

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

            # Go permits non-native routes; usage still uses the selected model's rates.
            assert request("opencode-go/minimax-m3")[0] == 200
            assert last_row() == 200011000
            assert request("opencode-go/gpt-5.6-luna")[0] == 200
            assert last_row() == 800028000
            assert request("opencode-go/glm-5.3-flash", "/responses")[0] == 200
            assert last_row() == 88834

            count = db.execute("SELECT COUNT(*) FROM requests").fetchone()[0]
            calls = len(seen)
            assert request("not-a-model")[0] == 400
            assert request("openai/gpt-5.5-pro")[0] == 400
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

            # Defective metadata must not cancel otherwise usable token counts.
            valid_usage = {"prompt_tokens": 300, "completion_tokens": 1000,
                           "prompt_tokens_details": {"cached_tokens": 100}}
            invalid_usage = dict(valid_usage, prompt_cache_hit_tokens=None,
                                 provider_details={"unrecognized_counter": 42})
            invalid_usage["prompt_tokens_details"] = {"cached_tokens": 100,
                                                      "cache_write_tokens": "not-a-number"}
            assert request("openai/gpt-4.1-mini", stream=True,
                           metadata={"usage_events": [valid_usage, invalid_usage]})[0] == 200
            assert last_row() == 1690000
            assert request("openai/gpt-4.1-mini",
                           metadata={"usage_events": [invalid_usage]})[0] == 200
            assert last_row() == 1690000
            for stream in (False, True):
                for usage, expected in (
                    ({"prompt_tokens": 1000, "completion_tokens": 200,
                      "prompt_tokens_details": {"cached_tokens": 1001}}, 720000),
                    (dict(valid_usage, prompt_tokens_details="bad"), 1720000),
                    (dict(valid_usage, prompt_tokens="bad"), 1610000),
                    (dict(valid_usage, completion_tokens="bad"), 90000),
                ):
                    assert request("openai/gpt-4.1-mini", stream=stream,
                                   metadata={"usage_events": [usage]})[0] == 200
                    assert last_row() == expected, (usage, last_row(), expected)

            # Go's optional null write count must not discard cached reads or
            # prevent the final snapshot from correcting an uncached estimate.
            reported_usage = {"completion_tokens": 139, "prompt_tokens": 211852,
                              "prompt_tokens_details": {"cache_write_tokens": None, "cached_tokens": 2688},
                              "total_tokens": 211991}
            provisional_usage = {"prompt_tokens": 211852, "completion_tokens": 139}
            nominal = 209164 * 150000 + 2688 * 3000 + 139 * 600000
            expected_costs = ((nominal + 7999) // 8000, (nominal * 2 + 7999) // 8000)
            assert request("opencode-go/deepseek-v4.1-flash", stream=True,
                           metadata={"usage_events": [provisional_usage, reported_usage]})[0] == 200
            assert last_row() in expected_costs
            assert request("opencode-go/deepseek-v4.1-flash",
                           metadata={"usage_events": [reported_usage]})[0] == 200
            assert last_row() in expected_costs

            # Transport errors must be logged even after a streaming head was sent.
            for content_type in ("text/plain", None):
                status, payload = request("openai/gpt-4.1-mini",
                                          metadata={"test_content_type": content_type})
                assert status == 200 and json.loads(payload)["usage"]
                assert last_row() is None

            failed_requests = []
            for route, model, protocol, service in (
                    ("/chat/completions", "openai/gpt-4.1-mini", "chat_completions", "openai"),
                    ("/responses", "openai/gpt-4.1-mini", "responses", "openai"),
                    ("/v1/messages", "opencode-go/minimax-m3", "anthropic_messages", "opencode-go")):
                for failure in ("headers", "body"):
                    status, payload = request(model, route, stream=True,
                                              metadata={"transport_failure": failure})
                    assert status == (502 if failure == "headers" else 200), (status, payload)
                    if failure == "body":
                        assert payload == b'data: {}\n\n', payload
                    row = None
                    for _ in range(100):
                        row = db.execute("SELECT id, state FROM requests ORDER BY id DESC LIMIT 1").fetchone()
                        if row and row[1] == "interrupted":
                            break
                        time.sleep(0.01)
                    assert row and row[1] == "interrupted", row
                    failed_requests.append((row[0], service, model.split("/", 1)[1], protocol, failure))
        finally:
            server.terminate()
            server.communicate(timeout=5)
            server_output.seek(0)
            output = server_output.read()
            server_output.close()
            db.close()

        failures = [line for line in output.splitlines() if "Upstream request failed:" in line]
        content_type_warnings = [line for line in output.splitlines() if "Upstream usage parsing skipped:" in line]
        assert len(content_type_warnings) == 2, content_type_warnings
        assert all("[warning]" in line and "status=200" in line for line in content_type_warnings)
        assert "content_type=text/plain" in content_type_warnings[0]
        assert "content_type=<missing or empty>" in content_type_warnings[1]
        assert len(failures) == len(failed_requests), failures
        for request_id, service, model, protocol, failure in failed_requests:
            matching = [line for line in failures if f"request={request_id}," in line]
            assert len(matching) == 1, matching
            line = matching[0]
            operation = "parse HTTP content length" if failure == "headers" else "parse HTTP chunk size"
            assert f"{operation}: Invalid HTTP message" in line, line
            assert "user=alice id=1" in line and f"service={service}," in line, line
            assert f"model={model}," in line and f"protocol={protocol})" in line, line
        assert not any(key in line for key in keys + ["test-openai", "test-go", "test-deepseek"]
                       for line in failures), "Failure logs must not contain credentials"

        diagnostics = [line for line in output.splitlines() if "field=prompt_cache_hit_tokens" in line]
        assert len(diagnostics) == 2
        for line in diagnostics:
            assert "reason=expected_number" in line
            logged_usage = json.loads(line.split(" usage=", 1)[1])
            assert logged_usage["prompt_cache_hit_tokens"] is None
            assert logged_usage["prompt_tokens_details"]["cache_write_tokens"] == "not-a-number"
            assert logged_usage["provider_details"]["unrecognized_counter"] == 42
            assert "user=alice id=1" in line and "request=" in line
            assert "service=openai" in line and "model=gpt-4.1-mini" in line
            assert "protocol=chat_completions" in line

        # Replace one configured provider with a catalog whose pricing can be
        # unavailable. The production routes, transport and database remain real.
        pricing_main = directory / "pricing_main.cpp"
        pricing_main.write_text('''#include "provider_registry.hpp"
namespace providers {
    class PricingFixtureProvider final : public Provider {
    protected:
        inline static constexpr ProviderDefinition provider_definition {
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
            Provider(provider_definition) {
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

    ProviderSet configure_with_pricing_fixture(SJSON::JSObject keys) {
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
