#include "Polyweb/polyweb.hpp"
#include "Polyweb/sse.hpp"
#include "SJSON/src/sjson.hpp"
#include "cost.hpp"
#include "util.hpp"
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

void report_usage(const SJSON::JSValue& usage, std::string_view model) {
    std::cout << "Got usage: " << usage.to_string(4) << std::endl;
    if (!usage.is_object()) {
        std::cout << "Cost estimate unavailable" << std::endl;
        return;
    }
    auto cost = deepseek_cost_range(usage.object(), model);
    if (!cost) {
        std::cout << "Cost estimate unavailable" << std::endl;
        return;
    }
    std::ostringstream amounts;
    amounts << std::fixed << std::setprecision(9)
            << "$" << static_cast<double>(cost->off_peak_nano_usd) / 1'000'000'000
            << " off-peak, $" << static_cast<double>(cost->peak_nano_usd) / 1'000'000'000 << " peak";
    std::cout << "Estimated cost (USD; published September 27, 2026 prices): " << amounts.str() << std::endl;
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

int main() {
    (void) pn::init();

    pw::Server server;

    server.route("/chat/completions",
        pw::Route {
            [](pw::Connection&, pw::Request& inbound_req) {
                if (inbound_req.method != "POST") {
                    return pw::Response::make_basic(405, {{"Allow", "POST"}});
                }

                pw::Headers outbound_req_headers;

                if (auto authorization_it = inbound_req.headers.find("Authorization"); authorization_it != inbound_req.headers.end()) {
                    outbound_req_headers["Authorization"] = authorization_it->second;
                } else {
                    return pw::Response::make_basic(401);
                }

                SJSON::JSObject req_body;
                try {
                    req_body = SJSON::Parse::string(inbound_req.body_to_string()).object();
                } catch (const SJSON::sjson_parse_error& e) {
                    return pw::Response::make_basic(400);
                } catch (const std::bad_variant_access& e) {
                    return pw::Response::make_basic(400);
                }
                outbound_req_headers["Content-Type"] = "application/json";
                std::string model;
                if (auto model_it = req_body.find("model"); model_it != req_body.end() && model_it->second.is_string()) {
                    model = model_it->second.string();
                }

                struct HeadMessage {
                    uint16_t status_code;
                    pw::Headers headers;
                };
                using BodyMessage = std::vector<char>;
                struct EndMessage {};
                using Message = std::variant<HeadMessage, BodyMessage, EndMessage>;
                auto channel = std::make_shared<Channel<Message>>(8000);

                pw::threadpool.schedule([outbound_req_headers = std::move(outbound_req_headers), outbound_req_body = std::move(inbound_req.body), model = std::move(model), channel = std::weak_ptr<Channel<Message>>(channel)]() {
                    bool sent_head = false;
                    try {
                        pw::SSEParser sse_parser([&model](pw::SSEEvent event) -> bool {
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
                                report_usage(usage_it->second, model);
                            }

                            return true;
                        });

                        SJSON::Parse json_parser;
                        json_parser.listen("usage", [&model](const SJSON::JSValue& value) {
                            report_usage(value, model);
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
                                if (!sent_head) {
                                    channel_locked->send(HeadMessage {502, {{"Content-Type", "text/plain"}}});
                                    std::string outbound_resp_body = "502 Bad Gateway";
                                    channel_locked->send(BodyMessage(outbound_resp_body.begin(), outbound_resp_body.end()));
                                }
                                channel_locked->send(EndMessage {});
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
                            if (!sent_head) {
                                channel_locked->send(HeadMessage {500, {{"Content-Type", "text/plain"}}});
                                std::string outbound_resp_body = "500 Internal Server Error";
                                channel_locked->send(BodyMessage(outbound_resp_body.begin(), outbound_resp_body.end()));
                            }
                            channel_locked->send(EndMessage {});
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
                    return pw::Response::make_basic(500);
                }
            },
        });

    if (pn::Status result = server.bind("127.0.0.1", 8787); !result) {
        std::cerr << "Bind failed: " << result.error().message() << std::endl;
        return 1;
    }

    std::cout << "Listening on http://127.0.0.1:8787" << std::endl;
    if (pn::Status result = server.listen(); !result) {
        std::cerr << "Listen failed: " << result.error().message() << std::endl;
        return 1;
    }
}
