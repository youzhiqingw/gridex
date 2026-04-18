#pragma once
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include "DatabaseAdapter.h"
#include "ConnectionConfig.h"
#include "SSHTunnelService.h"

namespace DBModels
{
    // Query log entry for the console log panel
    struct QueryLogEntry
    {
        std::wstring sql;
        std::wstring status;    // "success", "error"
        int rowCount = 0;
        double durationMs = 0.0;        // Exec — driver blocking call wall-clock
        double renderDurationMs = 0.0;  // Render — UI grid build (0 if no UI render)
        std::chrono::system_clock::time_point timestamp;
        std::wstring error;
    };

    class ConnectionManager
    {
    public:
        // Connect to a database, returns the adapter
        std::shared_ptr<DatabaseAdapter> connect(
            const ConnectionConfig& config,
            const std::wstring& password);

        // Disconnect current connection
        void disconnect();

        // Test a connection without keeping it open
        bool testConnection(
            const ConnectionConfig& config,
            const std::wstring& password);

        // Same as testConnection but surfaces the underlying adapter
        // error message (libpq / libmariadb / sqlite3 text) so UI can
        // show the real reason instead of a generic "check host/port"
        // line. outError stays empty when the call returns true.
        bool testConnectionWithError(
            const ConnectionConfig& config,
            const std::wstring& password,
            std::wstring& outError);

        // Get the active adapter (nullptr if not connected)
        std::shared_ptr<DatabaseAdapter> getActiveAdapter() const { return activeAdapter_; }

        // Check if connected
        bool isConnected() const { return activeAdapter_ && activeAdapter_->isConnected(); }

        // Get current connection config
        const ConnectionConfig& getConfig() const { return activeConfig_; }

        // Query log
        const std::vector<QueryLogEntry>& getQueryLog() const { return queryLog_; }
        void addQueryLog(const QueryLogEntry& entry) { queryLog_.push_back(entry); }
        void clearQueryLog() { queryLog_.clear(); }

    private:
        std::shared_ptr<DatabaseAdapter> activeAdapter_;
        ConnectionConfig activeConfig_;
        std::vector<QueryLogEntry> queryLog_;
        std::unique_ptr<SSHTunnelService> sshTunnel_;

        // Factory: create adapter based on database type
        std::shared_ptr<DatabaseAdapter> createAdapter(DatabaseType type);
    };
}
