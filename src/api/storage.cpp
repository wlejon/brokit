#include "api/api.h"
#include "api/arg_reader.h"
#include "api/host_proxy.h"
#include "api/object_builder.h"

#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace brokit::api {

namespace {

struct StorageState {
    std::map<std::string, std::string> items;
    std::string storagePath; // empty = in-memory only
};

thread_local StorageState g_localStorage;
thread_local StorageState g_sessionStorage;

static void loadStorage(StorageState* state)
{
    state->items.clear();
    if (state->storagePath.empty()) return;

    std::ifstream file(state->storagePath);
    if (!file.is_open()) return;

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    size_t pos = content.find('{');
    if (pos == std::string::npos) return;
    pos++;

    auto parseString = [&](size_t& p) -> std::string {
        if (p >= content.size() || content[p] != '"') return "";
        p++;
        std::string result;
        while (p < content.size() && content[p] != '"') {
            if (content[p] == '\\' && p + 1 < content.size()) {
                p++;
                switch (content[p]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    default: result += content[p]; break;
                }
            } else {
                result += content[p];
            }
            p++;
        }
        if (p < content.size()) p++;
        return result;
    };

    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
               content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
            pos++;
        if (pos >= content.size() || content[pos] == '}') break;

        std::string key = parseString(pos);
        while (pos < content.size() && content[pos] != ':') pos++;
        if (pos < content.size()) pos++;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;

        std::string value = parseString(pos);
        if (!key.empty()) {
            state->items[key] = value;
        }
    }
}

static void saveStorage(StorageState* state)
{
    if (!state || state->storagePath.empty()) return;

    std::ofstream file(state->storagePath);
    if (!file.is_open()) return;

    auto escapeJson = [](const std::string& s) -> std::string {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\t': result += "\\t"; break;
                default: result += c; break;
            }
        }
        return result;
    };

    file << "{\n";
    bool first = true;
    for (const auto& [key, val] : state->items) {
        if (!first) file << ",\n";
        file << "  \"" << escapeJson(key) << "\": \"" << escapeJson(val) << "\"";
        first = false;
    }
    file << "\n}\n";
}

bronze::Value makeStorageValue(StorageState* st)
{
    ObjectBuilder b;
    b.def("getItem", 1, [st](bronze::Value, std::span<const bronze::Value> a) {
        bronze::Value keyV = argAt(a, 0);
        if (ev::isObject(keyV) || ev::isUndefined(keyV)) return ev::null();
        std::string key = ev::toUtf8(keyV);
        auto it = st->items.find(key);
        if (it == st->items.end()) return ev::null();
        return ev::fromUtf8(it->second);
    });

    b.def("setItem", 2, [st](bronze::Value, std::span<const bronze::Value> a) {
        bronze::Value keyV = argAt(a, 0);
        bronze::Value valV = argAt(a, 1);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            std::string key = ev::toUtf8(keyV);
            std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV)) ? ev::toUtf8(valV) : "";
            st->items[key] = val;
            saveStorage(st);
        }
        return ev::undefined();
    });

    b.def("removeItem", 1, [st](bronze::Value, std::span<const bronze::Value> a) {
        bronze::Value keyV = argAt(a, 0);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            st->items.erase(ev::toUtf8(keyV));
            saveStorage(st);
        }
        return ev::undefined();
    });

    b.def("clear", 0, [st](bronze::Value, std::span<const bronze::Value>) {
        st->items.clear();
        saveStorage(st);
        return ev::undefined();
    });

    b.def("key", 1, [st](bronze::Value, std::span<const bronze::Value> a) {
        int idx = i32At(a, 0);
        if (idx < 0 || static_cast<size_t>(idx) >= st->items.size()) return ev::null();
        auto it = st->items.begin();
        std::advance(it, idx);
        return ev::fromUtf8(it->first);
    });

    b.accessor("length", [st](bronze::Value, std::span<const bronze::Value>) {
        return ev::fromDouble(static_cast<double>(st->items.size()));
    }, nullptr);

    HostProxyTraps t;
    t.methods = b.get();
    t.get = [st](const std::string& key, bronze::Value& out) {
        auto it = st->items.find(key);
        if (it == st->items.end()) return false;
        out = ev::fromUtf8(it->second);
        return true;
    };
    t.set = [st](const std::string& key, bronze::Value v) {
        if (ev::isObject(v)) return;
        st->items[key] = ev::isUndefined(v) ? "undefined" : ev::toUtf8(v);
        saveStorage(st);
    };
    t.has = [st](const std::string& key) {
        return st->items.find(key) != st->items.end();
    };
    t.ownKeys = [st]() {
        std::vector<std::string> keys;
        for (const auto& [k, v] : st->items) {
            (void)v;
            keys.push_back(k);
        }
        return keys;
    };
    t.remove = [st](const std::string& key) {
        if (st->items.erase(key)) saveStorage(st);
    };

    return makeHostProxy(std::move(t));
}

} // namespace

void installStorage()
{
    ev::setGlobalValue("localStorage", makeStorageValue(&g_localStorage));
    ev::setGlobalValue("sessionStorage", makeStorageValue(&g_sessionStorage));
}

void setStoragePath(const std::string& path)
{
    g_localStorage.storagePath = path;
    loadStorage(&g_localStorage);
}

void cleanupStorage()
{
    g_localStorage.items.clear();
    g_sessionStorage.items.clear();
}

} // namespace brokit::api
