#include "HexColourDialog.hpp"
#include "I18N.hpp"

#include <wx/clrpicker.h>

#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#include <commdlg.h>
#endif

#include <algorithm>
#include <iterator>

namespace Slic3r { namespace GUI {

#ifdef __WXMSW__
namespace {

// Control ids of the Windows colour dialog template (see <colordlg.h> in the Windows SDK).
constexpr int COLOR_RED      = 706;
constexpr int COLOR_GREEN    = 707;
constexpr int COLOR_BLUE     = 708;
constexpr int COLOR_ADD      = 712;
constexpr int COLOR_REDACCEL = 726;
// Our own controls, chosen not to clash with the template.
constexpr int ID_HEX_LABEL = 1100;
constexpr int ID_HEX_EDIT  = 1101;

constexpr UINT_PTR HEX_SUBCLASS_ID = 1;

RECT client_rect_of(HWND dlg, int id)
{
    RECT rc{};
    if (HWND ctrl = ::GetDlgItem(dlg, id)) {
        ::GetWindowRect(ctrl, &rc);
        ::MapWindowPoints(HWND_DESKTOP, dlg, reinterpret_cast<POINT *>(&rc), 2);
    }
    return rc;
}

bool parse_hex(wxString hex, COLORREF &out)
{
    hex.Trim(true).Trim(false);
    if (hex.StartsWith("#"))
        hex.Remove(0, 1);
    if (hex.length() != 6)
        return false;
    unsigned int value = 0;
    for (wxUniChar c : hex) {
        const int digit = wxString("0123456789ABCDEF").Find(wxUniChar(wxToupper(c)));
        if (digit == wxNOT_FOUND)
            return false;
        value = (value << 4) | unsigned(digit);
    }
    out = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}

// Rewrite the hex field from the dialog's R/G/B fields, unless the user is typing in it.
void sync_hex_from_rgb(HWND dlg)
{
    HWND hex_edit = ::GetDlgItem(dlg, ID_HEX_EDIT);
    if (hex_edit == nullptr || ::GetFocus() == hex_edit)
        return;
    BOOL ok_r = FALSE, ok_g = FALSE, ok_b = FALSE;
    const UINT r = ::GetDlgItemInt(dlg, COLOR_RED, &ok_r, FALSE);
    const UINT g = ::GetDlgItemInt(dlg, COLOR_GREEN, &ok_g, FALSE);
    const UINT b = ::GetDlgItemInt(dlg, COLOR_BLUE, &ok_b, FALSE);
    if (!ok_r || !ok_g || !ok_b || r > 255 || g > 255 || b > 255)
        return;
    ::SetWindowTextW(hex_edit, wxString::Format("#%02X%02X%02X", r, g, b).wc_str());
}

// Push a complete hex code typed by the user into the dialog's current colour.
void apply_hex_to_dialog(HWND dlg)
{
    HWND hex_edit = ::GetDlgItem(dlg, ID_HEX_EDIT);
    // Only react to the user's own edits, not to sync_hex_from_rgb().
    if (hex_edit == nullptr || ::GetFocus() != hex_edit)
        return;
    wchar_t text[32] = {};
    ::GetWindowTextW(hex_edit, text, int(std::size(text)));
    COLORREF colour;
    if (!parse_hex(wxString(text), colour))
        return;
    // Documented way for a hook procedure to set the colour of a ChooseColor dialog.
    static const UINT set_rgb_msg = ::RegisterWindowMessageW(SETRGBSTRINGW);
    ::SendMessage(dlg, set_rgb_msg, 0, colour);
}

LRESULT CALLBACK hex_subclass_proc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_NCDESTROY) {
        ::RemoveWindowSubclass(dlg, hex_subclass_proc, HEX_SUBCLASS_ID);
    } else if (msg == WM_COMMAND && HIWORD(wparam) == EN_CHANGE) {
        const int id = LOWORD(wparam);
        if (id == ID_HEX_EDIT) {
            apply_hex_to_dialog(dlg);
            return 0;
        }
        if (id == COLOR_RED || id == COLOR_GREEN || id == COLOR_BLUE) {
            // Let the dialog take the new value first, then mirror it.
            const LRESULT result = ::DefSubclassProc(dlg, msg, wparam, lparam);
            sync_hex_from_rgb(dlg);
            return result;
        }
    }
    return ::DefSubclassProc(dlg, msg, wparam, lparam);
}

} // namespace
#endif // __WXMSW__

HexColourDialog::HexColourDialog(wxWindow *parent, const wxColourData *data)
    : wxColourDialog(parent, data)
{
#ifdef __WXMSW__
    // The hex field lives next to R/G/B, which only exist in the expanded dialog.
    m_colourData.SetChooseFull(true);
#endif
}

#ifdef __WXMSW__
void HexColourDialog::MSWOnInitDone(WXHWND hDlg)
{
    HWND dlg = static_cast<HWND>(hDlg);

    const RECT red_label = client_rect_of(dlg, COLOR_REDACCEL);
    const RECT red_edit  = client_rect_of(dlg, COLOR_RED);
    const RECT blue_edit = client_rect_of(dlg, COLOR_BLUE);
    const RECT add_btn   = client_rect_of(dlg, COLOR_ADD);

    if (red_edit.right > red_edit.left && add_btn.bottom > add_btn.top) {
        // New row below "Add to Custom Colors", in the column of the R/G/B fields.
        const int gap    = std::max(4, int(add_btn.top - blue_edit.bottom));
        const int edit_h = red_edit.bottom - red_edit.top;
        const int row_y  = add_btn.bottom + gap;

        RECT window{};
        ::GetWindowRect(dlg, &window);
        ::SetWindowPos(dlg, nullptr, 0, 0, window.right - window.left, window.bottom - window.top + gap + edit_h,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        HINSTANCE instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtr(dlg, GWLP_HINSTANCE));
        HFONT     font     = reinterpret_cast<HFONT>(::SendMessage(dlg, WM_GETFONT, 0, 0));

        // Size the edit to fit "#DDDDDD" in the dialog font, right-aligned under the Add button.
        int text_w = 0;
        if (HDC dc = ::GetDC(dlg)) {
            HGDIOBJ old_font = ::SelectObject(dc, font);
            SIZE    extent{};
            ::GetTextExtentPoint32W(dc, L"#DDDDDD", 7, &extent);
            ::SelectObject(dc, old_font);
            ::ReleaseDC(dlg, dc);
            text_w = extent.cx;
        }
        const int edit_w = std::min(int(add_btn.right - add_btn.left) / 2,
                                    std::max(int(red_edit.right - red_edit.left), text_w + 4 * ::GetSystemMetrics(SM_CXEDGE) + 6));
        const int edit_x = add_btn.right - edit_w;

        // Label sits in the same column as "Red:" etc., right-aligned against the edit.
        const int      label_h = red_label.bottom - red_label.top;
        const int      label_x = add_btn.left;
        const wxString label   = "&" + _L("Hex") + ":";
        HWND hex_label = ::CreateWindowExW(0, WC_STATICW, label.wc_str(), WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                          label_x, row_y + (edit_h - label_h) / 2,
                                          edit_x - std::max(2, int(red_edit.left - red_label.right)) - label_x, label_h,
                                          dlg, reinterpret_cast<HMENU>(INT_PTR(ID_HEX_LABEL)), instance, nullptr);
        HWND hex_edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE,
                                         edit_x, row_y, edit_w, edit_h,
                                         dlg, reinterpret_cast<HMENU>(INT_PTR(ID_HEX_EDIT)), instance, nullptr);
        if (hex_label != nullptr && hex_edit != nullptr) {
            ::SendMessage(hex_label, WM_SETFONT, WPARAM(font), FALSE);
            ::SendMessage(hex_edit, WM_SETFONT, WPARAM(font), FALSE);
            ::SendMessage(hex_edit, EM_LIMITTEXT, 16, 0);

            // Tab from Blue straight into Hex; the label goes first so its "&H" mnemonic focuses the edit.
            if (HWND blue = ::GetDlgItem(dlg, COLOR_BLUE)) {
                ::SetWindowPos(hex_label, blue, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                ::SetWindowPos(hex_edit, hex_label, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }

            ::SetWindowSubclass(dlg, hex_subclass_proc, HEX_SUBCLASS_ID, 0);
            sync_hex_from_rgb(dlg);
        }
    }

    // Base class positions/centres the dialog, so it has to see the final size.
    wxColourDialog::MSWOnInitDone(hDlg);
}
#endif // __WXMSW__

void show_hex_colour_dialog(wxWindow *picker_button)
{
#ifdef __WXMSW__
    // Mirrors wxGenericColourButton::OnButtonClick(), with our dialog in place of wxColourDialog.
    auto *button = dynamic_cast<wxGenericColourButton *>(picker_button);
    if (button == nullptr)
        return;

    wxColourData &data = *button->GetColourData();
    data.SetColour(button->GetColour());

    HexColourDialog dlg(button, &data);
    dlg.Bind(wxEVT_COLOUR_CHANGED, [button](wxColourDialogEvent &evt) {
        wxWindow *parent = button->GetParent();
        wxColourPickerEvent event(parent, parent->GetId(), evt.GetColour(), wxEVT_COLOURPICKER_CURRENT_CHANGED);
        parent->ProcessWindowEvent(event);
    });

    wxEventType event_type = wxEVT_COLOURPICKER_DIALOG_CANCELLED;
    if (dlg.ShowModal() == wxID_OK) {
        data = dlg.GetColourData();
        button->SetColour(data.GetColour());
        event_type = wxEVT_COLOURPICKER_CHANGED;
    }

    // Like wx, make the event look like it came from the user-visible wxColourPickerCtrl.
    wxWindow *parent = button->GetParent();
    wxColourPickerEvent event(parent, parent->GetId(), button->GetColour(), event_type);
    button->ProcessWindowEvent(event);
#else
    (void)picker_button;
#endif
}

void use_hex_colour_dialog(wxColourPickerCtrl *picker)
{
#ifdef __WXMSW__
    // Handlers bound later run first; not calling Skip() keeps wx's own dialog from opening.
    if (wxWindow *button = picker->GetPickerCtrl())
        button->Bind(wxEVT_BUTTON, [button](wxCommandEvent &) { show_hex_colour_dialog(button); });
#else
    (void)picker;
#endif
}

}} // namespace Slic3r::GUI
