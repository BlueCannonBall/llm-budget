#include "sqlite.hpp"
#include <cassert>
#include <utility>

int main() {
    sqlite::Connection destination(":memory:");
    sqlite::Connection source(":memory:");
    source.exec("CREATE TABLE source_table (value INTEGER)");

    {
        sqlite::Statement active(destination, "SELECT 1");
        bool init_failed = false;
        try {
            destination.init(":memory:");
        } catch (const sqlite::Error&) {
            init_failed = true;
        }
        assert(init_failed);
        destination.exec("CREATE TABLE still_open (value INTEGER)");

        bool move_failed = false;
        try {
            destination = std::move(source);
        } catch (const sqlite::Error&) {
            move_failed = true;
        }
        assert(move_failed);
        source.exec("INSERT INTO source_table VALUES (7)");
    }

    destination = std::move(source);
    sqlite::Statement check(destination, "SELECT value FROM source_table");
    auto rows = check.exec<sqlite::Int>();
    assert(rows.size() == 1 && std::get<0>(rows[0]) == 7);

    bool open_failed = false;
    try {
        source.init("/nonexistent-llm-budget-test-directory/database.sqlite");
    } catch (const sqlite::Error&) {
        open_failed = true;
    }
    assert(open_failed);
    source.init(":memory:");
    source.exec("CREATE TABLE old_database (value INTEGER)");

    // A failed reinitialization discards the previously open database.
    open_failed = false;
    try {
        source.init("/nonexistent-llm-budget-test-directory/database.sqlite");
    } catch (const sqlite::Error&) {
        open_failed = true;
    }
    assert(open_failed);
    bool connection_is_empty = false;
    try {
        sqlite::Statement stmt(source, "SELECT 1");
    } catch (const sqlite::Error&) {
        connection_is_empty = true;
    }
    assert(connection_is_empty);
    source.init(":memory:");
    source.exec("CREATE TABLE newly_opened (value INTEGER)");
}
