#pragma once

namespace host
{

enum class ViewportUnavailableAction { ShowNotice, Exit };

// Missing engine pixels leave the editor chrome usable. Keep interactive
// editing/save available, but never produce automation results without a
// preview. The caller uses IsFullyInteractive() to include capture,
// drive/record and test-host, regardless of whether their HWND is visible.
inline ViewportUnavailableAction DecideViewportUnavailable(bool interactive)
{
    return interactive ? ViewportUnavailableAction::ShowNotice
                       : ViewportUnavailableAction::Exit;
}

} // namespace host
