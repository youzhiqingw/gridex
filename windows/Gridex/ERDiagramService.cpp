#include <windows.h>
#include "Models/ERDiagramService.h"
#include <atomic>
#include <fstream>
#include <sstream>
#include <set>
#include <algorithm>
#include <unordered_map>

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

    static std::wstring fromUtf8(const std::string& s)
    {
        if (s.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), &out[0], sz);
        return out;
    }

    // Replace anything not [A-Za-z0-9_] with '_'. Prefix t_ if starts with digit.
    std::wstring ERDiagramService::SanitizeIdentifier(const std::wstring& name)
    {
        std::wstring out;
        out.reserve(name.size());
        for (wchar_t c : name)
        {
            if ((c >= L'a' && c <= L'z') ||
                (c >= L'A' && c <= L'Z') ||
                (c >= L'0' && c <= L'9') ||
                c == L'_')
                out += c;
            else
                out += L'_';
        }
        if (out.empty()) out = L"_";
        if (out[0] >= L'0' && out[0] <= L'9') out = L"t_" + out;
        return out;
    }

    // d2 reserved keywords that conflict with column names like "width",
    // "height", "label". When a SQL column matches one of these, d2 thinks
    // we're setting an attribute and barfs ("non-integer width 'bigint'").
    // Wrap the identifier in double quotes to force literal interpretation.
    bool ERDiagramService::IsD2Reserved(const std::wstring& name)
    {
        // Lowercase compare since SQL is case-insensitive but d2 is not
        std::wstring lower;
        lower.reserve(name.size());
        for (wchar_t c : name)
            lower += static_cast<wchar_t>(towlower(c));

        static const wchar_t* keywords[] = {
            L"shape", L"width", L"height", L"style", L"near", L"direction",
            L"top", L"left", L"tooltip", L"link", L"icon", L"label",
            L"constraint", L"class", L"classes", L"vars", L"layers",
            L"scenarios", L"steps", L"source-arrowhead", L"target-arrowhead",
            L"grid-rows", L"grid-columns", L"grid-gap",
            L"vertical-gap", L"horizontal-gap"
        };
        for (auto* kw : keywords)
            if (lower == kw) return true;
        return false;
    }

    // Quote a column identifier if it collides with a d2 reserved keyword.
    std::wstring ERDiagramService::QuoteIfReserved(const std::wstring& name)
    {
        if (IsD2Reserved(name))
            return L"\"" + name + L"\"";
        return name;
    }

    // Escape a string for use as a D2 quoted label. Wraps in "…" and escapes
    // embedded quotes, backslashes, and newlines so labels with pk values
    // like O'Brien, paths, or multi-line text don't break the d2 parser.
    std::wstring ERDiagramService::EscapeD2Label(const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size() + 2);
        out += L'"';
        for (wchar_t c : text)
        {
            switch (c)
            {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': break;  // strip
            default:    out += c;       break;
            }
        }
        out += L'"';
        return out;
    }

    // Simplify type strings: "varchar(255)" -> "varchar",
    // "character varying" -> "varchar", strip parens / spaces / casts.
    static std::wstring simplifyType(const std::wstring& dataType)
    {
        if (dataType.empty()) return L"any";

        // Lowercase
        std::wstring t;
        t.reserve(dataType.size());
        for (wchar_t c : dataType) t += static_cast<wchar_t>(towlower(c));

        // Strip parens content: "varchar(255)" -> "varchar"
        auto parenPos = t.find(L'(');
        if (parenPos != std::wstring::npos) t = t.substr(0, parenPos);

        // Strip ::cast: "'foo'::text" not really a type, but defensive
        auto castPos = t.find(L"::");
        if (castPos != std::wstring::npos) t = t.substr(0, castPos);

        // Trim
        while (!t.empty() && t.back() == L' ') t.pop_back();
        while (!t.empty() && t.front() == L' ') t.erase(t.begin());

        // Common rewrites
        if (t == L"character varying") return L"varchar";
        if (t == L"timestamp with time zone") return L"timestamptz";
        if (t == L"timestamp without time zone") return L"timestamp";
        if (t == L"double precision") return L"double";

        // If still has spaces, take first word
        auto sp = t.find(L' ');
        if (sp != std::wstring::npos) t = t.substr(0, sp);

        return t.empty() ? L"any" : t;
    }

    // Locate d2.exe — try MSIX install dir first, then exe-relative dir
    std::wstring ERDiagramService::LocateD2Exe()
    {
        // Try via current process module path (works for both packaged and unpackaged)
        wchar_t buf[MAX_PATH] = {};
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
        {
            std::wstring exePath(buf);
            auto slashPos = exePath.find_last_of(L"\\/");
            if (slashPos != std::wstring::npos)
            {
                std::wstring exeDir = exePath.substr(0, slashPos);
                std::wstring candidate = exeDir + L"\\Assets\\d2\\d2.exe";

                // Check existence
                DWORD attrs = GetFileAttributesW(candidate.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES &&
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                    return candidate;
            }
        }
        return L"";
    }

    // Build %TEMP%\gridex\<prefix><tick>_<counter>.<extension>.
    // Counter is a process-wide atomic so concurrent callers (ER diagram +
    // row graph) don't collide on filename. Prefix defaults to "er_" to
    // preserve prior behavior; distinct prefixes namespace filenames.
    std::wstring ERDiagramService::TempPath(const std::wstring& extension,
                                            const std::wstring& prefix)
    {
        wchar_t tmp[MAX_PATH] = {};
        DWORD len = GetTempPathW(MAX_PATH, tmp);
        if (len == 0) return L"";

        std::wstring dir = std::wstring(tmp) + L"gridex";
        CreateDirectoryW(dir.c_str(), nullptr);

        static std::atomic<int> counter{0};
        wchar_t name[96];
        swprintf_s(name, 96, L"\\%ls%lu_%d.%ls",
            prefix.c_str(), GetTickCount(), counter++, extension.c_str());
        return dir + name;
    }

    // Run d2.exe with input/output paths. Returns exit code.
    int ERDiagramService::RunD2(const std::wstring& d2Exe,
        const std::wstring& inputPath, const std::wstring& outputPath,
        std::wstring& stderrOut)
    {
        // Build command line:
        //   "d2.exe" --layout=elk --theme=300 --bundle --no-xml-tag --pad=20 "in" "out"
        // --bundle inlines fonts/assets, --no-xml-tag drops the <?xml ?> prolog
        // since we inline the SVG into HTML body for WebView2 rendering.
        std::wstring cmd =
            L"\"" + d2Exe + L"\" "
            L"--layout=elk --theme=300 --bundle --no-xml-tag --pad=20 "
            L"\"" + inputPath + L"\" "
            L"\"" + outputPath + L"\"";

        // Pipe for stderr capture
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE rdErr = nullptr, wrErr = nullptr;
        if (!CreatePipe(&rdErr, &wrErr, &sa, 0))
            return -1;
        SetHandleInformation(rdErr, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdError = wrErr;
        si.hStdOutput = wrErr;
        si.hStdInput = nullptr;

        PROCESS_INFORMATION pi = {};
        std::wstring cmdMutable = cmd;  // CreateProcessW needs writable buffer

        BOOL ok = CreateProcessW(
            nullptr,
            &cmdMutable[0],
            nullptr, nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr, nullptr,
            &si, &pi);

        CloseHandle(wrErr);

        if (!ok)
        {
            CloseHandle(rdErr);
            stderrOut = L"无法启动 d2.exe：" +
                std::to_wstring(GetLastError());
            return -1;
        }

        // Read stderr until pipe closes
        std::string capturedErr;
        char readBuf[4096];
        DWORD bytesRead = 0;
        while (ReadFile(rdErr, readBuf, sizeof(readBuf), &bytesRead, nullptr)
               && bytesRead > 0)
        {
            capturedErr.append(readBuf, bytesRead);
        }
        CloseHandle(rdErr);

        // Wait up to 30s
        WaitForSingleObject(pi.hProcess, 30000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        stderrOut = fromUtf8(capturedErr);
        return static_cast<int>(exitCode);
    }

    // ── D2 Text Generation ─────────────────────────────

    std::wstring ERDiagramService::GenerateD2Text(
        std::shared_ptr<DatabaseAdapter> adapter,
        const std::wstring& schema,
        ProgressCallback progress)
    {
        auto report = [&](const std::wstring& msg) { if (progress) progress(msg); };

        if (!adapter || !adapter->isConnected()) return L"";

        std::wstringstream ss;
        ss << L"# Gridex ER Diagram for schema " << schema << L"\n";
        ss << L"direction: right\n\n";

        // Collect tables
        std::vector<TableInfo> tables;
        try { tables = adapter->listTables(schema); }
        catch (...) { return L""; }

        report(L"在 " + schema + L" 中找到 " + std::to_wstring(tables.size()) + L" 张表");

        // Build set of sanitized table names so we can skip orphan FKs
        std::set<std::wstring> tableNameSet;
        for (const auto& t : tables) tableNameSet.insert(t.name);

        // Emit tables with columns
        int idx = 0;
        for (const auto& tableInfo : tables)
        {
            const auto& tableName = tableInfo.name;
            idx++;
            report(L"[" + std::to_wstring(idx) + L"/" +
                std::to_wstring(tables.size()) + L"] " + tableName);

            std::vector<ColumnInfo> cols;
            try { cols = adapter->describeTable(tableName, schema); }
            catch (...) { continue; }

            ss << SanitizeIdentifier(tableName) << L": {\n";
            ss << L"  shape: sql_table\n";
            for (const auto& col : cols)
            {
                std::wstring colId = SanitizeIdentifier(col.name);
                std::wstring colKey = QuoteIfReserved(colId);
                std::wstring typeId = simplifyType(col.dataType);
                ss << L"  " << colKey << L": " << typeId;
                if (col.isPrimaryKey)
                    ss << L" {constraint: primary_key}";
                else if (col.isForeignKey)
                    ss << L" {constraint: foreign_key}";
                ss << L"\n";
            }
            ss << L"}\n\n";
        }

        // Emit FK relationships
        int relCount = 0;
        for (const auto& tableInfo : tables)
        {
            const auto& tableName = tableInfo.name;
            std::vector<ForeignKeyInfo> fks;
            try { fks = adapter->listForeignKeys(tableName, schema); }
            catch (...) { continue; }

            for (const auto& fk : fks)
            {
                // Skip FKs whose target is outside this schema
                if (tableNameSet.find(fk.referencedTable) == tableNameSet.end())
                    continue;

                // FK arrow without label — user prefers clean lines.
                // Quote column identifiers if they collide with d2 keywords.
                ss << SanitizeIdentifier(tableName) << L"."
                   << QuoteIfReserved(SanitizeIdentifier(fk.column))
                   << L" -> "
                   << SanitizeIdentifier(fk.referencedTable) << L"."
                   << QuoteIfReserved(SanitizeIdentifier(fk.referencedColumn))
                   << L"\n";
                relCount++;
            }
        }

        report(L"已生成 " + std::to_wstring(tables.size()) +
            L" 张表、" + std::to_wstring(relCount) + L" 条关系");

        return ss.str();
    }

    // ── JSON Generation (for the native WebView renderer) ─────────
    //
    // Walks the same schema as GenerateD2Text but emits a compact JSON
    // document consumed by the dagre + svg-pan-zoom front end. No d2.exe
    // or subprocess involved.

    namespace
    {
        // JSON string escape shared by this whole emitter. Escapes the
        // standard control chars + <, > so the JSON can be inlined into
        // a <script type="application/json"> without a stray </script>.
        std::wstring jsonEsc(const std::wstring& s)
        {
            std::wstring out;
            out.reserve(s.size() + 2);
            for (wchar_t c : s)
            {
                switch (c)
                {
                case L'"':  out += L"\\\""; break;
                case L'\\': out += L"\\\\"; break;
                case L'\b': out += L"\\b";  break;
                case L'\f': out += L"\\f";  break;
                case L'\n': out += L"\\n";  break;
                case L'\r': out += L"\\r";  break;
                case L'\t': out += L"\\t";  break;
                case L'<':  out += L"\\u003c"; break;
                case L'>':  out += L"\\u003e"; break;
                default:
                    if (c < 0x20)
                    {
                        wchar_t buf[8];
                        swprintf_s(buf, 8, L"\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else out += c;
                    break;
                }
            }
            return out;
        }

        void emitJsonStr(std::wstringstream& o, const std::wstring& k,
                         const std::wstring& v, bool last)
        {
            o << L"\"" << k << L"\":\"" << jsonEsc(v) << L"\"";
            if (!last) o << L",";
        }
        void emitJsonBool(std::wstringstream& o, const std::wstring& k,
                          bool v, bool last)
        {
            o << L"\"" << k << L"\":" << (v ? L"true" : L"false");
            if (!last) o << L",";
        }
    }

    ERDiagramResult ERDiagramService::GenerateJson(
        std::shared_ptr<DatabaseAdapter> adapter,
        const std::wstring& schema,
        ProgressCallback progress)
    {
        auto report = [&](const std::wstring& msg) { if (progress) progress(msg); };
        ERDiagramResult result;

        if (!adapter || !adapter->isConnected())
        {
            result.error = L"未连接";
            return result;
        }

        std::vector<TableInfo> tables;
        try { tables = adapter->listTables(schema); }
        catch (...)
        {
            result.error = L"无法列出表";
            return result;
        }

        report(L"在 " + schema + L" 中找到 " + std::to_wstring(tables.size()) + L" 张表");

        std::wstringstream ss;
        ss << L"{\"tables\":[";

        bool firstTable = true;
        int tableIdx = 0;
        // Track known table names for filtering orphan FK targets later.
        std::set<std::wstring> tableNames;
        for (const auto& t : tables) tableNames.insert(t.name);

        // Cache FK info per table so the edges pass below doesn't re-query.
        std::unordered_map<std::wstring, std::vector<ForeignKeyInfo>> fkCache;

        for (const auto& tableInfo : tables)
        {
            const auto& tableName = tableInfo.name;
            tableIdx++;
            report(L"[" + std::to_wstring(tableIdx) + L"/" +
                std::to_wstring(tables.size()) + L"] " + tableName);

            std::vector<ColumnInfo> cols;
            try { cols = adapter->describeTable(tableName, schema); }
            catch (...) { continue; }

            std::vector<ForeignKeyInfo> fks;
            try { fks = adapter->listForeignKeys(tableName, schema); }
            catch (...) {}
            fkCache[tableName] = fks;

            // Set of FK source columns so we can flag them on each column
            // entry (d2's equivalent was `constraint: foreign_key`).
            std::set<std::wstring> fkCols;
            for (const auto& fk : fks) fkCols.insert(fk.column);

            if (!firstTable) ss << L",";
            firstTable = false;

            ss << L"{";
            emitJsonStr(ss, L"name",   tableName, false);
            emitJsonStr(ss, L"schema", schema,    false);
            ss << L"\"columns\":[";
            bool firstCol = true;
            for (const auto& col : cols)
            {
                if (!firstCol) ss << L",";
                firstCol = false;
                ss << L"{";
                emitJsonStr(ss,  L"name",     col.name,                           false);
                emitJsonStr(ss,  L"type",     col.dataType,                       false);
                emitJsonBool(ss, L"isPk",     col.isPrimaryKey,                   false);
                emitJsonBool(ss, L"isFk",     col.isForeignKey || fkCols.count(col.name) > 0, false);
                emitJsonBool(ss, L"nullable", col.nullable,                       true);
                ss << L"}";
            }
            ss << L"]}";
        }

        ss << L"],\"edges\":[";
        bool firstEdge = true;
        int relCount = 0;
        for (const auto& t : tables)
        {
            auto it = fkCache.find(t.name);
            if (it == fkCache.end()) continue;
            for (const auto& fk : it->second)
            {
                if (tableNames.find(fk.referencedTable) == tableNames.end())
                    continue;  // skip cross-schema / unresolved
                if (!firstEdge) ss << L",";
                firstEdge = false;
                ss << L"{";
                emitJsonStr(ss, L"fromTable",  t.name,              false);
                emitJsonStr(ss, L"fromColumn", fk.column,           false);
                emitJsonStr(ss, L"toTable",    fk.referencedTable,  false);
                emitJsonStr(ss, L"toColumn",   fk.referencedColumn, true);
                ss << L"}";
                relCount++;
            }
        }
        ss << L"]}";

        report(L"已生成 " + std::to_wstring(tables.size()) +
            L" 张表、" + std::to_wstring(relCount) + L" 条关系");
        result.jsonText = ss.str();
        result.tableCount = static_cast<int>(tables.size());
        result.relationshipCount = relCount;
        result.success = true;
        return result;
    }

    // ── Full Pipeline ───────────────────────────────────

    ERDiagramResult ERDiagramService::Generate(
        std::shared_ptr<DatabaseAdapter> adapter,
        const std::wstring& schema,
        ProgressCallback progress)
    {
        auto report = [&](const std::wstring& msg) { if (progress) progress(msg); };
        ERDiagramResult result;

        if (!adapter || !adapter->isConnected())
        {
            result.error = L"未连接";
            return result;
        }

        // 1. Generate D2 text
        result.d2Text = GenerateD2Text(adapter, schema, progress);
        if (result.d2Text.empty())
        {
            result.error = L"模式中未找到表";
            return result;
        }

        // Count tables and relationships from text by scanning
        // ("shape: sql_table" appears once per table, "->" only in FK lines)
        const std::wstring tableMarker = L"shape: sql_table";
        const std::wstring arrow = L"->";
        size_t pos = 0;
        while ((pos = result.d2Text.find(tableMarker, pos)) != std::wstring::npos)
        {
            result.tableCount++;
            pos += tableMarker.size();
        }
        pos = 0;
        while ((pos = result.d2Text.find(arrow, pos)) != std::wstring::npos)
        {
            result.relationshipCount++;
            pos += arrow.size();
        }

        // 2. Locate d2.exe
        std::wstring d2Exe = LocateD2Exe();
        if (d2Exe.empty())
        {
            result.error = L"未找到 d2.exe。请将其放到 Assets\\d2\\d2.exe。";
            return result;
        }
        report(L"正在使用 d2.exe：" + d2Exe);

        // 3. Write D2 text to temp file (UTF-8)
        std::wstring inputPath = TempPath(L"d2");
        std::wstring outputPath = TempPath(L"svg");
        if (inputPath.empty() || outputPath.empty())
        {
            result.error = L"无法创建临时文件";
            return result;
        }

        {
            std::ofstream f(inputPath, std::ios::binary);
            if (!f.is_open())
            {
                result.error = L"无法写入 D2 输入文件";
                return result;
            }
            auto utf8 = toUtf8(result.d2Text);
            f.write(utf8.c_str(), utf8.size());
        }

        // 4. Run d2.exe
        report(L"正在运行 d2.exe...");
        std::wstring stderrOut;
        int exitCode = RunD2(d2Exe, inputPath, outputPath, stderrOut);

        if (exitCode != 0)
        {
            result.error = L"d2.exe 失败（退出码 " + std::to_wstring(exitCode) +
                L"）：\n" + stderrOut;
            DeleteFileW(inputPath.c_str());
            return result;
        }

        // 5. Verify output exists
        DWORD outAttrs = GetFileAttributesW(outputPath.c_str());
        if (outAttrs == INVALID_FILE_ATTRIBUTES ||
            (outAttrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            result.error = L"d2.exe 未生成 SVG 输出";
            DeleteFileW(inputPath.c_str());
            return result;
        }

        // SVG goes to WebView2 — no post-processing needed (browser renders text fine).
        result.svgPath = outputPath;
        result.success = true;
        report(L"SVG 已就绪：" + outputPath);

        // Keep input D2 file for debugging? Delete to be tidy.
        DeleteFileW(inputPath.c_str());

        return result;
    }
}
