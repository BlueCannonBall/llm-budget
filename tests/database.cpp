#include "database.hpp"
#include "sqlite.hpp"
#include <cassert>
#include <chrono>
#include <limits>
#include <openssl/sha.h>
#include <string>

int main() {
    // Run this test from a temporary directory: init() uses llm-budget.db.
    init();

    unsigned char raw_key[32] = {};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(raw_key, sizeof raw_key, digest);
    constexpr char hex_digits[] = "0123456789ABCDEF";
    std::string hash;
    for (unsigned char byte : digest) {
        hash += hex_digits[byte >> 4];
        hash += hex_digits[byte & 15];
    }

    sqlite::Connection db("llm-budget.db");
    sqlite::Statement insert(db, R"(
        INSERT INTO users (name, api_key_hash, five_hour_limit_nanodollars, weekly_limit_nanodollars)
        VALUES (?, ?, 100, 200)
    )");
    insert.bind(std::string("test-user"), 1);
    insert.bind(hash, 2);
    insert.exec_void();

    auto check_user = [](const User& user, bool five_hour_set, bool weekly_set) {
        assert(user.id == 1 && user.name == "test-user");
        assert(user.five_hour_limit_nanodollars == 100 && user.weekly_limit_nanodollars == 200);
        assert(user.five_hour_window_started_at.has_value() == five_hour_set);
        assert(user.weekly_window_started_at.has_value() == weekly_set);
        if (five_hour_set) assert(user.five_hour_window_started_at == std::chrono::system_clock::time_point {});
        if (weekly_set) assert(user.weekly_window_started_at == std::chrono::system_clock::time_point {std::chrono::milliseconds {1234}});
    };

    std::string key(64, '0');
    check_user(get_user(user_id_t {1}), false, false);
    check_user(get_user_by_api_key(pn::StringView(key)).value(), false, false);
    check_user(get_user_by_name("test-user").value(), false, false);
    db.exec("UPDATE users SET five_hour_window_started_at = 0 WHERE id = 1");
    check_user(get_user(user_id_t {1}), true, false);
    check_user(get_user_by_api_key(pn::StringView(key)).value(), true, false);
    db.exec("UPDATE users SET weekly_window_started_at = 1234 WHERE id = 1");
    check_user(get_user(user_id_t {1}), true, true);
    check_user(get_user_by_api_key(pn::StringView(key)).value(), true, true);
    db.exec("UPDATE users SET five_hour_window_started_at = NULL WHERE id = 1");
    check_user(get_user(user_id_t {1}), false, true);
    check_user(get_user_by_api_key(pn::StringView(key)).value(), false, true);
    check_user(get_user_by_name("test-user").value(), false, true);
    assert(!get_user_by_name("missing"));

    assert(!get_user_by_api_key(pn::StringView("invalid")));
    assert(!get_user_by_api_key(pn::StringView(std::string(64, 'z'))));
    assert(!get_user_by_api_key(pn::StringView(std::string(64, 'f'))));

    std::string created_key;
    auto created_id = make_user("created", 300, 400, created_key);
    assert(created_key.size() == 64);
    assert(get_user(created_id).name == "created");
    assert(get_user_by_api_key(pn::StringView(created_key))->id == created_id);
    assert(get_user_by_name("created")->id == created_id);

    auto users = list_users();
    assert(users.size() == 2);
    assert(users[0].id == 1 && users[0].name == "test-user");
    assert(users[1].id == created_id && users[1].name == "created");
    assert(users[0].weekly_window_started_at == std::chrono::system_clock::time_point {std::chrono::milliseconds {1234}});

    assert(set_user_limits(created_id, 500, 600));
    assert(get_user(created_id).five_hour_limit_nanodollars == 500);
    assert(get_user_by_api_key(pn::StringView(created_key))->weekly_limit_nanodollars == 600);
    assert(get_user_by_name("created")->five_hour_limit_nanodollars == 500);
    assert(!set_user_limits(-1, 1, 2));
    assert(!rotate_api_key(-1, created_key));
    assert(get_user_by_api_key(pn::StringView(created_key))->id == created_id);

    std::string old_key = created_key;
    assert(rotate_api_key(created_id, created_key));
    assert(created_key.size() == 64 && created_key != old_key);
    assert(!get_user_by_api_key(pn::StringView(old_key)));
    assert(get_user_by_api_key(pn::StringView(created_key))->id == created_id);
    old_key = created_key;
    assert(rotate_api_key(created_id, created_key));
    assert(created_key != old_key);
    assert(!get_user_by_api_key(pn::StringView(old_key)));
    assert(get_user_by_api_key(pn::StringView(created_key))->id == created_id);

    auto above_sqlite_max = (uint64_t) std::numeric_limits<sqlite::Int64>::max() + 1;
    bool rejected = false;
    try {
        make_user("too-large", above_sqlite_max, 1, created_key);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    assert(rejected);
    rejected = false;
    try {
        set_user_limits(created_id, above_sqlite_max, 1);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    assert(rejected);
    assert(get_user(created_id).five_hour_limit_nanodollars == 500);
    rejected = false;
    try {
        make_user("too-large", 1, above_sqlite_max, created_key);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    assert(rejected);
    rejected = false;
    try {
        db.exec("UPDATE users SET weekly_limit_nanodollars = -1 WHERE id = 1");
    } catch (const sqlite::Error&) {
        rejected = true;
    }
    assert(rejected);

    sqlite::Statement request_row(db, "SELECT state, cost_nanodollars FROM requests WHERE id = ?");
    auto check_request = [&](request_id_t id, const std::string& state, std::optional<sqlite::Int64> cost) {
        request_row.bind((sqlite::Int64) id, 1);
        auto [actual_state, actual_cost] = request_row.exec<std::string, std::optional<sqlite::Int64>>().at(0);
        assert(actual_state == state && actual_cost == cost);
    };

    auto request_id = begin_request(1).value();
    check_request(request_id, "in_flight", std::nullopt);
    update_request(request_id, 8);
    check_request(request_id, "in_flight", 8);
    end_request(request_id, REQUEST_STATE_INTERRUPTED, std::nullopt);
    check_request(request_id, "interrupted", 8);
    end_request(request_id, REQUEST_STATE_COMPLETED, 0);
    check_request(request_id, "completed", 0);

    rejected = false;
    try {
        update_request(request_id, above_sqlite_max);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    assert(rejected);
    rejected = false;
    try {
        end_request(request_id, REQUEST_STATE_COMPLETED, above_sqlite_max);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    assert(rejected);
    check_request(request_id, "completed", 0);

    auto unknown_request = begin_request(1).value();
    end_request(unknown_request, REQUEST_STATE_UNKNOWN, std::nullopt);
    check_request(unknown_request, "unknown", std::nullopt);

    using namespace std::chrono;
    auto base = system_clock::time_point {milliseconds {1'000'000'000'000}};
    std::string limit_key;
    db.exec("CREATE TABLE window_updates (user_id INTEGER)");
    db.exec(R"(
        CREATE TRIGGER count_window_updates AFTER UPDATE OF five_hour_window_started_at, weekly_window_started_at ON users
        BEGIN
            INSERT INTO window_updates VALUES (NEW.id);
        END;
    )");
    sqlite::Statement window_update_count(db, "SELECT COUNT(*) FROM window_updates WHERE user_id = ?");
    auto count_window_updates = [&](user_id_t id) {
        window_update_count.bind((sqlite::Int64) id, 1);
        return std::get<0>(window_update_count.exec<sqlite::Int64>().at(0));
    };

    auto five_hour_user = make_user("five-hour", 10, 100, limit_key);
    auto old_five_hour_request = begin_request(five_hour_user, base - hours {5}).value();
    assert(count_window_updates(five_hour_user) == 1);
    assert(get_user(five_hour_user).five_hour_window_started_at == base - hours {5});
    assert(get_user(five_hour_user).weekly_window_started_at == base - hours {5});
    end_request(old_five_hour_request, REQUEST_STATE_COMPLETED, 10);
    assert(!begin_request(five_hour_user, base - milliseconds {1}));
    assert(get_user(five_hour_user).five_hour_window_started_at == base - hours {5});
    auto new_five_hour_request = begin_request(five_hour_user, base).value();
    assert(count_window_updates(five_hour_user) == 2);
    assert(get_user(five_hour_user).five_hour_window_started_at == base);
    assert(get_user(five_hour_user).weekly_window_started_at == base - hours {5});
    assert(begin_request(five_hour_user, base + milliseconds {1})); // A NULL cost does not count yet.
    assert(count_window_updates(five_hour_user) == 2);
    update_request(new_five_hour_request, 10);
    assert(!begin_request(five_hour_user, base + milliseconds {2}));
    assert(count_window_updates(five_hour_user) == 2);
    assert(begin_request(five_hour_user, base + hours {5}));
    assert(count_window_updates(five_hour_user) == 3);

    auto weekly_user = make_user("weekly", 100, 10, limit_key);
    auto old_weekly_request = begin_request(weekly_user, base - days {7}).value();
    end_request(old_weekly_request, REQUEST_STATE_COMPLETED, 10);
    assert(!begin_request(weekly_user, base - milliseconds {1}));
    assert(get_user(weekly_user).five_hour_window_started_at == base - days {7});
    auto new_weekly_request = begin_request(weekly_user, base).value();
    assert(get_user(weekly_user).weekly_window_started_at == base);
    end_request(new_weekly_request, REQUEST_STATE_COMPLETED, 10);
    assert(!begin_request(weekly_user, base + milliseconds {1}));

    auto zero_limit_user = make_user("zero-limit", 0, 100, limit_key);
    assert(!begin_request(zero_limit_user, base));
    assert(!get_user(zero_limit_user).five_hour_window_started_at);
    assert(!begin_request(-1, base));
}
