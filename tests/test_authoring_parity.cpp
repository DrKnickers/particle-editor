// Authoring-parity oracle for the emitters/* track handlers
// (src/host/BridgeDispatch_EmitterTracks.cpp) — Phase 4A PR 2.
//
// Phase 4A moves those handlers onto the shared rules in
// src/effect/ParticleAuthoring (PR 3). Document state alone cannot catch the
// ways that refactor can go wrong, so every edit here records the handler's
// full behavioral fingerprint:
//   - the wire response (verbatim JSON, or a thrown exception);
//   - every call it makes, in order: captureUndo (with its coalesce key and a
//     hash of the state AT CAPTURE TIME, so capture-before-mutate is
//     checked), propagateLinkGroup, Engine::OnParticleSystemChanged (with its
//     track argument and the state hash at that point), MarkDirty, and the
//     tree / engine-state events;
//   - a hash of the resulting document: every emitter's keys, interpolation
//     and lock pointers, plus the SaveParticleSystem byte image.
//
// The handler TU is compiled for real. The BridgeDispatcher members it calls
// are defined below: getEmitterById and propagateLinkGroup are verbatim
// copies of BridgeDispatcher.cpp running against the real link-group code;
// the rest record their call. A fixed battery of edits replays against three
// fixtures (a synthetic system with locked, unlocked and linked emitters, and
// two shipped .alo files), each edit on a fresh load, plus one cumulative
// sequence. The result must match tests/fixtures/authoring-parity/baseline.golden.txt
// line for line; the baseline was recorded from the handlers before PR 3.
//
//   test_authoring_parity.exe            compare against the baseline
//   test_authoring_parity.exe --update   rewrite the baseline (only when a
//                                        behavior change is intended)
//
// Runs from the repo root (the native test runner's cwd). See the
// test_authoring_parity entry in tests/native-tests.json.

#include "host/AcceleratorBridge.h"
#include "host/BridgeDispatcher.h"
#include "host/BridgeRequestContext.h"
#include "host/LayoutBroker.h"

#include "effect/LinkGroup.h"
#include "effect/ParticleSystem.h"
#include "simulation/ParticleSystemInstance.h"
#include "common/files.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using nlohmann::json;
using Emitter = ParticleSystem::Emitter;
using Track   = ParticleSystem::Emitter::Track;
using Key     = ParticleSystem::Emitter::Track::Key;

static const char* kBaselinePath = "tests/fixtures/authoring-parity/baseline.golden.txt";

// ===========================================================================
// State fingerprint
// ===========================================================================

typedef std::vector<unsigned char> Bytes;

static uint64_t Fnv1a(const void* data, size_t size, uint64_t h = 1469598103934665603ull)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static std::string Hex64(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
    return buf;
}

static uint32_t FloatBits(float f) { uint32_t u; std::memcpy(&u, &f, sizeof(u)); return u; }

static Bytes Serialize(ParticleSystem& ps)
{
    MemoryFile* f = new MemoryFile();
    ps.write(f);
    f->seek(0);
    Bytes out(f->size());
    if (!out.empty()) f->read(out.data(), (unsigned long)out.size());
    f->Release();
    return out;
}

static std::unique_ptr<ParticleSystem> Load(const Bytes& image)
{
    MemoryFile* f = new MemoryFile();
    if (!image.empty()) f->write(image.data(), (unsigned long)image.size());
    f->seek(0);
    std::unique_ptr<ParticleSystem> ps(new ParticleSystem(f));
    f->Release();
    return ps;
}

// Everything the track handlers can touch, then the saved image. Lock state
// is pointer identity (tracks[i] -> trackContents[j]), which the saved image
// cannot show, so it is listed explicitly.
static uint64_t StateHash(ParticleSystem& ps)
{
    std::ostringstream s;
    const std::vector<Emitter*>& emitters = ps.getEmitters();
    for (size_t i = 0; i < emitters.size(); i++)
    {
        const Emitter* e = emitters[i];
        if (e == nullptr) { s << "E" << i << " null\n"; continue; }
        s << "E" << i << " " << e->name << " link=" << e->linkGroup << "\n";
        for (int t = 0; t < ParticleSystem::NUM_TRACKS; t++)
        {
            const Track* tr = e->tracks[t];
            const long src = tr == nullptr ? -2
                           : (tr >= e->trackContents && tr < e->trackContents + ParticleSystem::NUM_TRACKS)
                                 ? (long)(tr - e->trackContents) : -1;
            s << " t" << t << " src=" << src;
            if (tr != nullptr)
            {
                s << " it=" << (int)tr->interpolation;
                for (const Key& k : tr->keys)
                {
                    char kb[24];
                    std::snprintf(kb, sizeof(kb), " %08x:%08x", FloatBits(k.time), FloatBits(k.value));
                    s << kb;
                }
            }
            s << "\n";
        }
    }
    const std::string text = s.str();
    const Bytes image = Serialize(ps);
    uint64_t h = Fnv1a(text.data(), text.size());
    return Fnv1a(image.data(), image.size(), h);
}

// ===========================================================================
// Recording stubs for the members the handler TU calls
// ===========================================================================

static std::vector<std::string> g_calls;
static std::unique_ptr<ParticleSystem> g_system;   // bound to the dispatcher

static std::string CurrentStateTag() { return g_system ? Hex64(StateHash(*g_system)) : "nosys"; }

static int EmitterIndex(const Emitter* e)
{
    if (!g_system || e == nullptr) return -1;
    const std::vector<Emitter*>& emitters = g_system->getEmitters();
    for (size_t i = 0; i < emitters.size(); i++) if (emitters[i] == e) return (int)i;
    return -1;
}

// Link stub -- see test_rescale.cpp. No EmitterInstance is ever registered.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

// BindHostState links SpawnerDriver, whose Tick calls these; no spawner is
// bound here, so they are never reached (as in test_bridge_spawner_state.cpp).
ParticleSystemInstance* Engine::SpawnParticleSystem(const ParticleSystem&, Object3D*) { return nullptr; }
int Engine::ActiveSpawnerInstanceCount() const { return 0; }

void Engine::OnParticleSystemChanged(int track)
{
    g_calls.push_back("invalidate(" + std::to_string(track) + "@" + CurrentStateTag() + ")");
}

namespace host {

void BridgeRequestContext::MarkDirty() { g_calls.push_back("dirty"); }

void BridgeDispatcher::captureUndo(DWORD coalesceKey)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08lx", (unsigned long)coalesceKey);
    g_calls.push_back(std::string("undo(") + buf + "@" + CurrentStateTag() + ")");
}

void BridgeDispatcher::EmitEmittersTreeChanged() { g_calls.push_back("evt:tree"); }
void BridgeDispatcher::EmitEngineStateChanged()  { g_calls.push_back("evt:state"); }

// Verbatim copy of BridgeDispatcher.cpp's getEmitterById.
ParticleSystem::Emitter* BridgeDispatcher::getEmitterById(int id)
{
    if (id < 0) return nullptr;
    if (m_pParticleSystem == nullptr || !*m_pParticleSystem) return nullptr;
    const auto& emitters = (*m_pParticleSystem)->getEmitters();
    if (static_cast<size_t>(id) >= emitters.size()) return nullptr;
    return emitters[id];
}

// Recorded, then a verbatim copy of BridgeDispatcher.cpp's propagateLinkGroup,
// so linked siblings really receive the edit.
void BridgeDispatcher::propagateLinkGroup(ParticleSystem::Emitter* edited)
{
    g_calls.push_back("propagate(" + std::to_string(EmitterIndex(edited)) + ")");
    if (edited == nullptr || edited->linkGroup == 0) return;
    if (m_pParticleSystem == nullptr || !*m_pParticleSystem) return;
    ParticleSystem* sys = m_pParticleSystem->get();
    std::vector<ParticleSystem::Emitter*> members =
        GetLinkGroupMembers(*sys, edited->linkGroup);
    const LinkExemptFlags& exempt =
        sys->getLinkExemptFlags(edited->linkGroup);
    bool copied = false;
    for (size_t i = 0; i < members.size(); ++i)
    {
        if (members[i] != edited)
        {
            members[i]->copySharedParamsFrom(*edited, exempt);
            copied = true;
        }
    }
    if (copied && m_engine != nullptr)
        m_engine->OnParticleSystemChanged(-1);
}

struct AuthoringParityTestAccess
{
    static std::unique_ptr<BridgeDispatcher> Make(LayoutBroker& layout, AcceleratorBridge& accel, Engine* engine)
    {
        return std::unique_ptr<BridgeDispatcher>(
            new BridgeDispatcher(layout, accel, BridgeDispatcher::SpawnerConfigTestTag{}, engine));
    }

    // Runs one request through the real track handlers. Returns the response
    // envelope, or a "throw:" / "unhandled" marker.
    static std::string Dispatch(BridgeDispatcher& d, const std::string& kind, const json& params)
    {
        const std::string id = "p";
        BridgeRequestContext ctx{d, id, kind, params, json()};
        try
        {
            if (!d.TryDispatchEmitterTracks(ctx)) return "unhandled";
        }
        catch (const std::exception& e)
        {
            return std::string("throw:") + e.what();
        }
        return ctx.res.dump();
    }
};

} // namespace host

// Fake engine: the handlers only call OnParticleSystemChanged (stubbed above).
alignas(Engine) static unsigned char g_engineStorage[sizeof(Engine)];
static Engine* FakeEngine() { return reinterpret_cast<Engine*>(g_engineStorage); }

// ===========================================================================
// Fixtures
// ===========================================================================

static void SetKeys(Track& t, std::initializer_list<Key> keys)
{
    t.keys.clear();
    for (const Key& k : keys) t.keys.insert(k);
}

// E0: default locks (G/B/A -> R), interior red keys, a smooth scale track and
// a stepped index track. E1: every color channel unlocked, same-time interior
// green keys. E2: linked to E1. Interior times are multiples of 12.5 so they
// survive the save/load (a key time is stored as time/100), and the battery's
// fixed probe times hit real keys.
static Bytes SyntheticFixture()
{
    ParticleSystem ps;
    ps.setName("ParitySys");

    Emitter* e0 = ps.addRootEmitter();
    e0->name = "E0";
    e0->textureSize = 16;
    SetKeys(e0->trackContents[ParticleSystem::TRACK_RED_CHANNEL],
            {Key(0, 0.2f), Key(25, 0.4f), Key(50, 0.6f), Key(75, 0.8f), Key(100, 1)});
    SetKeys(e0->trackContents[ParticleSystem::TRACK_SCALE], {Key(0, 20), Key(40, 10), Key(100, 5)});
    e0->trackContents[ParticleSystem::TRACK_SCALE].interpolation = Track::IT_SMOOTH;
    SetKeys(e0->trackContents[ParticleSystem::TRACK_INDEX], {Key(0, 0), Key(50, 3), Key(100, 7)});

    Emitter* e1 = ps.addRootEmitter();
    e1->name = "E1";
    for (int i = 0; i < 4; i++) e1->tracks[i] = &e1->trackContents[i];
    SetKeys(e1->trackContents[0], {Key(0, 0), Key(25, 0.5f), Key(100, 1)});
    SetKeys(e1->trackContents[1], {Key(0, 0.1f), Key(50, 0.2f), Key(50, 0.3f), Key(100, 0.4f)});
    SetKeys(e1->trackContents[2], {Key(0, 0.5f), Key(100, 0.5f)});
    SetKeys(e1->trackContents[3], {Key(0, 1), Key(75, 0.5f), Key(100, 0)});

    Emitter* e2 = ps.addRootEmitter();
    e2->name = "E2";
    CreateLinkGroup(ps, {e1, e2});

    return Serialize(ps);
}

static bool ReadFile(const char* path, Bytes& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// ===========================================================================
// Battery
// ===========================================================================

struct Request { std::string kind; json params; };

static json Times(std::initializer_list<json> v) { return json(v); }

static const char* kTrackNames[ParticleSystem::NUM_TRACKS] = {
    "red", "green", "blue", "alpha", "scale", "index", "rotationSpeed",
};

// Every request kind across tracks and inputs, for one emitter id. `loaded`
// is the fixture as loaded: shipped files rarely put keys on round times, so
// set/delete also probe each track's actual first interior key time.
static std::vector<Request> BatteryFor(int id, const ParticleSystem& loaded)
{
    std::vector<Request> out;
    auto add = [&](const char* kind, json p) { p["id"] = id; out.push_back({kind, p}); };

    const std::vector<Emitter*>& emitters = loaded.getEmitters();
    const Emitter* e = (id >= 0 && (size_t)id < emitters.size()) ? emitters[id] : nullptr;
    for (int t = 0; e != nullptr && t < ParticleSystem::NUM_TRACKS; t++)
    {
        const Track* tr = e->tracks[t];
        if (tr == nullptr || tr->keys.size() < 3) continue;
        const float interior = (++tr->keys.begin())->time;   // float -> JSON double -> float is exact
        add("emitters/set-track-key", {{"track", kTrackNames[t]}, {"oldTime", interior}, {"newTime", 12.5f}, {"newValue", 0.75f}});
        add("emitters/set-track-key", {{"track", kTrackNames[t]}, {"oldTime", interior}, {"newTime", interior}, {"newValue", 0.25f}});
        add("emitters/delete-track-keys", {{"track", kTrackNames[t]}, {"times", Times({interior})}});
        add("emitters/add-track-key", {{"track", kTrackNames[t]}, {"time", interior}, {"value", 0.5}});
    }

    add("emitters/get-tracks", json::object());

    const char* delTracks[] = {"red", "green", "alpha", "scale", "index", "bogus"};
    const json delTimes[] = {
        json::array(), Times({0}), Times({100}), Times({25}), Times({50}),
        Times({25, 50, 75}), Times({"x", 25}), Times({33}), Times({0, 25, 100}),
    };
    for (const char* t : delTracks)
        for (const json& times : delTimes)
            add("emitters/delete-track-keys", {{"track", t}, {"times", times}});

    const char* interpTracks[] = {"red", "green", "scale", "index", "bogus"};
    const char* interps[] = {"linear", "smooth", "step", "cubic"};
    for (const char* t : interpTracks)
        for (const char* i : interps)
            add("emitters/set-track-interpolation", {{"track", t}, {"interpolation", i}});

    const char* channels[] = {"red", "green", "blue", "alpha", "scale", "bogus"};
    const char* lockTos[] = {"red", "green", "blue", "alpha", "scale", "bogus"};
    for (const char* c : channels)
    {
        add("emitters/set-track-lock", {{"channel", c}, {"lockTo", nullptr}});
        add("emitters/set-track-lock", {{"channel", c}});
        for (const char* l : lockTos)
            add("emitters/set-track-lock", {{"channel", c}, {"lockTo", l}});
    }

    const char* setTracks[] = {"red", "green", "scale", "index"};
    const float oldTimes[] = {0, 100, 25, 50, 33};
    const float moves[][2] = {{10, 0.5f}, {150, 0.9f}, {60, 0.25f}};
    for (const char* t : setTracks)
        for (float o : oldTimes)
            for (const auto& m : moves)
                add("emitters/set-track-key", {{"track", t}, {"oldTime", o}, {"newTime", m[0]}, {"newValue", m[1]}});
    add("emitters/set-track-key", {{"track", "red"}});
    add("emitters/set-track-key", {{"track", "red"}, {"oldTime", "x"}, {"newTime", 10}, {"newValue", 0.5}});

    const char* addTracks[] = {"red", "green", "scale", "bogus"};
    for (const char* t : addTracks)
    {
        add("emitters/add-track-key", {{"track", t}, {"time", 25}, {"value", 0.5}});
        add("emitters/add-track-key", {{"track", t}, {"time", 0}, {"value", 0.5}});
        add("emitters/add-track-key", {{"track", t}, {"time", 50}, {"value", 0.5}});
        add("emitters/add-track-key", {{"track", t}, {"time", 33}, {"value", 0.3}});
        add("emitters/add-track-key", {{"track", t}, {"time", 1e300}, {"value", 1}});
        add("emitters/add-track-key", {{"track", t}, {"value", 1}});
        add("emitters/add-track-key", {{"track", t}, {"time", "x"}, {"value", 1}});
    }

    const char* batchTracks[] = {"red", "scale", "bogus"};
    const json batches[] = {
        json("notarray"),
        json::array(),
        json::array({{{"time", 25}, {"value", 0.5}}, {{"time", 25}, {"value", 0.6}}}),
        json::array({{{"time", 0}, {"value", 0.1}}, {{"time", 100}, {"value", 0.2}}, {{"time", 0}, {"value", 0.3}}}),
        json::array({{{"time", 1e300}, {"value", 1}}}),
        json::array({{{"time", "x"}, {"value", 1}}}),
        json::array({{{"time", 25}, {"value", 0.5}}, 5}),
        json::array({{{"time", 33}, {"value", 0.1}}, {{"time", 34}, {"value", 0.2}}}),
    };
    for (const char* t : batchTracks)
        for (const json& keys : batches)
            add("emitters/add-track-keys", {{"track", t}, {"keys", keys}});

    return out;
}

// Ids that resolve to no emitter, plus a missing id: one request per kind.
static std::vector<Request> BadIdBattery()
{
    std::vector<Request> out;
    const json ids[] = {json(99), json(-1), json()};
    for (const json& id : ids)
    {
        auto add = [&](const char* kind, json p) { if (!id.is_null()) p["id"] = id; out.push_back({kind, p}); };
        add("emitters/get-tracks", json::object());
        add("emitters/delete-track-keys", {{"track", "red"}, {"times", Times({25})}});
        add("emitters/set-track-interpolation", {{"track", "red"}, {"interpolation", "step"}});
        add("emitters/set-track-lock", {{"channel", "green"}, {"lockTo", nullptr}});
        add("emitters/set-track-key", {{"track", "red"}, {"oldTime", 25}, {"newTime", 30}, {"newValue", 0.5}});
        add("emitters/add-track-key", {{"track", "red"}, {"time", 25}, {"value", 0.5}});
        add("emitters/add-track-keys", {{"track", "red"}, {"keys", json::array({{{"time", 25}, {"value", 0.5}}})}});
    }
    return out;
}

// A cumulative session on the synthetic fixture: coalescing set-key drags, an
// add onto an occupied time, a lock then an interpolation edit through the
// lock, an unlock, and a batch onto its own times.
static std::vector<Request> Sequence()
{
    return {
        {"emitters/set-track-key", {{"id", 1}, {"track", "red"}, {"oldTime", 25}, {"newTime", 40}, {"newValue", 0.6}}},
        {"emitters/set-track-key", {{"id", 1}, {"track", "red"}, {"oldTime", 40}, {"newTime", 45}, {"newValue", 0.7}}},
        {"emitters/add-track-key", {{"id", 1}, {"track", "red"}, {"time", 45}, {"value", 0.1}}},
        {"emitters/delete-track-keys", {{"id", 1}, {"track", "red"}, {"times", Times({45})}}},
        {"emitters/set-track-lock", {{"id", 1}, {"channel", "green"}, {"lockTo", "red"}}},
        {"emitters/set-track-interpolation", {{"id", 1}, {"track", "green"}, {"interpolation", "smooth"}}},
        {"emitters/set-track-lock", {{"id", 1}, {"channel", "green"}, {"lockTo", nullptr}}},
        {"emitters/add-track-keys", {{"id", 0}, {"track", "scale"}, {"keys", json::array({{{"time", 40}, {"value", 1}}, {{"time", 40}, {"value", 2}}})}}},
        {"emitters/delete-track-keys", {{"id", 0}, {"track", "scale"}, {"times", Times({40, 40.001})}}},
        {"emitters/get-tracks", {{"id", 0}}},
    };
}

// ===========================================================================
// Runner
// ===========================================================================

static std::string Record(host::BridgeDispatcher& d, size_t n, const std::string& fixture, const Request& r)
{
    g_calls.clear();
    std::string res = host::AuthoringParityTestAccess::Dispatch(d, r.kind, r.params);
    // get-tracks responses are whole-track dumps; keep the line readable.
    if (res.size() > 160) res = "res#" + Hex64(Fnv1a(res.data(), res.size()));

    std::string calls;
    for (const std::string& c : g_calls) calls += (calls.empty() ? "" : " ") + c;

    char num[16];
    std::snprintf(num, sizeof(num), "#%04u", (unsigned)n);
    return std::string(num) + " " + fixture + " " + r.kind + " " + r.params.dump()
         + " => " + res + " | " + (calls.empty() ? "-" : calls)
         + " | post=" + CurrentStateTag();
}

int main(int argc, char** argv)
{
    const bool update = argc > 1 && std::strcmp(argv[1], "--update") == 0;
    std::printf("test_authoring_parity%s\n", update ? " (--update)" : "");

    struct Fixture { std::string name; Bytes image; std::vector<int> ids; };
    std::vector<Fixture> fixtures;
    fixtures.push_back({"synthetic", SyntheticFixture(), {0, 1, 2}});
    {
        Fixture flip{"flipbook", {}, {0}};
        Fixture a11y{"a11y-base", {}, {0}};
        if (!ReadFile("site/guide/downloads/P_EXPLOSION_EXAMPLE_FLIPBOOK.ALO", flip.image)
            || !ReadFile("web/apps/editor/tests/fixtures/a11y-base-state.alo", a11y.image))
        {
            std::printf("  FAIL: fixture .alo files not found (run from the repo root)\n");
            return 1;
        }
        fixtures.push_back(flip);
        fixtures.push_back(a11y);
    }

    host::LayoutBroker layout;
    host::AcceleratorBridge accel;
    std::unique_ptr<host::BridgeDispatcher> d =
        host::AuthoringParityTestAccess::Make(layout, accel, FakeEngine());
    d->BindHostState(&g_system, nullptr, nullptr);

    std::vector<std::string> lines;
    size_t n = 0;
    for (const Fixture& f : fixtures)
    {
        std::vector<Request> battery;
        std::unique_ptr<ParticleSystem> loaded = Load(f.image);
        for (int id : f.ids)
        {
            std::vector<Request> b = BatteryFor(id, *loaded);
            battery.insert(battery.end(), b.begin(), b.end());
        }
        if (&f == &fixtures.front())
        {
            std::vector<Request> b = BadIdBattery();
            battery.insert(battery.end(), b.begin(), b.end());
        }
        for (const Request& r : battery)
        {
            g_system = Load(f.image);   // every edit starts from a fresh load
            lines.push_back(Record(*d, ++n, f.name, r));
        }
    }
    g_system = Load(fixtures.front().image);
    for (const Request& r : Sequence())
        lines.push_back(Record(*d, ++n, "synthetic-seq", r));
    g_system.reset();

    if (update)
    {
        std::ofstream out(kBaselinePath, std::ios::binary);
        for (const std::string& l : lines) out << l << "\n";
        std::printf("  wrote %u edits to %s\n", (unsigned)lines.size(), kBaselinePath);
        return out ? 0 : 1;
    }

    std::vector<std::string> expected;
    {
        std::ifstream in(kBaselinePath, std::ios::binary);
        std::string l;
        while (std::getline(in, l))
        {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            expected.push_back(l);
        }
    }
    if (expected.empty())
    {
        std::printf("  FAIL: no baseline at %s (record one with --update)\n", kBaselinePath);
        return 1;
    }

    int mismatches = 0;
    const size_t count = lines.size() > expected.size() ? lines.size() : expected.size();
    for (size_t i = 0; i < count; i++)
    {
        const std::string& want = i < expected.size() ? expected[i] : std::string("(none)");
        const std::string& got  = i < lines.size()    ? lines[i]    : std::string("(none)");
        if (want == got) continue;
        if (++mismatches <= 10)
            std::printf("[authoring-parity] MISMATCH line %u\n  expected: %s\n  actual:   %s\n",
                        (unsigned)(i + 1), want.c_str(), got.c_str());
    }

    char msg[128];
    std::snprintf(msg, sizeof(msg), "%u edits match the recorded handler baseline", (unsigned)lines.size());
    if (mismatches == 0) { std::printf("  ok: %s\n", msg); std::printf("all checks passed\n"); return 0; }
    std::printf("  FAIL: %s (%d mismatching)\n", msg, mismatches);
    return 1;
}
