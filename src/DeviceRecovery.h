#pragma once

#include "DeviceState.h"

// Executable D3D9Ex recovery coordinator.
//
// The adapter is templated so the native unit test can instantiate the exact
// production CheckDeviceState / ResetEx door with a small fake device instead
// of implementing the full IDirect3DDevice9Ex COM surface. Engine supplies the
// owner callbacks that release and reacquire every persistent video-memory
// owner; RunDeviceRecoveryStep supplies the one-attempt state machine.
namespace devicerecovery {

enum class Phase
{
    Ready,
    ResetExFailed, // a non-HUNG ResetEx failed; only probe/retry is legal
    Recovering,    // release / ResetEx / reacquire is in progress
    Terminal,      // restart (or full device recreation) is required
};

enum class Outcome
{
    Render,
    SkipFrame,
    ResetRequired,
    RetryResetEx,
    Fatal,
};

struct State
{
    Phase   phase                = Phase::Ready;
    HRESULT observedState        = D3D_OK;
    HRESULT failure              = D3D_OK;
    bool    hungRecoveryConsumed = false;
};

struct Result
{
    Outcome outcome;
    HRESULT observedState;
    HRESULT recoveryResult;
    bool    attemptedRecovery;
};

template <typename TDevice, typename TOwner>
class D3D9ExRecoveryPort
{
public:
    D3D9ExRecoveryPort(TDevice* device, TOwner& owner)
        : m_device(device), m_owner(owner)
    {
    }

    // This exact call is the G-UX-3 load-bearing production door.
    // TestCooperativeLevel always reports S_OK for a D3D9Ex device.
    HRESULT CheckDeviceState()
    {
        return m_device->CheckDeviceState(nullptr);
    }

    bool IsDeviceRecoveryThread() const
    {
        return m_owner.IsDeviceRecoveryThread();
    }

    void ReleaseAll()
    {
        m_owner.ReleaseDeviceResourcesForReset();
    }

    HRESULT ResetEx()
    {
        D3DPRESENT_PARAMETERS parameters =
            m_owner.GetDeviceRecoveryPresentationParameters();
        return m_device->ResetEx(&parameters, nullptr);
    }

    void ResetEffects()
    {
        m_owner.ResetDeviceEffectsAfterReset();
    }

    HRESULT RefreshPresentationParameters()
    {
        return m_owner.RefreshPresentationParametersAfterReset();
    }

    void ReacquireAll()
    {
        m_owner.ReacquireDeviceResourcesAfterReset();
    }

private:
    TDevice* m_device;
    TOwner&  m_owner;
};

inline void RecordResetExFailure(State& recovery, HRESULT failure)
{
    recovery.phase         = Phase::ResetExFailed;
    recovery.observedState = failure;
    recovery.failure       = failure;
}

inline void CompleteResetExRetry(State& recovery)
{
    recovery.phase         = Phase::Ready;
    recovery.observedState = D3D_OK;
    recovery.failure       = D3D_OK;
}

template <typename TPort>
Result RunDeviceRecoveryStep(
    State& recovery,
    TPort& port,
    bool renderWhenOccluded = false)
{
    // A failed ResetEx leaves the device in a state where only ResetEx,
    // CheckDeviceState, or Release is legal. The editor deliberately makes no
    // second attempt, so terminal means no more device calls at all.
    if (recovery.phase == Phase::Terminal)
    {
        return { Outcome::Fatal, recovery.observedState,
                 recovery.failure, false };
    }
    if (recovery.phase == Phase::Recovering)
    {
        // Reset/ResetEx may synchronously dispatch window messages. A nested
        // frame must make no device call and must not corrupt the outer
        // coordinator's state.
        return { Outcome::SkipFrame, recovery.observedState,
                 E_PENDING, false };
    }

    const HRESULT state = port.CheckDeviceState();
    devicestate::Action action = devicestate::ClassifyDeviceState(state);
    // A composed frame renders into an offscreen shared texture and never
    // presents through the D3D9 window. CheckDeviceState still reports
    // S_PRESENT_OCCLUDED for that hidden presentation window, but occlusion
    // cannot make the shared render target invisible. Direct D3D9 presentation
    // retains the classifier's SkipFrame result.
    if (renderWhenOccluded && state == S_PRESENT_OCCLUDED)
        action = devicestate::Action::Render;

    // After any failed ResetEx, ordinary Reset and normal rendering are not
    // legal. Probe until the device is retryable, then ask the Engine to retry
    // the ResetEx path that established this pending state.
    if (recovery.phase == Phase::ResetExFailed)
    {
        switch (action)
        {
            case devicestate::Action::Fatal:
                recovery.phase         = Phase::Terminal;
                recovery.observedState = state;
                recovery.failure       = state;
                return { Outcome::Fatal, state, state, false };

            case devicestate::Action::RecoverHung:
                break;

            case devicestate::Action::Render:
            case devicestate::Action::Reset:
                recovery.observedState = state;
                return { Outcome::RetryResetEx, state,
                         recovery.failure, false };

            case devicestate::Action::SkipFrame:
            default:
                recovery.observedState = state;
                return { Outcome::SkipFrame, state,
                         recovery.failure, false };
        }
    }

    switch (action)
    {
        case devicestate::Action::Render:
            return { Outcome::Render, state, D3D_OK, false };

        case devicestate::Action::Reset:
            return { Outcome::ResetRequired, state, D3D_OK, false };

        case devicestate::Action::Fatal:
            recovery.phase         = Phase::Terminal;
            recovery.observedState = state;
            recovery.failure       = state;
            return { Outcome::Fatal, state, state, false };

        case devicestate::Action::RecoverHung:
            break;

        case devicestate::Action::SkipFrame:
        default:
            return { Outcome::SkipFrame, state, D3D_OK, false };
    }

    // One HUNG recovery per device lifetime. A second hang after a successful
    // reset is not allowed to enter an unbounded release/reset loop.
    if (recovery.hungRecoveryConsumed)
    {
        recovery.phase         = Phase::Terminal;
        recovery.observedState = state;
        recovery.failure       = state;
        return { Outcome::Fatal, state, state, false };
    }

    // ResetEx must execute on the thread that created the device. Leave the
    // state retryable so the render-thread caller can perform the attempt.
    if (!port.IsDeviceRecoveryThread())
    {
        return { Outcome::SkipFrame, state, D3D_OK, false };
    }

    recovery.phase         = Phase::Recovering;
    recovery.observedState = state;
    recovery.failure       = D3D_OK;

    try
    {
        port.ReleaseAll();
    }
    catch (...)
    {
        recovery.phase   = Phase::Terminal;
        recovery.failure = E_FAIL;
        return { Outcome::Fatal, state, recovery.failure, true };
    }

    HRESULT resetResult = E_FAIL;
    try
    {
        resetResult = port.ResetEx();
    }
    catch (...)
    {
        resetResult = E_FAIL;
    }

    if (FAILED(resetResult))
    {
        recovery.phase   = Phase::Terminal;
        recovery.failure = resetResult;
        return { Outcome::Fatal, state, resetResult, true };
    }

    // D3DX effects must receive OnResetDevice before other post-reset device
    // work. ResetEx also zeros the in/out back-buffer extents, so refresh them
    // from the actual back buffer before any size-keyed allocation.
    try
    {
        port.ResetEffects();
    }
    catch (...)
    {
        recovery.phase   = Phase::Terminal;
        recovery.failure = E_FAIL;
        return { Outcome::Fatal, state, recovery.failure, true };
    }

    HRESULT refreshResult = E_FAIL;
    try
    {
        refreshResult = port.RefreshPresentationParameters();
    }
    catch (...)
    {
        refreshResult = E_FAIL;
    }
    if (FAILED(refreshResult))
    {
        recovery.phase   = Phase::Terminal;
        recovery.failure = refreshResult;
        return { Outcome::Fatal, state, refreshResult, true };
    }

    // No resource is reacquired until ResetEx and extent refresh both succeed.
    try
    {
        port.ReacquireAll();
    }
    catch (...)
    {
        recovery.phase   = Phase::Terminal;
        recovery.failure = E_FAIL;
        return { Outcome::Fatal, state, recovery.failure, true };
    }

    recovery.phase                = Phase::Ready;
    recovery.failure              = D3D_OK;
    recovery.hungRecoveryConsumed = true;
    return { Outcome::Render, state, D3D_OK, true };
}

} // namespace devicerecovery
