#include "native/ini.h"

#include <cctype>
#include <cstdio>
#include <fstream>

namespace ini {

namespace {
std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
    for (char& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}
}  // namespace

bool read(const std::string& path, std::map<std::string, std::string>& out) {
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr)
        return false;
    char line[1024];
    std::string section;
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        const size_t cut = s.find_first_of(";#");
        if (cut != std::string::npos)
            s.erase(cut);
        s = trim(s);
        if (s.empty())
            continue;
        if (s.front() == '[' && s.back() == ']') {
            section = lower(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        const size_t eq = s.find('=');
        if (eq == std::string::npos)
            continue;
        out[section + "." + lower(trim(s.substr(0, eq)))] = trim(s.substr(eq + 1));
    }
    fclose(f);
    return true;
}

bool write(const std::string& path, const std::vector<std::pair<std::string, std::string>>& values,
           const char* wanted, bool create) {
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        if (!in && !create)
            return false;
        for (std::string line; in && std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }
    std::vector<bool> done(values.size(), false);
    std::string section;
    int sectionEnd = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string t = lines[i];
        t.erase(0, t.find_first_not_of(" \t"));
        if (!t.empty() && t[0] == '[') {
            section = t.substr(1, t.find(']') - 1);
            if (section == wanted)
                sectionEnd = (int)i;
            continue;
        }
        if (section != wanted || t.empty() || t[0] == ';' || t[0] == '#')
            continue;
        sectionEnd = (int)i;
        const size_t eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = t.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        for (size_t k = 0; k < values.size(); k++)
            if (values[k].first == key) {
                lines[i] = key + " = " + values[k].second;
                done[k] = true;
            }
    }
    std::vector<std::string> extra;
    for (size_t k = 0; k < values.size(); k++)
        if (!done[k])
            extra.push_back(values[k].first + " = " + values[k].second);
    if (!extra.empty()) {
        if (sectionEnd < 0) {
            if (!lines.empty() && !lines.back().empty())
                lines.push_back("");
            lines.push_back(std::string("[") + wanted + "]");
            sectionEnd = (int)lines.size() - 1;
        }
        lines.insert(lines.begin() + sectionEnd + 1, extra.begin(), extra.end());
    }
    std::ofstream out(path, std::ios::trunc);
    for (const std::string& line : lines)
        out << line << "\n";
    return (bool)out;
}

}  // namespace ini
