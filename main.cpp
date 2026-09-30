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

struct HeadMessage {
    uint16_t status_code;
    pw::Headers headers;
};

using BodyMessage = std::vector<char>;

struct EndMessage {};

using Message = std::variant<HeadMessage, BodyMessage, EndMessage>;

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

pw::Response make_basic_resp(uint16_t status_code, pw::Headers headers = {}) {
    pw::Response resp(status_code, std::to_string(status_code) + ' ' + pw::status_code_to_reason_phrase(status_code), std::move(headers));
    if (!resp.headers.count("Content-Type")) {
        resp.headers["Content-Type"] = "text/plain";
    }
    return resp;
}

pw::Response make_basic_resp(uint16_t status_code, const std::string& what, pw::Headers headers = {}) {
    pw::Response resp(status_code, std::to_string(status_code) + ' ' + pw::status_code_to_reason_phrase(status_code) + ": " + what, std::move(headers));
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

std::string reset_time_utc(std::chrono::system_clock::time_point reset_at) {
    auto seconds = std::chrono::ceil<std::chrono::seconds>(reset_at);
    return pw::build_date(std::chrono::system_clock::to_time_t(seconds));
}

std::string window_reset_time_utc(std::optional<std::chrono::system_clock::time_point> started_at,
    std::chrono::system_clock::duration duration, std::chrono::system_clock::time_point time, uint64_t limit_nanodollars) {
    if (limit_nanodollars == 0) return "No automatic reset";
    if (!started_at || time >= *started_at + duration) return "Not scheduled";
    return reset_time_utc(*started_at + duration);
}

pw::Response usage_page(uint16_t status_code, const std::string& details = {}) {
    std::string html = R"(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>LLM Budget usage</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@picocss/pico@2.1.1/css/pico.classless.min.css" integrity="sha384-NZhm4G1I7BpEGdjDKnzEfy3d78xvy7ECKUwwnKTYi036z42IyF056PbHfpQLIYgL" crossorigin="anonymous">
<main><h1>LLM Budget usage</h1><form method="post" action="usage" autocomplete="off">
<label for="api-key">API key</label> <input id="api-key" name="api_key" type="password" maxlength="64" required autocomplete="off">
<button type="submit">Show usage</button></form>)";
    html += details;
    html += "</main></html>";
    return pw::Response(status_code, html, {
        {"Content-Type", "text/html; charset=utf-8"},
        {"Cache-Control", "no-store"},
        {"Referrer-Policy", "no-referrer"},
        {"Content-Security-Policy", "default-src 'none'; style-src https://cdn.jsdelivr.net; img-src data:; form-action 'self'; base-uri 'none'; frame-ancestors 'none'"},
        {"X-Content-Type-Options", "nosniff"},
    });
}

std::string usage_details(const User& user, const UserUsage& usage, std::chrono::system_clock::time_point time) {
    auto row = [time](std::string_view name, uint64_t cost, uint64_t limit,
                   std::optional<std::chrono::system_clock::time_point> started_at, std::chrono::system_clock::duration duration) {
        std::ostringstream result;
        result << "<tr><th scope=\"row\">" << name << "</th><td>";
        if (limit == 0) {
            result << "n/a";
        } else {
            result << std::fixed << std::setprecision(2) << (long double) cost * 100 / limit << '%';
        }
        result << "</td><td>" << window_reset_time_utc(started_at, duration, time, limit) << "</td></tr>";
        return result.str();
    };

    std::string details = "<h2>Usage for " + pw::xml_escape(user.name) + "</h2>";
    details += "<table><thead><tr><th>Window</th><th>Used</th><th>Reset (UTC)</th></tr></thead><tbody>";
    details += row("Five-hour", usage.five_hour_cost_nanodollars, usage.limits.five_hour_limit_nanodollars,
        usage.limits.five_hour_window_started_at, std::chrono::hours {5});
    details += row("Weekly", usage.weekly_cost_nanodollars, usage.limits.weekly_limit_nanodollars,
        usage.limits.weekly_window_started_at, std::chrono::weeks {1});
    details += "</tbody></table>";
    return details;
}

void send_basic_resp(Channel<Message>& channel, uint16_t status_code, pw::Headers headers = {}) {
    if (!headers.count("Content-Type")) {
        headers["Content-Type"] = "text/plain";
    }
    channel.send(HeadMessage {status_code, std::move(headers)});

    std::string outbound_resp_body;
    outbound_resp_body = std::to_string(status_code) + ' ' + pw::status_code_to_reason_phrase(status_code);
    channel.send(BodyMessage(outbound_resp_body.begin(), outbound_resp_body.end()));
};

void send_basic_resp(Channel<Message>& channel, uint16_t status_code, const std::string& what, pw::Headers headers = {}) {
    if (!headers.count("Content-Type")) {
        headers["Content-Type"] = "text/plain";
    }
    channel.send(HeadMessage {status_code, std::move(headers)});

    std::string outbound_resp_body;
    outbound_resp_body = std::to_string(status_code) + ' ' + pw::status_code_to_reason_phrase(status_code) + ": " + what;
    channel.send(BodyMessage(outbound_resp_body.begin(), outbound_resp_body.end()));
};

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

    server.route("/usage",
        pw::Route {
            logged_route([](pw::Connection&, pw::Request& request) {
                if (request.method == "GET") return usage_page(200);
                if (request.method != "POST") return usage_page(405);

                auto content_type = request.headers.find("Content-Type");
                if (content_type == request.headers.end() ||
                    !pw::string::to_lower_copy(content_type->second).starts_with("application/x-www-form-urlencoded")) {
                    return usage_page(400, "<p>Expected a form submission.</p>");
                }
                std::string body = request.body_to_string();
                if (body.size() > 256) return usage_page(400, "<p>Invalid form submission.</p>");

                pw::QueryParameters form(body);
                auto api_key = form->find("api_key");
                if (api_key == form->end()) return usage_page(400, "<p>API key is required.</p>");
                auto user = get_user_by_api_key(api_key->second);
                if (!user) return usage_page(401, "<p>Invalid API key.</p>");

                auto time = std::chrono::system_clock::now();
                auto usage = get_user_usage(user->id, time);
                if (!usage) return usage_page(500);
                return usage_page(200, usage_details(*user, *usage, time));
            }),
        });

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

                auto channel = std::make_shared<Channel<Message>>(8000);
                pw::threadpool.schedule([user = std::move(*user), outbound_req_headers = std::move(outbound_req_headers), req_body = std::move(req_body), model = std::move(model), service = std::move(*service), now, channel = std::weak_ptr<Channel<Message>>(channel)]() {
                    bool sent_head = false;
                    std::expected<request_id_t, BeginRequestError> request_id;
                    auto handle_failure = [&]() {
                        if (auto channel_locked = channel.lock()) {
                            if (!sent_head) send_basic_resp(*channel_locked, 500);
                            channel_locked->send(EndMessage {});
                        }
                        if (request_id) end_request(*request_id, REQUEST_STATE_UNKNOWN);
                    };
                    try {
                        UsageLimits usage_limits;
                        if (!(request_id = begin_request(user.id, usage_limits, now))) {
                            if (auto channel_locked = channel.lock()) {
                                if (request_id.error() == BEGIN_REQUEST_ERROR_USER_NOT_FOUND) {
                                    send_basic_resp(*channel_locked, 500);
                                } else if (!usage_limits.five_hour_limit_nanodollars || !usage_limits.weekly_limit_nanodollars) {
                                    send_basic_resp(*channel_locked, 403);
                                } else {
                                    switch (request_id.error()) {
                                    case BEGIN_REQUEST_ERROR_FIVE_HOUR_LIMIT:
                                        send_basic_resp(*channel_locked, 422, std::format("Five-hour limit exhausted. Reset at: {}", reset_time_utc(*usage_limits.five_hour_window_started_at + std::chrono::hours(5))));
                                        break;

                                    case BEGIN_REQUEST_ERROR_WEEKLY_LIMIT:
                                        send_basic_resp(*channel_locked, 422, std::format("Weekly limit exhausted. Reset at: {}", reset_time_utc(*usage_limits.weekly_window_started_at + std::chrono::weeks(1))));
                                        break;

                                    case BEGIN_REQUEST_ERROR_BOTH_LIMITS:
                                        send_basic_resp(*channel_locked, 422, std::format("Both weekly and five-hour limits exhausted. Reset at: {}", reset_time_utc(std::max(*usage_limits.five_hour_window_started_at + std::chrono::hours(5), *usage_limits.weekly_window_started_at + std::chrono::weeks(1)))));
                                        break;

                                    default:
                                        throw std::logic_error("Invalid BeginRequestError");
                                    }
                                }
                                channel_locked->send(EndMessage {});
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
                                if (!sent_head) send_basic_resp(*channel_locked, 502);
                                channel_locked->send(EndMessage {});
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
