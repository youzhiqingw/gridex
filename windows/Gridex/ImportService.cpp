#include <windows.h>
#include "Models/ImportService.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace DBModels
{
    // ── Helpers ──────────────────────────────────────────

    static std::wstring utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty()) return {};
        int sz = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
            static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring result(sz, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
            static_cast<int>(utf8.size()), &result[0], sz);
        return result;
    }

    static std::wstring trimWs(const std::wstring& s)
    {
        size_t start = s.find_first_not_of(L" \t\r\n");
        if (start == std::wstring::npos) return {};
        size_t end = s.find_last_not_of(L" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    // ── CSV Parser ──────────────────────────────────────

    // RFC 4180 compliant: handles quoted fields, embedded commas, newlines
    static std::vector<std::wstring> parseCsvLine(
        const std::wstring& content, size_t& pos)
    {
        std::vector<std::wstring> fields;
        std::wstring field;
        bool inQuote = false;

        while (pos < content.size())
        {
            wchar_t c = content[pos];

            if (inQuote)
            {
                if (c == L'"')
                {
                    // Peek next char for escaped quote ""
                    if (pos + 1 < content.size() && content[pos + 1] == L'"')
                    {
                        field += L'"';
                        pos += 2;
                    }
                    else
                    {
                        inQuote = false;
                        pos++;
                    }
                }
                else
                {
                    field += c;
                    pos++;
                }
            }
            else
            {
                if (c == L'"')
                {
                    inQuote = true;
                    pos++;
                }
                else if (c == L',')
                {
                    fields.push_back(field);
                    field.clear();
                    pos++;
                }
                else if (c == L'\n')
                {
                    pos++;
                    break; // End of line
                }
                else if (c == L'\r')
                {
                    pos++;
                    if (pos < content.size() && content[pos] == L'\n') pos++;
                    break; // End of line
                }
                else
                {
                    field += c;
                    pos++;
                }
            }
        }

        // Last field on line
        fields.push_back(field);
        return fields;
    }

    ImportResult ImportService::ParseCsv(const std::wstring& content)
    {
        ImportResult result;
        if (content.empty())
        {
            result.error = L"文件为空";
            return result;
        }

        // Skip UTF-8 BOM if present
        size_t pos = 0;
        if (content.size() >= 1 && content[0] == 0xFEFF) pos = 1;

        // Parse header row
        result.columnNames = parseCsvLine(content, pos);
        if (result.columnNames.empty())
        {
            result.error = L"未找到表头行";
            return result;
        }

        // Trim column names
        for (auto& col : result.columnNames)
            col = trimWs(col);

        // Parse data rows
        while (pos < content.size())
        {
            // Skip blank lines
            if (content[pos] == L'\r' || content[pos] == L'\n')
            {
                pos++;
                continue;
            }

            auto fields = parseCsvLine(content, pos);

            // Skip if all fields empty
            bool allEmpty = true;
            for (auto& f : fields)
                if (!f.empty()) { allEmpty = false; break; }
            if (allEmpty) continue;

            TableRow row;
            for (size_t i = 0; i < result.columnNames.size() && i < fields.size(); i++)
            {
                std::wstring val = trimWs(fields[i]);
                // Map empty CSV cells AND the literal text "NULL" to SQL NULL.
                // (Importing CSV is one of the rare places where the literal
                //  text "NULL" is conventionally treated as a null marker.)
                if (val.empty() || val == L"NULL")
                    row[result.columnNames[i]] = nullValue();
                else
                    row[result.columnNames[i]] = val;
            }
            result.rows.push_back(std::move(row));
        }

        result.totalParsed = static_cast<int>(result.rows.size());
        result.success = true;
        return result;
    }

    // ── JSON Parser ─────────────────────────────────────

    // Minimal JSON array-of-objects parser (no external deps)
    // Expects: [{"key":"val", ...}, ...]

    static std::wstring parseJsonString(const std::wstring& json, size_t& pos)
    {
        if (pos >= json.size() || json[pos] != L'"') return {};
        pos++; // skip opening quote
        std::wstring result;
        while (pos < json.size())
        {
            wchar_t c = json[pos];
            if (c == L'\\' && pos + 1 < json.size())
            {
                wchar_t next = json[pos + 1];
                if (next == L'"') { result += L'"'; pos += 2; }
                else if (next == L'\\') { result += L'\\'; pos += 2; }
                else if (next == L'n') { result += L'\n'; pos += 2; }
                else if (next == L't') { result += L'\t'; pos += 2; }
                else if (next == L'/') { result += L'/'; pos += 2; }
                else { result += c; pos++; }
            }
            else if (c == L'"')
            {
                pos++; // skip closing quote
                return result;
            }
            else
            {
                result += c;
                pos++;
            }
        }
        return result;
    }

    static void skipWhitespace(const std::wstring& json, size_t& pos)
    {
        while (pos < json.size() &&
               (json[pos] == L' ' || json[pos] == L'\t' ||
                json[pos] == L'\r' || json[pos] == L'\n'))
            pos++;
    }

    // Parse a JSON value (string, number, null, bool) → wstring cell content.
    // JSON null becomes the SQL NULL sentinel; everything else is the literal text.
    static std::wstring parseJsonValue(const std::wstring& json, size_t& pos)
    {
        skipWhitespace(json, pos);
        if (pos >= json.size()) return nullValue();

        wchar_t c = json[pos];
        if (c == L'"')
            return parseJsonString(json, pos);

        // null
        if (json.compare(pos, 4, L"null") == 0)
        {
            pos += 4;
            return nullValue();
        }
        // true/false
        if (json.compare(pos, 4, L"true") == 0)
        {
            pos += 4;
            return L"true";
        }
        if (json.compare(pos, 5, L"false") == 0)
        {
            pos += 5;
            return L"false";
        }
        // Number: read until delimiter
        std::wstring num;
        while (pos < json.size() &&
               json[pos] != L',' && json[pos] != L'}' &&
               json[pos] != L']' && json[pos] != L' ' &&
               json[pos] != L'\r' && json[pos] != L'\n')
        {
            num += json[pos++];
        }
        return num.empty() ? nullValue() : num;
    }

    ImportResult ImportService::ParseJson(const std::wstring& content)
    {
        ImportResult result;
        if (content.empty())
        {
            result.error = L"文件为空";
            return result;
        }

        size_t pos = 0;
        // Skip BOM
        if (content[0] == 0xFEFF) pos = 1;

        skipWhitespace(content, pos);
        if (pos >= content.size() || content[pos] != L'[')
        {
            result.error = L"期望以 '[' 开头的 JSON 数组";
            return result;
        }
        pos++; // skip '['

        // Collect unique column names in order
        std::vector<std::wstring> orderedCols;
        auto addCol = [&](const std::wstring& name) {
            for (auto& c : orderedCols)
                if (c == name) return;
            orderedCols.push_back(name);
        };

        // Parse objects
        while (pos < content.size())
        {
            skipWhitespace(content, pos);
            if (pos >= content.size() || content[pos] == L']') break;
            if (content[pos] == L',') { pos++; continue; }
            if (content[pos] != L'{')
            {
                result.error = L"位置 " + std::to_wstring(pos) + L" 处应为 '{'";
                return result;
            }
            pos++; // skip '{'

            TableRow row;
            while (pos < content.size())
            {
                skipWhitespace(content, pos);
                if (pos >= content.size() || content[pos] == L'}') { pos++; break; }
                if (content[pos] == L',') { pos++; continue; }

                std::wstring key = parseJsonString(content, pos);
                skipWhitespace(content, pos);
                if (pos < content.size() && content[pos] == L':') pos++;

                std::wstring val = parseJsonValue(content, pos);
                row[key] = val;
                addCol(key);
            }
            result.rows.push_back(std::move(row));
        }

        result.columnNames = orderedCols;
        result.totalParsed = static_cast<int>(result.rows.size());
        result.success = true;
        return result;
    }

    // ── SQL Parser ──────────────────────────────────────

    ImportResult ImportService::ParseSql(const std::wstring& content)
    {
        ImportResult result;
        if (content.empty())
        {
            result.error = L"文件为空";
            return result;
        }

        // Split by semicolons, respecting single-quoted strings
        std::wstring stmt;
        bool inQuote = false;

        for (size_t i = 0; i < content.size(); i++)
        {
            wchar_t c = content[i];
            if (c == L'\'' && !inQuote)
            {
                inQuote = true;
                stmt += c;
            }
            else if (c == L'\'' && inQuote)
            {
                // Check escaped quote ''
                if (i + 1 < content.size() && content[i + 1] == L'\'')
                {
                    stmt += L"''";
                    i++;
                }
                else
                {
                    inQuote = false;
                    stmt += c;
                }
            }
            else if (c == L';' && !inQuote)
            {
                auto trimmed = trimWs(stmt);
                if (!trimmed.empty())
                    result.sqlStatements.push_back(trimmed + L";");
                stmt.clear();
            }
            else
            {
                stmt += c;
            }
        }

        // Last statement without trailing semicolon
        auto trimmed = trimWs(stmt);
        if (!trimmed.empty())
            result.sqlStatements.push_back(trimmed + L";");

        result.totalParsed = static_cast<int>(result.sqlStatements.size());
        result.success = (result.totalParsed > 0);
        if (!result.success)
            result.error = L"未找到 SQL 语句";
        return result;
    }

    // ── File I/O ────────────────────────────────────────

    std::wstring ImportService::ReadFileAsWstring(const std::wstring& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return {};

        std::string bytes(
            (std::istreambuf_iterator<char>(file)),
            std::istreambuf_iterator<char>());
        file.close();

        // Strip UTF-8 BOM
        if (bytes.size() >= 3 &&
            bytes[0] == '\xEF' && bytes[1] == '\xBB' && bytes[2] == '\xBF')
            bytes = bytes.substr(3);

        return utf8ToWide(bytes);
    }

    std::wstring ImportService::DetectFormat(const std::wstring& filename)
    {
        auto dotPos = filename.rfind(L'.');
        if (dotPos == std::wstring::npos) return L"";
        std::wstring ext = filename.substr(dotPos);
        // Lowercase
        for (auto& c : ext) c = towlower(c);
        if (ext == L".csv") return L"csv";
        if (ext == L".json") return L"json";
        if (ext == L".sql") return L"sql";
        return L"";
    }
}
