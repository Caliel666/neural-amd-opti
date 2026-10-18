#pragma once
// [Network] Model in dlssnr-amd.ini: which model the network runs. Read by the runtime when it builds the
// network. -1: no file, no section or no readable value (the runtime's default, the small model).
#include <cctype>
#include <cstdio>
#include <string>

namespace nr {
// "small" -> 1, "original" -> 0, anything else -1 (case-insensitive)
inline int network_value(std::string v) {
    for (auto& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    return v == "small" ? 1 : v == "original" ? 0 : -1;
}
inline int ini_network(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return -1;
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    int r = -1;
    std::string section; char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto c = t.find_first_of(";#");
        if (c != std::string::npos) t.resize(c);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { const auto e = t.find(']'); section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1))); continue; }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section != "network") continue;
        if (lower(trim(t.substr(0, eq))) == "model") r = network_value(trim(t.substr(eq + 1)));
    }
    std::fclose(f);
    return r;
}
}  // namespace nr
