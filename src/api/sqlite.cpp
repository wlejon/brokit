// Native SQLite bindings over the bundled sqlite3 amalgamation — backs the
// `sqlite` / `node:sqlite` module (js/sqlite.js).

#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include "sqlite3.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace brokit::api {

namespace {

static thread_local std::unordered_map<int, sqlite3*> g_dbs;
static thread_local int g_nextDbId = 1;

sqlite3* getDb(int id) {
    auto it = g_dbs.find(id);
    return (it != g_dbs.end()) ? it->second : nullptr;
}

bool bindOneValue(sqlite3_stmt* stmt, int idx, Value val, std::string& errOut) {
    int rc = SQLITE_OK;
    if (ev::isUndefined(val) || ev::isNull(val)) {
        rc = sqlite3_bind_null(stmt, idx);
    } else if (ev::isBool(val)) {
        rc = sqlite3_bind_int(stmt, idx, ev::toBool(val) ? 1 : 0);
    } else if (ev::isNumber(val)) {
        double d = ev::toDouble(val);
        if (std::isfinite(d) && std::floor(d) == d &&
            d >= -9007199254740991.0 && d <= 9007199254740991.0) {
            rc = sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(d));
        } else {
            rc = sqlite3_bind_double(stmt, idx, d);
        }
    } else if (ev::isString(val)) {
        std::string s = ev::toUtf8(val);
        rc = sqlite3_bind_text(stmt, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
    } else {
        const uint8_t* bytes = nullptr;
        size_t len = 0;
        if (bufferBytes(val, &bytes, &len)) {
            rc = sqlite3_bind_blob(stmt, idx, bytes, static_cast<int>(len), SQLITE_TRANSIENT);
        } else {
            std::string s = ev::toUtf8(val);
            rc = sqlite3_bind_text(stmt, idx, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
        }
    }
    if (rc != SQLITE_OK) {
        errOut = sqlite3_errstr(rc);
        return false;
    }
    return true;
}

bool bindParams(sqlite3_stmt* stmt, Value paramsArray, std::string& errOut) {
    if (ev::isUndefined(paramsArray) || ev::isNull(paramsArray)) return true;
    if (!ev::isObject(paramsArray)) return true;

    ev::Persistent arrRoot{paramsArray};
    Value lenV = ev::getProperty(arrRoot.get(), "length");
    uint32_t len = ev::isNumber(lenV) ? saturateU32(ev::toDouble(lenV)) : 0;

    // If a single plain object was passed and the statement uses named
    // parameters (:foo, $foo, @foo), bind by parameter name.
    if (len == 1) {
        ev::Persistent first{ev::getElement(arrRoot.get(), 0)};
        if (ev::isObject(first.get())) {
            const uint8_t* dummy = nullptr;
            size_t dummyLen = 0;
            if (!bufferBytes(first.get(), &dummy, &dummyLen)) {
                int paramCount = sqlite3_bind_parameter_count(stmt);
                bool hasNamed = false;
                for (int i = 1; i <= paramCount; ++i) {
                    const char* rawName = sqlite3_bind_parameter_name(stmt, i);
                    if (!rawName || !*rawName) continue;
                    hasNamed = true;
                    // Look up both `:foo` / `$foo` / `@foo` and bare `foo`
                    Value v = ev::getProperty(first.get(), rawName);
                    if (ev::isUndefined(v) && (rawName[0] == ':' || rawName[0] == '$' || rawName[0] == '@')) {
                        v = ev::getProperty(first.get(), rawName + 1);
                    }
                    if (!bindOneValue(stmt, i, v, errOut)) return false;
                }
                if (hasNamed) return true;
            }
        }
    }

    for (uint32_t i = 0; i < len; ++i) {
        ev::Persistent v{ev::getElement(arrRoot.get(), i)};
        if (!bindOneValue(stmt, static_cast<int>(i + 1), v.get(), errOut)) return false;
    }
    return true;
}

Value readCurrentRow(sqlite3_stmt* stmt) {
    ObjectBuilder row;
    int cols = sqlite3_column_count(stmt);
    for (int i = 0; i < cols; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        if (!name) name = "";
        int type = sqlite3_column_type(stmt, i);
        switch (type) {
        case SQLITE_INTEGER:
            row.set(name, static_cast<double>(sqlite3_column_int64(stmt, i)));
            break;
        case SQLITE_FLOAT:
            row.set(name, sqlite3_column_double(stmt, i));
            break;
        case SQLITE_TEXT: {
            const char* txt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            int n = sqlite3_column_bytes(stmt, i);
            row.set(name, std::string(txt ? txt : "", txt ? n : 0));
            break;
        }
        case SQLITE_BLOB: {
            const auto* ptr = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, i));
            int n = sqlite3_column_bytes(stmt, i);
            ev::Persistent ab{ev::createArrayBuffer(std::span<const uint8_t>(ptr, ptr ? n : 0))};
            Value u8 = ev::createTypedArrayView(elements::Uint8, ab.get(), 0, static_cast<uint32_t>(ptr ? n : 0));
            row.set(name, u8);
            break;
        }
        case SQLITE_NULL:
        default:
            row.set(name, ev::null());
            break;
        }
    }
    return row.build();
}

static bronze::Value js_sqlite_open(bronze::Value, std::span<const bronze::Value> args) {
    std::string path = strAt(args, 0);
    if (path.empty()) path = ":memory:";
    else if (path != ":memory:") path = resolveAssetPath(path);

    sqlite3* db = nullptr;
    int rc = sqlite3_open(path.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::string msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        if (db) sqlite3_close(db);
        return ev::throwError("SQLite open failed: " + msg);
    }

    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);

    int id = g_nextDbId++;
    g_dbs[id] = db;
    return ev::fromDouble(static_cast<double>(id));
}

static bronze::Value js_sqlite_close(bronze::Value, std::span<const bronze::Value> args) {
    int id = i32At(args, 0);
    auto it = g_dbs.find(id);
    if (it != g_dbs.end()) {
        sqlite3_close_v2(it->second);
        g_dbs.erase(it);
    }
    return ev::undefined();
}

static bronze::Value js_sqlite_exec(bronze::Value, std::span<const bronze::Value> args) {
    int id = i32At(args, 0);
    sqlite3* db = getDb(id);
    if (!db) return ev::throwError("Database is not open");

    std::string sql = strAt(args, 1);
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : sqlite3_errmsg(db);
        if (errMsg) sqlite3_free(errMsg);
        return ev::throwError("SQLite error: " + err);
    }
    return ev::undefined();
}

static bronze::Value js_sqlite_query(bronze::Value, std::span<const bronze::Value> args) {
    int id = i32At(args, 0);
    sqlite3* db = getDb(id);
    if (!db) return ev::throwError("Database is not open");

    std::string mode = strAt(args, 1); // "run" | "get" | "all"
    std::string sql = strAt(args, 2);
    ev::Persistent params{argAt(args, 3)};

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        std::string err = sqlite3_errmsg(db);
        return ev::throwError("SQLite prepare error: " + err);
    }

    std::string bindErr;
    if (!bindParams(stmt, params.get(), bindErr)) {
        sqlite3_finalize(stmt);
        return ev::throwError("SQLite bind error: " + bindErr);
    }

    if (mode == "run") {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            std::string err = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return ev::throwError("SQLite step error: " + err);
        }
        int changes = sqlite3_changes(db);
        sqlite3_int64 lastId = sqlite3_last_insert_rowid(db);
        sqlite3_finalize(stmt);

        ObjectBuilder res;
        res.set("changes", static_cast<double>(changes));
        res.set("lastInsertRowid", static_cast<double>(lastId));
        return res.build();
    }

    if (mode == "get") {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            ev::Persistent row{readCurrentRow(stmt)};
            sqlite3_finalize(stmt);
            return row.get();
        }
        if (rc != SQLITE_DONE) {
            std::string err = sqlite3_errmsg(db);
            sqlite3_finalize(stmt);
            return ev::throwError("SQLite step error: " + err);
        }
        sqlite3_finalize(stmt);
        return ev::undefined();
    }

    // "all"
    ArrayBuilder rows;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        rows.push(readCurrentRow(stmt));
    }
    if (rc != SQLITE_DONE) {
        std::string err = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return ev::throwError("SQLite step error: " + err);
    }
    sqlite3_finalize(stmt);
    return rows.get();
}

} // namespace

void installSqlite() {
    ev::registerFunction("__brokit_sqlite_open", js_sqlite_open, 1);
    ev::registerFunction("__brokit_sqlite_close", js_sqlite_close, 1);
    ev::registerFunction("__brokit_sqlite_exec", js_sqlite_exec, 2);
    ev::registerFunction("__brokit_sqlite_query", js_sqlite_query, 4);
}

void cleanupSqlite() {
    for (auto& [id, db] : g_dbs) {
        if (db) sqlite3_close_v2(db);
    }
    g_dbs.clear();
}

} // namespace brokit::api
