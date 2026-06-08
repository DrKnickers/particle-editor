// FramePublisher implementation. See FramePublisher.h for the design.

#include "FramePublisher.h"

#include "AlphaCompositor.h"

#include <cstdio>

namespace host {

namespace {

// Standard base64 alphabet + encoder, identical to the one in
// AlphaCompositor.cpp's CaptureSnapshotPng. Inlined here to keep
// FramePublisher self-contained; it's 30 lines.
void Base64EncodeInto(const std::vector<uint8_t>& src, std::string& out)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.clear();
    out.reserve(((src.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < src.size(); i += 3)
    {
        const uint32_t v = (uint32_t(src[i]) << 16) |
                           (uint32_t(src[i + 1]) << 8) |
                           (uint32_t(src[i + 2]));
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >>  6) & 0x3F]);
        out.push_back(alphabet[ v        & 0x3F]);
    }
    if (i < src.size())
    {
        uint32_t v = uint32_t(src[i]) << 16;
        if (i + 1 < src.size()) v |= uint32_t(src[i + 1]) << 8;
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < src.size()) ? alphabet[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
}

} // namespace

FramePublisher::FramePublisher(AlphaCompositor* compositor, EmitFn emit, int quality)
    : m_compositor(compositor)
    , m_emit(std::move(emit))
    , m_quality(quality)
{
    // Phase 3 follow-up: opt the compositor into the per-frame
    // pre-stamp DIB cache. EncodeFrameJpeg in OnFrameComposited below
    // reads it; without this flip the cache stays empty in arch B
    // (layered popup) and OnFrameComposited would return false
    // forever in arch C. Owning the flip here keeps the
    // dependency localized to the class.
    if (m_compositor) m_compositor->SetPerFrameCacheEnabled(true);
}

bool FramePublisher::OnFrameComposited()
{
    if (!m_compositor || !m_emit) return false;

    int w = 0, h = 0;
    if (!m_compositor->EncodeFrameJpeg(m_quality, m_jpegBuf, w, h))
        return false;  // No frame yet (boot state, device-lost) — silent skip.

    m_lastW = w;
    m_lastH = h;
    m_frameId += 1;

    // Base64-encode straight into a scratch string. Reused per frame;
    // capacity stabilises after the first few frames.
    std::string b64;
    Base64EncodeInto(m_jpegBuf, b64);

    // Build the JSON envelope. base64 is ASCII-safe so no escaping
    // is needed beyond the wrapping quotes. Wire shape matches the
    // existing event format: `{"type":"evt","kind":"...","payload":{...}}`.
    std::string json;
    json.reserve(160 + b64.size());
    json += "{\"type\":\"evt\",\"kind\":\"viewport/frame-ready\","
            "\"payload\":{\"w\":";
    json += std::to_string(w);
    json += ",\"h\":";
    json += std::to_string(h);
    json += ",\"frameId\":";
    json += std::to_string(m_frameId);
    json += ",\"jpegBase64\":\"";
    json += b64;
    json += "\"}}";

    m_emit(json);

    // 1 Hz throttled diagnostic line. Caller decides what to do with it
    // (log to host.log, ignore, ...).
    if (m_log)
    {
        DWORD now = GetTickCount();
        if (now - m_lastLogTick > 1000)
        {
            m_lastLogTick = now;
            char buf[256];
            _snprintf_s(buf, _TRUNCATE,
                        "[ArchC] frame=%llu size=%dx%d jpegBytes=%zu b64Bytes=%zu q=%d",
                        static_cast<unsigned long long>(m_frameId),
                        w, h, m_jpegBuf.size(), b64.size(), m_quality);
            m_log(buf);
        }
    }

    return true;
}

} // namespace host
