#pragma once

#include "Polyweb/polyweb.hpp"
#include "database.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

inline constexpr std::string_view usage_details_marker = "<!-- usage-details -->";

struct UsagePageAssets {
    std::string html;
    std::string css;
    std::string js;
    size_t details_offset;
};

inline std::string read_usage_page_asset(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size < 0) throw std::runtime_error(std::string("Could not read ") + path);
    std::string content((size_t) size, '\0');
    file.seekg(0);
    if (!file.read(content.data(), content.size())) {
        throw std::runtime_error(std::string("Could not read ") + path);
    }

    return content;
}

inline UsagePageAssets load_usage_page_assets() {
    UsagePageAssets assets {
        .html = read_usage_page_asset("web/usage.html"),
        .css = read_usage_page_asset("web/usage.css"),
        .js = read_usage_page_asset("web/usage.js"),
        .details_offset = 0,
    };
    assets.details_offset = assets.html.find(usage_details_marker);
    if (assets.details_offset == std::string::npos) {
        throw std::runtime_error("web/usage.html must contain the usage-details marker");
    }

    return assets;
}

inline pw::Response make_usage_asset_resp(pw::Request& request, std::string_view content, std::string_view content_type) {
    if (request.method != "GET") return pw::Response::make_basic(405, {{"Allow", "GET"}});

    return pw::Response(
        200,
        std::vector<char>(content.begin(), content.end()),
        {
            {"Content-Type", std::string(content_type)},
            {"Cache-Control", "no-cache"},
            {"X-Content-Type-Options", "nosniff"},
        });
}

inline pw::Response make_usage_page_resp(const UsagePageAssets& assets, uint16_t status_code, std::string_view details = {}) {
    const std::string& html = assets.html;
    size_t offset = assets.details_offset;
    std::vector<char> body;
    body.reserve(html.size() - usage_details_marker.size() + details.size());
    body.insert(body.end(), html.begin(), html.begin() + offset);
    body.insert(body.end(), details.begin(), details.end());
    body.insert(body.end(), html.begin() + offset + usage_details_marker.size(), html.end());

    return pw::Response(
        status_code,
        std::move(body),
        {
            {"Content-Type", "text/html; charset=utf-8"},
            {"Cache-Control", "no-store"},
            {"Referrer-Policy", "no-referrer"},
            {"Content-Security-Policy", "default-src 'none'; script-src 'self'; connect-src 'self'; style-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'"},
            {"X-Content-Type-Options", "nosniff"},
        });
}

inline std::string build_usage_details(const User& user, const UserUsage& usage, std::chrono::system_clock::time_point time) {
    auto row = [time](std::string_view name, uint64_t cost, uint64_t limit, std::optional<std::chrono::system_clock::time_point> started_at, std::chrono::system_clock::duration duration) {
        std::ostringstream result;
        result << "<tr><th scope=\"row\">" << name << "</th><td data-label=\"Used\">";
        if (limit == 0) {
            result << "n/a</td><td data-label=\"Reset\">No automatic reset";
        } else {
            long double percent = (long double) cost * 100 / limit;
            result << std::fixed << std::setprecision(2) << "<span>" << percent << "%</span>"
                   << "<progress max=\"100\" value=\"" << std::min(percent, 100.0L)
                   << "\" aria-label=\"" << name << " budget used\"></progress></td><td data-label=\"Reset\">";
            if (!started_at || time >= *started_at + duration) {
                result << "Not scheduled";
            } else {
                auto reset_at = std::chrono::ceil<std::chrono::seconds>(*started_at + duration);
                result << std::format("<time datetime=\"{:%FT%TZ}\">{:%F %T} UTC</time>", reset_at, reset_at);
            }
        }

        result << "</td></tr>";

        return result.str();
    };

    std::string result = "<h2>Usage for " + pw::xml_escape(user.name) + "</h2>";
    result += "<div class=\"table-scroll\"><table><thead><tr><th>Window</th><th>Used</th><th id=\"reset-timezone\">Reset (UTC)</th></tr></thead><tbody>";
    result += row("Five-hour", usage.five_hour_cost_nanodollars, usage.limits.five_hour_limit_nanodollars, usage.limits.five_hour_window_started_at, std::chrono::hours {5});
    result += row("Weekly", usage.weekly_cost_nanodollars, usage.limits.weekly_limit_nanodollars, usage.limits.weekly_window_started_at, std::chrono::weeks {1});
    result += "</tbody></table></div>";

    return result;
}

inline pw::Response handle_usage_request(const UsagePageAssets& assets, pw::Request& request) {
    if (request.method == "GET") return make_usage_page_resp(assets, 200);
    if (request.method != "POST") {
        auto response = make_usage_page_resp(assets, 405);
        response.headers["Allow"] = "GET, POST";
        return response;
    }

    auto content_type = request.headers.find("Content-Type");
    if (content_type == request.headers.end() || !pw::string::to_lower_copy(content_type->second).starts_with("application/x-www-form-urlencoded")) {
        return make_usage_page_resp(assets, 400, "<p>Expected a form submission.</p>");
    }

    std::string body = request.body_to_string();
    if (body.size() > 256) return make_usage_page_resp(assets, 400, "<p>Invalid form submission.</p>");

    pw::QueryParameters form(body);
    auto api_key = form->find("api_key");
    if (api_key == form->end()) return make_usage_page_resp(assets, 400, "<p>API key is required.</p>");

    auto user = database::get_user_by_api_key(api_key->second);
    if (!user) return make_usage_page_resp(assets, 401, "<p>Invalid API key.</p>");

    auto time = std::chrono::system_clock::now();
    auto usage = database::get_user_usage(user->id, time);
    if (!usage) return make_usage_page_resp(assets, 500, "<p>Could not load usage. Please try again.</p>");

    return make_usage_page_resp(assets, 200, build_usage_details(*user, *usage, time));
}
