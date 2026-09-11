#include "api/api.h"
#include "api/arg_reader.h"
#include "api/object_builder.h"

#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <span>
#include <cstdio>

#include "sqlite3.h"

namespace brokit::api {

// ---------------------------------------------------------------------------
// SQLite-backed IndexedDB
//
// Schema per object store:
//   CREATE TABLE <store> (key TEXT PRIMARY KEY, value TEXT)
//   Keys and values are JSON-stringified for simplicity.
//
// Each database is a SQLite file: <basePath>/<dbName>.idb
// ---------------------------------------------------------------------------

struct IdbState {
    std::unordered_map<std::string, sqlite3*> dbs;
};

static thread_local IdbState g_idbState;
static thread_local std::string g_idbBasePath = ".";

static std::string dbPath(const std::string& name) {
    std::string base = g_idbBasePath;
    if (base.empty()) base = ".";
    if (base.back() != '/' && base.back() != '\\') base += '/';
    return base + name + ".idb";
}

static sqlite3* openDb(const std::string& name) {
    auto it = g_idbState.dbs.find(name);
    if (it != g_idbState.dbs.end()) return it->second;

    std::string path = dbPath(name);
    sqlite3* db = nullptr;
    int rc = sqlite3_open(path.c_str(), &db);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }

    // WAL mode for better concurrent read/write
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);

    // Create metadata table for version tracking
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS __idb_meta "
                      "(key TEXT PRIMARY KEY, value TEXT)", nullptr, nullptr, nullptr);

    g_idbState.dbs[name] = db;
    return db;
}

static int getVersion(sqlite3* db) {
    int version = 0;
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT value FROM __idb_meta WHERE key='version'",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return version;
}

static void setVersion(sqlite3* db, int version) {
    char sql[128];
    snprintf(sql, sizeof(sql),
             "INSERT OR REPLACE INTO __idb_meta(key,value) VALUES('version','%d')", version);
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Native helpers exposed to JS
// ---------------------------------------------------------------------------

// __brokit_idb_open(name, version) → { db handle info }
static bronze::Value js_idb_open(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::throwTypeError("idb_open: name required");

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    int requestedVersion = reader.getInt(1, 1);

    sqlite3* db = openDb(dbName);
    if (!db) {
        return ev::throwError("idb_open: failed to open database");
    }

    int currentVersion = getVersion(db);
    bool needsUpgrade = (requestedVersion > currentVersion);

    ObjectBuilder result;
    result.set("name", dbName);
    result.set("version", static_cast<double>(requestedVersion));
    result.set("oldVersion", static_cast<double>(currentVersion));
    result.set("needsUpgrade", needsUpgrade);

    if (needsUpgrade) {
        setVersion(db, requestedVersion);
    }

    return result.build();
}

// __brokit_idb_create_store(dbName, storeName, options?)
static bronze::Value js_idb_create_store(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::throwTypeError("create_store: dbName and storeName required");

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    std::string storeName = reader.getString(1, "");

    sqlite3* db = openDb(dbName);
    if (!db) {
        return ev::throwError("create_store: database not open");
    }

    // Check for autoIncrement option
    bool autoIncrement = false;
    std::string keyPath;
    if (args.size() >= 3 && ev::isObject(args[2])) {
        bronze::Value ai = ev::getProperty(args[2], "autoIncrement");
        autoIncrement = ev::isBool(ai) && ev::toBool(ai);

        bronze::Value kp = ev::getProperty(args[2], "keyPath");
        if (ev::isString(kp)) {
            keyPath = ev::toUtf8(kp);
        }
    }

    // Create table — key column + value column (JSON)
    std::string sql;
    if (autoIncrement) {
        sql = "CREATE TABLE IF NOT EXISTS [" + storeName +
              "](key INTEGER PRIMARY KEY AUTOINCREMENT, value TEXT)";
    } else {
        sql = "CREATE TABLE IF NOT EXISTS [" + storeName +
              "](key TEXT PRIMARY KEY, value TEXT)";
    }

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "SQL error";
        if (errMsg) sqlite3_free(errMsg);
        return ev::throwError("create_store: " + err);
    }

    return ev::fromBool(true);
}

// __brokit_idb_delete_store(dbName, storeName)
static bronze::Value js_idb_delete_store(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::fromBool(false);

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    std::string storeName = reader.getString(1, "");

    sqlite3* db = openDb(dbName);
    if (!db) return ev::fromBool(false);

    std::string sql = "DROP TABLE IF EXISTS [" + storeName + "]";
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    return ev::fromBool(true);
}

// __brokit_idb_put(dbName, storeName, key, value) → key
static bronze::Value js_idb_put(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 4) return ev::throwTypeError("idb_put: requires dbName, storeName, key, value");

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    std::string storeName = reader.getString(1, "");
    std::string key = reader.getString(2, "");
    std::string value = reader.getString(3, "");

    sqlite3* db = openDb(dbName);
    if (!db) {
        return ev::throwError("idb_put: database not open");
    }

    std::string sql = "INSERT OR REPLACE INTO [" + storeName +
                      "](key, value) VALUES(?, ?)";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return ev::throwError(std::string("idb_put: ") + sqlite3_errmsg(db));
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return ev::throwError(std::string("idb_put: ") + sqlite3_errmsg(db));
    }

    return ev::fromUtf8(key);
}

// __brokit_idb_get(dbName, storeName, key) → value string | undefined
static bronze::Value js_idb_get(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 3) return ev::undefined();

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    std::string storeName = reader.getString(1, "");
    std::string key = reader.getString(2, "");

    sqlite3* db = openDb(dbName);
    if (!db) return ev::undefined();

    std::string sql = "SELECT value FROM [" + storeName + "] WHERE key=?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return ev::undefined();
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    bronze::Value result = ev::undefined();
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (val) result = ev::fromUtf8(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

// __brokit_idb_delete(dbName, storeName, key) → bool
static bronze::Value js_idb_delete(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 3) return ev::fromBool(false);

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    std::string storeName = reader.getString(1, "");
    std::string key = reader.getString(2, "");

    sqlite3* db = openDb(dbName);
    if (!db) return ev::fromBool(false);

    std::string sql = "DELETE FROM [" + storeName + "] WHERE key=?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return ev::fromBool(false);

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return ev::fromBool(rc == SQLITE_DONE);
}

// __brokit_idb_clear(dbName, storeName) → bool
static bronze::Value js_idb_clear(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::fromBool(false);

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    std::string storeName = reader.getString(1, "");

    sqlite3* db = openDb(dbName);
    if (!db) return ev::fromBool(false);

    std::string sql = "DELETE FROM [" + storeName + "]";
    return ev::fromBool(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
}

// __brokit_idb_get_all(dbName, storeName, limit?) → [[key, value], ...]
static bronze::Value js_idb_get_all(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return hostArrayOf(std::span<const bronze::Value>{});

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    std::string storeName = reader.getString(1, "");
    int limit = reader.getInt(2, -1);

    sqlite3* db = openDb(dbName);
    if (!db) return hostArrayOf(std::span<const bronze::Value>{});

    std::string sql = "SELECT key, value FROM [" + storeName + "] ORDER BY key";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return hostArrayOf(std::span<const bronze::Value>{});
    }

    std::vector<bronze::Value> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        bronze::Value pairArr[2] = {
            key ? ev::fromUtf8(key) : ev::null(),
            val ? ev::fromUtf8(val) : ev::null()
        };
        rows.push_back(hostArrayOf(std::span<const bronze::Value>(pairArr, 2)));
    }
    sqlite3_finalize(stmt);
    return hostArrayOf(rows);
}

// __brokit_idb_count(dbName, storeName) → int
static bronze::Value js_idb_count(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.size() < 2) return ev::fromDouble(0);

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");
    std::string storeName = reader.getString(1, "");

    sqlite3* db = openDb(dbName);
    if (!db) return ev::fromDouble(0);

    std::string sql = "SELECT COUNT(*) FROM [" + storeName + "]";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return ev::fromDouble(0);
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return ev::fromDouble(count);
}

// __brokit_idb_store_names(dbName) → [name, ...]
static bronze::Value js_idb_store_names(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return hostArrayOf(std::span<const bronze::Value>{});

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");

    sqlite3* db = openDb(dbName);
    if (!db) return hostArrayOf(std::span<const bronze::Value>{});

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE '__idb_%'",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return hostArrayOf(std::span<const bronze::Value>{});
    }

    std::vector<bronze::Value> names;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (name) names.push_back(ev::fromUtf8(name));
    }
    sqlite3_finalize(stmt);
    return hostArrayOf(names);
}

// __brokit_idb_delete_db(name) → bool
static bronze::Value js_idb_delete_db(bronze::Value, std::span<const bronze::Value> args)
{
    if (args.empty()) return ev::fromBool(false);

    ArgReader reader(args);
    std::string dbName = reader.getString(0, "");

    // Close if open
    auto it = g_idbState.dbs.find(dbName);
    if (it != g_idbState.dbs.end()) {
        sqlite3_close(it->second);
        g_idbState.dbs.erase(it);
    }

    // Delete the file
    std::string path = dbPath(dbName);
    remove(path.c_str());
    // Also remove WAL and SHM files
    remove((path + "-wal").c_str());
    remove((path + "-shm").c_str());

    return ev::fromBool(true);
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installIndexedDB()
{
    ev::registerFunction("__brokit_idb_open", js_idb_open);
    ev::registerFunction("__brokit_idb_create_store", js_idb_create_store);
    ev::registerFunction("__brokit_idb_delete_store", js_idb_delete_store);
    ev::registerFunction("__brokit_idb_put", js_idb_put);
    ev::registerFunction("__brokit_idb_get", js_idb_get);
    ev::registerFunction("__brokit_idb_delete", js_idb_delete);
    ev::registerFunction("__brokit_idb_clear", js_idb_clear);
    ev::registerFunction("__brokit_idb_get_all", js_idb_get_all);
    ev::registerFunction("__brokit_idb_count", js_idb_count);
    ev::registerFunction("__brokit_idb_store_names", js_idb_store_names);
    ev::registerFunction("__brokit_idb_delete_db", js_idb_delete_db);
}

void setIndexedDBPath(const std::string& path)
{
    g_idbBasePath = path;
}

void cleanupIndexedDB()
{
    for (auto& [name, db] : g_idbState.dbs) {
        if (db) sqlite3_close(db);
    }
    g_idbState.dbs.clear();
}

} // namespace brokit::api
