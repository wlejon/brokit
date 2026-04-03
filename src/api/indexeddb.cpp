#include "api/api.h"
#include "runtime/runtime.h"

#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <algorithm>

#include "sqlite3.h"

extern "C" {
#include "quickjs.h"
}

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

static const char* kIdbBasePath = "__brokit_idb_base_path";

static std::string getBasePath(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kIdbBasePath);
    std::string path;
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        if (s) { path = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return path;
}

static std::string dbPath(JSContext* ctx, const std::string& name) {
    std::string base = getBasePath(ctx);
    if (base.empty()) base = ".";
    if (base.back() != '/' && base.back() != '\\') base += '/';
    return base + name + ".idb";
}

// ---------------------------------------------------------------------------
// DB handle cache — one SQLite connection per database name per context
// ---------------------------------------------------------------------------
struct IdbState {
    std::unordered_map<std::string, sqlite3*> dbs;
};

static const char* kIdbStateKey = "__brokit_idb_state_ptr";

static IdbState* getIdbState(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, kIdbStateKey);
    IdbState* state = nullptr;
    if (JS_IsNumber(val)) {
        int64_t ptr = 0;
        JS_ToInt64(ctx, &ptr, val);
        state = reinterpret_cast<IdbState*>(static_cast<intptr_t>(ptr));
    }
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, global);
    return state;
}

static sqlite3* openDb(JSContext* ctx, const std::string& name) {
    auto* state = getIdbState(ctx);
    if (!state) return nullptr;

    auto it = state->dbs.find(name);
    if (it != state->dbs.end()) return it->second;

    std::string path = dbPath(ctx, name);
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

    state->dbs[name] = db;
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
// Returns synchronously — real IndexedDB is async, JS wrapper handles that
static JSValue js_idb_open(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "idb_open: name required");

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    std::string dbName(name);
    JS_FreeCString(ctx, name);

    int requestedVersion = 1;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        JS_ToInt32(ctx, &requestedVersion, argv[1]);
    }

    sqlite3* db = openDb(ctx, dbName);
    if (!db) {
        return JS_ThrowInternalError(ctx, "idb_open: failed to open database");
    }

    int currentVersion = getVersion(db);
    bool needsUpgrade = (requestedVersion > currentVersion);

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "name", JS_NewString(ctx, dbName.c_str()));
    JS_SetPropertyStr(ctx, result, "version", JS_NewInt32(ctx, requestedVersion));
    JS_SetPropertyStr(ctx, result, "oldVersion", JS_NewInt32(ctx, currentVersion));
    JS_SetPropertyStr(ctx, result, "needsUpgrade", JS_NewBool(ctx, needsUpgrade));

    if (needsUpgrade) {
        setVersion(db, requestedVersion);
    }

    return result;
}

// __brokit_idb_create_store(dbName, storeName, options?)
static JSValue js_idb_create_store(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "create_store: dbName and storeName required");

    const char* dbName = JS_ToCString(ctx, argv[0]);
    const char* storeName = JS_ToCString(ctx, argv[1]);
    if (!dbName || !storeName) {
        if (dbName) JS_FreeCString(ctx, dbName);
        if (storeName) JS_FreeCString(ctx, storeName);
        return JS_EXCEPTION;
    }

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);

    if (!db) {
        JS_FreeCString(ctx, storeName);
        return JS_ThrowInternalError(ctx, "create_store: database not open");
    }

    // Check for autoIncrement option
    bool autoIncrement = false;
    std::string keyPath;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        JSValue ai = JS_GetPropertyStr(ctx, argv[2], "autoIncrement");
        autoIncrement = JS_ToBool(ctx, ai);
        JS_FreeValue(ctx, ai);

        JSValue kp = JS_GetPropertyStr(ctx, argv[2], "keyPath");
        if (JS_IsString(kp)) {
            const char* s = JS_ToCString(ctx, kp);
            if (s) { keyPath = s; JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, kp);
    }

    // Create table — key column + value column (JSON)
    std::string sql;
    if (autoIncrement) {
        sql = "CREATE TABLE IF NOT EXISTS [" + std::string(storeName) +
              "](key INTEGER PRIMARY KEY AUTOINCREMENT, value TEXT)";
    } else {
        sql = "CREATE TABLE IF NOT EXISTS [" + std::string(storeName) +
              "](key TEXT PRIMARY KEY, value TEXT)";
    }

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    JS_FreeCString(ctx, storeName);

    if (rc != SQLITE_OK) {
        std::string err = errMsg ? errMsg : "SQL error";
        if (errMsg) sqlite3_free(errMsg);
        return JS_ThrowInternalError(ctx, "create_store: %s", err.c_str());
    }

    // Store metadata (keyPath, autoIncrement) in __idb_meta
    // (simplified — store as store_<name>_keyPath etc.)

    return JS_TRUE;
}

// __brokit_idb_delete_store(dbName, storeName)
static JSValue js_idb_delete_store(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_FALSE;

    const char* dbName = JS_ToCString(ctx, argv[0]);
    const char* storeName = JS_ToCString(ctx, argv[1]);
    if (!dbName || !storeName) {
        if (dbName) JS_FreeCString(ctx, dbName);
        if (storeName) JS_FreeCString(ctx, storeName);
        return JS_FALSE;
    }

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);

    if (!db) { JS_FreeCString(ctx, storeName); return JS_FALSE; }

    std::string sql = "DROP TABLE IF EXISTS [" + std::string(storeName) + "]";
    JS_FreeCString(ctx, storeName);
    sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    return JS_TRUE;
}

// __brokit_idb_put(dbName, storeName, key, value) → key
static JSValue js_idb_put(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 4) return JS_ThrowTypeError(ctx, "idb_put: requires dbName, storeName, key, value");

    const char* dbName = JS_ToCString(ctx, argv[0]);
    const char* storeName = JS_ToCString(ctx, argv[1]);
    const char* key = JS_ToCString(ctx, argv[2]);
    const char* value = JS_ToCString(ctx, argv[3]);

    if (!dbName || !storeName || !key || !value) {
        if (dbName) JS_FreeCString(ctx, dbName);
        if (storeName) JS_FreeCString(ctx, storeName);
        if (key) JS_FreeCString(ctx, key);
        if (value) JS_FreeCString(ctx, value);
        return JS_EXCEPTION;
    }

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);

    if (!db) {
        JS_FreeCString(ctx, storeName);
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, value);
        return JS_ThrowInternalError(ctx, "idb_put: database not open");
    }

    std::string sql = "INSERT OR REPLACE INTO [" + std::string(storeName) +
                      "](key, value) VALUES(?, ?)";
    JS_FreeCString(ctx, storeName);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, value);
        return JS_ThrowInternalError(ctx, "idb_put: %s", sqlite3_errmsg(db));
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    JSValue result = JS_NewString(ctx, key);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, value);

    if (rc != SQLITE_DONE) {
        JS_FreeValue(ctx, result);
        return JS_ThrowInternalError(ctx, "idb_put: %s", sqlite3_errmsg(db));
    }

    return result;
}

// __brokit_idb_get(dbName, storeName, key) → value string | undefined
static JSValue js_idb_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return JS_UNDEFINED;

    const char* dbName = JS_ToCString(ctx, argv[0]);
    const char* storeName = JS_ToCString(ctx, argv[1]);
    const char* key = JS_ToCString(ctx, argv[2]);

    if (!dbName || !storeName || !key) {
        if (dbName) JS_FreeCString(ctx, dbName);
        if (storeName) JS_FreeCString(ctx, storeName);
        if (key) JS_FreeCString(ctx, key);
        return JS_UNDEFINED;
    }

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);

    if (!db) {
        JS_FreeCString(ctx, storeName);
        JS_FreeCString(ctx, key);
        return JS_UNDEFINED;
    }

    std::string sql = "SELECT value FROM [" + std::string(storeName) + "] WHERE key=?";
    JS_FreeCString(ctx, storeName);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        JS_FreeCString(ctx, key);
        return JS_UNDEFINED;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    JS_FreeCString(ctx, key);

    JSValue result = JS_UNDEFINED;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (val) result = JS_NewString(ctx, val);
    }
    sqlite3_finalize(stmt);
    return result;
}

// __brokit_idb_delete(dbName, storeName, key) → bool
static JSValue js_idb_delete(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 3) return JS_FALSE;

    const char* dbName = JS_ToCString(ctx, argv[0]);
    const char* storeName = JS_ToCString(ctx, argv[1]);
    const char* key = JS_ToCString(ctx, argv[2]);

    if (!dbName || !storeName || !key) {
        if (dbName) JS_FreeCString(ctx, dbName);
        if (storeName) JS_FreeCString(ctx, storeName);
        if (key) JS_FreeCString(ctx, key);
        return JS_FALSE;
    }

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);

    if (!db) {
        JS_FreeCString(ctx, storeName);
        JS_FreeCString(ctx, key);
        return JS_FALSE;
    }

    std::string sql = "DELETE FROM [" + std::string(storeName) + "] WHERE key=?";
    JS_FreeCString(ctx, storeName);

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) { JS_FreeCString(ctx, key); return JS_FALSE; }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    JS_FreeCString(ctx, key);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return JS_NewBool(ctx, rc == SQLITE_DONE);
}

// __brokit_idb_clear(dbName, storeName) → bool
static JSValue js_idb_clear(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_FALSE;

    const char* dbName = JS_ToCString(ctx, argv[0]);
    const char* storeName = JS_ToCString(ctx, argv[1]);

    if (!dbName || !storeName) {
        if (dbName) JS_FreeCString(ctx, dbName);
        if (storeName) JS_FreeCString(ctx, storeName);
        return JS_FALSE;
    }

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);

    if (!db) { JS_FreeCString(ctx, storeName); return JS_FALSE; }

    std::string sql = "DELETE FROM [" + std::string(storeName) + "]";
    JS_FreeCString(ctx, storeName);
    return JS_NewBool(ctx, sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
}

// __brokit_idb_get_all(dbName, storeName, limit?) → [[key, value], ...]
static JSValue js_idb_get_all(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_NewArray(ctx);

    const char* dbName = JS_ToCString(ctx, argv[0]);
    const char* storeName = JS_ToCString(ctx, argv[1]);

    if (!dbName || !storeName) {
        if (dbName) JS_FreeCString(ctx, dbName);
        if (storeName) JS_FreeCString(ctx, storeName);
        return JS_NewArray(ctx);
    }

    int limit = -1;
    if (argc >= 3 && !JS_IsUndefined(argv[2])) {
        JS_ToInt32(ctx, &limit, argv[2]);
    }

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);

    if (!db) { JS_FreeCString(ctx, storeName); return JS_NewArray(ctx); }

    std::string sql = "SELECT key, value FROM [" + std::string(storeName) + "] ORDER BY key";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    JS_FreeCString(ctx, storeName);

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return JS_NewArray(ctx);
    }

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        JSValue pair = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, pair, 0, key ? JS_NewString(ctx, key) : JS_NULL);
        JS_SetPropertyUint32(ctx, pair, 1, val ? JS_NewString(ctx, val) : JS_NULL);
        JS_SetPropertyUint32(ctx, arr, idx++, pair);
    }
    sqlite3_finalize(stmt);
    return arr;
}

// __brokit_idb_count(dbName, storeName) → int
static JSValue js_idb_count(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_NewInt32(ctx, 0);

    const char* dbName = JS_ToCString(ctx, argv[0]);
    const char* storeName = JS_ToCString(ctx, argv[1]);

    if (!dbName || !storeName) {
        if (dbName) JS_FreeCString(ctx, dbName);
        if (storeName) JS_FreeCString(ctx, storeName);
        return JS_NewInt32(ctx, 0);
    }

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);

    if (!db) { JS_FreeCString(ctx, storeName); return JS_NewInt32(ctx, 0); }

    std::string sql = "SELECT COUNT(*) FROM [" + std::string(storeName) + "]";
    JS_FreeCString(ctx, storeName);

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return JS_NewInt32(ctx, 0);
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return JS_NewInt32(ctx, count);
}

// __brokit_idb_store_names(dbName) → [name, ...]
static JSValue js_idb_store_names(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_NewArray(ctx);

    const char* dbName = JS_ToCString(ctx, argv[0]);
    if (!dbName) return JS_NewArray(ctx);

    sqlite3* db = openDb(ctx, std::string(dbName));
    JS_FreeCString(ctx, dbName);
    if (!db) return JS_NewArray(ctx);

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE '__idb_%'",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return JS_NewArray(ctx);
    }

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (name) JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, name));
    }
    sqlite3_finalize(stmt);
    return arr;
}

// __brokit_idb_delete_db(name) → bool
static JSValue js_idb_delete_db(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_FALSE;

    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    std::string dbName(name);
    JS_FreeCString(ctx, name);

    // Close if open
    auto* state = getIdbState(ctx);
    if (state) {
        auto it = state->dbs.find(dbName);
        if (it != state->dbs.end()) {
            sqlite3_close(it->second);
            state->dbs.erase(it);
        }
    }

    // Delete the file
    std::string path = dbPath(ctx, dbName);
    remove(path.c_str());
    // Also remove WAL and SHM files
    remove((path + "-wal").c_str());
    remove((path + "-shm").c_str());

    return JS_TRUE;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installIndexedDB(JSContext* ctx)
{
    auto* state = new IdbState();

    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, kIdbStateKey,
                      JS_NewInt64(ctx, static_cast<int64_t>(
                          reinterpret_cast<intptr_t>(state))));

    // Default base path — current directory
    JS_SetPropertyStr(ctx, global, kIdbBasePath, JS_NewString(ctx, "."));

    JS_SetPropertyStr(ctx, global, "__brokit_idb_open",
        JS_NewCFunction(ctx, js_idb_open, "__brokit_idb_open", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_create_store",
        JS_NewCFunction(ctx, js_idb_create_store, "__brokit_idb_create_store", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_delete_store",
        JS_NewCFunction(ctx, js_idb_delete_store, "__brokit_idb_delete_store", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_put",
        JS_NewCFunction(ctx, js_idb_put, "__brokit_idb_put", 4));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_get",
        JS_NewCFunction(ctx, js_idb_get, "__brokit_idb_get", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_delete",
        JS_NewCFunction(ctx, js_idb_delete, "__brokit_idb_delete", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_clear",
        JS_NewCFunction(ctx, js_idb_clear, "__brokit_idb_clear", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_get_all",
        JS_NewCFunction(ctx, js_idb_get_all, "__brokit_idb_get_all", 3));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_count",
        JS_NewCFunction(ctx, js_idb_count, "__brokit_idb_count", 2));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_store_names",
        JS_NewCFunction(ctx, js_idb_store_names, "__brokit_idb_store_names", 1));
    JS_SetPropertyStr(ctx, global, "__brokit_idb_delete_db",
        JS_NewCFunction(ctx, js_idb_delete_db, "__brokit_idb_delete_db", 1));

    JS_FreeValue(ctx, global);
}

void setIndexedDBPath(JSContext* ctx, const std::string& path)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kIdbBasePath, JS_NewString(ctx, path.c_str()));
    JS_FreeValue(ctx, global);
}

void cleanupIndexedDB(JSContext* ctx)
{
    auto* state = getIdbState(ctx);
    if (state) {
        for (auto& [name, db] : state->dbs) {
            sqlite3_close(db);
        }
        delete state;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, kIdbStateKey, JS_UNDEFINED);
    JS_FreeValue(ctx, global);
}

} // namespace brokit::api
