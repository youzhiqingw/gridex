#pragma once
#include <string>
#include <vector>

namespace DBModels
{
    // Persisted app settings — JSON file in %LOCALAPPDATA%\Gridex\settings.json
    struct AppSettings
    {
        // Appearance
        int themeIndex = 0;              // 0=System, 1=Light, 2=Dark

        // AI
        int aiProviderIndex = 0;         // 0=Anthropic, 1=OpenAI, 2=Ollama
        std::wstring anthropicEndpoint;
        std::wstring openaiEndpoint;
        std::wstring aiApiKey;
        std::wstring aiModel;
        std::wstring ollamaEndpoint;

        // Editor
        int editorFontSize = 13;
        int rowLimit = 100;

        // Navigation state (for back button)
        std::wstring lastPageBeforeSettings;  // e.g. "Gridex.WorkspacePage"

        // Connection groups (user-defined labels)
        std::vector<std::wstring> connectionGroups;

        // Load from file (returns default if file missing)
        static AppSettings Load();

        // Save to file
        bool Save() const;

    private:
        static std::wstring GetSettingsPath();
    };
}
