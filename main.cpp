#include "Polyweb/polyweb.hpp"
#include "Polyweb/sse.hpp"
#include "SJSON/src/sjson.hpp"
#include "channel.hpp"
#include "cli.hpp"
#include "cost.hpp"
#include "database.hpp"
#include "provider_config.hpp"
#include "providers.hpp"
#include "usage_page.hpp"
#include <fstream>
#include <functional>
#include <iomanip>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>

struct HeadMessage {
    uint16_t status_code;
    pw::Headers headers;
};

using BodyMessage = std::vector<char>;

struct EndMessage {};

using Message = std::variant<HeadMessage, BodyMessage, EndMessage>;

// Bound upstream inactivity without imposing a total streaming duration.
const pw::ClientConfig upstream_client_config {
    .tcp = {
        .send_timeout = std::chrono::seconds {30},
        .recv_timeout = std::chrono::seconds {120},
    },
};

// Request cost accounting.
void print_cost(uint64_t nanodollars, const User& user, request_id_t request_id, std::string_view service, std::string_view model, const cost::TokenUsage& token_usage) {
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(9)
              << (double) nanodollars / 1'000'000'000;

    // Cache misses include cache writes; subtract writes for input reporting.
    uint64_t input_tokens = token_usage.cache_miss_tokens - token_usage.cache_creation_tokens;

    SPDLOG_INFO("Estimated cost: ${} (user={} id={}, request={}, service={}, model={}) tokens: input={} cache_creation={} cache_read={} output={} cache_hit_rate={:.2f}%", formatted.str(), user.name, user.id, request_id, service, model, input_tokens, token_usage.cache_creation_tokens, token_usage.cache_hit_tokens, token_usage.output_tokens, cost::cache_hit_rate(token_usage) * 100.0);
}

void record_request_cost(
    const std::optional<cost::TokenUsage>& token_usage,
    const User& user,
    request_id_t request_id,
    const providers::Provider& provider,
    const providers::Model& model,
    std::chrono::system_clock::time_point now,
    const cost::Multiplier& multiplier) {
    if (!token_usage) {
        SPDLOG_WARN("Cost estimate unavailable: Failed to parse usage");
        return;
    }

    auto amount = cost::calculate(model, *token_usage, now, multiplier);
    if (!amount) {
        SPDLOG_WARN("Cost estimate unavailable");
        return;
    }

    print_cost(*amount, user, request_id, provider.name, model.name, *token_usage);
    database::update_request(request_id, *amount);
}

// HTTP responses and route error handling.
pw::Response make_basic_resp(uint16_t status_code, pw::Headers headers = {}) {
    pw::Response resp(status_code, pw::status_code_to_reason_phrase(status_code), std::move(headers));
    if (!resp.headers.count("Content-Type")) {
        resp.headers["Content-Type"] = "text/plain";
    }
    return resp;
}

pw::Response make_basic_resp(uint16_t status_code, const std::string& what, pw::Headers headers = {}) {
    pw::Response resp(status_code, pw::status_code_to_reason_phrase(status_code) + ": " + what, std::move(headers));
    if (!resp.headers.count("Content-Type")) {
        resp.headers["Content-Type"] = "text/plain";
    }
    return resp;
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

void send_basic_resp(Channel<Message>& channel, uint16_t status_code, pw::Headers headers = {}) {
    if (!headers.count("Content-Type")) {
        headers["Content-Type"] = "text/plain";
    }
    channel.send(HeadMessage {status_code, std::move(headers)});

    std::string outbound_resp_body;
    outbound_resp_body = pw::status_code_to_reason_phrase(status_code);
    channel.send(BodyMessage(outbound_resp_body.begin(), outbound_resp_body.end()));
};

void send_basic_resp(Channel<Message>& channel, uint16_t status_code, const std::string& what, pw::Headers headers = {}) {
    if (!headers.count("Content-Type")) {
        headers["Content-Type"] = "text/plain";
    }
    channel.send(HeadMessage {status_code, std::move(headers)});

    std::string outbound_resp_body;
    outbound_resp_body = pw::status_code_to_reason_phrase(status_code) + ": " + what;
    channel.send(BodyMessage(outbound_resp_body.begin(), outbound_resp_body.end()));
};

void send_usage_error_resp(Channel<Message>& channel, BeginRequestError error, const UsageLimits& usage_limits) {
    if (error == BEGIN_REQUEST_ERROR_USER_NOT_FOUND) {
        send_basic_resp(channel, 500);
        return;
    }
    if (!usage_limits.five_hour_limit_nanodollars || !usage_limits.weekly_limit_nanodollars) {
        send_basic_resp(channel, 403);
        return;
    }

    std::string_view exhausted_limits;
    std::chrono::system_clock::time_point reset_at;
    switch (error) {
    case BEGIN_REQUEST_ERROR_FIVE_HOUR_LIMIT:
        exhausted_limits = "Five-hour";
        reset_at = *usage_limits.five_hour_window_started_at + std::chrono::hours(5);
        break;

    case BEGIN_REQUEST_ERROR_WEEKLY_LIMIT:
        exhausted_limits = "Weekly";
        reset_at = *usage_limits.weekly_window_started_at + std::chrono::weeks(1);
        break;

    case BEGIN_REQUEST_ERROR_BOTH_LIMITS:
        exhausted_limits = "Both weekly and five-hour";
        reset_at = std::max(
            *usage_limits.five_hour_window_started_at + std::chrono::hours(5),
            *usage_limits.weekly_window_started_at + std::chrono::weeks(1));
        break;

    default:
        throw std::invalid_argument("Invalid BeginRequestError");
    }

    auto retry_delay = std::chrono::ceil<std::chrono::seconds>(reset_at - std::chrono::system_clock::now());
    auto retry_after_seconds = std::max<std::int64_t>(1, retry_delay.count());
    auto reset_at_seconds = std::chrono::ceil<std::chrono::seconds>(reset_at);
    send_basic_resp(channel, 429, std::format("{} limit exhausted. Reset at: {}", exhausted_limits, pw::build_date(std::chrono::system_clock::to_time_t(reset_at_seconds))), {{"Retry-After", std::to_string(retry_after_seconds)}});
}

// Streaming proxy response transport and usage parsing.
pw::Response receive_proxy_response(std::shared_ptr<Channel<Message>> channel) {
    Message message = channel->recv();
    if (!std::holds_alternative<HeadMessage>(message)) {
        return make_basic_resp(500);
    }

    auto head_message = std::get<HeadMessage>(message);

    pw::Headers outbound_resp_headers;
    auto content_type_it = head_message.headers.find("Content-Type");
    if (content_type_it != head_message.headers.end()) {
        outbound_resp_headers["Content-Type"] = content_type_it->second;
    }

    auto retry_after_it = head_message.headers.find("Retry-After");
    if (retry_after_it != head_message.headers.end()) {
        outbound_resp_headers["Retry-After"] = retry_after_it->second;
    }

    return pw::Response(
        head_message.status_code,
        [channel = std::move(channel)](this auto) -> std::generator<std::vector<char>> {
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

void configure_response_receiver(
    pw::Response& inbound_resp,
    const std::weak_ptr<Channel<Message>>& channel,
    bool& sent_head,
    pw::SSEParser& sse_parser,
    SJSON::Parse& json_parser) {
    inbound_resp.recv_cb = metered_receiver(
        [&channel, &sent_head, &sse_parser, &json_parser, &inbound_resp, content_type = std::string()](std::vector<char> chunk) mutable -> bool {
            auto channel_locked = channel.lock();
            if (!channel_locked) {
                return false;
            }

            if (!sent_head) {
                auto content_type_it = inbound_resp.headers.find("Content-Type");
                if (content_type_it != inbound_resp.headers.end()) {
                    content_type = pw::string::to_lower_copy(content_type_it->second);
                }

                channel_locked->send(HeadMessage {
                    inbound_resp.status_code,
                    inbound_resp.headers,
                });
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
}

// Startup, route registration, and server lifecycle.
int main(int argc, char** argv) {
    (void) pn::init();
    spdlog::cfg::load_env_levels();
    if (argc > 1) return run_cli(argc, argv);
    database::init();

    providers::Configuration configured;
    {
        std::ifstream keys_file("keys.json");
        if (!keys_file.is_open()) {
            SPDLOG_ERROR("Could not open keys.json");
            return 1;
        }

        try {
            auto json = SJSON::Parse::string(std::string(std::istreambuf_iterator<char> {keys_file}, std::istreambuf_iterator<char> {}));
            configured = providers::configure(std::move(json.object()));
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Failed to parse keys.json: {}", e.what());
            return 1;
        }
    }

    UsagePageAssets usage_assets;
    try {
        usage_assets = load_usage_page_assets();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to load usage page: {}", e.what());
        return 1;
    }

    pw::Server server;

    server.error_cb = [](uint16_t status_code, pn::StringView what) {
        if (what.empty()) {
            return make_basic_resp(status_code);
        }
        return make_basic_resp(status_code, std::string(what));
    };

    server.route("/usage.css",
        pw::Route {
            logged_route([&usage_assets](pw::Connection&, pw::Request& request) {
                return make_usage_asset_resp(request, usage_assets.css, "text/css; charset=utf-8");
            }),
        });

    server.route("/usage.js",
        pw::Route {
            logged_route([&usage_assets](pw::Connection&, pw::Request& request) {
                return make_usage_asset_resp(request, usage_assets.js, "text/javascript; charset=utf-8");
            }),
        });

    server.route("/usage",
        pw::Route {
            logged_route([&usage_assets](pw::Connection&, pw::Request& request) {
                return handle_usage_request(usage_assets, request);
            }),
        });

    for (auto protocol : {providers::PROTOCOL_CHAT_COMPLETIONS, providers::PROTOCOL_RESPONSES}) {
        server.route(protocol == providers::PROTOCOL_RESPONSES ? "/responses" : "/chat/completions",
            pw::Route {
                logged_route([&configured, protocol](pw::Connection&, pw::Request& inbound_req) {
                    if (inbound_req.method != "POST") {
                        return make_basic_resp(405, {{"Allow", "POST"}});
                    }

                    std::optional<User> user;
                    if (auto authorization_it = inbound_req.headers.find("Authorization"); authorization_it != inbound_req.headers.end()) {
                        auto authorization_split = pw::string::split_and_trim(authorization_it->second, ' ');
                        if (authorization_split.size() != 2 || !pw::string::iequals(authorization_split.front(), "Bearer")) {
                            return make_basic_resp(401);
                        }

                        if (!(user = database::get_user_by_api_key(authorization_split.back()))) {
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

                    std::string_view model;
                    if (auto model_it = req_body.find("model"); model_it != req_body.end() && model_it->second.is_string()) {
                        model = model_it->second.string();
                    } else {
                        return make_basic_resp(400, "Invalid model specified");
                    }

                    auto selection = providers::resolve(configured, model);
                    if (!selection) return make_basic_resp(400, "Invalid model specified");

                    if (!selection->model->supports(protocol)) {
                        return make_basic_resp(400, protocol == providers::PROTOCOL_RESPONSES ? "Model does not support Responses" : "Model does not support Chat Completions");
                    }

                    const auto* provider = selection->provider;
                    if (!provider->configured()) {
                        return make_basic_resp(503, "Provider is not configured");
                    }

                    req_body["model"] = selection->model->name;

                    if (auto error = provider->prepare_request(protocol,
                            inbound_req.headers,
                            outbound_req_headers,
                            req_body,
                            user->name)) {
                        return make_basic_resp(400, std::string(*error));
                    }

                    auto multiplier = provider->multiplier(*selection->model);
                    if (!multiplier) return make_basic_resp(503, "Pricing unavailable");

                    auto now = std::chrono::system_clock::now();

                    auto channel = std::make_shared<Channel<Message>>(8000);
                    pw::threadpool.schedule([user = std::move(*user),
                                                outbound_req_headers = std::move(outbound_req_headers),
                                                req_body = std::move(req_body),
                                                definition = &provider->definition(),
                                                selected_model = selection->model,
                                                multiplier = *multiplier,
                                                now,
                                                protocol,
                                                channel = std::weak_ptr<Channel<Message>>(channel)]() {
                        bool sent_head = false;
                        std::expected<request_id_t, BeginRequestError> request_id;
                        auto handle_failure = [&]() {
                            if (auto channel_locked = channel.lock()) {
                                if (!sent_head) send_basic_resp(*channel_locked, 500);
                                channel_locked->send(EndMessage {});
                            }
                            if (request_id) database::end_request(*request_id, REQUEST_STATE_UNKNOWN);
                        };
                        try {
                            UsageLimits usage_limits;
                            if (!(request_id = database::begin_request(user.id, usage_limits, now))) {
                                if (auto channel_locked = channel.lock()) {
                                    send_usage_error_resp(*channel_locked, request_id.error(), usage_limits);
                                    channel_locked->send(EndMessage {});
                                }
                                return;
                            }

                            pw::SSEParser sse_parser([&user, definition, selected_model, &multiplier, now, protocol, request_id = *request_id](pw::SSEEvent event) -> bool {
                                auto is_terminal_response = [](std::string_view type) {
                                    return type == "response.completed" || type == "response.incomplete" || type == "response.failed";
                                };

                                if (protocol == providers::PROTOCOL_RESPONSES) {
                                    if (event.type != "message" && !is_terminal_response(event.type)) return true;
                                } else {
                                    if (event.type != "message") return false;
                                    if (event.data == "[DONE]") return true;
                                }

                                SJSON::JSObject message;
                                try {
                                    message = SJSON::Parse::string(event.data).object();
                                } catch (const SJSON::sjson_parse_error& e) {
                                    return false;
                                } catch (const std::bad_variant_access& e) {
                                    return false;
                                }

                                const SJSON::JSObject* response = &message;
                                if (protocol == providers::PROTOCOL_RESPONSES) {
                                    if (!is_terminal_response(event.type)) {
                                        auto type = message.find("type");
                                        if (type == message.end() || !type->second.is_string() || !is_terminal_response(type->second.string())) return true;
                                    }

                                    auto reponse_it = message.find("response");
                                    if (reponse_it == message.end() || !reponse_it->second.is_object()) return true;
                                    response = &reponse_it->second.object();
                                }

                                if (auto usage_it = response->find("usage"); usage_it != response->end() && usage_it->second.is_object()) {
                                    auto usage = (protocol == providers::PROTOCOL_RESPONSES) ? cost::from_responses_usage(usage_it->second.object()) : cost::from_chat_completions_usage(usage_it->second.object());
                                    record_request_cost(usage, user, request_id, *definition, *selected_model, now, multiplier);
                                }

                                return true;
                            });

                            SJSON::Parse json_parser;
                            json_parser.listen("usage", [&user, definition, selected_model, &multiplier, now, protocol, request_id = *request_id](const SJSON::JSValue& usage) {
                                if (!usage.is_object()) {
                                    SPDLOG_WARN("Cost estimate unavailable");
                                    return;
                                }
                                auto tokens = protocol == providers::PROTOCOL_RESPONSES ? cost::from_responses_usage(usage.object()) : cost::from_chat_completions_usage(usage.object());
                                record_request_cost(tokens, user, request_id, *definition, *selected_model, now, multiplier);
                            });

                            pw::Response inbound_resp;
                            configure_response_receiver(inbound_resp, channel, sent_head, sse_parser, json_parser);
                            std::string upstream_url = protocol == providers::PROTOCOL_RESPONSES
                                                           ? std::string(definition->responses_base_url) + "/responses"
                                                           : std::string(definition->chat_completions_base_url) + "/chat/completions";
                            if (pn::Status result = pw::fetch("POST", upstream_url, inbound_resp, SJSON::JSValue(req_body).to_string(), outbound_req_headers, upstream_client_config); !result) {
                                if (auto channel_locked = channel.lock()) {
                                    if (!sent_head) send_basic_resp(*channel_locked, 502);
                                    channel_locked->send(EndMessage {});
                                }
                                database::end_request(*request_id, REQUEST_STATE_INTERRUPTED);
                                return;
                            }

                            if (auto channel_locked = channel.lock()) {
                                if (!sent_head) {
                                    channel_locked->send(HeadMessage {inbound_resp.status_code, std::move(inbound_resp.headers)});
                                }
                                channel_locked->send(EndMessage {});
                            }
                            database::end_request(*request_id, REQUEST_STATE_COMPLETED);
                        } catch (const std::exception& e) {
                            SPDLOG_ERROR("Proxy request failed: {}", e.what());
                            handle_failure();
                        } catch (...) {
                            SPDLOG_ERROR("Proxy request failed with a non-standard exception");
                            handle_failure();
                        }
                    },
                        true);

                    return receive_proxy_response(std::move(channel));
                }),
            });
    }

    server.route("/v1/messages",
        pw::Route {
            logged_route([&configured](pw::Connection&, pw::Request& inbound_req) {
                if (inbound_req.method != "POST") {
                    return make_basic_resp(405, {{"Allow", "POST"}});
                }

                std::optional<User> user;
                if (auto authorization_it = inbound_req.headers.find("Authorization"); authorization_it != inbound_req.headers.end()) {
                    auto authorization_split = pw::string::split_and_trim(authorization_it->second, ' ');
                    if (authorization_split.size() != 2 || !pw::string::iequals(authorization_split.front(), "Bearer")) {
                        return make_basic_resp(401);
                    }

                    if (!(user = database::get_user_by_api_key(authorization_split.back()))) {
                        return make_basic_resp(401);
                    }
                } else if (auto api_key_it = inbound_req.headers.find("x-api-key"); api_key_it != inbound_req.headers.end()) {
                    if (!(user = database::get_user_by_api_key(api_key_it->second))) {
                        return make_basic_resp(401);
                    }
                } else {
                    return make_basic_resp(401);
                }

                pw::Headers outbound_req_headers = {
                    {"Content-Type", "application/json"},
                    {"anthropic-version", "2023-06-01"},
                };

                SJSON::JSObject req_body;
                try {
                    req_body = SJSON::Parse::string(inbound_req.body_to_string()).object();
                } catch (const SJSON::sjson_parse_error& e) {
                    return make_basic_resp(400);
                } catch (const std::bad_variant_access& e) {
                    return make_basic_resp(400);
                }

                std::string_view model;
                if (auto model_it = req_body.find("model"); model_it != req_body.end() && model_it->second.is_string()) {
                    model = model_it->second.string();
                } else {
                    return make_basic_resp(400, "Invalid model specified");
                }

                auto selection = providers::resolve(configured, model);
                if (!selection) return make_basic_resp(400, "Invalid model specified");

                if (!selection->model->supports(providers::PROTOCOL_ANTHROPIC_MESSAGES)) {
                    return make_basic_resp(400, "Model does not support Anthropic Messages");
                }

                const auto* provider = selection->provider;
                if (!provider->configured()) {
                    return make_basic_resp(503, "Provider is not configured");
                }

                req_body["model"] = selection->model->name;

                if (auto error = provider->prepare_request(providers::PROTOCOL_ANTHROPIC_MESSAGES,
                        inbound_req.headers,
                        outbound_req_headers,
                        req_body,
                        user->name)) {
                    return make_basic_resp(400, std::string(*error));
                }

                auto multiplier = provider->multiplier(*selection->model);
                if (!multiplier) return make_basic_resp(503, "Pricing unavailable");

                auto now = std::chrono::system_clock::now();

                auto channel = std::make_shared<Channel<Message>>(8000);
                pw::threadpool.schedule([user = std::move(*user),
                                            outbound_req_headers = std::move(outbound_req_headers),
                                            req_body = std::move(req_body),
                                            definition = &provider->definition(),
                                            selected_model = selection->model,
                                            multiplier = *multiplier,
                                            now,
                                            channel = std::weak_ptr<Channel<Message>>(channel)]() {
                    bool sent_head = false;
                    std::expected<request_id_t, BeginRequestError> request_id;
                    auto handle_failure = [&]() {
                        if (auto channel_locked = channel.lock()) {
                            if (!sent_head) send_basic_resp(*channel_locked, 500);
                            channel_locked->send(EndMessage {});
                        }
                        if (request_id) database::end_request(*request_id, REQUEST_STATE_UNKNOWN);
                    };
                    try {
                        UsageLimits usage_limits;
                        if (!(request_id = database::begin_request(user.id, usage_limits, now))) {
                            if (auto channel_locked = channel.lock()) {
                                send_usage_error_resp(*channel_locked, request_id.error(), usage_limits);
                                channel_locked->send(EndMessage {});
                            }
                            return;
                        }

                        pw::SSEParser sse_parser([&user, definition, selected_model, &multiplier, now, request_id = *request_id, current_usage = SJSON::JSObject {}](pw::SSEEvent event) mutable -> bool {
                            if (event.type != "message_start" && event.type != "message_delta") return true;

                            SJSON::JSObject message;
                            try {
                                message = SJSON::Parse::string(event.data).object();
                            } catch (const SJSON::sjson_parse_error& e) {
                                return false;
                            } catch (const std::bad_variant_access& e) {
                                return false;
                            }

                            const SJSON::JSObject* event_body = &message;
                            if (event.type == "message_start") {
                                current_usage.clear();
                                auto message_it = message.find("message");
                                if (message_it == message.end() || !message_it->second.is_object()) return true;
                                event_body = &message_it->second.object();
                            }

                            auto usage_it = event_body->find("usage");
                            if (usage_it == event_body->end() || !usage_it->second.is_object()) return true;
                            // Counts are cumulative: replace reported fields and
                            // retain omitted fields, including initial input/cache counts.
                            for (const auto& [field, value] : usage_it->second.object()) {
                                current_usage[field] = value;
                            }
                            record_request_cost(cost::from_anthropic_usage(current_usage), user, request_id, *definition, *selected_model, now, multiplier);

                            return true;
                        });

                        SJSON::Parse json_parser;
                        json_parser.listen("usage", [&user, definition, selected_model, &multiplier, now, request_id = *request_id](const SJSON::JSValue& usage) {
                            if (!usage.is_object()) {
                                SPDLOG_WARN("Cost estimate unavailable");
                                return;
                            }
                            record_request_cost(cost::from_anthropic_usage(usage.object()), user, request_id, *definition, *selected_model, now, multiplier);
                        });

                        pw::Response inbound_resp;
                        configure_response_receiver(inbound_resp, channel, sent_head, sse_parser, json_parser);
                        if (pn::Status result = pw::fetch("POST", std::string(definition->anthropic_messages_base_url) + "/v1/messages", inbound_resp, SJSON::JSValue(req_body).to_string(), outbound_req_headers, upstream_client_config); !result) {
                            if (auto channel_locked = channel.lock()) {
                                if (!sent_head) send_basic_resp(*channel_locked, 502);
                                channel_locked->send(EndMessage {});
                            }
                            database::end_request(*request_id, REQUEST_STATE_INTERRUPTED);
                            return;
                        }

                        if (auto channel_locked = channel.lock()) {
                            if (!sent_head) {
                                channel_locked->send(HeadMessage {inbound_resp.status_code, std::move(inbound_resp.headers)});
                            }
                            channel_locked->send(EndMessage {});
                        }
                        database::end_request(*request_id, REQUEST_STATE_COMPLETED);
                    } catch (const std::exception& e) {
                        SPDLOG_ERROR("Proxy request failed: {}", e.what());
                        handle_failure();
                    } catch (...) {
                        SPDLOG_ERROR("Proxy request failed with a non-standard exception");
                        handle_failure();
                    }
                },
                    true);

                return receive_proxy_response(std::move(channel));
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
