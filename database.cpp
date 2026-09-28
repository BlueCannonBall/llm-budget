#include "database.hpp"
#include "sqlite.hpp"
#include <algorithm>
#include <ctype.h>
#include <limits>
#include <openssl/rand.h>
#include <openssl/sha.h>

static sqlite::Connection make_conn() {
    sqlite::Connection ret("llm-budget.db");
    ret.exec("PRAGMA foreign_keys = ON");
    return ret;
}

thread_local sqlite::Connection conn = make_conn();

void init() {
    conn.exec("PRAGMA journal_mode = WAL");

    // All times are in UNIX epoch milliseconds

    conn.exec(R"(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE,
            api_key_hash TEXT NOT NULL UNIQUE,
            five_hour_limit_nanodollars INTEGER NOT NULL
                CHECK (five_hour_limit_nanodollars >= 0),
            weekly_limit_nanodollars INTEGER NOT NULL
                CHECK (weekly_limit_nanodollars >= 0),
            five_hour_window_started_at INTEGER,
            weekly_window_started_at INTEGER
        );
    )");

    conn.exec(R"(
        CREATE TABLE IF NOT EXISTS requests (
            id INTEGER PRIMARY KEY,
            user_id INTEGER NOT NULL REFERENCES users(id),
            started_at INTEGER NOT NULL,
            finished_at INTEGER,
            state TEXT NOT NULL CHECK (
                state IN (
                    'in_flight',
                    'completed',
                    'interrupted',
                    'unknown'
                )
            ),
            cost_nanodollars INTEGER
                CHECK (cost_nanodollars >= 0)
        );
    )");

    conn.exec(R"(
        CREATE INDEX IF NOT EXISTS requests_user_started_at_idx
            ON requests(user_id, started_at);
    )");

    conn.exec(R"(
        CREATE TABLE IF NOT EXISTS request_contents (
            request_id INTEGER PRIMARY KEY
                REFERENCES requests(id) ON DELETE CASCADE,
            body BLOB NOT NULL,
            response BLOB
        );
    )");
}

static std::string bytes_to_hex(const unsigned char* data, size_t size) {
    std::string ret(size * 2 + 1, '\0');
    if (!OPENSSL_buf2hexstr_ex(ret.data(), ret.size(), nullptr, data, size, '\0')) {
        throw std::runtime_error("Failed to encode hex");
    }

    ret.pop_back();
    return ret;
}

static std::vector<unsigned char> hex_to_bytes(pn::StringView hex) {
    std::vector<unsigned char> ret(hex.size() / 2);
    size_t written = 0;
    if (!OPENSSL_hexstr2buf_ex(ret.data(), ret.size(), &written, hex.c_str(), '\0')) {
        throw std::invalid_argument("Invalid hex string");
    }
    ret.resize(written);
    return ret;
}

static std::optional<std::chrono::system_clock::time_point> from_unix_ms(std::optional<sqlite::Int64> milliseconds) {
    if (!milliseconds) return std::nullopt;
    return std::chrono::sys_time<std::chrono::milliseconds> {std::chrono::milliseconds {*milliseconds}};
}

static sqlite::Int64 to_unix_ms(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

static sqlite::Int64 checked_nanodollars(uint64_t amount) {
    if (amount > (uint64_t) std::numeric_limits<sqlite::Int64>::max()) {
        throw std::out_of_range("Nanodollars exceed SQLite's signed 64-bit range");
    }
    return (sqlite::Int64) amount;
}

static pn::StringView request_state_to_string(RequestState state) {
    switch (state) {
    case REQUEST_STATE_IN_FLIGHT: return "in_flight";
    case REQUEST_STATE_COMPLETED: return "completed";
    case REQUEST_STATE_INTERRUPTED: return "interrupted";
    case REQUEST_STATE_UNKNOWN: return "unknown";
    default: throw std::invalid_argument("Invalid request state");
    }
}

static std::string generate_api_key() {
    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof buf) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }

    return bytes_to_hex(buf, sizeof buf);
}

static std::string hash_api_key(pn::StringView api_key) {
    auto buf = hex_to_bytes(api_key);

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(buf.data(), buf.size(), digest);

    return bytes_to_hex(digest, sizeof digest);
}

using UserRow = sqlite::Row<sqlite::Int64, std::string, sqlite::Int64, sqlite::Int64, std::optional<sqlite::Int64>, std::optional<sqlite::Int64>>;

static User user_from_row(const UserRow& row) {
    return User {
        .id = (user_id_t) std::get<0>(row),
        .name = std::get<1>(row),
        .five_hour_limit_nanodollars = (uint64_t) std::get<2>(row),
        .weekly_limit_nanodollars = (uint64_t) std::get<3>(row),
        .five_hour_window_started_at = from_unix_ms(std::get<4>(row)),
        .weekly_window_started_at = from_unix_ms(std::get<5>(row)),
    };
}

user_id_t make_user(pn::StringView name, uint64_t five_hour_limit_nanodollars, uint64_t weekly_limit_nanodollars, std::string& api_key) {
    thread_local sqlite::Statement stmt(conn, R"(
        INSERT INTO users (
            name,
            api_key_hash,
            five_hour_limit_nanodollars,
            weekly_limit_nanodollars)
        VALUES (
            ?,
            ?,
            ?,
            ?
        )
        RETURNING id;
    )");

    std::string new_key = generate_api_key();

    stmt.bind(name, 1);
    stmt.bind(hash_api_key(new_key), 2);
    stmt.bind(checked_nanodollars(five_hour_limit_nanodollars), 3);
    stmt.bind(checked_nanodollars(weekly_limit_nanodollars), 4);

    auto result = stmt.exec<sqlite::Int64>().at(0);
    api_key = std::move(new_key);
    return std::get<0>(result);
}

User get_user(user_id_t id) {
    thread_local sqlite::Statement stmt(conn, R"(
        SELECT id, name, five_hour_limit_nanodollars, weekly_limit_nanodollars, five_hour_window_started_at, weekly_window_started_at
        FROM users
        WHERE id = ?;
    )");

    stmt.bind((sqlite::Int64) id, 1);

    auto result = stmt.exec<sqlite::Int64, std::string, sqlite::Int64, sqlite::Int64, std::optional<sqlite::Int64>, std::optional<sqlite::Int64>>().at(0);
    return user_from_row(result);
}

std::optional<User> get_user(pn::StringView api_key) {
    if (api_key.size() != 64 || !std::all_of(api_key.begin(), api_key.end(), [](unsigned char ch) {
            return isxdigit(ch) != 0;
        })) {
        return std::nullopt;
    }

    thread_local sqlite::Statement stmt(conn, R"(
        SELECT id, name, five_hour_limit_nanodollars, weekly_limit_nanodollars, five_hour_window_started_at, weekly_window_started_at
        FROM users
        WHERE api_key_hash = ?;
    )");

    stmt.bind(hash_api_key(api_key), 1);

    auto rows = stmt.exec<sqlite::Int64, std::string, sqlite::Int64, sqlite::Int64, std::optional<sqlite::Int64>, std::optional<sqlite::Int64>>();
    if (rows.empty()) return std::nullopt;

    return user_from_row(rows.front());
}

std::vector<User> list_users() {
    thread_local sqlite::Statement stmt(conn, R"(
        SELECT id, name, five_hour_limit_nanodollars, weekly_limit_nanodollars, five_hour_window_started_at, weekly_window_started_at
        FROM users
        ORDER BY id;
    )");

    auto rows = stmt.exec<sqlite::Int64, std::string, sqlite::Int64, sqlite::Int64, std::optional<sqlite::Int64>, std::optional<sqlite::Int64>>();
    std::vector<User> users;
    users.reserve(rows.size());
    for (const auto& row : rows) {
        users.push_back(user_from_row(row));
    }

    return users;
}

bool set_user_limits(user_id_t id, uint64_t five_hour_limit_nanodollars, uint64_t weekly_limit_nanodollars) {
    thread_local sqlite::Statement stmt(conn, R"(
        UPDATE users
        SET
            five_hour_limit_nanodollars = ?,
            weekly_limit_nanodollars = ?
        WHERE id = ?
        RETURNING id;
    )");

    stmt.bind(checked_nanodollars(five_hour_limit_nanodollars), 1);
    stmt.bind(checked_nanodollars(weekly_limit_nanodollars), 2);
    stmt.bind((sqlite::Int64) id, 3);

    return !stmt.exec<sqlite::Int64>().empty();
}

bool rotate_api_key(user_id_t id, std::string& new_key) {
    thread_local sqlite::Statement stmt(conn, R"(
        UPDATE users
        SET
            api_key_hash = ?
        WHERE id = ?
        RETURNING id;
    )");

    std::string generated_key = generate_api_key();

    stmt.bind(hash_api_key(generated_key), 1);
    stmt.bind((sqlite::Int64) id, 2);

    if (stmt.exec<sqlite::Int64>().empty()) return false;

    new_key = std::move(generated_key);
    return true;
}

request_id_t begin_request(user_id_t user_id, std::chrono::system_clock::time_point time) {
    thread_local sqlite::Statement stmt(conn, R"(
        INSERT INTO requests (
            user_id,
            started_at,
            state)
        VALUES (
            ?,
            ?,
            'in_flight'
        )
        RETURNING id;
    )");

    stmt.bind((sqlite::Int64) user_id, 1);
    stmt.bind((sqlite::Int64) to_unix_ms(time), 2);

    auto result = stmt.exec<sqlite::Int64>().at(0);
    return std::get<0>(result);
}

void update_request(request_id_t id, uint64_t cost_nanodollars) {
    thread_local sqlite::Statement stmt(conn, R"(
        UPDATE requests
        SET
            cost_nanodollars = ?
        WHERE id = ?;
    )");

    stmt.bind(checked_nanodollars(cost_nanodollars), 1);
    stmt.bind((sqlite::Int64) id, 2);

    stmt.exec_void();
}

void end_request(request_id_t id, RequestState state, std::optional<uint64_t> cost_nanodollars, std::chrono::system_clock::time_point time) {
    thread_local sqlite::Statement stmt(conn, R"(
        UPDATE requests
        SET
            state = ?,
            finished_at = ?,
            cost_nanodollars = COALESCE(?, cost_nanodollars)
        WHERE id = ?;
    )");

    stmt.bind(request_state_to_string(state), 1);
    stmt.bind((sqlite::Int64) to_unix_ms(time), 2);
    if (cost_nanodollars) {
        stmt.bind(checked_nanodollars(*cost_nanodollars), 3);
    } else {
        stmt.bind_null(3);
    }
    stmt.bind((sqlite::Int64) id, 4);

    stmt.exec_void();
}
