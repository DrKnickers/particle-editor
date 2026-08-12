// Regression test for the production async-startup callback adapter
// (src/host/StartupCallbackAdapter.cpp).
//
// WebView2's environment-completed and composition-controller-completed
// callbacks are one-shot: they cannot be unsubscribed, so HostWindow's
// WM_DESTROY sweep — which removes every registered handler precisely because
// the lambdas capture `this` — never reached them. And WM_DESTROY does not stop
// the pump (PostQuitMessage only posts WM_QUIT, delivered after the queue
// drains), so a completion the runtime already queued still dispatches against
// a torn-down owner (2026-07 audit).
//
// The case that matters is #3: a token issued BEFORE the owner existed no more,
// read AFTER the owner is gone. That is the exact use-after-free shape, and it
// is the assertion that flips when the guard is reverted.
//
// Case #1 is the overreach half and is just as load-bearing: a guard that
// answered false while the owner is alive would abort startup every time and
// the editor would never show UI at all. "Always false" must not pass this
// suite.
//
// The builder links the exact production adapter TU; see
// tests/build_test_startup_callback_guard.bat.

#include "host/StartupCallbackAdapter.h"
#include "host/CompositionStartupPolicy.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

static int g_failed = 0;

#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

static std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

static size_t CountOccurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos)
    {
        ++count;
        offset += needle.size();
    }
    return count;
}

int main()
{
    std::printf("test_startup_callback_guard\n");

    // --- 1. THE OVERREACH GUARD. A live, un-retired owner must let its
    // callback through. An "always false" guard passes every other case in this
    // file and fails only here — which is why this case exists.
    {
        host::StartupCallbackGuard guard;
        host::StartupCallbackGuard::Token token = guard.Issue();
        CHECK(token.OwnerAlive(), "live owner: callback proceeds");
        CHECK(guard.Alive(),      "live owner: guard reports alive");
    }

    // --- 2. Retire closes the door for a token issued beforehand. This is the
    // WM_DESTROY case: the token was handed to the runtime at startup, the user
    // closed the window, and the completion has not fired yet.
    {
        host::StartupCallbackGuard guard;
        host::StartupCallbackGuard::Token token = guard.Issue();
        CHECK(token.OwnerAlive(), "pre-retire: token is live");
        guard.Retire();
        CHECK(!token.OwnerAlive(), "post-retire: token issued earlier is dead");
        CHECK(!guard.Alive(),      "post-retire: guard reports dead");
    }

    // --- 3. THE REVERT ASSERTION. Token outlives the owner entirely. Reading
    // it must be well-defined and must answer false — this is the dispatch that
    // used to run `make_unique<Compositor>(hMain)` on a destroyed window.
    {
        host::StartupCallbackGuard::Token token{std::shared_ptr<const bool>()};
        {
            host::StartupCallbackGuard guard;
            token = guard.Issue();
            CHECK(token.OwnerAlive(), "in scope: token is live");
        }   // guard destructs here
        CHECK(!token.OwnerAlive(), "owner destroyed: token reads dead, not freed memory");
    }

    // --- 4. One-way. There is no revive, and a token issued AFTER the retire
    // is dead on arrival — a late callback must not be able to resurrect an
    // owner that is already partway through teardown.
    {
        host::StartupCallbackGuard guard;
        guard.Retire();
        host::StartupCallbackGuard::Token late = guard.Issue();
        CHECK(!late.OwnerAlive(), "token issued after retire is dead on arrival");
    }

    // --- 5. Retire is idempotent. WM_DESTROY calls it and the destructor calls
    // it again; the second call must not throw, flip state back, or crash.
    {
        host::StartupCallbackGuard guard;
        host::StartupCallbackGuard::Token token = guard.Issue();
        guard.Retire();
        guard.Retire();
        guard.Retire();
        CHECK(!token.OwnerAlive(), "triple retire stays dead");
    }

    // --- 6. Guards do not cross-talk. Rules out the "one process-global flag"
    // shortcut, which would work in this single-window host today and break the
    // moment anything else adopts the primitive.
    {
        host::StartupCallbackGuard a;
        host::StartupCallbackGuard b;
        host::StartupCallbackGuard::Token ta = a.Issue();
        host::StartupCallbackGuard::Token tb = b.Issue();
        a.Retire();
        CHECK(!ta.OwnerAlive(), "retiring A kills A's token");
        CHECK(tb.OwnerAlive(),  "retiring A leaves B's token live");
    }

    // --- 7. Multiple tokens from one guard all observe the retire. Startup
    // issues two (environment + composition controller) and both must decline.
    {
        host::StartupCallbackGuard guard;
        host::StartupCallbackGuard::Token envToken  = guard.Issue();
        host::StartupCallbackGuard::Token ctlToken  = guard.Issue();
        guard.Retire();
        CHECK(!envToken.OwnerAlive(), "env-creation token declines after retire");
        CHECK(!ctlToken.OwnerAlive(), "controller token declines after retire");
    }

    // --- 8. THE PRODUCTION CALLBACKS, LIVE. Invoke both real COM handlers.
    // Distinct sentinels prove the correct continuation ran exactly once and
    // that the adapter returned its value rather than manufacturing S_OK.
    {
        const HRESULT envSentinel = static_cast<HRESULT>(0x8004A001L);
        const HRESULT ctlSentinel = static_cast<HRESULT>(0x8004A002L);
        int envCalls = 0;
        int ctlCalls = 0;
        HRESULT envInput = S_OK;
        HRESULT ctlInput = S_OK;

        host::StartupCallbackGuard guard;
        auto envCallback = host::MakeEnvironmentStartupCallback(
            guard.Issue(),
            [&](HRESULT hr, ICoreWebView2Environment*) -> HRESULT
            {
                ++envCalls;
                envInput = hr;
                return envSentinel;
            });
        auto ctlCallback = host::MakeCompositionControllerStartupCallback(
            guard.Issue(),
            [&](HRESULT hr, ICoreWebView2CompositionController*) -> HRESULT
            {
                ++ctlCalls;
                ctlInput = hr;
                return ctlSentinel;
            });

        CHECK(envCallback.Get() != nullptr,
              "live environment factory returns a COM handler");
        CHECK(ctlCallback.Get() != nullptr,
              "live controller factory returns a COM handler");
        CHECK(envCallback->Invoke(E_ABORT, nullptr) == envSentinel,
              "live environment callback returns its distinct sentinel");
        CHECK(ctlCallback->Invoke(E_ACCESSDENIED, nullptr) == ctlSentinel,
              "live controller callback returns its distinct sentinel");
        CHECK(envCalls == 1 && envInput == E_ABORT,
              "live environment continuation runs exactly once with its input");
        CHECK(ctlCalls == 1 && ctlInput == E_ACCESSDENIED,
              "live controller continuation runs exactly once with its input");
    }

    // --- 9. THE TWO RETIRE MUTANTS. Each production handler must decline with
    // S_OK and zero owner effects after WM_DESTROY retires the shared guard.
    // Removing either adapter check changes its corresponding call count to 1.
    {
        int envCalls = 0;
        int ctlCalls = 0;
        host::StartupCallbackGuard guard;
        auto envCallback = host::MakeEnvironmentStartupCallback(
            guard.Issue(),
            [&](HRESULT, ICoreWebView2Environment*) -> HRESULT
            {
                ++envCalls;
                return E_FAIL;
            });
        auto ctlCallback = host::MakeCompositionControllerStartupCallback(
            guard.Issue(),
            [&](HRESULT, ICoreWebView2CompositionController*) -> HRESULT
            {
                ++ctlCalls;
                return E_ABORT;
            });

        guard.Retire();
        CHECK(envCallback->Invoke(E_FAIL, nullptr) == S_OK,
              "retired environment callback declines with S_OK");
        CHECK(ctlCallback->Invoke(E_FAIL, nullptr) == S_OK,
              "retired controller callback declines with S_OK");
        CHECK(envCalls == 0,
              "retired environment callback has zero owner effects");
        CHECK(ctlCalls == 0,
              "retired controller callback has zero owner effects");
    }

    // --- 10. The COM handlers can outlive the guard object itself. Their token
    // keeps only the liveness flag alive, and both must still decline safely.
    {
        int envCalls = 0;
        int ctlCalls = 0;
        Microsoft::WRL::ComPtr<
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> envCallback;
        Microsoft::WRL::ComPtr<
            ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>
            ctlCallback;
        {
            host::StartupCallbackGuard guard;
            envCallback = host::MakeEnvironmentStartupCallback(
                guard.Issue(),
                [&](HRESULT, ICoreWebView2Environment*) -> HRESULT
                {
                    ++envCalls;
                    return E_FAIL;
                });
            ctlCallback = host::MakeCompositionControllerStartupCallback(
                guard.Issue(),
                [&](HRESULT, ICoreWebView2CompositionController*) -> HRESULT
                {
                    ++ctlCalls;
                    return E_FAIL;
                });
        }

        CHECK(envCallback->Invoke(E_FAIL, nullptr) == S_OK,
              "owner-destroyed environment callback declines with S_OK");
        CHECK(ctlCallback->Invoke(E_FAIL, nullptr) == S_OK,
              "owner-destroyed controller callback declines with S_OK");
        CHECK(envCalls == 0 && ctlCalls == 0,
              "owner-destroyed callbacks have zero owner effects");
    }

    // --- 11. PRODUCTION WIRING. Behavioral adapter coverage is not enough if
    // InitWebView2 constructs the right handlers but does not pass each result
    // to its matching WebView2 create call. Bind both call sites and the real
    // owner-retirement ordering, and forbid both raw handler factories.
    {
        const std::string source = ReadSource(
            std::filesystem::current_path() / "src" / "host" / "HostWindow.cpp");
        const size_t initStart = source.find(
            "HRESULT HostWindowImpl::InitWebView2()");
        const size_t initEnd = source.find(
            "HRESULT HostWindowImpl::FinishWebView2ControllerSetup", initStart);
        const std::string initBody =
            initStart != std::string::npos && initEnd != std::string::npos
                ? source.substr(initStart, initEnd - initStart)
                : std::string();

        CHECK(!initBody.empty(), "InitWebView2 production body is readable");
        CHECK(CountOccurrences(
                  initBody,
                  "auto startupToken = m_startupGuard.Issue();") == 1,
              "InitWebView2 issues exactly one token from the retired member guard");
        CHECK(CountOccurrences(
                  initBody,
                  "MakeEnvironmentStartupCallback(") == 1,
              "InitWebView2 calls the environment callback factory exactly once");
        CHECK(CountOccurrences(
                  initBody,
                  "MakeCompositionControllerStartupCallback(") == 1,
              "InitWebView2 calls the controller callback factory exactly once");
        CHECK(initBody.find(
                  "Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>") ==
                  std::string::npos,
              "InitWebView2 has no raw environment-completed handler");
        CHECK(initBody.find(
                  "Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>") ==
                  std::string::npos,
              "InitWebView2 has no raw controller-completed handler");
        CHECK(initBody.find("OwnerAlive()") == std::string::npos,
              "InitWebView2 delegates both liveness checks to the production adapter");

        const std::string environmentFactoryText =
            "MakeEnvironmentStartupCallback(";
        const std::string controllerFactoryText =
            "MakeCompositionControllerStartupCallback(";
        const std::string startupTokenText = "startupToken";
        const auto firstArgumentIsStartupToken =
            [&initBody, &startupTokenText](
                size_t factory,
                const std::string& factoryText) -> bool
            {
                if (factory == std::string::npos)
                    return false;
                const size_t firstArgument = initBody.find_first_not_of(
                    " \t\r\n", factory + factoryText.size());
                if (firstArgument == std::string::npos ||
                    initBody.compare(firstArgument,
                                     startupTokenText.size(),
                                     startupTokenText) != 0)
                {
                    return false;
                }
                const size_t delimiter = initBody.find_first_not_of(
                    " \t\r\n", firstArgument + startupTokenText.size());
                return delimiter != std::string::npos &&
                       initBody[delimiter] == ',';
            };

        const size_t environmentFactory = initBody.find(
            environmentFactoryText);
        CHECK(firstArgumentIsStartupToken(
                  environmentFactory,
                  environmentFactoryText),
              "environment factory receives the member-issued startup token");
        const size_t environmentCreate = initBody.find(
            "HRESULT envCreateHr = CreateCoreWebView2EnvironmentWithOptions(",
            environmentFactory);
        const size_t environmentBinding = initBody.find(
            "environmentHandler.Get()", environmentCreate);
        const size_t environmentCallEnd = initBody.find(
            ");", environmentCreate);
        CHECK(environmentFactory != std::string::npos &&
              environmentCreate != std::string::npos &&
              environmentBinding != std::string::npos &&
              environmentCallEnd != std::string::npos &&
              environmentFactory < environmentCreate &&
              environmentBinding < environmentCallEnd,
              "environment create receives the environment factory result");

        const size_t controllerFactory = initBody.find(
            controllerFactoryText);
        CHECK(firstArgumentIsStartupToken(
                  controllerFactory,
                  controllerFactoryText),
              "controller factory receives the member-issued startup token");
        const size_t controllerCreate = initBody.find(
            "env3->CreateCoreWebView2CompositionController(",
            controllerFactory);
        const size_t controllerBinding = initBody.find(
            "compositionControllerHandler.Get()", controllerCreate);
        const size_t controllerCallEnd = initBody.find(
            ");", controllerCreate);
        CHECK(controllerFactory != std::string::npos &&
              controllerCreate != std::string::npos &&
              controllerBinding != std::string::npos &&
              controllerCallEnd != std::string::npos &&
              controllerFactory < controllerCreate &&
              controllerBinding < controllerCallEnd,
              "composition create receives the controller factory result");

        // WM_DESTROY is the other half of the production contract. Isolate the
        // main-window handler (there is a separate viewport WM_DESTROY later in
        // this TU), then require retirement before every owner teardown.
        const size_t mainWndProcStart = source.find(
            "LRESULT HostWindowImpl::MainWndProc(");
        const size_t mainWndProcEnd = source.find(
            "LRESULT HostWindowImpl::ViewportWndProc(",
            mainWndProcStart);
        const bool foundMainWndProc =
            mainWndProcStart != std::string::npos &&
            mainWndProcEnd != std::string::npos &&
            mainWndProcEnd > mainWndProcStart;
        const std::string mainWndProcBody = foundMainWndProc
            ? source.substr(mainWndProcStart,
                            mainWndProcEnd - mainWndProcStart)
            : std::string();
        const size_t destroyStart =
            mainWndProcBody.find("case WM_DESTROY:");
        const size_t destroyEnd =
            mainWndProcBody.find("return DefWindowProc(",
                                 destroyStart);
        const bool foundDestroyHandler =
            destroyStart != std::string::npos &&
            destroyEnd != std::string::npos &&
            destroyEnd > destroyStart;
        const std::string destroyBody = foundDestroyHandler
            ? mainWndProcBody.substr(destroyStart,
                                     destroyEnd - destroyStart)
            : std::string();
        const size_t retire =
            destroyBody.find("m_startupGuard.Retire();");
        const size_t firstTimerTeardown =
            destroyBody.find("KillTimer(hwnd, kStatsTimerId);");
        const size_t webViewTeardown =
            destroyBody.find(
                "webView->remove_WebMessageReceived(webMessageTok);");
        const size_t compositorTeardown =
            destroyBody.find("m_compositor.reset();");
        const size_t engineTeardown =
            destroyBody.find("engine.reset();");

        CHECK(foundMainWndProc,
              "main-window production handler is isolated exactly");
        CHECK(foundDestroyHandler,
              "main-window WM_DESTROY production body is isolated exactly");
        CHECK(CountOccurrences(
                  destroyBody,
                  "m_startupGuard.Retire();") == 1,
              "main-window WM_DESTROY retires startup callbacks exactly once");
        CHECK(retire != std::string::npos &&
              firstTimerTeardown != std::string::npos &&
              webViewTeardown != std::string::npos &&
              compositorTeardown != std::string::npos &&
              engineTeardown != std::string::npos &&
              retire < firstTimerTeardown &&
              retire < webViewTeardown &&
              retire < compositorTeardown &&
              retire < engineTeardown,
              "startup callback retirement precedes timers and owner teardown");
    }

    // --- 12. The second WebView2 create has its OWN synchronous HRESULT. A
    // failed dispatch never calls the composition-controller completion, so it
    // must enter the fatal path directly. S_FALSE is the overreach value: it is
    // successful even though it is not bitwise-equal to S_OK.
    CHECK(host::ShouldFailCompositionControllerDispatch(E_ACCESSDENIED),
          "controller dispatch E_ACCESSDENIED is fatal");
    CHECK(host::ShouldFailCompositionControllerDispatch(E_FAIL),
          "controller dispatch E_FAIL is fatal");
    CHECK(!host::ShouldFailCompositionControllerDispatch(S_OK),
          "controller dispatch S_OK is not fatal");
    CHECK(!host::ShouldFailCompositionControllerDispatch(S_FALSE),
          "controller dispatch S_FALSE success is not fatal (overreach guard)");

    // --- 13. PRODUCTION BINDING. The pure predicate alone cannot prove the
    // environment callback observes the second create call. This source check
    // pins that exact call site, the existing fatal message, and the original
    // failure value. Without it, a header+test pair would stay green while the
    // production callback silently returned the HRESULT to a runtime that
    // discards it.
    {
        const std::string source = ReadSource(
            std::filesystem::current_path() / "src" / "host" / "HostWindow.cpp");
        CHECK(!source.empty(), "HostWindow production source is readable");

        const size_t dispatch = source.find(
            "const HRESULT controllerCreateHr =");
        const size_t createCall = source.find(
            "env3->CreateCoreWebView2CompositionController(", dispatch);
        const size_t failureCheck = source.find(
            "ShouldFailCompositionControllerDispatch(controllerCreateHr)",
            createCall);
        const size_t fatalPost = source.find(
            "PostMessageW(hMain, WM_APP_COMPOSITION_FALLBACK",
            failureCheck);
        const size_t fatalValue = source.find(
            "static_cast<WPARAM>(controllerCreateHr)", fatalPost);
        const size_t callbackReturn = source.find(
            "return controllerCreateHr;", fatalValue);

        CHECK(dispatch != std::string::npos &&
              createCall != std::string::npos && createCall > dispatch,
              "production stores the second create call's own HRESULT");
        CHECK(failureCheck != std::string::npos && failureCheck > createCall,
              "production checks the synchronous controller-dispatch result");
        CHECK(fatalPost != std::string::npos &&
              fatalValue != std::string::npos &&
              fatalPost > failureCheck && fatalValue > fatalPost,
              "synchronous controller failure posts the fatal-composition message");
        CHECK(callbackReturn != std::string::npos && callbackReturn > fatalValue,
              "environment callback returns only after surfacing the second failure");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
