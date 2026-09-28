#include "Polyweb/polyweb.hpp"
#include "Polyweb/sse.hpp"
#include "SJSON/src/sjson.hpp"
#include "cost.hpp"
#include "util.hpp"
#include <functional>
#include <iomanip>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

void report_usage(const SJSON::JSValue& usage, std::string_view model, std::chrono::system_clock::time_point request_time) {
    SPDLOG_INFO("Got usage: {}", usage.to_string(4));
    if (!usage.is_object()) {
        SPDLOG_WARN("Cost estimate unavailable");
        return;
    }
    auto amount = cost::calculate("deepseek", model, usage.object(), request_time);
    if (!amount) {
        SPDLOG_WARN("Cost estimate unavailable");
        return;
    }
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(9)
              << static_cast<double>(amount->nano_units) / 1'000'000'000;
    SPDLOG_INFO("Estimated cost ({}, published prices): {}", amount->currency, formatted.str());
}

template <typename F>
std::move_only_function<bool(std::vector<char>)> metered_receiver(F func, size_t limit) {
    return [func = std::move(func), limit, received = (size_t) 0](std::vector<char> chunk) mutable -> bool {
        if (chunk.size() >= limit - received) {
            chunk.resize(limit - received);
            if (!chunk.empty()) func(std::move(chunk));
            return false;
        }

        received += chunk.size();
        func(std::move(chunk));
        return true;
    };
}

pw::Response make_basic_resp(uint16_t status_code, pw::Headers headers = {}, std::string http_version = "HTTP/1.1") {
    pw::Response resp(status_code, std::to_string(status_code) + ' ' + pw::status_code_to_reason_phrase(status_code), std::move(headers), std::move(http_version));
    if (!resp.headers.count("Content-Type")) {
        resp.headers["Content-Type"] = "text/plain";
    }
    return resp;
}

pw::Response make_basic_resp(uint16_t status_code, const std::string& what, pw::Headers headers = {}, std::string http_version = "HTTP/1.1") {
    pw::Response resp(status_code, std::to_string(status_code) + ' ' + pw::status_code_to_reason_phrase(status_code) + ": " + what, std::move(headers), std::move(http_version));
    if (!resp.headers.count("Content-Type")) {
        resp.headers["Content-Type"] = "text/plain";
    }
    return resp;
}

int main() {
    (void) pn::init();
    spdlog::cfg::load_env_levels();

    pw::Server server;

    server.error_cb = [](uint16_t status_code, pn::StringView what) {
        if (what.empty()) {
            return make_basic_resp(status_code);
        }
        return make_basic_resp(status_code, std::string(what));
    };

    server.route("/chat/completions",
        pw::Route {
            [](pw::Connection&, pw::Request& inbound_req) {
                if (inbound_req.method != "POST") {
                    return make_basic_resp(405, {{"Allow", "POST"}});
                }

                pw::Headers outbound_req_headers;

                if (auto authorization_it = inbound_req.headers.find("Authorization"); authorization_it != inbound_req.headers.end()) {
                    outbound_req_headers["Authorization"] = authorization_it->second;
                } else {
                    return make_basic_resp(401);
                }

                SJSON::JSObject req_body;
                try {
                    req_body = SJSON::Parse::string(inbound_req.body_to_string()).object();
                } catch (const SJSON::sjson_parse_error& e) {
                    return make_basic_resp(400);
                } catch (const std::bad_variant_access& e) {
                    return make_basic_resp(400);
                }
                outbound_req_headers["Content-Type"] = "application/json";

                std::string model;
                if (auto model_it = req_body.find("model"); model_it != req_body.end() && model_it->second.is_string()) {
                    model = model_it->second.string();
                } else {
                    return make_basic_resp(400, "Invalid model specified");
                }

                auto now = std::chrono::system_clock::now();

                struct HeadMessage {
                    uint16_t status_code;
                    pw::Headers headers;
                };
                using BodyMessage = std::vector<char>;
                struct EndMessage {};
                using Message = std::variant<HeadMessage, BodyMessage, EndMessage>;
                auto channel = std::make_shared<Channel<Message>>(8000);

                pw::threadpool.schedule([outbound_req_headers = std::move(outbound_req_headers), outbound_req_body = std::move(inbound_req.body), model = std::move(model), now, channel = std::weak_ptr<Channel<Message>>(channel)]() {
                    bool sent_head = false;

                    auto send_basic_resp = [&sent_head](Channel<Message>& channel, uint16_t status_code, const std::string& what = {}) {
                        if (!sent_head) {
                            channel.send(HeadMessage {status_code, {{"Content-Type", "text/plain"}}});
                            sent_head = true;

                            std::string outbound_resp_body;
                            if (what.empty()) {
                                outbound_resp_body = std::to_string(status_code) + ' ' + pw::status_code_to_reason_phrase(status_code);
                            } else {
                                outbound_resp_body = std::to_string(status_code) + ' ' + pw::status_code_to_reason_phrase(status_code) + ": " + what;
                            }
                            channel.send(BodyMessage(outbound_resp_body.begin(), outbound_resp_body.end()));
                        }
                        channel.send(EndMessage {});
                    };

                    try {
                        pw::SSEParser sse_parser([&model, now](pw::SSEEvent event) -> bool {
                            if (event.type != "message") return false;
                            if (event.data == "[DONE]") return true;

                            SJSON::JSObject message;
                            try {
                                message = SJSON::Parse::string(event.data).object();
                            } catch (const SJSON::sjson_parse_error& e) {
                                return false;
                            } catch (const std::bad_variant_access& e) {
                                return false;
                            }

                            if (auto usage_it = message.find("usage"); usage_it != message.end() && usage_it->second.is_object()) {
                                report_usage(usage_it->second, model, now);
                            }

                            return true;
                        });

                        SJSON::Parse json_parser;
                        json_parser.listen("usage", [&model, now](const SJSON::JSValue& value) {
                            report_usage(value, model, now);
                        });

                        pw::Response inbound_resp;
                        inbound_resp.recv_cb = metered_receiver([&channel, &sent_head, &sse_parser, &json_parser, &inbound_resp, content_type = std::string()](std::vector<char> chunk) mutable -> bool {
                            auto channel_locked = channel.lock();
                            if (!channel_locked) return false;

                            if (!sent_head) {
                                if (auto content_type_it = inbound_resp.headers.find("Content-Type"); content_type_it != inbound_resp.headers.end()) {
                                    content_type = pw::string::to_lower_copy(content_type_it->second);
                                }
                                channel_locked->send(HeadMessage {inbound_resp.status_code, inbound_resp.headers});
                                sent_head = true;
                            }

                            bool ret = true;
                            if (content_type.contains("application/json")) {
                                try {
                                    json_parser.recv(std::string(chunk.begin(), chunk.end()));
                                } catch (const SJSON::sjson_parse_error& e) {
                                    ret = false;
                                }
                            } else if (content_type.contains("text/event-stream")) {
                                ret = sse_parser(chunk);
                            }
                            channel_locked->send(std::move(chunk));
                            return ret;
                        },
                            32'000'000);
                        if (pn::Status result = pw::fetch("POST", "https://api.deepseek.com/chat/completions", inbound_resp, outbound_req_body, outbound_req_headers); !result) {
                            if (auto channel_locked = channel.lock()) {
                                send_basic_resp(*channel_locked.get(), 502);
                            }
                            return;
                        }

                        if (auto channel_locked = channel.lock()) {
                            if (!sent_head) {
                                channel_locked->send(HeadMessage {inbound_resp.status_code, std::move(inbound_resp.headers)});
                            }
                            channel_locked->send(EndMessage {});
                        }
                    } catch (...) {
                        if (auto channel_locked = channel.lock()) {
                            send_basic_resp(*channel_locked.get(), 500);
                        }
                    }
                },
                    true);

                Message message = channel->recv();
                if (std::holds_alternative<HeadMessage>(message)) {
                    auto head_message = std::get<HeadMessage>(message);

                    pw::Headers outbound_resp_headers;
                    if (auto content_type_it = head_message.headers.find("Content-Type"); content_type_it != head_message.headers.end()) {
                        outbound_resp_headers["Content-Type"] = content_type_it->second;
                    }
                    if (auto retry_after_it = head_message.headers.find("Retry-After"); retry_after_it != head_message.headers.end()) {
                        outbound_resp_headers["Retry-After"] = retry_after_it->second;
                    }

                    return pw::Response(head_message.status_code, [channel = std::move(channel)]() -> std::generator<std::vector<char>> {
                        for (;;) {
                            Message message = channel->recv();
                            if (std::holds_alternative<BodyMessage>(message)) {
                                co_yield std::move(std::get<BodyMessage>(message));
                            } else {
                                co_return;
                            }
                        }
                    },
                        outbound_resp_headers);
                } else {
                    return make_basic_resp(500);
                }
            },
        });

    if (pn::Status result = server.bind("127.0.0.1", 8787); !result) {
        SPDLOG_ERROR("Bind failed: {}", result.error().message());
        return 1;
    }

    SPDLOG_INFO("Listening on http://127.0.0.1:8787");
    if (pn::Status result = server.listen(); !result) {
        SPDLOG_ERROR("Listen failed: {}", result.error().message());
        return 1;
    }
}
