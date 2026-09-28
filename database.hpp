#pragma once

#include "Polyweb/Polynet/string.hpp"
#include <chrono>
#include <optional>
#include <stdint.h>
#include <string>
#include <vector>

typedef int64_t user_id_t;
typedef int64_t request_id_t;

enum RequestState {
    REQUEST_STATE_IN_FLIGHT,
    REQUEST_STATE_COMPLETED,
    REQUEST_STATE_INTERRUPTED,
    REQUEST_STATE_UNKNOWN,
};

struct User {
    user_id_t id;
    std::string name;
    uint64_t five_hour_limit_nanodollars;
    uint64_t weekly_limit_nanodollars;
    std::optional<std::chrono::system_clock::time_point> five_hour_window_started_at;
    std::optional<std::chrono::system_clock::time_point> weekly_window_started_at;
};

void init();

user_id_t make_user(pn::StringView name, uint64_t five_hour_limit_nanodollars, uint64_t weekly_limit_nanodollars, std::string& api_key);
User get_user(user_id_t id);
std::optional<User> get_user(pn::StringView api_key);
std::vector<User> list_users();
bool set_user_limits(user_id_t id, uint64_t five_hour_limit_nanodollars, uint64_t weekly_limit_nanodollars);
bool rotate_api_key(user_id_t id, std::string& new_key);

request_id_t begin_request(user_id_t user_id, std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
void update_request(request_id_t id, uint64_t cost_nanodollars);
void end_request(request_id_t id, RequestState state, std::optional<uint64_t> cost_nanodollars = {}, std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
