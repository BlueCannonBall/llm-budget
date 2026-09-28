#include "database.hpp"
#include "sqlite.hpp"
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

    unsigned char buf[32];
    if (RAND_bytes(buf, sizeof buf) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(buf, sizeof buf, digest);

    stmt.bind(name, 1);
    stmt.bind(bytes_to_hex(digest, sizeof digest), 2);
    stmt.bind(checked_nanodollars(five_hour_limit_nanodollars), 3);
    stmt.bind(checked_nanodollars(weekly_limit_nanodollars), 4);

    auto result = stmt.exec<sqlite::Int64>().at(0);
    api_key = bytes_to_hex(buf, sizeof buf);
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
    return User {
        .id = (user_id_t) std::get<0>(result),
        .name = std::get<1>(result),
        .five_hour_limit_nanodollars = (uint64_t) std::get<2>(result),
        .weekly_limit_nanodollars = (uint64_t) std::get<3>(result),
        .five_hour_window_started_at = from_unix_ms(std::get<4>(result)),
        .weekly_window_started_at = from_unix_ms(std::get<5>(result)),
    };
}

User get_user(pn::StringView api_key) {
    thread_local sqlite::Statement stmt(conn, R"(
        SELECT id, name, five_hour_limit_nanodollars, weekly_limit_nanodollars, five_hour_window_started_at, weekly_window_started_at
        FROM users
        WHERE api_key_hash = ?;
    )");

    auto buf = hex_to_bytes(api_key);

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(buf.data(), buf.size(), digest);

    stmt.bind(bytes_to_hex(digest, sizeof digest), 1);

    auto result = stmt.exec<sqlite::Int64, std::string, sqlite::Int64, sqlite::Int64, std::optional<sqlite::Int64>, std::optional<sqlite::Int64>>().at(0);
    return User {
        .id = (user_id_t) std::get<0>(result),
        .name = std::get<1>(result),
        .five_hour_limit_nanodollars = (uint64_t) std::get<2>(result),
        .weekly_limit_nanodollars = (uint64_t) std::get<3>(result),
        .five_hour_window_started_at = from_unix_ms(std::get<4>(result)),
        .weekly_window_started_at = from_unix_ms(std::get<5>(result)),
    };
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
