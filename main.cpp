#include "Polyweb/polyweb.hpp"
#include "SJSON/src/sjson.hpp"
#include "util.hpp"
#include <functional>
#include <iostream>
#include <string>

class SSEEvent {
public:
    std::string type = "message";
    std::string data;

    SSEEvent(std::string data = {}):
        data(std::move(data)) {}
    SSEEvent(std::string event, std::string data):
        type(std::move(event)),
        data(std::move(data)) {}

    std::string build() const {
        std::string ret = "event: " + type + "\r\n";

        size_t line_start = 0;
        for (size_t i = 0; i < data.size(); ++i) {
            if (data[i] != '\r' && data[i] != '\n') {
                continue;
            }

            ret += "data: ";
            ret.append(data, line_start, i - line_start);
            ret += "\r\n";

            if (data[i] == '\r' && i + 1 < data.size() && data[i + 1] == '\n') {
                ++i;
            }
            line_start = i + 1;
        }

        ret += "data: ";
        ret.append(data, line_start, data.size() - line_start);
        ret += "\r\n\r\n";

        return ret;
    }
};

class SSEParser {
protected:
    std::string buf;
    SSEEvent current_event;
    std::move_only_function<bool(SSEEvent)> event_cb;
    bool skip_next_lf = false;

    bool handle_data() {
        while (!buf.empty()) {
            if (skip_next_lf && buf.front() == '\n') {
                buf.erase(0, 1);
            }
            skip_next_lf = false;

            size_t end = buf.find_first_of("\r\n");
            if (end == std::string::npos) {
                break;
            }

            std::string line = buf.substr(0, end);

            size_t delimiter_length = 1;
            if (buf[end] == '\r') {
                if (end + 1 < buf.size() && buf[end + 1] == '\n') {
                    delimiter_length = 2;
                } else {
                    skip_next_lf = true;
                }
            }
            buf.erase(0, end + delimiter_length);

            if (line.empty()) {
                if (current_event.data.empty()) {
                    current_event = {};
                    continue;
                }
                if (current_event.data.back() == '\n') {
                    current_event.data.pop_back();
                }

                if (!event_cb(std::exchange(current_event, {}))) {
                    return false;
                }
            }

            size_t colon_pos = line.find(':');

            std::string field = line.substr(0, colon_pos);
            if (field.empty()) {
                continue;
            }

            std::string value;
            if (colon_pos < line.size()) {
                if (colon_pos + 1 < line.size() && line[colon_pos + 1] == ' ') {
                    value = line.substr(colon_pos + 2);
                } else {
                    value = line.substr(colon_pos + 1);
                }
            }

            if (field == "event") {
                current_event.type = value.empty() ? "message" : std::move(value);
            } else if (field == "data") {
                current_event.data += value + '\n';
            }
        }
        return true;
    }

public:
    SSEParser(decltype(event_cb) event_cb):
        event_cb(std::move(event_cb)) {}

    template <typename T>
    bool operator()(const T& data) {
        buf.insert(buf.end(), data.begin(), data.end());
        return handle_data();
    }
};

int main() {
    (void) pn::init();

    pw::Server server;

    server.route("/chat/completions",
        pw::Route {
            [](pw::Connection&, pw::RequestReceiver& req) {
                if (req.method != "POST") {
                    return pw::Response::make_basic(405, {{"Allow", "POST"}});
                }

                pw::Headers req_headers;
                if (auto authorization_it = req.headers.find("Authorization"); authorization_it != req.headers.end()) {
                    req_headers["Authorization"] = authorization_it->second;
                } else {
                    return pw::Response::make_basic(401);
                }
                req_headers["Content-Type"] = "application/json";

                SJSON::JSObject req_body;
                try {
                    req_body = SJSON::Parse::string(req.body_to_string()).object();
                } catch (const SJSON::sjson_parse_error& e) {
                    return pw::Response::make_basic(400);
                } catch (const std::bad_variant_access& e) {
                    return pw::Response::make_basic(400);
                }

                bool stream = false;
                if (auto stream_it = req_body.find("stream"); stream_it != req_body.end() && stream_it->second.is_boolean()) {
                    stream = stream_it->second.boolean();
                }
                req_headers["Accept"] = stream ? "text/event-stream" : "application/json";

                using StatusCodeMessage = uint16_t;
                using BodyMessage = std::vector<char>;
                struct ErrorMessage {};
                struct SuccessMessage {};
                using Message = std::variant<StatusCodeMessage, BodyMessage, ErrorMessage, SuccessMessage>;

                auto channel = std::make_shared<Channel<Message>>();

                pw::threadpool.schedule([req_headers = std::move(req_headers), req_body = std::move(req_body), stream, channel = std::weak_ptr<Channel<Message>>(channel)]() {
                    try {
                        pw::Response resp;
                        bool sent_status_code = false;
                        resp.recv_cb = [stream, &channel, &resp, &sent_status_code](std::vector<char> chunk) mutable -> bool {
                            auto channel_locked = channel.lock();
                            if (!channel_locked) return false;

                            if (!sent_status_code) {
                                if (auto content_type_it = resp.headers.find("Content-Type"); content_type_it != resp.headers.end()) {
                                    std::string_view expected_content_type = stream ? "text/event-stream" : "application/json";
                                    if (!content_type_it->second.contains(expected_content_type)) {
                                        return false;
                                    }
                                }

                                channel_locked->send(resp.status_code);
                                sent_status_code = true;
                            }

                            channel_locked->send(std::move(chunk));
                            return true;
                        };

                        if (pn::Status result = pw::fetch("POST", "https://api.deepseek.com/chat/completions", resp, SJSON::JSValue(req_body).to_string(), req_headers); !result) {
                            if (auto channel_locked = channel.lock()) {
                                channel_locked->send(ErrorMessage {});
                            }
                            return;
                        }

                        if (auto channel_locked = channel.lock()) {
                            if (!sent_status_code) {
                                channel_locked->send(resp.status_code);
                            }
                            channel_locked->send(SuccessMessage {});
                        }
                    } catch (...) {
                        if (auto channel_locked = channel.lock()) {
                            channel_locked->send(ErrorMessage {});
                        }
                    }
                },
                    true);

                Message message = channel->recv();
                if (std::holds_alternative<StatusCodeMessage>(message)) {
                    uint16_t status_code = std::get<StatusCodeMessage>(message);
                    return pw::Response(status_code, [stream, channel = std::move(channel)]() -> std::generator<std::vector<char>> {
                        if (stream) {
                            SSEParser parser([](SSEEvent event) -> bool {
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
                                    SJSON::JSObject usage = usage_it->second.object();
                                    std::cout << "Got usage: " << SJSON::JSValue(usage).to_string(4) << std::endl;
                                }

                                return true;
                            });

                            for (;;) {
                                Message message = channel->recv();
                                if (std::holds_alternative<BodyMessage>(message)) {
                                    std::vector<char> chunk = std::move(std::get<BodyMessage>(message));
                                    co_yield chunk;
                                    if (!parser(chunk)) {
                                        co_return;
                                    }
                                } else {
                                    co_return;
                                }
                            }
                        } else {
                            SJSON::Parse parser;
                            parser.listen("usage", [](const SJSON::JSValue& value) {
                                std::cout << "Got usage: " << value.to_string(4) << std::endl;
                            });

                            for (;;) {
                                Message message = channel->recv();
                                if (std::holds_alternative<BodyMessage>(message)) {
                                    std::vector<char> chunk = std::move(std::get<BodyMessage>(message));
                                    parser.recv(std::string(chunk.begin(), chunk.end()));
                                    co_yield std::move(chunk);
                                } else {
                                    co_return;
                                }
                            }
                        }
                    },
                        {{"Content-Type", stream ? "text/event-stream" : "application/json"}});
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
