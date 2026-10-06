#pragma once

#include "Polyweb/Polynet/string.hpp"
#include <chrono>
#include <expected>
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

struct UsageLimits {
    uint64_t five_hour_limit_nanodollars;
    uint64_t weekly_limit_nanodollars;
    std::optional<std::chrono::system_clock::time_point> five_hour_window_started_at;
    std::optional<std::chrono::system_clock::time_point> weekly_window_started_at;
};

struct User {
    user_id_t id;
    std::string name;
    UsageLimits usage_limits;
};

struct UserUsage {
    UsageLimits limits;
    uint64_t five_hour_cost_nanodollars;
    uint64_t weekly_cost_nanodollars;
};

enum BeginRequestError {
    BEGIN_REQUEST_ERROR_USER_NOT_FOUND,
    BEGIN_REQUEST_ERROR_FIVE_HOUR_LIMIT,
    BEGIN_REQUEST_ERROR_WEEKLY_LIMIT,
    BEGIN_REQUEST_ERROR_BOTH_LIMITS,
};

namespace database {
    void init();

    user_id_t make_user(pn::StringView name, uint64_t five_hour_limit_nanodollars, uint64_t weekly_limit_nanodollars, std::string& api_key);
    std::optional<User> get_user(user_id_t id);
    std::optional<User> get_user_by_api_key(pn::StringView api_key);
    std::optional<User> get_user_by_name(pn::StringView name);
    std::vector<User> list_users();
    std::optional<UsageLimits> get_usage_limits(user_id_t id);
    std::optional<UserUsage> get_user_usage(user_id_t id, std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
    bool set_usage_limits(user_id_t id, uint64_t five_hour_limit_nanodollars, uint64_t weekly_limit_nanodollars);
    bool rotate_api_key(user_id_t id, std::string& api_key);

    std::expected<request_id_t, BeginRequestError> begin_request(user_id_t user_id, UsageLimits& usage_limits, std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
    void update_request(request_id_t id, uint64_t cost_nanodollars);
    void end_request(request_id_t id, RequestState state, std::optional<uint64_t> cost_nanodollars = {}, std::chrono::system_clock::time_point time = std::chrono::system_clock::now());
} // namespace database
