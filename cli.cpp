#include "cli.hpp"
#include "database.hpp"
#include "money.hpp"
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>

constexpr uint64_t nanos_per_dollar = 1'000'000'000;

struct Limits {
    uint64_t five_hour;
    uint64_t weekly;
};

static void print_usage(std::ostream& out, const char* program) {
    out << "Usage:\n"
        << "  " << program << "                         Start the server\n"
        << "  " << program << " user add NAME --five-hour-limit DOLLARS --weekly-limit DOLLARS\n"
        << "  " << program << " user list\n"
        << "  " << program << " user show NAME\n"
        << "  " << program << " user usage NAME\n"
        << "  " << program << " user set-limits NAME --five-hour-limit DOLLARS --weekly-limit DOLLARS\n"
        << "  " << program << " key rotate NAME\n";
}

static Limits parse_limits(int argc, char** argv) {
    static option options[] = {
        {"five-hour-limit", required_argument, nullptr, 'f'},
        {"weekly-limit", required_argument, nullptr, 'w'},
        {nullptr, 0, nullptr, 0},
    };
    std::optional<uint64_t> five_hour;
    std::optional<uint64_t> weekly;
    optind = 4;
    opterr = 0;
    int option;
    while ((option = getopt_long(argc, argv, "", options, nullptr)) != -1) {
        switch (option) {
        case 'f':
            if (five_hour) throw std::invalid_argument("Duplicate five-hour limit");
            five_hour = parse_nanodollars(optarg);
            break;

        case 'w':
            if (weekly) throw std::invalid_argument("Duplicate weekly limit");
            weekly = parse_nanodollars(optarg);
            break;

        default:
            throw std::invalid_argument("Unknown or incomplete limit option");
        }
    }

    if (optind != argc || !five_hour || !weekly) throw std::invalid_argument("Both limits are required");

    return {*five_hour, *weekly};
}

static std::string format_dollars(uint64_t amount) {
    std::ostringstream out;
    out << amount / nanos_per_dollar << '.' << std::setfill('0') << std::setw(9) << amount % nanos_per_dollar;
    return out.str();
}

static void print_user(const User& user) {
    std::cout << user.id << '\t' << user.name << '\t'
              << format_dollars(user.usage_limits.five_hour_limit_nanodollars) << '\t'
              << format_dollars(user.usage_limits.weekly_limit_nanodollars) << '\n';
}

static std::string format_reset_time(std::chrono::system_clock::time_point time) {
    auto seconds = std::chrono::floor<std::chrono::seconds>(time);
    time_t timestamp = std::chrono::system_clock::to_time_t(seconds);
    tm calendar_time {};
    if (!localtime_r(&timestamp, &calendar_time)) throw std::runtime_error("Cannot format reset time");

    std::ostringstream out;
    out << std::put_time(&calendar_time, "%Y-%m-%d %H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3)
        << std::chrono::duration_cast<std::chrono::milliseconds>(time - seconds).count()
        << ' ' << std::put_time(&calendar_time, "%Z %z");
    return out.str();
}

static void print_window_usage(std::string_view name, uint64_t cost_nanodollars, uint64_t limit_nanodollars, std::optional<std::chrono::system_clock::time_point> started_at, std::chrono::system_clock::duration duration, std::chrono::system_clock::time_point time) {
    std::cout << name << ": $" << format_dollars(cost_nanodollars) << " / $" << format_dollars(limit_nanodollars) << " (";
    if (limit_nanodollars == 0) {
        std::cout << "n/a: zero limit";
    } else {
        std::cout << std::fixed << std::setprecision(2)
                  << (long double) cost_nanodollars * 100 / limit_nanodollars << '%';
    }
    std::cout << ")";

    if (limit_nanodollars == 0) {
        std::cout << "; no automatic reset (zero limit)";
    } else if (started_at && time < *started_at + duration) {
        std::cout << "; resets at " << format_reset_time(*started_at + duration);
    } else {
        std::cout << "; reset not scheduled";
    }

    std::cout << '\n';
}

static User find_user(pn::StringView name) {
    auto user = database::get_user_by_name(name);
    if (!user) throw std::invalid_argument("User not found: " + std::string(name));

    return *user;
}

static int run_command(int argc, char** argv) {
    if (argc < 3) {
        print_usage(std::cerr, argv[0]);
        return 2;
    }

    std::string_view group = argv[1];
    std::string_view command = argv[2];

    if (group == "user" && command == "list" && argc == 3) {
        database::init();
        for (const User& user : database::list_users()) print_user(user);
        return 0;
    }

    if (group == "user" && (command == "add" || command == "set-limits")) {
        if (argc < 4 || !*argv[3]) throw std::invalid_argument("Name must not be empty");

        Limits limits = parse_limits(argc, argv);
        database::init();

        if (command == "add") {
            std::string key;
            user_id_t id = database::make_user(argv[3], limits.five_hour, limits.weekly, key);
            std::cout << "User ID: " << id << "\nAPI key: " << key << '\n';
        } else {
            User user = find_user(argv[3]);
            if (!database::set_usage_limits(user.id, limits.five_hour, limits.weekly)) throw std::runtime_error("User no longer exists");

            print_user(database::get_user(user.id).value());
        }

        return 0;
    }

    if (group == "user" && (command == "show" || command == "usage") && argc == 4) {
        if (!*argv[3]) throw std::invalid_argument("Name must not be empty");

        database::init();
        User user = find_user(argv[3]);
        if (command == "show") {
            print_user(user);
        } else {
            auto time = std::chrono::system_clock::now();
            auto usage = database::get_user_usage(user.id, time);
            if (!usage) throw std::runtime_error("User no longer exists");

            std::cout << user.name << '\n';
            print_window_usage("Five-hour", usage->five_hour_cost_nanodollars, usage->limits.five_hour_limit_nanodollars, usage->limits.five_hour_window_started_at, std::chrono::hours {5}, time);
            print_window_usage("Weekly", usage->weekly_cost_nanodollars, usage->limits.weekly_limit_nanodollars, usage->limits.weekly_window_started_at, std::chrono::weeks {1}, time);
        }

        return 0;
    }

    if (group == "key" && command == "rotate" && argc == 4) {
        if (!*argv[3]) throw std::invalid_argument("Name must not be empty");

        database::init();
        User user = find_user(argv[3]);
        std::string key;
        if (!database::rotate_api_key(user.id, key)) throw std::runtime_error("User no longer exists");

        std::cout << "API key: " << key << '\n';
        return 0;
    }

    print_usage(std::cerr, argv[0]);
    return 2;
}

int run_cli(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        print_usage(std::cout, argv[0]);
        return 0;
    }

    try {
        return run_command(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
