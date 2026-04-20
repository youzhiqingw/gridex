// MongoDBAdapter.cpp — connection lifecycle, schema inspection, server info.
//
// Query execution and CRUD operations are in MongoDBAdapterQuery.cpp
// to keep each file under 200 lines per project modularization rules.

// NOMINMAX prevents <windows.h> from defining min/max macros that collide
// with std::numeric_limits::max() used inside bsoncxx/mongocxx headers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "Models/MongoDBAdapter.h"

#include <mongocxx/instance.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/uri.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>

namespace DBModels
{
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    // ── Static instance (singleton, required by mongocxx) ─────
    static std::once_flag s_instanceFlag;
    static std::unique_ptr<mongocxx::instance> s_instance;

    void MongoDBAdapter::ensureInstance()
    {
        std::call_once(s_instanceFlag, []() {
            s_instance = std::make_unique<mongocxx::instance>();
        });
    }

    // ── ctor / dtor ───────────────────────────────────────────
    MongoDBAdapter::MongoDBAdapter()  { ensureInstance(); }
    MongoDBAdapter::~MongoDBAdapter() { disconnect(); }

    // ── UTF-8 helpers ─────────────────────────────────────────
    std::string MongoDBAdapter::toUtf8(const std::wstring& s)
    {
        if (s.empty()) return {};
        int sz = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(sz, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), &out[0], sz, nullptr, nullptr);
        return out;
    }

    std::wstring MongoDBAdapter::fromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), &out[0], sz);
        return out;
    }

    // ── URI builder ───────────────────────────────────────────
    std::string MongoDBAdapter::buildUriString(
        const ConnectionConfig& config, const std::wstring& password)
    {
        // If user provided a full URI, use it — but ensure it has a
        // /database path. libmongoc's URI parser stores the database
        // field as a raw C string that is NULL when no path segment is
        // present, and downstream code calls strlen(NULL) → crash.
        // Append "/test" (MongoDB's default DB) when the URI has no
        // path after the host:port authority section.
        if (!config.connectionUri.empty())
        {
            auto raw = toUtf8(config.connectionUri);

            // Find the authority end: after "mongodb://" or "mongodb+srv://",
            // skip past user:pass@host:port. The first '/' after the
            // authority is the database path. If there's no '/' after the
            // scheme's "://", the URI has no database → append "/test".
            // Also handle "...host:port?" (query params, no path).
            auto schemeEnd = raw.find("://");
            if (schemeEnd != std::string::npos)
            {
                auto afterScheme = schemeEnd + 3;  // skip "://"
                auto slashPos = raw.find('/', afterScheme);
                auto queryPos = raw.find('?', afterScheme);

                if (slashPos == std::string::npos)
                {
                    // No /database at all — insert "/test" before query
                    if (queryPos != std::string::npos)
                        raw.insert(queryPos, "/test");
                    else
                        raw += "/test";
                }
                else if (slashPos + 1 == raw.size() ||
                         slashPos + 1 == queryPos)
                {
                    // Trailing "/" with no database name (e.g. "...host:27017/")
                    raw.insert(slashPos + 1, "test");
                }
            }
            return raw;
        }

        // Build mongodb://user:pass@host:port/db
        // URL-encode user/pass so special chars (@, :, /, %) don't break the URI.
        auto urlEncode = [](const std::string& s) -> std::string {
            std::ostringstream enc;
            for (unsigned char c : s)
            {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    enc << c;
                else
                {
                    enc << '%' << std::uppercase;
                    enc << std::hex << std::setw(2) << std::setfill('0') << (int)c;
                }
            }
            return enc.str();
        };

        std::ostringstream uri;
        uri << "mongodb://";
        auto user = toUtf8(config.username);
        auto pass = toUtf8(password);
        if (!user.empty())
        {
            uri << urlEncode(user);
            if (!pass.empty()) uri << ":" << urlEncode(pass);
            uri << "@";
        }
        auto host = toUtf8(config.host.empty() ? L"127.0.0.1" : config.host);
        uri << host << ":" << (config.port > 0 ? config.port : 27017);

        // ALWAYS include a /database path in the URI. libmongoc's URI
        // parser stores the database field as a raw C string that may
        // be NULL when no path is present. Downstream code (e.g.
        // mongoc_uri_get_database) then passes NULL to strlen and
        // crashes with an access violation. Using "test" (MongoDB's
        // default database) avoids the null and matches the server's
        // own fallback when no database is specified.
        auto db = toUtf8(config.database);
        uri << "/" << (db.empty() ? "test" : db);

        // Append URI query options (authSource, replicaSet, tls, etc.)
        // entered in the "Options" form field. User types key=val pairs
        // separated by & — we prepend ? and attach.
        auto opts = toUtf8(config.mongoOptions);
        if (!opts.empty())
        {
            // Strip leading ? or & if user accidentally included it
            if (opts.front() == '?' || opts.front() == '&')
                opts = opts.substr(1);
            if (!opts.empty())
                uri << "?" << opts;
        }
        return uri.str();
    }

    // ── Connection lifecycle ──────────────────────────────────
    void MongoDBAdapter::connect(
        const ConnectionConfig& config, const std::wstring& password)
    {
        disconnect();
        lastConfig_ = config;
        lastPassword_ = password;

        try
        {
            auto uriStr = buildUriString(config, password);
            mongocxx::uri uri{uriStr};
            client_ = std::make_unique<mongocxx::client>(uri);

            // Determine database name from (in priority order):
            //   1. config.database form field
            //   2. URI path segment (parsed manually — mongocxx::uri::database()
            //      can return empty due to ABI mismatch in Debug builds)
            //   3. fallback "test"
            currentDbName_ = toUtf8(config.database);
            if (currentDbName_.empty())
            {
                // Manual parse: extract /database from URI string.
                // URI format: mongodb://[user:pass@]host[:port]/DATABASE[?opts]
                auto schemeEnd = uriStr.find("://");
                if (schemeEnd != std::string::npos)
                {
                    auto afterScheme = schemeEnd + 3;
                    auto slashPos = uriStr.find('/', afterScheme);
                    if (slashPos != std::string::npos && slashPos + 1 < uriStr.size())
                    {
                        auto dbStart = slashPos + 1;
                        auto dbEnd = uriStr.find('?', dbStart);
                        auto dbName = (dbEnd != std::string::npos)
                            ? uriStr.substr(dbStart, dbEnd - dbStart)
                            : uriStr.substr(dbStart);
                        if (!dbName.empty())
                            currentDbName_ = dbName;
                    }
                }
            }
            if (currentDbName_.empty())
                currentDbName_ = "test";

            // Ping to verify connection is alive
            auto db = (*client_)[currentDbName_];
            db.run_command(make_document(kvp("ping", 1)));
            connected_ = true;
        }
        catch (const std::exception& ex)
        {
            client_.reset();
            connected_ = false;
            throw DatabaseError(DatabaseError::Code::ConnectionFailed,
                std::string("MongoDB connection failed: ") + ex.what());
        }
    }

    void MongoDBAdapter::disconnect()
    {
        session_.reset();
        client_.reset();
        connected_ = false;
        schemaCache_.clear();
    }

    bool MongoDBAdapter::testConnection(
        const ConnectionConfig& config, const std::wstring& password)
    {
        try
        {
            auto uriStr = buildUriString(config, password);
            mongocxx::uri uri{uriStr};
            mongocxx::client tempClient{uri};

            auto dbName = toUtf8(config.database);
            if (dbName.empty())
            {
                // Manual parse DB from URI (same logic as connect)
                auto schemeEnd = uriStr.find("://");
                if (schemeEnd != std::string::npos)
                {
                    auto afterScheme = schemeEnd + 3;
                    auto slashPos = uriStr.find('/', afterScheme);
                    if (slashPos != std::string::npos && slashPos + 1 < uriStr.size())
                    {
                        auto dbStart = slashPos + 1;
                        auto dbEnd = uriStr.find('?', dbStart);
                        dbName = (dbEnd != std::string::npos)
                            ? uriStr.substr(dbStart, dbEnd - dbStart)
                            : uriStr.substr(dbStart);
                    }
                }
            }
            if (dbName.empty()) dbName = "test";

            auto db = tempClient[dbName];
            db.run_command(make_document(kvp("ping", 1)));
            return true;
        }
        catch (...) { return false; }
    }

    bool MongoDBAdapter::isConnected() const
    {
        return connected_ && client_ != nullptr;
    }

    // ── Helper: get database by schema name ───────────────────
    mongocxx::database MongoDBAdapter::getDatabase(const std::wstring& schema)
    {
        auto name = schema.empty() ? currentDbName_ : toUtf8(schema);
        return (*client_)[name];
    }

    // ── Schema Inspection ─────────────────────────────────────
    std::vector<std::wstring> MongoDBAdapter::listDatabases()
    {
        if (!isConnected())
            throw DatabaseError(DatabaseError::Code::ConnectionFailed, "Not connected");
        auto names = client_->list_database_names();
        std::vector<std::wstring> result;
        result.reserve(names.size());
        for (auto& n : names) result.push_back(fromUtf8(n));
        return result;
    }

    std::vector<std::wstring> MongoDBAdapter::listSchemas()
    {
        // MongoDB has no schema concept between database and collection.
        // Return the current database name as the single "schema".
        return { fromUtf8(currentDbName_) };
    }

    std::vector<TableInfo> MongoDBAdapter::listTables(const std::wstring& schema)
    {
        auto db = getDatabase(schema);
        auto names = db.list_collection_names();
        std::vector<TableInfo> result;
        for (auto& n : names)
        {
            TableInfo t;
            t.name = fromUtf8(n);
            t.type = L"collection";
            result.push_back(t);
        }
        // Sort alphabetically
        std::sort(result.begin(), result.end(),
            [](const TableInfo& a, const TableInfo& b) { return a.name < b.name; });
        return result;
    }

    std::vector<TableInfo> MongoDBAdapter::listViews(const std::wstring& schema)
    {
        // MongoDB views appear in list_collections with type "view"
        auto db = getDatabase(schema);
        auto filter = make_document(kvp("type", "view"));
        auto cursor = db.list_collections(filter.view());
        std::vector<TableInfo> result;
        for (auto& doc : cursor)
        {
            TableInfo t;
            auto nameEl = doc["name"];
            if (nameEl) t.name = fromUtf8(std::string(nameEl.get_string().value));
            t.type = L"view";
            result.push_back(t);
        }
        return result;
    }

    // ── BSON type name mapping ────────────────────────────────
    std::wstring MongoDBAdapter::bsonTypeName(int bsonType)
    {
        // BSON type codes: https://bsonspec.org/spec.html
        switch (bsonType)
        {
        case 1:  return L"双精度";
        case 2:  return L"字符串";
        case 3:  return L"对象";
        case 4:  return L"数组";
        case 5:  return L"二进制";
        case 7:  return L"对象 ID";
        case 8:  return L"布尔值";
        case 9:  return L"日期";
        case 10: return L"空值";
        case 16: return L"Int32";
        case 18: return L"Int64";
        case 19: return L"Decimal128";
        default: return L"未知";
        }
    }

    std::vector<ColumnInfo> MongoDBAdapter::sampleFields(
        const std::wstring& collection, const std::wstring& schema, int sampleSize)
    {
        auto db = getDatabase(schema);
        auto coll = db[toUtf8(collection)];

        // Sample documents to discover field names and types
        mongocxx::v_noabi::options::find opts;
        opts.limit(sampleSize);

        auto cursor = coll.find({}, opts);

        // Track field names in insertion order, and count BSON types per field
        std::vector<std::string> fieldOrder;
        std::set<std::string> fieldSeen;
        // field -> { bsonType -> count }
        std::unordered_map<std::string, std::unordered_map<int, int>> fieldTypeCounts;

        for (auto& doc : cursor)
        {
            for (auto& elem : doc)
            {
                std::string key(elem.key());
                if (fieldSeen.insert(key).second)
                    fieldOrder.push_back(key);
                int btype = static_cast<int>(elem.type());
                fieldTypeCounts[key][btype]++;
            }
        }

        // Ensure _id is always first
        auto idIt = std::find(fieldOrder.begin(), fieldOrder.end(), "_id");
        if (idIt != fieldOrder.end() && idIt != fieldOrder.begin())
        {
            fieldOrder.erase(idIt);
            fieldOrder.insert(fieldOrder.begin(), "_id");
        }

        // Build ColumnInfo from sampled data
        std::vector<ColumnInfo> columns;
        for (auto& field : fieldOrder)
        {
            ColumnInfo col;
            col.name = fromUtf8(field);
            col.isPrimaryKey = (field == "_id");
            col.nullable = (field != "_id");

            // Pick the majority BSON type for this field
            auto& counts = fieldTypeCounts[field];
            int maxCount = 0;
            int majorType = 2; // default String
            for (auto& [btype, cnt] : counts)
            {
                if (cnt > maxCount) { maxCount = cnt; majorType = btype; }
            }
            col.dataType = bsonTypeName(majorType);
            columns.push_back(col);
        }

        return columns;
    }

    std::vector<ColumnInfo> MongoDBAdapter::describeTable(
        const std::wstring& table, const std::wstring& schema)
    {
        auto key = toUtf8(schema) + "." + toUtf8(table);
        auto it = schemaCache_.find(key);
        if (it != schemaCache_.end())
            return it->second;

        auto cols = sampleFields(table, schema);
        schemaCache_[key] = cols;
        return cols;
    }

    std::vector<IndexInfo> MongoDBAdapter::listIndexes(
        const std::wstring& table, const std::wstring& schema)
    {
        auto db = getDatabase(schema);
        auto coll = db[toUtf8(table)];
        auto cursor = coll.list_indexes();

        std::vector<IndexInfo> result;
        for (auto& doc : cursor)
        {
            IndexInfo idx;
            if (auto n = doc["name"]) idx.name = fromUtf8(std::string(n.get_string().value));
            idx.isUnique = false;
            if (auto u = doc["unique"]) idx.isUnique = u.get_bool().value;

            // Extract key fields (IndexInfo.columns is comma-separated wstring)
            if (auto keyDoc = doc["key"])
            {
                for (auto& k : keyDoc.get_document().value)
                {
                    if (!idx.columns.empty()) idx.columns += L",";
                    idx.columns += fromUtf8(std::string(k.key()));
                }
            }
            result.push_back(idx);
        }
        return result;
    }

    std::vector<ForeignKeyInfo> MongoDBAdapter::listForeignKeys(
        const std::wstring&, const std::wstring&)
    {
        return {};  // MongoDB has no foreign key constraints
    }

    std::vector<std::wstring> MongoDBAdapter::listFunctions(const std::wstring&)
    {
        return {};  // MongoDB has no stored functions in the SQL sense
    }

    std::wstring MongoDBAdapter::getFunctionSource(const std::wstring&, const std::wstring&)
    {
        return L"";
    }

    std::wstring MongoDBAdapter::getCreateTableSQL(
        const std::wstring& table, const std::wstring&)
    {
        // Return pseudo-DDL for MongoDB collection creation
        return L"db.createCollection(\"" + table + L"\")";
    }

    // ── Server Info ───────────────────────────────────────────
    std::wstring MongoDBAdapter::serverVersion()
    {
        if (!isConnected()) return L"";
        try
        {
            // Use buildInfo instead of serverStatus — doesn't require admin privileges
            auto db = (*client_)[currentDbName_];
            auto result = db.run_command(make_document(kvp("buildInfo", 1)));
            auto view = result.view();
            if (auto ver = view["version"])
                return fromUtf8(std::string(ver.get_string().value));
        }
        catch (...) {}
        return L"未知";
    }

    std::wstring MongoDBAdapter::currentDatabase()
    {
        return fromUtf8(currentDbName_);
    }
}
