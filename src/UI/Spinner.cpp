#include "UI.h"
#include <sstream>
#include <iomanip>
using namespace std;

enum DragType
{
	NONE,
	HOLDING_UP,
	HOLDING_DOWN,
	DRAGGING
};

namespace SystemInfo
{
	HCURSOR  hSpinCursor;
	HCURSOR  hNormalCursor;
	int      spinnerWidth;
	int      holdDelay;
	int      holdSpeed;
};

struct SpinnerControl
{
	SPINNER_INFO info;
	HWND     hSpinner;
	HWND     hEdit;
    bool     allowNotify;
	bool     readOnly;       // input blocked but value still visible
	UINT_PTR hTimer;
	WNDPROC  EditWindowProc;

	int      dragStartY;
	float	 dragStartValueF;
	long	 dragStartValueI;

	DragType dragType;
};

static void Spinner_Paint(HWND hWnd, SpinnerControl* control)
{
	PAINTSTRUCT ps;
	HDC hDC = BeginPaint(hWnd, &ps);

	RECT rect;
	GetClientRect(hWnd, &rect);

	int styleUp = (control->dragType == HOLDING_UP   || control->dragType == DRAGGING) ? DFCS_PUSHED : 0;
	int styleDn = (control->dragType == HOLDING_DOWN || control->dragType == DRAGGING) ? DFCS_PUSHED : 0;
	// Show the up/down buttons as inactive when the spinner is in
	// read-only mode — gives the same visual cue as a disabled
	// control without losing text legibility in the edit.
	if (control->readOnly)
	{
		styleUp |= DFCS_INACTIVE;
		styleDn |= DFCS_INACTIVE;
	}

	// Render up-down buttons
	rect.left = max(0, rect.right - SystemInfo::spinnerWidth);
	rect.top  = rect.bottom - rect.bottom / 2;
	DrawFrameControl(hDC, &rect, DFC_SCROLL, styleDn | DFCS_SCROLLDOWN);
	rect.bottom /= 2;
	rect.top     = 0;
	DrawFrameControl(hDC, &rect, DFC_SCROLL, styleUp | DFCS_SCROLLUP);

	EndPaint(hWnd, &ps);
}

static void Spinner_Update(SpinnerControl* control)
{
	wstringstream ss;
	if (control->info.IsFloat)
	{
		control->info.f.Value = max(min(control->info.f.Value, control->info.f.MaxValue), control->info.f.MinValue);
		ss << fixed << setprecision(2) << control->info.f.Value;
	}
	else
	{
		control->info.i.Value = max(min(control->info.i.Value, control->info.i.MaxValue), control->info.i.MinValue);
		ss << control->info.i.Value;
	}
	SetWindowText(control->hEdit, ss.str().c_str());
}

static void Spinner_Redraw(HWND hWnd, SpinnerControl* control)
{
	RECT size;
	GetClientRect(hWnd, &size);
	size.left = size.right - SystemInfo::spinnerWidth;
	RedrawWindow(hWnd, &size, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

// Mouse-wheel nudge. Direction follows the sign of wheelDelta. The base step is
// the spinner's already-configured Increment (same value VK_UP/VK_DOWN use).
// Modifiers:
//   - Shift  -> 10x the increment (coarse)
//   - Ctrl   -> 0.1x the increment (fine), float spinners only; integers fall
//               back to a 1x step so the wheel always does something.
static void Spinner_WheelStep(SpinnerControl* control, int wheelDelta, WORD modKeys)
{
	if (control == NULL || wheelDelta == 0) return;
	int direction = (wheelDelta > 0) ? 1 : -1;

	if (control->info.IsFloat)
	{
		float step = control->info.f.Increment;
		if      (modKeys & MK_SHIFT)   step *= 10.0f;
		else if (modKeys & MK_CONTROL) step *= 0.1f;
		control->info.f.Value += direction * step;
	}
	else
	{
		long step = control->info.i.Increment;
		if (modKeys & MK_SHIFT) step *= 10;
		// MK_CONTROL on integer spinners: keep 1x step; rounding 0.1 to 0 would no-op
		control->info.i.Value += direction * step;
	}

	Spinner_Update(control);
}

static LRESULT CALLBACK SpinnerEditWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	SpinnerControl* control = (SpinnerControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);

	// When the spinner is read-only we paint the EDIT ourselves so the
	// value text gets a clear "auto / disabled" grey colour. Overriding
	// WM_CTLCOLOREDIT in the parent didn't work — Win11 themes ignore
	// the colour we hand them — so we bypass the EDIT's default paint
	// entirely for the read-only state.
	if (uMsg == WM_PAINT && control != NULL && control->readOnly)
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);

		RECT rc;
		GetClientRect(hWnd, &rc);

		// Slightly-greyed background + grey text reads as "read-only"
		// without losing the value. Matches the visual language of a
		// disabled-but-rendered Windows control. Use hardcoded
		// RGB values rather than COLOR_3DFACE / COLOR_GRAYTEXT — those
		// system colours can collapse to the same hue on some Win11
		// themes and make text invisible.
		HBRUSH bg = CreateSolidBrush(RGB(232, 232, 232));
		FillRect(hdc, &rc, bg);
		DeleteObject(bg);

		HFONT hFont = (HFONT)SendMessage(hWnd, WM_GETFONT, 0, 0);
		HFONT old   = hFont ? (HFONT)SelectObject(hdc, hFont) : NULL;
		SetTextColor(hdc, RGB(60, 60, 60));
		SetBkMode   (hdc, TRANSPARENT);

		wchar_t text[64];
		int len = GetWindowText(hWnd, text, _countof(text));
		// ES_RIGHT default: right-justify with 2 px margin.
		rc.right -= 2;
		DrawText(hdc, text, len, &rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		if (old) SelectObject(hdc, old);
		EndPaint(hWnd, &ps);
		return 0;
	}

	// Here we can trap certain message, otherwise sent to the edit control
	switch (uMsg)
	{
		case WM_KEYUP:
			if (wParam == VK_UP || wParam == VK_DOWN)
			{
				return 0;
			}
			break;
		
		case WM_CHAR:
		{
			// First-level filter: Ignore everything but digits, minus and periods (if the number is a float)
			char c = (char)wParam;
			if (wParam > 127 || (!iscntrl(c) && !isdigit(c) && c != '-' && (c != '.' || !control->info.IsFloat)))
			{
				return 0;
			}

			if (wParam == '.')
			{
				// Check to see if it already contains a period
				TCHAR text[256];
				GetWindowText(hWnd, text, 256);
				if (wcschr(text,'.') != NULL)
				{
					return 0;
				}
			}
			break;
		}

		case WM_KEYDOWN:
			if (wParam == VK_UP)
			{
				if (control->readOnly) return 0;
				if (control->info.IsFloat)
					control->info.f.Value += control->info.f.Increment;
				else
					control->info.i.Value += control->info.i.Increment;
				Spinner_Update(control);
				return 0;
			}

			if (wParam == VK_DOWN)
			{
				if (control->readOnly) return 0;
				if (control->info.IsFloat)
					control->info.f.Value -= control->info.f.Increment;
				else
					control->info.i.Value -= control->info.i.Increment;
				Spinner_Update(control);
				return 0;
			}
			break;

		case WM_MOUSEWHEEL:
			if (control->readOnly) return 0;
			Spinner_WheelStep(control,
			                  GET_WHEEL_DELTA_WPARAM(wParam),
			                  GET_KEYSTATE_WPARAM(wParam));
			return 0;
	}

	// Pass message on to edit control
	return control->EditWindowProc(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK SpinnerWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	SpinnerControl* control = (SpinnerControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	switch (uMsg)
	{
		case WM_CREATE:
		{
			CREATESTRUCT* pcs = (CREATESTRUCT*)lParam;
			RECT size;
			GetClientRect(hWnd, &size);

			control = new SpinnerControl;
			control->hSpinner = hWnd;
            control->allowNotify = true;
            control->readOnly    = false;
			control->info.IsFloat = false;
			control->info.i.Value     = 0;
			control->info.i.MinValue  = 0;
			control->info.i.MaxValue  = 100;
			control->info.i.Increment = 1;

			control->dragType     = NONE;

			LONG style = pcs->style & WS_DISABLED;
			if ((control->hEdit = CreateWindow(L"EDIT", L"0", style | WS_CHILD | WS_VISIBLE | ES_RIGHT | ES_AUTOHSCROLL | WS_TABSTOP,
				0, 0, size.right - SystemInfo::spinnerWidth, size.bottom, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
			{
				delete control;
				return -1;
			}

			// We hijack the edit control's message window because we need to trap certain messages.
			control->EditWindowProc = (WNDPROC)(LONG_PTR)GetWindowLongPtr(control->hEdit, GWLP_WNDPROC);
			SetWindowLongPtr(control->hEdit, GWLP_USERDATA, (LONG_PTR)control);
			SetWindowLongPtr(control->hEdit, GWLP_WNDPROC,  (LONG_PTR)SpinnerEditWindowProc);
			SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)control);
			break;
		}

		case WM_DESTROY:
			DestroyWindow(control->hEdit);
			delete control;
			break;

		case WM_COMMAND:
			if (control != NULL && (HWND)lParam == control->hEdit)
			{
				// Notification from edit control
				switch (HIWORD(wParam))
				{
					case EN_CHANGE:
                        if (control->allowNotify && !control->readOnly)
    					{
	    					// Forward to parent
		    				SendMessage(GetParent(hWnd), WM_COMMAND, (WPARAM)MAKELONG(GetDlgCtrlID(hWnd), SN_CHANGE), (LPARAM)hWnd );
                        }
						break;

					case EN_UPDATE:
					{
						// Don't accept typed-text into the model when the
						// spinner is read-only — the value belongs to a
						// caller that's driving it externally (e.g. a
						// force-aligned fill light). The edit may still
						// have intermediate text the user typed; the
						// EN_KILLFOCUS handler below resets it.
						if (control->readOnly) break;
						// Check new text to make sure
						TCHAR text[256];
						GetWindowText(control->hEdit, text, 256);
						wstringstream ss;
						ss << text;
						if (control->info.IsFloat)
						{
                            float val;
							ss >> val;
                            if (!ss.fail())
							    control->info.f.Value = val;
						}
						else
						{
							long val;
							ss >> val;
                            if (!ss.fail())
    							control->info.i.Value = val;
						}
						break;
					}

					case EN_KILLFOCUS:
						// Forward to parent
						SendMessage(GetParent(hWnd), WM_COMMAND, (WPARAM)MAKELONG(GetDlgCtrlID(hWnd), SN_KILLFOCUS), (LPARAM)hWnd );
						// Spinner_Update reformats the edit from the
						// authoritative control->info.X.Value, which
						// also restores read-only spinners that the
						// user typed garbage into.
						Spinner_Update(control);
						break;
				}
			}
			break;
		
		case WM_ENABLE:
			EnableWindow(control->hEdit, (BOOL)wParam);
			// Force a redraw of the up/down buttons so they reflect the
			// new state, and invalidate the edit so the disabled-text
			// colour we set in WM_CTLCOLORSTATIC takes effect.
			InvalidateRect(hWnd, NULL, TRUE);
			if (control->hEdit != NULL) InvalidateRect(control->hEdit, NULL, TRUE);
			break;

		// When the inner edit is disabled it sends WM_CTLCOLORSTATIC to
		// its parent (us). DefWindowProc's default returns
		// COLOR_GRAYTEXT which on modern Windows themes is so light
		// against COLOR_3DFACE that the value becomes essentially
		// invisible. We override with a darker grey so values like a
		// force-aligned fill light's Z angle stay readable while still
		// looking unmistakably disabled.
		case WM_CTLCOLORSTATIC:
		{
			// Disabled EDIT children paint via WM_CTLCOLORSTATIC. Some
			// Windows themes refuse to draw the text at all when this
			// returns the default colors. We force a readable
			// foreground/background pair here so values stay visible
			// when the spinner is e.g. greyed by a force-align checkbox.
			HDC hdc = (HDC)wParam;
			SetTextColor(hdc, RGB(96, 96, 96));
			SetBkColor  (hdc, GetSysColor(COLOR_3DFACE));
			return (LRESULT)GetSysColorBrush(COLOR_3DFACE);
		}

		// Note: we deliberately don't override WM_CTLCOLOREDIT here.
		// Tried setting COLOR_GRAYTEXT for read-only spinners; on
		// Win11 themes returning a brush from this message reliably
		// causes the EDIT control to skip drawing its text entirely.
		// The DFCS_INACTIVE styling on the up/down buttons
		// (Spinner_Paint above) is the visual "this is read-only"
		// cue; the value text itself stays in the normal colour.

		case WM_LBUTTONUP:
		{
			KillTimer(hWnd, control->hTimer);
			ReleaseCapture();
			control->dragType = NONE;

			if (control->dragType == DRAGGING)
			{
				SetCursor(SystemInfo::hNormalCursor);
			}

			Spinner_Redraw(hWnd, control);
			break;
		}

		case WM_MOUSEMOVE:
		{
			if (control->dragType != NONE)
			{
				int y = (short)HIWORD(lParam);
				if (control->dragType != DRAGGING)
				{
					KillTimer(hWnd, control->hTimer);
					control->dragType   = DRAGGING;
					control->dragStartY = y;
					if (control->info.IsFloat)
						control->dragStartValueF = control->info.f.Value;
					else
						control->dragStartValueI = control->info.i.Value;
					SetCursor(SystemInfo::hSpinCursor);
					Spinner_Redraw(hWnd, control);
				}

				long amount = control->dragStartY - y;
				if (control->info.IsFloat)
					control->info.f.Value = control->dragStartValueF + amount * control->info.f.Increment;
				else
					control->info.i.Value = control->dragStartValueI + amount * control->info.i.Increment;
				Spinner_Update(control);
			}
			break;
		}

		case WM_MOUSEWHEEL:
			if (control->readOnly) return 0;
			Spinner_WheelStep(control,
			                  GET_WHEEL_DELTA_WPARAM(wParam),
			                  GET_KEYSTATE_WPARAM(wParam));
			return 0;

		case WM_LBUTTONDOWN:
		{
			if (control->readOnly) return 0;
			SetFocus(control->hEdit);
			SetCapture(hWnd);

			RECT size;
			GetClientRect(hWnd, &size);
			control->dragType = (HIWORD(lParam) < size.bottom / 2) ? HOLDING_UP : HOLDING_DOWN;
			control->hTimer   = SetTimer(hWnd, 1, SystemInfo::holdDelay, NULL);

			Spinner_Redraw(hWnd, control);
			wParam = 0;
		}
		
		case WM_TIMER:
		{
			if (wParam == 1)
			{
				// Initial hold delay has passed
				KillTimer(hWnd, control->hTimer);
				control->hTimer = SetTimer(hWnd, 2, 1000 / SystemInfo::holdSpeed, NULL);
			}

			// Time step has passed, increase
			long sign = (control->dragType == HOLDING_UP) ? 1 : -1;
			if (control->info.IsFloat)
				control->info.f.Value = control->info.f.Value + sign * control->info.f.Increment;
			else
				control->info.i.Value = control->info.i.Value + sign * control->info.i.Increment;
			Spinner_Update(control);
			break;
		}

		case WM_SETFOCUS:
			SetFocus(control->hEdit);
			break;

		case WM_SETFONT:
			SendMessage(control->hEdit, WM_SETFONT, wParam, lParam);
			break;

		case WM_PAINT:
			Spinner_Paint(hWnd, control);
			break;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void Spinner_SetInfo(HWND hWnd, const SPINNER_INFO* psi)
{
	SpinnerControl* control = (SpinnerControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	if (control != NULL)
	{
		control->info.IsFloat = psi->IsFloat;
		if (psi->IsFloat)
		{
			if (psi->Mask & SPIF_RANGE)   { control->info.f.MinValue = psi->f.MinValue; control->info.f.MaxValue = psi->f.MaxValue; }
			if (psi->Mask & SPIF_INCREMENT) control->info.f.Increment = psi->f.Increment;
			if (psi->Mask & SPIF_VALUE)	    control->info.f.Value = psi->f.Value;
		}
		else
		{
			if (psi->Mask & SPIF_RANGE)   { control->info.i.MinValue = psi->i.MinValue; control->info.i.MaxValue = psi->i.MaxValue; }
			if (psi->Mask & SPIF_INCREMENT) control->info.i.Increment = psi->i.Increment;
			if (psi->Mask & SPIF_VALUE)	    control->info.i.Value = psi->i.Value;
		}

        // We don't notify the parent for explicitely set values
        control->allowNotify = false;
		Spinner_Update(control);
        control->allowNotify = true;

		Spinner_Redraw(hWnd, control);
	}
}

// Read-only short-circuits the spinner's up/down buttons, mouse wheel,
// and arrow-key increments. Visually the control still renders its
// value clearly (enabled-edit colours), unlike EnableWindow(FALSE)
// which on modern Windows themes suppresses the text entirely. The
// EDIT remains writable; callers that want strict input blocking
// should also discard SN_CHANGE for read-only spinners.
void Spinner_SetReadOnly(HWND hWnd, bool readOnly)
{
	SpinnerControl* control = (SpinnerControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	if (control == NULL) return;
	if (control->readOnly == readOnly) return;
	control->readOnly = readOnly;
	// Redraw the up/down buttons (Spinner_Paint reads readOnly).
	Spinner_Redraw(hWnd, control);
	// Re-trigger the edit's paint so WM_CTLCOLOREDIT picks the new
	// text colour. bErase=FALSE keeps Win11's themed edit happy —
	// invalidating with erase causes some themes to skip text draw.
	if (control->hEdit != NULL)
	{
		InvalidateRect(control->hEdit, NULL, FALSE);
		UpdateWindow(control->hEdit);
	}
}

// Returns the read-only flag set via Spinner_SetReadOnly.
bool Spinner_IsReadOnly(HWND hWnd)
{
	const SpinnerControl* control = (const SpinnerControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	return control != NULL && control->readOnly;
}

void Spinner_GetInfo(HWND hWnd, SPINNER_INFO* psi)
{
	const SpinnerControl* control = (SpinnerControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	if (control != NULL)
	{
		psi->IsFloat = control->info.IsFloat;
		if (control->info.IsFloat)
		{
			if (psi->Mask & SPIF_VALUE)	    psi->f.Value = control->info.f.Value;
			if (psi->Mask & SPIF_RANGE)   { psi->f.MinValue = control->info.f.MinValue; psi->f.MaxValue = control->info.f.MaxValue; }
			if (psi->Mask & SPIF_INCREMENT) psi->f.Increment = control->info.f.Increment;
		}
		else
		{
			if (psi->Mask & SPIF_VALUE)	   psi->i.Value = control->info.i.Value;
			if (psi->Mask & SPIF_RANGE)   { psi->i.MinValue = control->info.i.MinValue; psi->i.MaxValue = control->info.i.MaxValue; }
			if (psi->Mask & SPIF_INCREMENT) psi->i.Increment = control->info.i.Increment;
		}
	}
}

bool Spinner_Initialize( HINSTANCE hInstance )
{
	// Get system settings info
	SystemInfo::spinnerWidth = GetSystemMetrics(SM_CXVSCROLL);
	SystemParametersInfo(SPI_GETKEYBOARDDELAY, 0, &SystemInfo::holdDelay, 0 );
	SystemParametersInfo(SPI_GETKEYBOARDSPEED, 0, &SystemInfo::holdSpeed, 0 );
	SystemInfo::holdDelay = (SystemInfo::holdDelay + 1) * 250;	// To milliseconds
	SystemInfo::holdSpeed = SystemInfo::holdSpeed + 3;			// Repetitions per second (sort of)
	SystemInfo::hNormalCursor = LoadCursor(NULL, IDC_ARROW);
	SystemInfo::hSpinCursor   = LoadCursor(NULL, IDC_SIZENS);

	WNDCLASSEX wcx;
	wcx.cbSize        = sizeof(WNDCLASSEX);
	wcx.style         = CS_HREDRAW | CS_VREDRAW;
	wcx.lpfnWndProc   = SpinnerWindowProc;
	wcx.cbClsExtra    = 0;
	wcx.cbWndExtra    = 0;
	wcx.hInstance     = hInstance;
	wcx.hIcon         = NULL;
	wcx.hCursor       = SystemInfo::hNormalCursor;
	wcx.hbrBackground = NULL;
	wcx.lpszMenuName  = NULL;
	wcx.lpszClassName = L"Spinner";
	wcx.hIconSm       = NULL;
	
	if (!RegisterClassEx(&wcx))
	{
		return false;
	}

	return true;
}
