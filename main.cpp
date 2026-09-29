#include "Polyweb/polyweb.hpp"
#include "Polyweb/sse.hpp"
#include "SJSON/src/sjson.hpp"
#include "cli.hpp"
#include "cost.hpp"
#include "database.hpp"
#include "util.hpp"
#include <fstream>
#include <functional>
#include <iomanip>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

struct Service {
    std::string name;
    std::string base_url;
};

std::optional<Service> model_to_service(std::string_view model) {
    if (model == "deepseek-v4-pro" || model == "deepseek-flash") {
        return Service {"deepseek", "https://api.deepseek.com"};
    }
    return std::nullopt;
}

void print_cost(const cost::Money& amount, const User& user, request_id_t request_id, std::string_view service, std::string_view model) {
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(9)
              << (double) amount.nano_units / 1'000'000'000;
    SPDLOG_INFO("Estimated cost: {} {} (user={} id={}, request={}, service={}, model={})", formatted.str(), amount.currency, user.name, user.id, request_id, service, model);
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

template <typename F>
auto logged_route(F func) {
    return [func = std::move(func)](pw::Connection& conn, pw::RequestReceiver& req) -> pw::Response {
        try {
            pw::Response resp = func(conn, req);
            SPDLOG_INFO("Request method={} status={} target={}", req.method, resp.status_code, req.target);
            return resp;
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Request method={} status=500 target={} failed: {}", req.method, req.target, e.what());
            return make_basic_resp(500);
        } catch (...) {
            SPDLOG_ERROR("Request method={} status=500 target={} failed with a non-standard exception", req.method, req.target);
            return make_basic_resp(500);
        }
    };
}

int main(int argc, char** argv) {
    (void) pn::init();
    spdlog::cfg::load_env_levels();
    if (argc > 1) return cli::run(argc, argv);
    init();

    SJSON::JSObject keys;
    {
        std::ifstream keys_file("keys.json");
        if (!keys_file.is_open()) {
            SPDLOG_ERROR("Could not open keys.json");
            return 1;
        }

        try {
            keys = SJSON::Parse::string(std::string(std::istreambuf_iterator<char> {keys_file}, std::istreambuf_iterator<char> {})).object();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to parse keys.json: {}", e.what());
            return 1;
        }
    }

    pw::Server server;

    server.error_cb = [](uint16_t status_code, pn::StringView what) {
        if (what.empty()) {
            return make_basic_resp(status_code);
        }
        return make_basic_resp(status_code, std::string(what));
    };

    server.route("/chat/completions",
        pw::Route {
            logged_route([&keys](pw::Connection&, pw::Request& inbound_req) {
                if (inbound_req.method != "POST") {
                    return make_basic_resp(405, {{"Allow", "POST"}});
                }

                std::optional<User> user;
                if (auto authorization_it = inbound_req.headers.find("Authorization"); authorization_it != inbound_req.headers.end()) {
                    auto authorization_split = pw::string::split_and_trim(authorization_it->second, ' ');
                    if (authorization_split.size() != 2 || !pw::string::iequals(authorization_split.front(), "Bearer")) {
                        return make_basic_resp(401);
                    }

                    if (!(user = get_user_by_api_key(authorization_split.back()))) {
                        return make_basic_resp(401);
                    }
                } else {
                    return make_basic_resp(401);
                }

                pw::Headers outbound_req_headers = {{"Content-Type", "application/json"}};

                SJSON::JSObject req_body;
                try {
                    req_body = SJSON::Parse::string(inbound_req.body_to_string()).object();
                } catch (const SJSON::sjson_parse_error& e) {
                    return make_basic_resp(400);
                } catch (const std::bad_variant_access& e) {
                    return make_basic_resp(400);
                }

                std::string model;
                if (auto model_it = req_body.find("model"); model_it != req_body.end() && model_it->second.is_string()) {
                    model = model_it->second.string();
                } else {
                    return make_basic_resp(400, "Invalid model specified");
                }

                std::optional<Service> service = model_to_service(model);
                if (!service) {
                    return make_basic_resp(400, "Invalid model specified");
                }

                outbound_req_headers["Authorization"] = "Bearer " + keys.at(service->name).string();
                if (service->name == "deepseek") {
                    req_body["user_id"] = user->name;
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

                pw::threadpool.schedule([user = std::move(*user), outbound_req_headers = std::move(outbound_req_headers), req_body = std::move(req_body), model = std::move(model), service = std::move(*service), now, channel = std::weak_ptr<Channel<Message>>(channel)]() {
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

                    std::expected<request_id_t, BeginRequestError> request_id;
                    auto handle_failure = [&]() {
                        if (auto channel_locked = channel.lock()) {
                            send_basic_resp(*channel_locked, 500);
                        }
                        if (request_id) end_request(*request_id, REQUEST_STATE_UNKNOWN);
                    };
                    try {
                        UsageLimits usage_limits;
                        if (!(request_id = begin_request(user.id, usage_limits, now))) {
                            if (auto channel_locked = channel.lock()) {
                                switch (request_id.error()) {
                                case BEGIN_REQUEST_ERROR_USER_NOT_FOUND:
                                    send_basic_resp(*channel_locked, 500);
                                    break;

                                case BEGIN_REQUEST_ERROR_FIVE_HOUR_LIMIT:
                                    send_basic_resp(*channel_locked, 422, std::format("Five-hour limit exhausted. Reset at: {}", pw::build_date(std::chrono::system_clock::to_time_t(*usage_limits.five_hour_window_started_at + std::chrono::hours(5)))));
                                    break;

                                case BEGIN_REQUEST_ERROR_WEEKLY_LIMIT:
                                    send_basic_resp(*channel_locked, 422, std::format("Weekly limit exhausted. Reset at: {}", pw::build_date(std::chrono::system_clock::to_time_t(*usage_limits.weekly_window_started_at + std::chrono::weeks(1)))));
                                    break;

                                case BEGIN_REQUEST_ERROR_BOTH_LIMITS:
                                    send_basic_resp(*channel_locked, 422, std::format("Both weekly and five-hour limits exhausted. Reset at: {}", pw::build_date(std::chrono::system_clock::to_time_t(std::max(*usage_limits.five_hour_window_started_at + std::chrono::hours(5), *usage_limits.weekly_window_started_at + std::chrono::weeks(1))))));
                                    break;
                                }
                            }
                            return;
                        }

                        pw::SSEParser sse_parser([&user, &model, &service, now, request_id = *request_id](pw::SSEEvent event) -> bool {
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
                                auto amount = cost::calculate(service.name, model, usage_it->second.object(), now);
                                if (!amount) {
                                    SPDLOG_WARN("Cost estimate unavailable");
                                    return true;
                                }
                                print_cost(*amount, user, request_id, service.name, model);

                                if (amount->currency == "USD") {
                                    update_request(request_id, amount->nano_units);
                                }
                            }

                            return true;
                        });

                        SJSON::Parse json_parser;
                        json_parser.listen("usage", [&user, &model, &service, now, request_id = *request_id](const SJSON::JSValue& usage) {
                            if (!usage.is_object()) {
                                SPDLOG_WARN("Cost estimate unavailable");
                                return;
                            }

                            auto amount = cost::calculate(service.name, model, usage.object(), now);
                            if (!amount) {
                                SPDLOG_WARN("Cost estimate unavailable");
                                return;
                            }
                            print_cost(*amount, user, request_id, service.name, model);

                            if (amount->currency == "USD") {
                                update_request(request_id, amount->nano_units);
                            }
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
                        if (pn::Status result = pw::fetch("POST", service.base_url + "/chat/completions", inbound_resp, SJSON::JSValue(req_body).to_string(), outbound_req_headers); !result) {
                            if (auto channel_locked = channel.lock()) {
                                send_basic_resp(*channel_locked, 502);
                            }
                            end_request(*request_id, REQUEST_STATE_INTERRUPTED);
                            return;
                        }

                        if (auto channel_locked = channel.lock()) {
                            if (!sent_head) {
                                channel_locked->send(HeadMessage {inbound_resp.status_code, std::move(inbound_resp.headers)});
                            }
                            channel_locked->send(EndMessage {});
                        }
                        end_request(*request_id, REQUEST_STATE_COMPLETED);
                    } catch (const std::exception& e) {
                        SPDLOG_ERROR("Proxy request failed: {}", e.what());
                        handle_failure();
                    } catch (...) {
                        SPDLOG_ERROR("Proxy request failed with a non-standard exception");
                        handle_failure();
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
            }),
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
