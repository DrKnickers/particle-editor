#pragma once

inline bool ShouldShowWebViewInitErrorModal(bool captureMode, bool ephemeral)
{
    return !captureMode && !ephemeral;
}
