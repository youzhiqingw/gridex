#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Windows.UI.h>
#include <winrt/Windows.System.h>
#include "DetailsPanel.h"
#if __has_include("DetailsPanel.g.cpp")
#include "DetailsPanel.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxm = winrt::Microsoft::UI::Xaml::Media;

    DetailsPanel::DetailsPanel()
    {
        InitializeComponent();

        // Wire chat events in code-behind (not XAML) to avoid IDL requirements
        this->Loaded([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            SendChatButton().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    std::wstring text(ChatInputBox().Text());
                    if (text.empty()) return;
                    ChatInputBox().Text(L"");
                    SendChatMessage(text);
                });

            ChatInputBox().KeyDown(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::Input::KeyRoutedEventArgs const& e)
                {
                    if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
                    {
                        std::wstring text(ChatInputBox().Text());
                        if (text.empty()) return;
                        ChatInputBox().Text(L"");
                        SendChatMessage(text);
                        e.Handled(true);
                    }
                });

            // "+" context picker: populate the list each time the flyout
            // opens so the table set stays in sync with the current DB.
            TablePickerFlyout().Opened(
                [this](winrt::Windows::Foundation::IInspectable const&,
                       winrt::Windows::Foundation::IInspectable const&)
                {
                    OpenTablePickerFlyout();
                });

            // Search filter: re-run OpenTablePickerFlyout so the ListView
            // reflects the current query (cheap — just rebuilds items).
            TablePickerSearch().TextChanged(
                [this](muxc::AutoSuggestBox const&,
                       muxc::AutoSuggestBoxTextChangedEventArgs const&)
                {
                    OpenTablePickerFlyout();
                });

            // Click a table row → add it as a context chip and close flyout.
            TablePickerList().IsItemClickEnabled(true);
            TablePickerList().ItemClick(
                [this](winrt::Windows::Foundation::IInspectable const&,
                       muxc::ItemClickEventArgs const& e)
                {
                    auto item = e.ClickedItem();
                    if (!item) return;
                    try
                    {
                        auto hs = winrt::unbox_value<winrt::hstring>(item);
                        std::wstring name(hs);
                        AddContextTable(name);
                        TablePickerFlyout().Hide();
                    }
                    catch (...) {}
                });
        });
    }

    // ── Details Tab ────────────────────────────────────

    void DetailsPanel::ShowRow(
        const std::vector<std::wstring>& columnNames,
        const DBModels::TableRow&        row)
    {
        currentColumns_ = columnNames;
        currentRow_ = row;
        EmptyState().Visibility(mux::Visibility::Collapsed);
        DetailsScroller().Visibility(mux::Visibility::Visible);
        RebuildFields(columnNames, row);
    }

    void DetailsPanel::ClearRow()
    {
        currentColumns_.clear();
        currentRow_.clear();
        DetailsContainer().Children().Clear();
        DetailsScroller().Visibility(mux::Visibility::Collapsed);
        EmptyState().Visibility(mux::Visibility::Visible);
    }

    void DetailsPanel::DetailsTab_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (!DetailsTab() || !AssistantTab()) return;
        DetailsTab().IsChecked(true);
        AssistantTab().IsChecked(false);
        DetailsContent().Visibility(mux::Visibility::Visible);
        AssistantContent().Visibility(mux::Visibility::Collapsed);
    }

    void DetailsPanel::AssistantTab_Click(
        winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
    {
        if (!DetailsTab() || !AssistantTab()) return;
        DetailsTab().IsChecked(false);
        AssistantTab().IsChecked(true);
        DetailsContent().Visibility(mux::Visibility::Collapsed);
        AssistantContent().Visibility(mux::Visibility::Visible);
    }

    void DetailsPanel::FieldSearch_TextChanged(
        muxc::AutoSuggestBox const& sender,
        muxc::AutoSuggestBoxTextChangedEventArgs const&)
    {
        if (currentColumns_.empty()) return;

        std::wstring query(sender.Text());
        if (query.empty())
        {
            RebuildFields(currentColumns_, currentRow_);
            return;
        }

        std::wstring queryLower = query;
        std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::towlower);

        std::vector<std::wstring> filtered;
        for (auto& col : currentColumns_)
        {
            std::wstring colLower = col;
            std::transform(colLower.begin(), colLower.end(), colLower.begin(), ::towlower);

            std::wstring val;
            auto it = currentRow_.find(col);
            if (it != currentRow_.end()) val = it->second;
            std::wstring valLower = val;
            std::transform(valLower.begin(), valLower.end(), valLower.begin(), ::towlower);

            if (colLower.find(queryLower) != std::wstring::npos ||
                valLower.find(queryLower) != std::wstring::npos)
            {
                filtered.push_back(col);
            }
        }
        RebuildFields(filtered, currentRow_);
    }

    void DetailsPanel::RebuildFields(
        const std::vector<std::wstring>& columnNames,
        const DBModels::TableRow&        row)
    {
        DetailsContainer().Children().Clear();
        for (auto& col : columnNames)
        {
            std::wstring val;
            auto it = row.find(col);
            if (it != row.end()) val = it->second;
            AddField(DetailsContainer(), col, val);
        }
    }

    void DetailsPanel::AddField(
        muxc::StackPanel const& container,
        const std::wstring& label,
        const std::wstring& value)
    {
        muxc::StackPanel field;
        field.Spacing(2.0);
        field.Padding(mux::Thickness{ 0.0, 6.0, 0.0, 0.0 });

        muxc::TextBlock labelText;
        labelText.Text(winrt::hstring(label));
        labelText.FontSize(10.0);
        labelText.Opacity(0.5);
        field.Children().Append(labelText);

        const bool isNull = DBModels::isNullCell(value);

        // Read-only mode (Redis connections): render as a selectable
        // TextBlock wrapped in the same separator layout. No edit handlers,
        // no TextBox, no OnFieldEdited callback. Keeps the read path while
        // removing the destructive SQL-style update path.
        if (readOnly_)
        {
            muxc::TextBlock valueText;
            valueText.Text(winrt::hstring(isNull ? L"NULL" : value));
            valueText.FontSize(12.0);
            valueText.TextWrapping(mux::TextWrapping::Wrap);
            valueText.IsTextSelectionEnabled(true);
            if (isNull || value.empty())
                valueText.Opacity(0.4);
            field.Children().Append(valueText);

            muxc::Border sepRo;
            sepRo.Height(1.0);
            sepRo.Margin(mux::Thickness{ 0.0, 6.0, 0.0, 0.0 });
            sepRo.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(20, 128, 128, 128)));
            field.Children().Append(sepRo);

            container.Children().Append(field);
            return;
        }

        // Render NULL sentinel as visible empty TextBox with dim opacity.
        // User typing into it sets a real string value; clearing back to empty
        // round-trips to NULL.
        muxc::TextBox valueBox;
        valueBox.Text(winrt::hstring(isNull ? L"" : value));
        valueBox.FontSize(12.0);
        valueBox.TextWrapping(mux::TextWrapping::Wrap);
        valueBox.BorderThickness(mux::Thickness{ 0,0,0,1 });
        valueBox.BorderBrush(muxm::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(30, 128, 128, 128)));
        valueBox.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        if (isNull || value.empty())
            valueBox.Opacity(0.4);

        // Edit support: highlight on change, save on Enter or focus lost
        std::wstring colName = label;
        std::wstring oldVal = value;

        valueBox.GotFocus([](winrt::Windows::Foundation::IInspectable const& sender,
                             mux::RoutedEventArgs const&)
        {
            auto tb = sender.try_as<muxc::TextBox>();
            if (tb)
            {
                tb.Opacity(1.0);
                // Blue border to indicate editable/focused
                tb.BorderBrush(muxm::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(180, 0, 120, 212)));
            }
        });

        // Helper lambda to commit the edit
        auto commitEdit = [this, colName, oldVal](muxc::TextBox const& tb)
        {
            if (!tb) return;
            std::wstring newVal(tb.Text());
            // Empty input -> SQL NULL sentinel
            if (newVal.empty()) newVal = DBModels::nullValue();

            if (DBModels::isNullCell(newVal)) tb.Opacity(0.4);

            // Reset border
            tb.BorderBrush(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(30, 128, 128, 128)));

            if (newVal != oldVal)
            {
                // Orange border to show "modified"
                tb.BorderBrush(muxm::SolidColorBrush(
                    winrt::Windows::UI::ColorHelper::FromArgb(200, 255, 165, 0)));

                currentRow_[colName] = newVal;
                if (OnFieldEdited)
                    OnFieldEdited(colName, oldVal, newVal);
            }
        };

        valueBox.KeyDown([commitEdit](winrt::Windows::Foundation::IInspectable const& sender,
                                      mux::Input::KeyRoutedEventArgs const& e)
        {
            if (e.Key() == winrt::Windows::System::VirtualKey::Enter)
            {
                auto tb = sender.try_as<muxc::TextBox>();
                if (tb) commitEdit(tb);
                e.Handled(true);
            }
        });

        valueBox.LostFocus([commitEdit](winrt::Windows::Foundation::IInspectable const& sender,
                                        mux::RoutedEventArgs const&)
        {
            auto tb = sender.try_as<muxc::TextBox>();
            if (tb) commitEdit(tb);
        });

        field.Children().Append(valueBox);

        muxc::Border sep;
        sep.Height(1.0);
        sep.Margin(mux::Thickness{ 0.0, 6.0, 0.0, 0.0 });
        sep.Background(muxm::SolidColorBrush(
            winrt::Windows::UI::ColorHelper::FromArgb(20, 128, 128, 128)));
        field.Children().Append(sep);

        container.Children().Append(field);
    }

    // ── Chat / Assistant Tab ───────────────────────────

    void DetailsPanel::SendChatMessage(const std::wstring& text)
    {
        // Show user message
        AddChatBubble(L"user", text);

        // Add to history
        chatHistory_.push_back({ L"user", text });

        // Show loading indicator
        AddChatBubble(L"system", L"正在思考...");

        // Build the effective schema context: base schemaContext_ plus the
        // DDL of any tables the user pinned via the "+" picker. Building
        // here (UI thread) keeps the background thread free of any XAML
        // or callback reentrancy.
        std::wstring effectiveSchema = schemaContext_;
        std::wstring pickedTablesCtx = BuildSelectedTablesContext();
        if (!pickedTablesCtx.empty())
        {
            if (!effectiveSchema.empty()) effectiveSchema += L"\n\n";
            effectiveSchema += pickedTablesCtx;
        }

        // Get dispatcher for UI thread callback
        auto dispatcher = this->DispatcherQueue();
        auto history = chatHistory_;
        auto schema = effectiveSchema;
        auto& aiSvc = aiService_;

        // Run AI call on background thread
        std::thread([this, dispatcher, history, schema, &aiSvc]()
        {
            std::wstring systemPrompt =
                L"你是一个有帮助的数据库助手。请协助用户处理 SQL 查询和数据库问题。";
            if (!schema.empty())
                systemPrompt += L"\n\n数据库模式：\n" + schema;

            auto response = aiSvc.SendChat(history, systemPrompt);

            // Marshal back to UI thread
            dispatcher.TryEnqueue([this, response]()
            {
                // Remove "Thinking..." bubble (last child)
                auto children = ChatMessages().Children();
                if (children.Size() > 0)
                    children.RemoveAtEnd();

                // Show response
                AddChatBubble(L"assistant", response);
                chatHistory_.push_back({ L"assistant", response });
            });
        }).detach();
    }

    void DetailsPanel::AddChatBubble(const std::wstring& role, const std::wstring& content)
    {
        bool isUser = (role == L"user");
        bool isSystem = (role == L"system");

        muxc::Border bubble;
        bubble.CornerRadius(mux::CornerRadius{ 8, 8, 8, 8 });
        bubble.Padding(mux::Thickness{ 10, 6, 10, 6 });
        bubble.Margin(mux::Thickness{ 0, 2, 0, 2 });
        bubble.MaxWidth(220.0);
        bubble.HorizontalAlignment(
            isUser ? mux::HorizontalAlignment::Right : mux::HorizontalAlignment::Left);

        if (isUser)
        {
            bubble.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(255, 0, 120, 212)));
        }
        else if (isSystem)
        {
            bubble.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(20, 128, 128, 128)));
        }
        else
        {
            bubble.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 128, 128, 128)));
        }

        muxc::TextBlock txt;
        txt.Text(winrt::hstring(content));
        txt.FontSize(12.0);
        txt.TextWrapping(mux::TextWrapping::Wrap);
        txt.IsTextSelectionEnabled(true);

        if (isUser)
            txt.Foreground(muxm::SolidColorBrush(winrt::Windows::UI::Colors::White()));
        if (isSystem)
            txt.Opacity(0.5);

        bubble.Child(txt);
        ChatMessages().Children().Append(bubble);
    }

    // ── Context table picker ───────────────────────────

    // Populate the flyout ListView from OnRequestTableList, optionally
    // filtered by the search box text. Already-selected tables are not
    // shown (no point picking them twice).
    void DetailsPanel::OpenTablePickerFlyout()
    {
        TablePickerList().Items().Clear();

        if (!OnRequestTableList) return;
        auto tables = OnRequestTableList();
        if (tables.empty()) return;

        std::wstring query(TablePickerSearch().Text());
        std::wstring queryLower = query;
        std::transform(queryLower.begin(), queryLower.end(), queryLower.begin(), ::towlower);

        for (auto& name : tables)
        {
            // Skip already-selected tables
            if (std::find(selectedContextTables_.begin(),
                          selectedContextTables_.end(), name) != selectedContextTables_.end())
                continue;

            if (!queryLower.empty())
            {
                std::wstring nameLower = name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
                if (nameLower.find(queryLower) == std::wstring::npos) continue;
            }

            // ListView items are plain strings — easier than wrapping in a
            // custom type. IPropertyValue unboxing in the click handler
            // retrieves the name back.
            TablePickerList().Items().Append(winrt::box_value(winrt::hstring(name)));
        }
    }

    void DetailsPanel::AddContextTable(const std::wstring& tableName)
    {
        if (tableName.empty()) return;
        // Dedupe
        if (std::find(selectedContextTables_.begin(),
                      selectedContextTables_.end(), tableName) != selectedContextTables_.end())
            return;
        selectedContextTables_.push_back(tableName);
        RebuildContextChips();
    }

    void DetailsPanel::RemoveContextTable(const std::wstring& tableName)
    {
        auto it = std::find(selectedContextTables_.begin(),
                            selectedContextTables_.end(), tableName);
        if (it == selectedContextTables_.end()) return;
        selectedContextTables_.erase(it);
        RebuildContextChips();
    }

    // Render the chips row. Empty list collapses the scroller entirely so
    // the input row sits flush against the divider.
    void DetailsPanel::RebuildContextChips()
    {
        auto container = ChatContextChips();
        container.Children().Clear();

        if (selectedContextTables_.empty())
        {
            ChatContextChipsScroller().Visibility(mux::Visibility::Collapsed);
            return;
        }
        ChatContextChipsScroller().Visibility(mux::Visibility::Visible);

        for (auto const& name : selectedContextTables_)
        {
            muxc::Border chip;
            chip.CornerRadius(mux::CornerRadius{ 10, 10, 10, 10 });
            chip.Padding(mux::Thickness{ 8, 2, 2, 2 });
            chip.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 0, 120, 212)));
            chip.BorderThickness(mux::Thickness{ 1, 1, 1, 1 });
            chip.BorderBrush(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(80, 0, 120, 212)));

            muxc::StackPanel sp;
            sp.Orientation(muxc::Orientation::Horizontal);
            sp.Spacing(4.0);
            sp.VerticalAlignment(mux::VerticalAlignment::Center);

            // Small table icon + name
            muxc::FontIcon tblIcon;
            tblIcon.Glyph(L"\xE8C1"); // table glyph
            tblIcon.FontSize(10.0);
            tblIcon.Opacity(0.7);
            sp.Children().Append(tblIcon);

            muxc::TextBlock lbl;
            lbl.Text(winrt::hstring(name));
            lbl.FontSize(11.0);
            lbl.VerticalAlignment(mux::VerticalAlignment::Center);
            sp.Children().Append(lbl);

            // Remove button (×). Captures name by value — no cycle because
            // the lambda doesn't capture the chip Border itself.
            muxc::Button closeBtn;
            closeBtn.Padding(mux::Thickness{ 0, 0, 0, 0 });
            closeBtn.Width(18.0);
            closeBtn.Height(18.0);
            closeBtn.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
            closeBtn.BorderThickness(mux::Thickness{ 0, 0, 0, 0 });
            muxc::FontIcon xIcon;
            xIcon.Glyph(L"\xE711");
            xIcon.FontSize(9.0);
            closeBtn.Content(xIcon);
            std::wstring capturedName = name;
            closeBtn.Click([this, capturedName](
                winrt::Windows::Foundation::IInspectable const&,
                mux::RoutedEventArgs const&)
            {
                RemoveContextTable(capturedName);
            });
            sp.Children().Append(closeBtn);

            chip.Child(sp);
            container.Children().Append(chip);
        }
    }

    // Concatenate the structure (DDL-like text) of every selected table
    // into one block for the system prompt. Empty when nothing is picked
    // or the host never wired OnFetchTableStructure.
    std::wstring DetailsPanel::BuildSelectedTablesContext() const
    {
        if (selectedContextTables_.empty() || !OnFetchTableStructure) return {};

        std::wstring out;
        for (auto const& name : selectedContextTables_)
        {
            std::wstring ddl = OnFetchTableStructure(name);
            if (ddl.empty()) continue;
            if (!out.empty()) out += L"\n\n";
            out += ddl;
        }
        return out;
    }
}
