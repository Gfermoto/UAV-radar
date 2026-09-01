#pragma once

#include <string>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdlib>

class String : public std::string {
public:
    String() : std::string() {}
    String(const char *s) : std::string(s ? s : "") {}
    String(const std::string &s) : std::string(s) {}

    const char *c_str() const { return std::string::c_str(); }

    String &operator=(const char *s) {
        std::string::operator=(s ? s : "");
        return *this;
    }

    bool concat(const char *s) {
        if (!s) return false;
        append(s);
        return true;
    }

    bool concat(char c) {
        push_back(c);
        return true;
    }

    int indexOf(const char *needle, size_t from = 0) const {
        const size_t position = find(needle ? needle : "", from);
        return position == npos ? -1 : static_cast<int>(position);
    }

    int indexOf(char needle, size_t from = 0) const {
        const size_t position = find(needle, from);
        return position == npos ? -1 : static_cast<int>(position);
    }

    String substring(size_t start) const {
        return start < size() ? String(substr(start)) : String();
    }

    String substring(size_t start, size_t end) const {
        if (start >= size() || end <= start) return String();
        return String(substr(start, end - start));
    }

    bool startsWith(const char *prefix) const {
        if (!prefix) return false;
        const size_t length = std::strlen(prefix);
        return size() >= length && compare(0, length, prefix) == 0;
    }

    void trim() {
        auto first = std::find_if_not(begin(), end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        auto last = std::find_if_not(rbegin(), rend(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }).base();
        if (first >= last) {
            clear();
        } else {
            std::string::operator=(
                substr(static_cast<size_t>(first - begin()),
                       static_cast<size_t>(last - first)));
        }
    }

    void replace(const char *from, const char *to) {
        if (!from || !from[0]) return;
        const std::string replacement = to ? to : "";
        size_t position = 0;
        while ((position = find(from, position)) != npos) {
            std::string::replace(position, std::strlen(from), replacement);
            position += replacement.size();
        }
    }

    long toInt() const {
        return std::strtol(c_str(), nullptr, 10);
    }
};
