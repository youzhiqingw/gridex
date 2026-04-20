#include "pch.h"
#include "xaml-includes.h"
#include "StatusBarControl.h"
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#if __has_include("StatusBarControl.g.cpp")
#include "StatusBarControl.g.cpp"
#endif

namespace winrt::Gridex::implementation
{
    namespace mux  = winrt::Microsoft::UI::Xaml;
    namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

    StatusBarControl::StatusBarControl()
    {
        InitializeComponent();
    }

    void StatusBarControl::SetStatus(
        const std::wstring& connection,
        const std::wstring& schema,
        int                 rowCount,
        double              queryTimeMs,
        double              renderTimeMs)
    {
        connection_   = connection;
        schema_       = schema;
        rowCount_     = rowCount;
        queryTimeMs_  = queryTimeMs;
        renderTimeMs_ = renderTimeMs;

        ConnectionText().Text(winrt::hstring(connection_));
        SchemaText().Text(winrt::hstring(schema_));

        std::wstring rowStr = std::to_wstring(rowCount_) + L" 行";
        RowCountText().Text(winrt::hstring(rowStr));

        // "执行 Nms · 渲染 Nms" — two-way split so a slow UI build on a
        // wide table does not get mis-attributed to a slow SQL query.
        std::wstring timeStr =
            L"执行 "   + std::to_wstring(static_cast<int>(queryTimeMs_))  + L"ms" +
            L"  \x00B7  " +
            L"渲染 " + std::to_wstring(static_cast<int>(renderTimeMs_)) + L"ms";
        QueryTimeText().Text(winrt::hstring(timeStr));

        // Tooltip clarifies what each number actually measures -- users
        // otherwise read "执行" as pure server execution when it really
        // includes the full driver round-trip and result transfer.
        muxc::ToolTip tip;
        tip.Content(winrt::box_value(winrt::hstring(
            L"执行 = 驱动阻塞调用：发送 SQL、服务器执行、接收所有结果字节。"
            L"在远程数据库和宽行结果下，主要成本来自网络传输。\n"
            L"渲染 = 构建可视网格的 UI 成本（每个单元格一个 StackPanel + TextBlock）。")));
        muxc::ToolTipService::SetToolTip(QueryTimeText(), tip);
    }
}
