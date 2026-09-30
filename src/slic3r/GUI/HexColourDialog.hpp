#pragma once

#include <wx/colordlg.h>

class wxColourPickerCtrl;

namespace Slic3r { namespace GUI {

// Drop-in replacement for wxColourDialog. On Windows the native colour dialog only accepts
// decimal RGB / HSL, so this adds a "Hex" field (#RRGGBB) that stays in sync with the rest
// of the dialog. The macOS and GTK native pickers already accept hex, so it is a plain
// wxColourDialog there.
class HexColourDialog : public wxColourDialog
{
public:
    HexColourDialog(wxWindow *parent, const wxColourData *data = nullptr);

#ifdef __WXMSW__
    void MSWOnInitDone(WXHWND hDlg) override;
#endif
};

// Make a wxColourPickerCtrl open HexColourDialog instead of the stock wxColourDialog.
void use_hex_colour_dialog(wxColourPickerCtrl *picker);

// Show HexColourDialog for the button of a wxColourPickerCtrl, as the button itself would.
void show_hex_colour_dialog(wxWindow *picker_button);

}} // namespace Slic3r::GUI
