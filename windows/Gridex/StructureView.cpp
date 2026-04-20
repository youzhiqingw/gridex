#include "pch.h"
#include "xaml-includes.h"
#include <winrt/Windows.UI.Text.h>
#include "StructureView.h"
#if __has_include("StructureView.g.cpp")
#include "StructureView.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
    namespace muxm = winrt::Microsoft::UI::Xaml::Media;

    StructureView::StructureView()
    {
        InitializeComponent();

        this->Loaded([this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
        {
            AddColumnBtn().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    // Add new column with defaults
                    DBModels::ColumnInfo newCol;
                    newCol.name = L"new_column";
                    newCol.dataType = L"text";
                    newCol.nullable = true;
                    newCol.ordinalPosition = static_cast<int>(columns_.size()) + 1;
                    columns_.push_back(newCol);

                    AlterOp op;
                    op.type = AlterOp::AddColumn;
                    op.column = L"new_column";
                    op.newValue = L"text";
                    TrackAlter(op);
                    Rebuild();
                });

            ApplyAlterBtn().Click(
                [this](winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
                {
                    if (pendingAlters_.empty()) return;
                    auto sqls = GenerateAlterSQL();
                    if (OnApplyAlter && !sqls.empty())
                        OnApplyAlter(sqls);
                    pendingAlters_.clear();
                    UpdatePendingUI();
                });
        });
    }

    void StructureView::SetTableContext(const std::wstring& table, const std::wstring& schema,
                                        DBModels::DatabaseType dbType)
    {
        tableName_ = table;
        schemaName_ = schema;
        dbType_ = dbType;
    }

    void StructureView::SetData(
        const std::vector<DBModels::ColumnInfo>&     columns,
        const std::vector<DBModels::IndexInfo>&      indexes,
        const std::vector<DBModels::ForeignKeyInfo>& foreignKeys,
        const std::vector<DBModels::ConstraintInfo>& constraints)
    {
        columns_     = columns;
        indexes_     = indexes;
        foreignKeys_ = foreignKeys;
        constraints_ = constraints;
        pendingAlters_.clear();
        UpdatePendingUI();
        Rebuild();
    }

    void StructureView::Rebuild()
    {
        StructureContainer().Children().Clear();

        // Columns section
        AddSectionHeader(StructureContainer(), L"列");
        for (int i = 0; i < static_cast<int>(columns_.size()); i++)
            AddColumnRow(StructureContainer(), columns_[i], i, i % 2 == 1);

        // Indexes section
        if (!indexes_.empty())
        {
            AddSectionHeader(StructureContainer(), L"索引");
            for (int i = 0; i < static_cast<int>(indexes_.size()); i++)
                AddIndexRow(StructureContainer(), indexes_[i], i % 2 == 1);
        }
    }

    void StructureView::AddSectionHeader(
        muxc::StackPanel const& container,
        const std::wstring& title)
    {
        muxc::TextBlock hdr;
        hdr.Text(winrt::hstring(title));
        hdr.FontSize(11.0);
        hdr.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        hdr.Padding(mux::Thickness{ 12.0, 10.0, 8.0, 4.0 });
        hdr.Opacity(0.6);
        container.Children().Append(hdr);
    }

    void StructureView::AddColumnRow(
        muxc::StackPanel const& container,
        const DBModels::ColumnInfo& col,
        int colIndex,
        bool isAlternate)
    {
        muxc::Grid row;
        row.MinHeight(32.0);
        row.Padding(mux::Thickness{ 12.0, 4.0, 8.0, 4.0 });

        auto bgColor = isAlternate
            ? winrt::Windows::UI::ColorHelper::FromArgb(8, 128, 128, 128)
            : winrt::Windows::UI::Colors::Transparent();
        row.Background(muxm::SolidColorBrush(bgColor));

        // Columns: name | type | nullable | badges | delete btn
        muxc::ColumnDefinition c1; c1.Width(mux::GridLengthHelper::FromPixels(150));
        muxc::ColumnDefinition c2; c2.Width(mux::GridLengthHelper::FromPixels(120));
        muxc::ColumnDefinition c3; c3.Width(mux::GridLengthHelper::FromPixels(70));
        muxc::ColumnDefinition c4; c4.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        muxc::ColumnDefinition c5; c5.Width(mux::GridLengthHelper::FromPixels(28));
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);
        row.ColumnDefinitions().Append(c3);
        row.ColumnDefinitions().Append(c4);
        row.ColumnDefinitions().Append(c5);

        // Name — editable TextBox
        muxc::TextBox nameBox;
        nameBox.Text(winrt::hstring(col.name));
        nameBox.FontSize(12.0);
        nameBox.BorderThickness(mux::Thickness{ 0,0,0,0 });
        nameBox.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        nameBox.Padding(mux::Thickness{ 2,0,2,0 });
        nameBox.VerticalAlignment(mux::VerticalAlignment::Center);
        if (col.isPrimaryKey)
            nameBox.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        muxc::Grid::SetColumn(nameBox, 0);

        std::wstring origName = col.name;
        nameBox.LostFocus([this, colIndex, origName]
            (winrt::Windows::Foundation::IInspectable const& sender, mux::RoutedEventArgs const&)
        {
            auto tb = sender.try_as<muxc::TextBox>();
            if (!tb) return;
            std::wstring newName(tb.Text());
            if (newName != origName && !newName.empty())
            {
                columns_[colIndex].name = newName;
                AlterOp op;
                op.type = AlterOp::RenameColumn;
                op.column = origName;
                op.newValue = newName;
                TrackAlter(op);
            }
        });
        row.Children().Append(nameBox);

        // Type — ComboBox with DB-specific types
        muxc::ComboBox typeCombo;
        typeCombo.FontSize(11.0);
        typeCombo.MinWidth(0.0);
        typeCombo.MinHeight(0.0);
        typeCombo.Padding(mux::Thickness{ 4,2,4,2 });
        typeCombo.BorderThickness(mux::Thickness{ 0,0,0,0 });
        typeCombo.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        typeCombo.VerticalAlignment(mux::VerticalAlignment::Center);
        typeCombo.IsEditable(true); // Allow custom types not in list
        muxc::Grid::SetColumn(typeCombo, 1);

        auto typeList = DBModels::ColumnTypesForDB(dbType_);
        int selectedIdx = -1;
        for (int t = 0; t < static_cast<int>(typeList.size()); t++)
        {
            muxc::ComboBoxItem item;
            item.Content(winrt::box_value(winrt::hstring(typeList[t])));
            item.FontSize(11.0);
            typeCombo.Items().Append(item);
            // Match current type (case-insensitive prefix match)
            std::wstring lower1 = col.dataType, lower2 = typeList[t];
            std::transform(lower1.begin(), lower1.end(), lower1.begin(), ::towlower);
            std::transform(lower2.begin(), lower2.end(), lower2.begin(), ::towlower);
            if (lower1 == lower2 || lower1.find(lower2) == 0)
                selectedIdx = t;
        }
        if (selectedIdx >= 0)
            typeCombo.SelectedIndex(selectedIdx);
        else
            typeCombo.Text(winrt::hstring(col.dataType)); // custom type not in list

        std::wstring origType = col.dataType;
        std::wstring colName = col.name;
        typeCombo.SelectionChanged(
            [this, colIndex, colName, origType](winrt::Windows::Foundation::IInspectable const& sender,
                                                muxc::SelectionChangedEventArgs const&)
        {
            auto combo = sender.try_as<muxc::ComboBox>();
            if (!combo) return;
            auto selected = combo.SelectedItem();
            if (!selected) return;
            auto item = selected.try_as<muxc::ComboBoxItem>();
            if (!item) return;
            std::wstring newType(winrt::unbox_value<winrt::hstring>(item.Content()));
            if (newType != origType && !newType.empty() && colIndex < static_cast<int>(columns_.size()))
            {
                columns_[colIndex].dataType = newType;
                AlterOp op;
                op.type = AlterOp::AlterType;
                op.column = colName;
                op.oldValue = origType;
                op.newValue = newType;
                TrackAlter(op);
            }
        });
        row.Children().Append(typeCombo);

        // Nullable — toggle
        muxc::ToggleSwitch nullToggle;
        nullToggle.IsOn(col.nullable);
        nullToggle.OnContent(winrt::box_value(winrt::hstring(L"null")));
        nullToggle.OffContent(winrt::box_value(winrt::hstring(L"req")));
        nullToggle.MinWidth(0.0);
        nullToggle.MinHeight(0.0);
        nullToggle.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(nullToggle, 2);

        nullToggle.Toggled([this, colIndex, colName]
            (winrt::Windows::Foundation::IInspectable const& sender, mux::RoutedEventArgs const&)
        {
            auto ts = sender.try_as<muxc::ToggleSwitch>();
            if (!ts) return;
            bool newNullable = ts.IsOn();
            columns_[colIndex].nullable = newNullable;
            AlterOp op;
            op.type = AlterOp::AlterNullable;
            op.column = colName;
            op.newValue = newNullable ? L"NULL" : L"NOT NULL";
            TrackAlter(op);
        });
        row.Children().Append(nullToggle);

        // Badges (PK / FK) — read-only
        muxc::StackPanel badges;
        badges.Orientation(muxc::Orientation::Horizontal);
        badges.Spacing(4.0);
        badges.VerticalAlignment(mux::VerticalAlignment::Center);

        if (col.isPrimaryKey)
        {
            muxc::Border pkBadge;
            pkBadge.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(3));
            pkBadge.Padding(mux::Thickness{ 4.0, 1.0, 4.0, 1.0 });
            pkBadge.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 0, 120, 212)));
            muxc::TextBlock pkText;
            pkText.Text(L"PK");
            pkText.FontSize(9.0);
            pkBadge.Child(pkText);
            badges.Children().Append(pkBadge);
        }
        if (col.isForeignKey)
        {
            muxc::Border fkBadge;
            fkBadge.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(3));
            fkBadge.Padding(mux::Thickness{ 4.0, 1.0, 4.0, 1.0 });
            fkBadge.Background(muxm::SolidColorBrush(
                winrt::Windows::UI::ColorHelper::FromArgb(40, 99, 153, 34)));
            muxc::TextBlock fkText;
            fkText.Text(L"FK");
            fkText.FontSize(9.0);
            fkBadge.Child(fkText);
            badges.Children().Append(fkBadge);
        }
        muxc::Grid::SetColumn(badges, 3);
        row.Children().Append(badges);

        // Delete column button
        muxc::Button delBtn;
        delBtn.Width(24.0);
        delBtn.Height(24.0);
        delBtn.Padding(mux::Thickness{ 0,0,0,0 });
        delBtn.Background(muxm::SolidColorBrush(winrt::Windows::UI::Colors::Transparent()));
        delBtn.BorderThickness(mux::Thickness{ 0,0,0,0 });
        delBtn.Opacity(0.4);
        delBtn.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::FontIcon delIcon;
        delIcon.Glyph(L"\xE74D");
        delIcon.FontSize(10.0);
        delBtn.Content(delIcon);
        muxc::Grid::SetColumn(delBtn, 4);

        // Don't allow deleting PK columns
        if (!col.isPrimaryKey)
        {
            delBtn.Click([this, colIndex, colName]
                (winrt::Windows::Foundation::IInspectable const&, mux::RoutedEventArgs const&)
            {
                if (colIndex < static_cast<int>(columns_.size()))
                {
                    AlterOp op;
                    op.type = AlterOp::DropColumn;
                    op.column = colName;
                    TrackAlter(op);
                    columns_.erase(columns_.begin() + colIndex);
                    Rebuild();
                }
            });
        }
        else
        {
            delBtn.IsEnabled(false);
            delBtn.Opacity(0.1);
        }
        row.Children().Append(delBtn);

        container.Children().Append(row);
    }

    void StructureView::AddIndexRow(
        muxc::StackPanel const& container,
        const DBModels::IndexInfo& idx,
        bool isAlternate)
    {
        muxc::Grid row;
        row.MinHeight(28.0);
        row.Padding(mux::Thickness{ 12.0, 4.0, 8.0, 4.0 });

        auto bgColor = isAlternate
            ? winrt::Windows::UI::ColorHelper::FromArgb(8, 128, 128, 128)
            : winrt::Windows::UI::Colors::Transparent();
        row.Background(muxm::SolidColorBrush(bgColor));

        muxc::ColumnDefinition c1; c1.Width(mux::GridLengthHelper::FromPixels(200));
        muxc::ColumnDefinition c2; c2.Width(mux::GridLengthHelper::FromPixels(120));
        muxc::ColumnDefinition c3; c3.Width(mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star));
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);
        row.ColumnDefinitions().Append(c3);

        muxc::TextBlock nameText;
        nameText.Text(winrt::hstring(idx.name));
        nameText.FontSize(12.0);
        nameText.VerticalAlignment(mux::VerticalAlignment::Center);
        nameText.TextTrimming(mux::TextTrimming::CharacterEllipsis);
        muxc::Grid::SetColumn(nameText, 0);
        row.Children().Append(nameText);

        muxc::TextBlock colText;
        colText.Text(winrt::hstring(idx.columns));
        colText.FontSize(11.0);
        colText.Opacity(0.7);
        colText.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(colText, 1);
        row.Children().Append(colText);

        std::wstring flags;
        if (idx.isPrimary) flags += L"PRIMARY  ";
        if (idx.isUnique)  flags += L"UNIQUE  ";
        flags += idx.algorithm;

        muxc::TextBlock flagText;
        flagText.Text(winrt::hstring(flags));
        flagText.FontSize(10.0);
        flagText.Opacity(0.5);
        flagText.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(flagText, 2);
        row.Children().Append(flagText);

        container.Children().Append(row);
    }

    // ── Alter tracking ─────────────────────────────────

    void StructureView::TrackAlter(const AlterOp& op)
    {
        // If renaming/retyping a column that was just added in this
        // batch (still pending, not yet on the server), fold the
        // change into the original AddColumn op instead of emitting
        // a separate ALTER/RENAME that references a column the server
        // doesn't know about yet.
        if (op.type == AlterOp::RenameColumn ||
            op.type == AlterOp::AlterType ||
            op.type == AlterOp::AlterNullable ||
            op.type == AlterOp::AlterDefault)
        {
            for (auto& pending : pendingAlters_)
            {
                if (pending.type == AlterOp::AddColumn && pending.column == op.column)
                {
                    if (op.type == AlterOp::RenameColumn)
                        pending.column = op.newValue;  // use new name in ADD COLUMN
                    else if (op.type == AlterOp::AlterType)
                        pending.newValue = op.newValue; // use new type in ADD COLUMN
                    // AlterNullable / AlterDefault on a pending ADD are
                    // edge cases — skip for now (ADD COLUMN doesn't carry
                    // NOT NULL / DEFAULT in our SQL template).
                    UpdatePendingUI();
                    return;
                }
            }
        }

        pendingAlters_.push_back(op);
        UpdatePendingUI();
    }

    void StructureView::UpdatePendingUI()
    {
        int count = static_cast<int>(pendingAlters_.size());
        if (count > 0)
        {
            PendingAlterText().Text(winrt::hstring(std::to_wstring(count) + L" change(s)"));
            ApplyAlterBtn().Visibility(mux::Visibility::Visible);
        }
        else
        {
            PendingAlterText().Text(L"");
            ApplyAlterBtn().Visibility(mux::Visibility::Collapsed);
        }
    }

    std::vector<std::wstring> StructureView::GenerateAlterSQL() const
    {
        std::vector<std::wstring> sqls;
        const bool isMSSQL = (dbType_ == DBModels::DatabaseType::MSSQLServer);
        const bool isMySQL = (dbType_ == DBModels::DatabaseType::MySQL);

        // Dialect-aware table + column quoting
        // MSSQL: [schema].[table], [column]
        // MySQL: `schema`.`table`, `column`
        // PostgreSQL/SQLite: "schema"."table", "column"
        std::wstring tbl;
        if (isMSSQL)
            tbl = L"[" + schemaName_ + L"].[" + tableName_ + L"]";
        else if (isMySQL)
            tbl = L"`" + schemaName_ + L"`.`" + tableName_ + L"`";
        else
            tbl = L"\"" + schemaName_ + L"\".\"" + tableName_ + L"\"";

        auto qCol = [&](const std::wstring& c) -> std::wstring {
            if (isMSSQL) return L"[" + c + L"]";
            if (isMySQL) return L"`" + c + L"`";
            return L"\"" + c + L"\"";
        };

        for (auto& op : pendingAlters_)
        {
            std::wstring sql;
            switch (op.type)
            {
            case AlterOp::AddColumn:
                // MSSQL: ALTER TABLE [t] ADD [col] type (no COLUMN keyword)
                // MySQL: ALTER TABLE `t` ADD COLUMN `col` type
                // PG:    ALTER TABLE "t" ADD COLUMN "col" type
                if (isMSSQL)
                    sql = L"ALTER TABLE " + tbl + L" ADD " + qCol(op.column) + L" " + op.newValue;
                else
                    sql = L"ALTER TABLE " + tbl + L" ADD COLUMN " + qCol(op.column) + L" " + op.newValue;
                break;

            case AlterOp::DropColumn:
                sql = L"ALTER TABLE " + tbl + L" DROP COLUMN " + qCol(op.column);
                break;

            case AlterOp::AlterType:
                if (isMSSQL)
                    // MSSQL: ALTER TABLE [t] ALTER COLUMN [col] newtype
                    sql = L"ALTER TABLE " + tbl + L" ALTER COLUMN " + qCol(op.column) + L" " + op.newValue;
                else if (isMySQL)
                    sql = L"ALTER TABLE " + tbl + L" MODIFY COLUMN " + qCol(op.column) + L" " + op.newValue;
                else
                    sql = L"ALTER TABLE " + tbl + L" ALTER COLUMN " + qCol(op.column) + L" TYPE " + op.newValue;
                break;

            case AlterOp::AlterNullable:
                if (isMSSQL)
                {
                    // MSSQL requires full column definition for ALTER COLUMN
                    // Just skip for now — complex to reconstruct full type+null
                }
                else if (op.newValue == L"NOT NULL")
                    sql = L"ALTER TABLE " + tbl + L" ALTER COLUMN " + qCol(op.column) + L" SET NOT NULL";
                else
                    sql = L"ALTER TABLE " + tbl + L" ALTER COLUMN " + qCol(op.column) + L" DROP NOT NULL";
                break;

            case AlterOp::AlterDefault:
                if (isMSSQL)
                    // MSSQL uses ADD DEFAULT ... FOR
                    sql = L"ALTER TABLE " + tbl + L" ADD DEFAULT " + op.newValue + L" FOR " + qCol(op.column);
                else
                    sql = L"ALTER TABLE " + tbl + L" ALTER COLUMN " + qCol(op.column) + L" SET DEFAULT " + op.newValue;
                break;

            case AlterOp::RenameColumn:
                if (isMSSQL)
                    // MSSQL uses sp_rename
                    sql = L"EXEC sp_rename '" + schemaName_ + L"." + tableName_ + L"." + op.column + L"', '" + op.newValue + L"', 'COLUMN'";
                else
                    sql = L"ALTER TABLE " + tbl + L" RENAME COLUMN " + qCol(op.column) + L" TO " + qCol(op.newValue);
                break;
            }
            if (!sql.empty()) sqls.push_back(sql);
        }
        return sqls;
    }
}
