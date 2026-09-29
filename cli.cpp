#include "cli.hpp"
#include "database.hpp"
#include <charconv>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cli {
    namespace {
        constexpr uint64_t nanos_per_dollar = 1'000'000'000;

        struct Limits {
            uint64_t five_hour;
            uint64_t weekly;
        };

        void print_usage(std::ostream& out, const char* program) {
            out << "Usage:\n"
                << "  " << program << "                         Start the server\n"
                << "  " << program << " user add NAME --five-hour-limit DOLLARS --weekly-limit DOLLARS\n"
                << "  " << program << " user list\n"
                << "  " << program << " user show NAME\n"
                << "  " << program << " user usage NAME\n"
                << "  " << program << " user set-limits NAME --five-hour-limit DOLLARS --weekly-limit DOLLARS\n"
                << "  " << program << " key rotate NAME\n";
        }

        uint64_t parse_nanodollars(std::string_view dollars) {
            auto decimal = dollars.find('.');
            auto whole_text = dollars.substr(0, decimal);
            auto fraction_text = decimal == std::string_view::npos ? std::string_view {} : dollars.substr(decimal + 1);
            if (whole_text.empty() || (decimal != std::string_view::npos && fraction_text.empty()) || fraction_text.size() > 9) {
                throw std::invalid_argument("Limit must be a nonnegative dollar amount with at most nine decimal places");
            }

            uint64_t whole = 0;
            uint64_t fraction = 0;
            auto [whole_end, whole_error] = std::from_chars(whole_text.data(), whole_text.data() + whole_text.size(), whole);
            if (whole_error != std::errc {} || whole_end != whole_text.data() + whole_text.size()) {
                throw std::invalid_argument("Invalid dollar amount");
            }
            if (!fraction_text.empty()) {
                auto [fraction_end, fraction_error] = std::from_chars(fraction_text.data(), fraction_text.data() + fraction_text.size(), fraction);
                if (fraction_error != std::errc {} || fraction_end != fraction_text.data() + fraction_text.size()) {
                    throw std::invalid_argument("Invalid dollar amount");
                }
            }
            for (size_t i = fraction_text.size(); i < 9; ++i) fraction *= 10;

            constexpr uint64_t max_amount = (uint64_t) std::numeric_limits<int64_t>::max();
            if (whole > (max_amount - fraction) / nanos_per_dollar) throw std::out_of_range("Limit is too large");
            return whole * nanos_per_dollar + fraction;
        }

        Limits parse_limits(int argc, char** argv) {
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

        std::string dollars(uint64_t amount) {
            std::ostringstream out;
            out << amount / nanos_per_dollar << '.' << std::setfill('0') << std::setw(9) << amount % nanos_per_dollar;
            return out.str();
        }

        void print_user(const User& user) {
            std::cout << user.id << '\t' << user.name << '\t'
                      << dollars(user.usage_limits.five_hour_limit_nanodollars) << '\t'
                      << dollars(user.usage_limits.weekly_limit_nanodollars) << '\n';
        }

        void print_window_usage(std::string_view name, uint64_t cost_nanodollars, uint64_t limit_nanodollars) {
            std::cout << name << ": $" << dollars(cost_nanodollars) << " / $" << dollars(limit_nanodollars) << " (";
            if (limit_nanodollars == 0) {
                std::cout << "n/a: zero limit";
            } else {
                std::cout << std::fixed << std::setprecision(2)
                          << (long double) cost_nanodollars * 100 / limit_nanodollars << '%';
            }
            std::cout << ")\n";
        }

        User find_user(pn::StringView name) {
            auto user = get_user_by_name(name);
            if (!user) throw std::invalid_argument("User not found: " + std::string(name));
            return *user;
        }

        int run_command(int argc, char** argv) {
            std::string_view group = argc > 1 ? argv[1] : "";
            std::string_view command = argc > 2 ? argv[2] : "";
            bool add = group == "user" && command == "add";
            bool set_limits = group == "user" && command == "set-limits";
            bool list = group == "user" && command == "list" && argc == 3;
            bool show = group == "user" && command == "show" && argc == 4;
            bool usage = group == "user" && command == "usage" && argc == 4;
            bool rotate = group == "key" && command == "rotate" && argc == 4;

            if (!list && !show && !usage && !rotate && !add && !set_limits) {
                print_usage(std::cerr, argv[0]);
                return 2;
            }

            if (list) {
                init();
                for (const User& user : list_users()) print_user(user);
                return 0;
            }

            if (argc < 4 || !*argv[3]) throw std::invalid_argument("Name must not be empty");
            Limits limits {};
            if (add || set_limits) limits = parse_limits(argc, argv);
            init();

            if (add) {
                std::string key;
                user_id_t id = make_user(argv[3], limits.five_hour, limits.weekly, key);
                std::cout << "User ID: " << id << "\nAPI key: " << key << '\n';
            } else {
                User user = find_user(argv[3]);
                if (show) {
                    print_user(user);
                } else if (usage) {
                    auto current_usage = get_user_usage(user.id);
                    if (!current_usage) throw std::runtime_error("User no longer exists");
                    std::cout << user.name << '\n';
                    print_window_usage("Five-hour", current_usage->five_hour_cost_nanodollars, current_usage->limits.five_hour_limit_nanodollars);
                    print_window_usage("Weekly", current_usage->weekly_cost_nanodollars, current_usage->limits.weekly_limit_nanodollars);
                } else if (set_limits) {
                    if (!set_usage_limits(user.id, limits.five_hour, limits.weekly)) throw std::runtime_error("User no longer exists");
                    print_user(get_user(user.id).value());
                } else {
                    std::string key;
                    if (!rotate_api_key(user.id, key)) throw std::runtime_error("User no longer exists");
                    std::cout << "API key: " << key << '\n';
                }
            }
            return 0;
        }
    } // namespace

    int run(int argc, char** argv) {
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
} // namespace cli
