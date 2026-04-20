#include <windows.h>
#include "Models/DumpRestoreService.h"
#include "Models/ImportService.h"
#include <fstream>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <algorithm>

namespace DBModels
{
    // ── Helpers ──────────────────────────────────────────

    static std::string toUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
            static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(sz, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
            static_cast<int>(w.size()), &out[0], sz, nullptr, nullptr);
        return out;
    }

    // Quote a SQL string literal: 'value with ''embedded'' quotes'
    static std::wstring quoteLiteral(const std::wstring& v)
    {
        std::wstring out;
        out.reserve(v.size() + 2);
        out += L'\'';
        for (wchar_t c : v)
        {
            if (c == L'\'') out += L"''";
            else out += c;
        }
        out += L'\'';
        return out;
    }

    static std::wstring quoteIdent(const std::wstring& name)
    {
        return L"\"" + name + L"\"";
    }

    static std::wstring nowTimestamp()
    {
        auto t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        wchar_t buf[64];
        wcsftime(buf, 64, L"%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }

    enum class ColTypeKind { String, Numeric, Boolean };

    // Map a database type string to a kind (numeric/boolean/string).
    static ColTypeKind classifyType(const std::wstring& dataType)
    {
        // Lowercase copy
        std::wstring t;
        t.reserve(dataType.size());
        for (wchar_t c : dataType) t += static_cast<wchar_t>(towlower(c));

        // Numeric types (PostgreSQL + MySQL + SQLite)
        if (t == L"integer" || t == L"int" || t == L"int4" || t == L"int8" ||
            t == L"int2"    || t == L"bigint" || t == L"smallint" ||
            t == L"tinyint" || t == L"mediumint" ||
            t == L"numeric" || t == L"decimal" || t == L"real" ||
            t == L"double precision" || t == L"double" || t == L"float" ||
            t == L"float4" || t == L"float8" || t == L"serial" ||
            t == L"bigserial" || t == L"smallserial")
            return ColTypeKind::Numeric;

        // Boolean
        if (t == L"boolean" || t == L"bool")
            return ColTypeKind::Boolean;

        return ColTypeKind::String;
    }

    // Format a single value based on its column type. SQL NULL sentinel
    // becomes literal "NULL" SQL keyword (no quotes); real strings get quoted.
    static std::wstring formatValue(const std::wstring& val, ColTypeKind kind)
    {
        if (isNullCell(val)) return L"NULL";

        switch (kind)
        {
        case ColTypeKind::Numeric:
            // Empty string for numeric -> NULL (defensive)
            if (val.empty()) return L"NULL";
            return val;  // raw, no quotes

        case ColTypeKind::Boolean:
            if (val == L"t" || val == L"true" || val == L"1" ||
                val == L"T" || val == L"TRUE")
                return L"true";
            if (val == L"f" || val == L"false" || val == L"0" ||
                val == L"F" || val == L"FALSE")
                return L"false";
            // Unknown boolean -> NULL (defensive)
            return val.empty() ? L"NULL" : quoteLiteral(val);

        case ColTypeKind::String:
        default:
            return quoteLiteral(val);
        }
    }

    // Returns true if statement has any executable SQL (non-comment, non-empty)
    // after stripping `-- line comments` (respecting single-quoted strings).
    static bool hasExecutableContent(const std::wstring& stmt)
    {
        std::wstring stripped;
        bool inLineComment = false;
        bool inSingleQuote = false;
        for (size_t i = 0; i < stmt.size(); i++)
        {
            wchar_t c = stmt[i];
            if (inLineComment)
            {
                if (c == L'\n') inLineComment = false;
                continue;
            }
            if (inSingleQuote)
            {
                stripped += c;
                if (c == L'\'')
                {
                    if (i + 1 < stmt.size() && stmt[i + 1] == L'\'')
                    {
                        stripped += L'\'';
                        i++;
                    }
                    else inSingleQuote = false;
                }
                continue;
            }
            if (c == L'\'')
            {
                inSingleQuote = true;
                stripped += c;
                continue;
            }
            if (c == L'-' && i + 1 < stmt.size() && stmt[i + 1] == L'-')
            {
                inLineComment = true;
                i++;
                continue;
            }
            stripped += c;
        }
        for (wchar_t c : stripped)
        {
            if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n' && c != L';')
                return true;
        }
        return false;
    }

    // Build a single INSERT statement for one row
    static std::wstring buildInsertSQL(
        const std::wstring& table,
        const std::vector<std::wstring>& columnNames,
        const std::vector<ColTypeKind>& colKinds,
        const TableRow& row)
    {
        std::wstring sql = L"INSERT INTO " + quoteIdent(table) + L" (";
        for (size_t i = 0; i < columnNames.size(); i++)
        {
            if (i > 0) sql += L", ";
            sql += quoteIdent(columnNames[i]);
        }
        sql += L") VALUES (";
        for (size_t i = 0; i < columnNames.size(); i++)
        {
            if (i > 0) sql += L", ";
            std::wstring val;
            auto it = row.find(columnNames[i]);
            if (it != row.end()) val = it->second;
            ColTypeKind kind = (i < colKinds.size()) ? colKinds[i] : ColTypeKind::String;
            sql += formatValue(val, kind);
        }
        sql += L");\n";
        return sql;
    }

    // Write a UTF-8 wstring chunk to an open ofstream
    static void writeUtf8(std::ofstream& file, const std::wstring& s)
    {
        if (s.empty()) return;
        auto utf8 = toUtf8(s);
        file.write(utf8.c_str(), utf8.size());
    }

    // ── Dump ────────────────────────────────────────────

    DumpResult DumpRestoreService::DumpDatabase(
        std::shared_ptr<DatabaseAdapter> adapter,
        DatabaseType dbType,
        const std::wstring& schema,
        const std::wstring& outputFile,
        int batchSize,
        ProgressCallback progress)
    {
        auto report = [&](const std::wstring& msg) { if (progress) progress(msg); };

        DumpResult result;
        if (!adapter || !adapter->isConnected())
        {
            result.error = L"未连接";
            report(L"[错误] 未连接");
            return result;
        }
        if (batchSize <= 0) batchSize = DEFAULT_BATCH_SIZE;

        std::ofstream file(outputFile, std::ios::binary);
        if (!file.is_open())
        {
            result.error = L"无法打开输出文件：" + outputFile;
            return result;
        }

        // UTF-8 BOM for editor compatibility
        file.write("\xEF\xBB\xBF", 3);

        // Adapter-specific syntax
        const bool isMySQL = (dbType == DatabaseType::MySQL);
        const bool isPostgreSQL = (dbType == DatabaseType::PostgreSQL);
        const std::wstring fkOff =
            isMySQL ? L"SET FOREIGN_KEY_CHECKS=0;\n"
                    : L"";  // PG: handled per-DROP via CASCADE
        const std::wstring fkOn =
            isMySQL ? L"SET FOREIGN_KEY_CHECKS=1;\n"
                    : L"";
        const std::wstring dropSuffix =
            isPostgreSQL ? L" CASCADE;\n" : L";\n";
        const std::wstring sep =
            L"-- ----------------------------------------\n";

        try
        {
            // Header
            std::wstring header =
                L"-- Gridex dump\n"
                L"-- Database: " + adapter->currentDatabase() + L"\n"
                L"-- Schema:   " + schema + L"\n"
                L"-- Generated: " + nowTimestamp() + L"\n"
                L"-- Batch size: " + std::to_wstring(batchSize) + L"\n\n";
            writeUtf8(file, header);

            // List tables
            auto tables = adapter->listTables(schema);
            report(L"在模式 '" + schema + L"' 中找到 " + std::to_wstring(tables.size()) + L" 张表");

            // Disable FK checks (MySQL only)
            if (!fkOff.empty())
            {
                writeUtf8(file, L"-- 还原期间禁用外键检查\n");
                writeUtf8(file, fkOff + L"\n");
            }

            int tableIdx = 0;
            for (const auto& tableInfo : tables)
            {
                const auto& tableName = tableInfo.name;
                tableIdx++;
                report(L"[" + std::to_wstring(tableIdx) + L"/" +
                    std::to_wstring(tables.size()) + L"] 正在导出表：" + tableName);

                writeUtf8(file, sep);
                writeUtf8(file, L"-- Table: " + tableName + L"\n");
                writeUtf8(file, sep);

                // Drop existing — CASCADE for PG to handle FK references
                writeUtf8(file, L"DROP TABLE IF EXISTS " +
                    quoteIdent(tableName) + dropSuffix);

                // CREATE TABLE
                std::wstring ddl = adapter->getCreateTableSQL(tableName, schema);
                if (ddl.empty())
                {
                    writeUtf8(file, L"-- 警告：无法为 " + tableName + L" 生成 DDL\n\n");
                    continue;
                }
                writeUtf8(file, ddl);
                if (!ddl.empty() && ddl.back() != L';') writeUtf8(file, L";");
                writeUtf8(file, L"\n\n");

                // Get column types so INSERTs use proper formatting
                // (numeric/boolean unquoted, others quoted as strings)
                auto colInfos = adapter->describeTable(tableName, schema);
                std::unordered_map<std::wstring, ColTypeKind> typeByName;
                for (const auto& ci : colInfos)
                    typeByName[ci.name] = classifyType(ci.dataType);

                // Stream rows in batches
                int offset = 0;
                int tableRowCount = 0;
                std::vector<ColTypeKind> colKinds;  // built from first batch
                while (true)
                {
                    auto batch = adapter->fetchRows(
                        tableName, schema, batchSize, offset, L"", true);
                    if (!batch.success || batch.rows.empty()) break;

                    if (colKinds.empty())
                    {
                        colKinds.reserve(batch.columnNames.size());
                        for (const auto& cn : batch.columnNames)
                        {
                            auto it = typeByName.find(cn);
                            colKinds.push_back(it != typeByName.end()
                                ? it->second : ColTypeKind::String);
                        }
                    }

                    for (const auto& row : batch.rows)
                    {
                        writeUtf8(file, buildInsertSQL(
                            tableName, batch.columnNames, colKinds, row));
                        tableRowCount++;
                        result.rowsExported++;
                    }

                    if (static_cast<int>(batch.rows.size()) < batchSize) break;
                    offset += batchSize;
                }

                writeUtf8(file, L"-- " + std::to_wstring(tableRowCount) + L" 行\n\n");
                result.tablesProcessed++;
                report(L"  -> " + std::to_wstring(tableRowCount) + L" 行");
            }

            // Re-enable FK checks (MySQL only)
            if (!fkOn.empty()) writeUtf8(file, fkOn);

            report(L"导出完成：" + std::to_wstring(result.tablesProcessed) +
                L" 张表，共 " + std::to_wstring(result.rowsExported) + L" 行");
        }
        catch (const DatabaseError& ex)
        {
            auto what = ex.what();
            int sz = MultiByteToWideChar(CP_UTF8, 0, what, -1, nullptr, 0);
            if (sz > 0)
            {
                result.error.resize(sz);
                MultiByteToWideChar(CP_UTF8, 0, what, -1, &result.error[0], sz);
                if (!result.error.empty() && result.error.back() == L'\0')
                    result.error.pop_back();
            }
            file.close();
            return result;
        }
        catch (...)
        {
            result.error = L"导出过程中出现未知错误";
            file.close();
            return result;
        }

        file.close();
        result.success = true;
        return result;
    }

    // ── Restore ─────────────────────────────────────────

    RestoreResult DumpRestoreService::RestoreDatabase(
        std::shared_ptr<DatabaseAdapter> adapter,
        const std::wstring& inputFile,
        ProgressCallback progress)
    {
        auto report = [&](const std::wstring& msg) { if (progress) progress(msg); };

        RestoreResult result;
        if (!adapter || !adapter->isConnected())
        {
            result.error = L"未连接";
            report(L"[错误] 未连接");
            return result;
        }

        // Read file
        report(L"正在读取文件：" + inputFile);
        std::wstring content = ImportService::ReadFileAsWstring(inputFile);
        if (content.empty())
        {
            result.error = L"无法读取或文件为空：" + inputFile;
            report(L"[错误] " + result.error);
            return result;
        }

        // Parse SQL statements (reuses existing import parser)
        report(L"正在解析 SQL 语句...");
        auto parsed = ImportService::ParseSql(content);
        if (!parsed.success)
        {
            result.error = parsed.error;
            report(L"[错误] 解析失败：" + parsed.error);
            return result;
        }
        report(L"已解析 " + std::to_wstring(parsed.sqlStatements.size()) + L" 条语句");

        // Helper to capture first failure context (statement preview + error msg)
        auto captureFailure = [&](const std::wstring& stmt, const std::wstring& errMsg)
        {
            if (!result.error.empty()) return;
            // Truncate statement preview for readability
            std::wstring preview = stmt;
            if (preview.size() > 250) preview = preview.substr(0, 250) + L"...";
            result.error = L"第 " + std::to_wstring(result.statementsExecuted + 1) +
                L" 条语句失败：\n\n" + preview + L"\n\n错误：" + errMsg;
        };

        try
        {
            adapter->beginTransaction();
            report(L"事务已开始");

            int idx = 0;
            int totalStmts = static_cast<int>(parsed.sqlStatements.size());
            for (const auto& stmt : parsed.sqlStatements)
            {
                idx++;
                // Skip statements that contain only comments or whitespace —
                // PostgreSQL returns PGRES_EMPTY_QUERY which our adapter
                // treats as a failure, aborting the whole transaction.
                if (!hasExecutableContent(stmt)) continue;

                // Log occasional progress (every 25 statements + first few)
                if (idx <= 5 || idx % 25 == 0 || idx == totalStmts)
                {
                    // Extract first ~60 chars for context
                    std::wstring preview;
                    for (wchar_t c : stmt)
                    {
                        if (c == L'\n' || c == L'\r') { if (!preview.empty()) break; continue; }
                        preview += c;
                        if (preview.size() >= 60) { preview += L"..."; break; }
                    }
                    report(L"[" + std::to_wstring(idx) + L"/" +
                        std::to_wstring(totalStmts) + L"] " + preview);
                }

                try
                {
                    auto r = adapter->execute(stmt);
                    if (r.success) result.statementsExecuted++;
                    else
                    {
                        result.statementsFailed++;
                        captureFailure(stmt, r.error);
                    }
                }
                catch (const DatabaseError& ex)
                {
                    result.statementsFailed++;
                    std::wstring errMsg;
                    auto what = ex.what();
                    int sz = MultiByteToWideChar(CP_UTF8, 0, what, -1, nullptr, 0);
                    if (sz > 0)
                    {
                        errMsg.resize(sz);
                        MultiByteToWideChar(CP_UTF8, 0, what, -1, &errMsg[0], sz);
                        if (!errMsg.empty() && errMsg.back() == L'\0') errMsg.pop_back();
                    }
                    captureFailure(stmt, errMsg);
                }
                catch (...)
                {
                    result.statementsFailed++;
                    captureFailure(stmt, L"Unknown exception");
                }
            }

            if (result.statementsFailed == 0)
            {
                adapter->commitTransaction();
                result.success = true;
                report(L"Committed. " + std::to_wstring(result.statementsExecuted) +
                    L" statements executed successfully.");
            }
            else
            {
                adapter->rollbackTransaction();
                report(L"Rolled back due to " +
                    std::to_wstring(result.statementsFailed) + L" failure(s).");
            }
        }
        catch (...)
        {
            try { adapter->rollbackTransaction(); } catch (...) {}
            if (result.error.empty()) result.error = L"Transaction error";
            report(L"[error] Transaction error");
        }

        return result;
    }
}
