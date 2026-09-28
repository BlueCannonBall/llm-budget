#ifndef SQLITE_HPP_
#define SQLITE_HPP_

#include "Polyweb/Polynet/string.hpp"
#include <generator>
#include <optional>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sqlite {
    typedef std::vector<char> Blob;
    typedef double Double;
    typedef int Int;
    typedef sqlite3_int64 Int64;
    typedef std::string Text;

    inline std::string errstr(int error) {
        return sqlite3_errstr(error);
    }

    using Error = std::runtime_error;

    class Connection {
    protected:
        friend class Statement;

        sqlite3* raw_conn = nullptr;

    public:
        Connection() = default;
        Connection(pn::StringView filename, int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX) {
            init(filename, flags);
        }
        Connection(Connection&& conn) noexcept:
            raw_conn(std::exchange(conn.raw_conn, nullptr)) {}

        Connection& operator=(Connection&& conn) {
            if (this != &conn) {
                if (raw_conn) {
                    if (int result = sqlite3_close(raw_conn); result != SQLITE_OK) {
                        throw Error(errstr(result));
                    }
                }
                raw_conn = std::exchange(conn.raw_conn, nullptr);
            }
            return *this;
        }

        ~Connection() {
            if (raw_conn) sqlite3_close_v2(raw_conn);
        }

        void init(pn::StringView filename, int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX) {
            if (raw_conn) {
                if (int result = sqlite3_close(raw_conn); result != SQLITE_OK) {
                    throw Error(errstr(result));
                }
                raw_conn = nullptr;
            }

            if (int result = sqlite3_open_v2(filename.c_str(), &raw_conn, flags, nullptr); result != SQLITE_OK) {
                sqlite3_close(raw_conn);
                raw_conn = nullptr;
                throw Error(errstr(result));
            }
        }

        int errcode() const {
            return sqlite3_errcode(raw_conn);
        }

        std::string errmsg() const {
            return sqlite3_errmsg(raw_conn);
        }

        void exec(pn::StringView sql) {
            if (int result = sqlite3_exec(raw_conn, sql.c_str(), nullptr, nullptr, nullptr); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void exec_nothrow(pn::StringView sql) noexcept {
            sqlite3_exec(raw_conn, sql.c_str(), nullptr, nullptr, nullptr);
        }
    };

    template <typename... Ts>
    using Row = std::tuple<Ts...>;

    template <typename... Ts>
    using Table = std::vector<Row<Ts...>>;

    class Statement {
    protected:
        template <size_t I, typename... Ts>
        void push_value(Row<Ts...>& row) {
            typename std::tuple_element<I, Row<Ts...>>::type value;
            get_column(value, I);
            std::get<I>(row) = value;
        }

        template <typename... Ts, size_t... Is>
        Row<Ts...> make_row(std::index_sequence<Is...>) {
            Row<Ts...> ret;
            (push_value<Is, Ts...>(ret), ...);
            return ret;
        }

        void get_column(Blob& ret, size_t index) const {
            const char* raw_blob = (const char*) sqlite3_column_blob(raw_stmt, index);
            ret = Blob(raw_blob, raw_blob + sqlite3_column_bytes(raw_stmt, index));
        }

        void get_column(Double& ret, size_t index) const {
            ret = sqlite3_column_double(raw_stmt, index);
        }

        void get_column(Int& ret, size_t index) const {
            ret = sqlite3_column_int(raw_stmt, index);
        }

        void get_column(Int64& ret, size_t index) const {
            ret = sqlite3_column_int64(raw_stmt, index);
        }

        void get_column(Text& ret, size_t index) const {
            const char* raw_text = (const char*) sqlite3_column_text(raw_stmt, index);
            ret = Text(raw_text, raw_text + sqlite3_column_bytes(raw_stmt, index));
        }

        template <typename T>
        void get_column(std::optional<T>& ret, size_t index) {
            if (sqlite3_column_type(raw_stmt, index) == SQLITE_NULL) {
                ret = std::nullopt;
            } else {
                T value;
                get_column(value, index);
                ret = value;
            }
        }

        sqlite3_stmt* raw_stmt = nullptr;

    public:
        Statement() = default;
        Statement(const Connection& conn, pn::StringView sql) {
            if (int result = sqlite3_prepare_v2(conn.raw_conn, sql.c_str(), -1, &raw_stmt, nullptr); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }
        Statement(Statement&& stmt) {
            *this = std::move(stmt);
        }

        Statement& operator=(Statement&& stmt) {
            if (this != &stmt) {
                sqlite3_finalize(std::exchange(raw_stmt, stmt.raw_stmt));
                stmt.raw_stmt = nullptr;
            }
            return *this;
        }

        ~Statement() {
            sqlite3_finalize(raw_stmt);
        }

        void init(const Connection& conn, pn::StringView sql) {
            sqlite3_finalize(raw_stmt);

            if (int result = sqlite3_prepare_v2(conn.raw_conn, sql.c_str(), -1, &raw_stmt, nullptr); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void bind(const Blob& value, size_t index) {
            if (int result = sqlite3_bind_blob(raw_stmt, index, value.data(), value.size(), SQLITE_TRANSIENT); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void bind(Double value, size_t index) {
            if (int result = sqlite3_bind_double(raw_stmt, index, value); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void bind(Int value, size_t index) {
            if (int result = sqlite3_bind_int(raw_stmt, index, value); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void bind(Int64 value, size_t index) {
            if (int result = sqlite3_bind_int64(raw_stmt, index, value); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void bind(const Text& value, size_t index) {
            if (int result = sqlite3_bind_text(raw_stmt, index, value.c_str(), -1, SQLITE_TRANSIENT); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void bind(pn::StringView value, size_t index) {
            if (int result = sqlite3_bind_text(raw_stmt, index, value.c_str(), -1, SQLITE_TRANSIENT); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void bind(std::nullopt_t, size_t index) {
            if (int result = sqlite3_bind_null(raw_stmt, index); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void reset() {
            if (int result = sqlite3_reset(raw_stmt); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        void clear_bindings() {
            if (int result = sqlite3_clear_bindings(raw_stmt); result != SQLITE_OK) {
                throw Error(errstr(result));
            }
        }

        template <typename... Us>
        [[nodiscard]] Table<Us...> exec() {
            Table<Us...> ret;
            for (;;) {
                switch (int result = sqlite3_step(raw_stmt); result) {
                case SQLITE_ROW:
                    ret.push_back(make_row<Us...>(std::make_index_sequence<sizeof...(Us)>()));
                    break;

                case SQLITE_DONE:
                    goto done;

                default:
                    throw Error(errstr(result));
                }
            }
        done:
            reset();
            return ret;
        }

        template <typename... Us>
        std::generator<Row<Us...>> exec_generator() {
            struct ResetOnExit {
                sqlite3_stmt* stmt;

                ~ResetOnExit() {
                    if (stmt) sqlite3_reset(stmt);
                }
            } reset_on_exit {raw_stmt};
            for (;;) {
                switch (int result = sqlite3_step(raw_stmt); result) {
                case SQLITE_ROW:
                    co_yield make_row<Us...>(std::make_index_sequence<sizeof...(Us)>());
                    break;

                case SQLITE_DONE:
                    reset();
                    reset_on_exit.stmt = nullptr;
                    co_return;

                default:
                    throw Error(errstr(result));
                }
            }
        }

        void exec_void() {
            for (;;) {
                switch (int result = sqlite3_step(raw_stmt); result) {
                case SQLITE_ROW:
                    break;

                case SQLITE_DONE:
                    goto done;

                default:
                    throw Error(errstr(result));
                }
            }
        done:
            reset();
        }
    };

    enum TransactionType {
        TRANSACTION_DEFAULT,
        TRANSACTION_IMMEDIATE,
        TRANSACTION_DEFERRED,
        TRANSACTION_EXCLUSIVE,
    };

    class Transaction {
    protected:
        Connection* conn;

    public:
        Transaction(Connection* conn, TransactionType type):
            conn(conn) {
            switch (type) {
            default:
            case TRANSACTION_DEFAULT:
                conn->exec("BEGIN");
                break;

            case TRANSACTION_IMMEDIATE:
                conn->exec("BEGIN IMMEDIATE");
                break;

            case TRANSACTION_DEFERRED:
                conn->exec("BEGIN DEFERRED");
                break;

            case TRANSACTION_EXCLUSIVE:
                conn->exec("BEGIN EXCLUSIVE");
                break;
            }
        }
        Transaction(const Transaction&) = delete;
        Transaction(Transaction&&) = delete;

        Transaction& operator=(const Transaction&) = delete;
        Transaction& operator=(Transaction&&) = delete;

        void commit() {
            if (conn) {
                conn->exec("COMMIT");
                conn = nullptr;
            } else {
                throw std::logic_error("Commit called on inactive transaction");
            }
        }

        ~Transaction() noexcept {
            if (conn) {
                conn->exec_nothrow("ROLLBACK");
            }
        }
    };
} // namespace sqlite

#endif
