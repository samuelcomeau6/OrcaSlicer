#include "SpoolmanDialog.hpp"

#include "FilamentColorUtils.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/TextInput.hpp"
#include "wxExtensions.hpp"
#include "../Utils/SpoolmanClient.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/control.h>
#include <wx/utils.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <iterator>

namespace Slic3r
{
namespace GUI
{
namespace
{

const wxColour AccentColour("#019687");

wxString FromUtf8(const std::string& value)
{
    return wxString::FromUTF8(value.c_str());
}

wxFont DialogFont(const wxFont& base, wxFontWeight weight)
{
    wxFont font = base;
    font.SetWeight(weight);
    return font;
}

wxColour SwatchBorderColour()
{
    return StateColor::darkModeColorFor(wxColour(180, 180, 180));
}

wxString FormatGrams(double grams)
{
    return wxString::Format("%.0f g", grams);
}

// "PLA · 742 g of 1000 g · Shelf A · Lot 123"
wxString SpoolDetails(const SpoolmanSpool& spool, bool includeLot)
{
    std::vector<wxString> parts;
    if (!spool.material.empty())
        parts.emplace_back(FromUtf8(spool.material));
    if (spool.remainingWeight >= 0.0 && spool.initialWeight > 0.0)
        parts.emplace_back(wxString::Format(_L("%s of %s"), FormatGrams(spool.remainingWeight), FormatGrams(spool.initialWeight)));
    else if (spool.remainingWeight >= 0.0)
        parts.emplace_back(wxString::Format(_L("%s left"), FormatGrams(spool.remainingWeight)));
    if (!spool.location.empty())
        parts.emplace_back(FromUtf8(spool.location));
    if (includeLot && !spool.lotNr.empty())
        parts.emplace_back(wxString::Format(_L("Lot %s"), FromUtf8(spool.lotNr)));

    wxString details;
    for (const wxString& part : parts)
    {
        if (!details.empty())
            details += wxString::FromUTF8(" \xC2\xB7 ");
        details += part;
    }
    return details;
}

FilamentColor SpoolColour(const SpoolmanSpool& spool)
{
    // Spools without a colour in Spoolman are drawn as a neutral grey.
    return spool.color.Empty() ? FilamentColor::FromColors({ "#D0D0D0" }, FilamentColorMode::Segment) : spool.color;
}

void DrawSwatch(wxDC& dc, const FilamentColor& color, const wxRect& rect)
{
    wxBitmap* bitmap = FilamentColorUtils::GetFilamentColorIcon(color.colors, color.NormalizedMode(), "",
                                                                rect.GetWidth(), rect.GetHeight(), SwatchBorderColour());
    if (bitmap != nullptr)
        dc.DrawBitmap(*bitmap, rect.GetLeft(), rect.GetTop(), true);
}

wxBitmap MakePreviewBitmap(const FilamentColor& color, int size)
{
    size = std::max(1, size);
    wxBitmap output(size, size);
    wxMemoryDC dc;
    dc.SelectObject(output);
    dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
    dc.Clear();
    DrawSwatch(dc, color, wxRect(0, 0, size, size));
    dc.SelectObject(wxNullBitmap);
    return output;
}

/**
 * @brief Flat button matching the "Other Colors" entry of FilamentColorDialog.
 */
class OtherColorsPanel : public wxPanel
{
public:
    OtherColorsPanel(wxWindow* parent, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _icon(this, "filament_color_more", 16)
    {
        SetMinSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(_L("Pick a color without assigning a Spoolman spool"));
        Bind(wxEVT_PAINT, &OtherColorsPanel::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#F0F0F0"))));
        dc.Clear();

        dc.SetFont(DialogFont(Label::Body_14, wxFONTWEIGHT_MEDIUM));
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#242424")));
        const wxString label = _L("Other Colors");
        const wxSize textSize = dc.GetTextExtent(label);
        const int iconWidth = _icon.bmp().IsOk() ? _icon.GetBmpWidth() : 0;
        const int gap = iconWidth > 0 ? FromDIP(4) : 0;
        const int x = std::max(0, (size.GetWidth() - iconWidth - gap - textSize.GetWidth()) / 2);
        if (iconWidth > 0)
            dc.DrawBitmap(_icon.bmp(), x, (size.GetHeight() - _icon.GetBmpHeight()) / 2, true);
        dc.DrawText(label, x + iconWidth + gap, (size.GetHeight() - textSize.GetHeight()) / 2);
    }

    ScalableBitmap _icon;
};

const char* SortConfigKey = "spoolman_sort";

} // namespace

/**
 * @brief Square icon button with an "active" state, used for the filter and sort toggles.
 */
class SpoolmanIconToggle : public wxPanel
{
public:
    SpoolmanIconToggle(wxWindow* parent, const std::string& iconName, const wxString& tooltip)
        : wxPanel(parent, wxID_ANY)
        , _icon(this, iconName, 16)
    {
        const wxSize size(FromDIP(32), FromDIP(32));
        SetMinSize(size);
        SetSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(tooltip);
        Bind(wxEVT_PAINT, &SpoolmanIconToggle::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { _hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { _hover = false; Refresh(); });
    }

    void SetActive(bool active)
    {
        if (_active == active)
            return;
        _active = active;
        Refresh();
    }

    bool IsActive() const { return _active; }

    void Rescale()
    {
        _icon.msw_rescale();
        SetMinSize(wxSize(FromDIP(32), FromDIP(32)));
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();

        wxColour fill = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
        wxColour border = StateColor::darkModeColorFor(wxColour("#D1D5DC"));
        if (_active)
        {
            fill = StateColor::darkModeColorFor(wxColour("#E6F4F3"));
            border = AccentColour;
        }
        else if (_hover)
        {
            fill = StateColor::darkModeColorFor(wxColour("#F5F5F5"));
        }
        dc.SetBrush(wxBrush(fill));
        dc.SetPen(wxPen(border, FromDIP(1)));
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), FromDIP(4));

        if (_icon.bmp().IsOk())
            dc.DrawBitmap(_icon.bmp(), (size.GetWidth() - _icon.GetBmpWidth()) / 2,
                          (size.GetHeight() - _icon.GetBmpHeight()) / 2, true);
    }

    ScalableBitmap _icon;
    bool _active { false };
    bool _hover { false };
};

/**
 * @brief One spool in the list: swatch, name, details and a remaining-weight bar.
 */
class SpoolmanSpoolRow : public wxPanel
{
public:
    SpoolmanSpoolRow(wxWindow* parent, const SpoolmanSpool& spool)
        : wxPanel(parent, wxID_ANY)
        , _spool(spool)
    {
        SetMinSize(wxSize(-1, FromDIP(48)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        if (!spool.comment.empty())
            SetToolTip(FromUtf8(spool.comment));
        Bind(wxEVT_PAINT, &SpoolmanSpoolRow::OnPaint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { SetHover(true); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { SetHover(false); });
    }

    const SpoolmanSpool& Spool() const { return _spool; }

    void SetSelected(bool selected)
    {
        if (_selected == selected)
            return;
        _selected = selected;
        Refresh();
    }

private:
    void SetHover(bool hover)
    {
        if (_hover == hover)
            return;
        _hover = hover;
        Refresh();
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();

        const int radius = FromDIP(4);
        if (_selected || _hover)
        {
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(_selected ? wxColour("#E6F4F3") : wxColour("#F5F5F5"))));
            dc.SetPen(_selected ? wxPen(AccentColour, std::max(FromDIP(1), 2)) : *wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(1, 1, size.GetWidth() - 2, size.GetHeight() - 2, radius);
        }

        const int padding = FromDIP(10);
        const int swatchSize = FromDIP(28);
        DrawSwatch(dc, SpoolColour(_spool), wxRect(padding, (size.GetHeight() - swatchSize) / 2, swatchSize, swatchSize));

        // Remaining weight, right aligned, with a bar underneath when the full weight is known.
        const int barWidth = FromDIP(56);
        const int rightEdge = size.GetWidth() - padding;
        int textRight = rightEdge;
        dc.SetFont(Label::Body_12);
        if (_spool.remainingWeight >= 0.0)
        {
            const wxString grams = FormatGrams(_spool.remainingWeight);
            const wxSize gramsSize = dc.GetTextExtent(grams);
            dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
            dc.DrawText(grams, rightEdge - gramsSize.GetWidth(), FromDIP(8));
            textRight = rightEdge - std::max(barWidth, gramsSize.GetWidth()) - padding;

            if (_spool.initialWeight > 0.0)
            {
                const double fraction = std::clamp(_spool.remainingWeight / _spool.initialWeight, 0.0, 1.0);
                const wxRect bar(rightEdge - barWidth, size.GetHeight() - FromDIP(14), barWidth, FromDIP(4));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#E0E0E0"))));
                dc.DrawRoundedRectangle(bar, bar.GetHeight() / 2.0);
                const int filled = static_cast<int>(bar.GetWidth() * fraction);
                if (filled > 0)
                {
                    dc.SetBrush(wxBrush(fraction < 0.15 ? wxColour("#E2574C") : AccentColour));
                    dc.DrawRoundedRectangle(wxRect(bar.GetLeft(), bar.GetTop(), filled, bar.GetHeight()), bar.GetHeight() / 2.0);
                }
            }
        }

        const int textLeft = padding + swatchSize + padding;
        const int textWidth = std::max(0, textRight - textLeft);

        dc.SetFont(DialogFont(Label::Body_14, wxFONTWEIGHT_MEDIUM));
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#242424")));
        dc.DrawText(wxControl::Ellipsize(FromUtf8(_spool.DisplayName()), dc, wxELLIPSIZE_END, textWidth), textLeft, FromDIP(6));

        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        dc.DrawText(wxControl::Ellipsize(SpoolDetails(_spool, false), dc, wxELLIPSIZE_END, textWidth), textLeft, FromDIP(27));
    }

    SpoolmanSpool _spool;
    bool _selected { false };
    bool _hover { false };
};

SpoolmanDialog::SpoolmanDialog(wxWindow* parent, const std::string& baseUrl, const std::string& filamentType,
                               int currentSpoolId, const FilamentColor& currentColor)
    : DPIDialog(parent, wxID_ANY, _L("Choose a Spoolman spool"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , _baseUrl(baseUrl)
    , _filamentType(filamentType)
    , _selectedSpoolId(currentSpoolId)
    , _currentColor(currentColor)
    , _alive(std::make_shared<AliveToken>())
{
    _alive->dialog = this;
    if (wxGetApp().app_config != nullptr && wxGetApp().app_config->get(SortConfigKey) == "recent")
        _sortOrder = SpoolmanSortOrder::RecentlyUsed;
    BuildUi();
    UpdateToolbar();
    UpdatePreview();
    LoadSpools();
    CenterOnParent();
}

SpoolmanDialog::~SpoolmanDialog()
{
    _alive->dialog = nullptr;
    if (_request)
        _request->cancel();
}

void SpoolmanDialog::BuildUi()
{
    const wxColour background = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    SetBackgroundColour(background);

    const int dialogWidth = FromDIP(460);
    const int margin = FromDIP(16);
    const int contentWidth = dialogWidth - margin * 2;

    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);

    // Current selection preview, same layout as the filament color dialog.
    wxBoxSizer* currentRow = new wxBoxSizer(wxHORIZONTAL);
    _previewBitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(FromDIP(60), FromDIP(60)));
    currentRow->Add(_previewBitmap, 0, wxALIGN_TOP | wxRIGHT, FromDIP(12));
    wxBoxSizer* infoSizer = new wxBoxSizer(wxVERTICAL);
    _nameLabel = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    _nameLabel->SetFont(DialogFont(Label::Body_14, wxFONTWEIGHT_SEMIBOLD));
    _nameLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
    _detailLabel = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    _detailLabel->SetFont(DialogFont(Label::Body_13, wxFONTWEIGHT_NORMAL));
    _detailLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
    // Tiny min widths let long spool names ellipsize instead of widening the dialog.
    _nameLabel->SetMinSize(wxSize(FromDIP(40), -1));
    _detailLabel->SetMinSize(wxSize(FromDIP(40), -1));
    infoSizer->Add(_nameLabel, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
    infoSizer->Add(_detailLabel, 0, wxEXPAND);
    currentRow->Add(infoSizer, 1, wxALIGN_CENTER_VERTICAL);
    root->AddSpacer(FromDIP(20));
    root->Add(currentRow, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    // Search + refresh.
    wxBoxSizer* searchRow = new wxBoxSizer(wxHORIZONTAL);
    _searchInput = new TextInput(this, wxEmptyString, wxEmptyString, "search", wxDefaultPosition,
                                 wxSize(-1, FromDIP(32)), wxTE_PROCESS_ENTER);
    _searchInput->SetToolTip(_L("Filter by ID, vendor, name, material, color, location or lot"));
    _searchInput->GetTextCtrl()->SetHint(_L("Search spools"));
    _searchInput->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { ApplyFilter(); });
    _searchInput->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&)
    {
        // Enter picks the spool when the search narrowed the list down to exactly one.
        std::vector<SpoolmanSpoolRow*> visible;
        std::copy_if(_rows.begin(), _rows.end(), std::back_inserter(visible),
                     [](SpoolmanSpoolRow* row) { return row->IsShown(); });
        if (visible.size() == 1)
        {
            SelectSpool(visible.front()->Spool().id);
            EndModal(wxID_OK);
        }
    });
    _typeFilterToggle = new SpoolmanIconToggle(this, "spoolman_filter", wxEmptyString);
    _typeFilterToggle->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&)
    {
        _showAllTypes = !_showAllTypes;
        UpdateToolbar();
        ApplyFilter();
    });
    _typeFilterToggle->Show(!_filamentType.empty());

    _sortByIdToggle = new SpoolmanIconToggle(this, "spoolman_sort_number", _L("Sort by spool number"));
    _sortByIdToggle->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { SetSortOrder(SpoolmanSortOrder::Id); });
    _sortByRecentToggle = new SpoolmanIconToggle(this, "spoolman_sort_recent", _L("Sort by most recently used"));
    _sortByRecentToggle->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { SetSortOrder(SpoolmanSortOrder::RecentlyUsed); });

    _refreshButton = new SpoolmanIconToggle(this, "refresh", _L("Reload spools from Spoolman"));
    _refreshButton->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { LoadSpools(); });
    _openButton = new SpoolmanIconToggle(this, "spoolman_open", _L("Open Spoolman in your browser"));
    _openButton->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { wxLaunchDefaultBrowser(FromUtf8(_baseUrl)); });

    // Search row: what is listed (search, type filter) and where it comes from (reload, open Spoolman).
    searchRow->Add(_searchInput, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    searchRow->Add(_typeFilterToggle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    searchRow->Add(_refreshButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    searchRow->Add(_openButton, 0, wxALIGN_CENTER_VERTICAL);
    root->AddSpacer(FromDIP(20));
    root->Add(searchRow, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    // List header: title on the left, sort order on the right.
    wxBoxSizer* listHeader = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* listLabel = new wxStaticText(this, wxID_ANY, _L("Spoolman Spools"));
    listLabel->SetFont(DialogFont(Label::Body_14, wxFONTWEIGHT_MEDIUM));
    listLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
    listHeader->Add(listLabel, 0, wxALIGN_CENTER_VERTICAL);
    listHeader->AddStretchSpacer();
    listHeader->Add(_sortByIdToggle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    listHeader->Add(_sortByRecentToggle, 0, wxALIGN_CENTER_VERTICAL);
    root->AddSpacer(FromDIP(16));
    root->Add(listHeader, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    _statusLabel = new wxStaticText(this, wxID_ANY, wxEmptyString);
    _statusLabel->SetFont(Label::Body_13);
    _statusLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
    root->AddSpacer(FromDIP(8));
    root->Add(_statusLabel, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    _listPanel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    _listPanel->SetBackgroundColour(background);
    _listPanel->SetScrollRate(0, FromDIP(12));
    _listPanel->SetSizer(new wxBoxSizer(wxVERTICAL));
    _listPanel->SetMinSize(wxSize(contentWidth, FromDIP(50) * 6));
    root->Add(_listPanel, 1, wxEXPAND | wxLEFT | wxRIGHT, margin);

    OtherColorsPanel* otherColors = new OtherColorsPanel(this, wxSize(contentWidth, FromDIP(40)));
    otherColors->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { EndModal(wxID_MORE); });
    root->AddSpacer(FromDIP(16));
    root->Add(otherColors, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    root->AddSpacer(FromDIP(16));
    wxPanel* divider = new wxPanel(this, wxID_ANY);
    divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    divider->SetMinSize(wxSize(dialogWidth, FromDIP(1)));
    root->Add(divider, 0, wxEXPAND);

    const wxSize buttonSize(FromDIP(206), FromDIP(38));
    Button* cancel = new Button(this, _L("Cancel"), wxEmptyString, wxBORDER_NONE, 0, wxID_CANCEL);
    cancel->SetMinSize(buttonSize);
    cancel->SetCornerRadius(FromDIP(4));
    cancel->SetBorderWidth(FromDIP(1));
    cancel->SetBackgroundColorNormal(wxColour("#FFFFFF"));
    cancel->SetBorderColorNormal(wxColour("#D1D5DC"));
    cancel->SetTextColorNormal(wxColour("#242424"));
    cancel->SetFont(DialogFont(Label::Body_14, wxFONTWEIGHT_MEDIUM));
    Button* ok = new Button(this, _L("OK"), wxEmptyString, wxBORDER_NONE, 0, wxID_OK);
    ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    ok->SetMinSize(buttonSize);
    ok->SetCornerRadius(FromDIP(4));
    ok->SetBackgroundColorNormal(AccentColour);
    ok->SetBorderColorNormal(AccentColour);
    ok->SetTextColorNormal(wxColour("#FFFFFF"));
    ok->SetFont(DialogFont(Label::Body_14, wxFONTWEIGHT_MEDIUM));
    _okButton = ok;

    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        if (_selection.id > 0)
            EndModal(wxID_OK);
    });

    wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    buttons->Add(ok, 0, wxALIGN_CENTER_VERTICAL);
    root->AddSpacer(FromDIP(12));
    root->Add(buttons, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(20));
    root->AddSpacer(FromDIP(12));

    SetSizer(root);
    root->SetSizeHints(this);
    SetMinClientSize(wxSize(dialogWidth, root->CalcMin().GetHeight()));
    Fit();
}

void SpoolmanDialog::LoadSpools()
{
    if (_request)
        _request->cancel();

    ShowStatus(wxString::Format(_L("Loading spools from %s..."), FromUtf8(_baseUrl)));

    std::weak_ptr<AliveToken> alive = _alive;
    _request = SpoolmanClient::FetchSpools(
        _baseUrl,
        [alive](std::vector<SpoolmanSpool> spools)
        {
            wxGetApp().CallAfter([alive, spools]()
            {
                if (std::shared_ptr<AliveToken> token = alive.lock(); token && token->dialog != nullptr)
                    token->dialog->OnSpoolsLoaded(spools);
            });
        },
        [alive](std::string error)
        {
            wxGetApp().CallAfter([alive, error]()
            {
                if (std::shared_ptr<AliveToken> token = alive.lock(); token && token->dialog != nullptr)
                    token->dialog->OnLoadFailed(error);
            });
        });
}

void SpoolmanDialog::OnSpoolsLoaded(std::vector<SpoolmanSpool> spools)
{
    _request.reset();
    _spools = std::move(spools);
    RebuildRows();

    // Bring the spool already assigned to this filament into view.
    for (SpoolmanSpoolRow* row : _rows)
        if (row->Spool().id == _selection.id && _selection.id > 0 && row->IsShown())
        {
            int unitX = 0;
            int unitY = 1;
            _listPanel->GetScrollPixelsPerUnit(&unitX, &unitY);
            _listPanel->Scroll(0, row->GetPosition().y / std::max(1, unitY));
            break;
        }
}

void SpoolmanDialog::RebuildRows()
{
    SortSpoolmanSpools(_spools, _sortOrder);

    wxWindowUpdateLocker noUpdates(_listPanel);
    _listPanel->GetSizer()->Clear(true);
    _rows.clear();
    _rows.reserve(_spools.size());
    for (const SpoolmanSpool& spool : _spools)
    {
        SpoolmanSpoolRow* row = new SpoolmanSpoolRow(_listPanel, spool);
        const int spoolId = spool.id;
        row->Bind(wxEVT_LEFT_UP, [this, spoolId](wxMouseEvent&) { SelectSpool(spoolId); });
        row->Bind(wxEVT_LEFT_DCLICK, [this, spoolId](wxMouseEvent&)
        {
            SelectSpool(spoolId);
            EndModal(wxID_OK);
        });
        _listPanel->GetSizer()->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
        _rows.emplace_back(row);
    }

    SelectSpool(_selectedSpoolId);
    ApplyFilter();
    _listPanel->Scroll(0, 0);
}

void SpoolmanDialog::SetSortOrder(SpoolmanSortOrder order)
{
    if (_sortOrder == order)
        return;
    _sortOrder = order;
    if (wxGetApp().app_config != nullptr)
        wxGetApp().app_config->set(SortConfigKey, order == SpoolmanSortOrder::RecentlyUsed ? "recent" : "id");
    UpdateToolbar();
    if (!_spools.empty())
        RebuildRows();
}

void SpoolmanDialog::UpdateToolbar()
{
    _sortByIdToggle->SetActive(_sortOrder == SpoolmanSortOrder::Id);
    _sortByRecentToggle->SetActive(_sortOrder == SpoolmanSortOrder::RecentlyUsed);

    // Highlighted while the list is narrowed to the filament's type.
    const wxString type = FromUtf8(_filamentType);
    _typeFilterToggle->SetActive(!_showAllTypes);
    _typeFilterToggle->SetToolTip(_showAllTypes ? wxString::Format(_L("Showing all filament types. Click to show only %s spools."), type)
                                                : wxString::Format(_L("Showing only %s spools. Click to show all filament types."), type));
}

void SpoolmanDialog::OnLoadFailed(const std::string& error)
{
    _request.reset();
    ShowStatus(wxString::Format(_L("Could not load spools from %s: %s\nCheck the Spoolman address in Preferences."),
                                FromUtf8(_baseUrl), FromUtf8(error)));
}

void SpoolmanDialog::ApplyFilter()
{
    if (_rows.empty())
    {
        if (_request == nullptr)
            ShowStatus(_L("Spoolman has no active spools."));
        return;
    }

    const std::string filter = into_u8(_searchInput->GetTextCtrl()->GetValue());
    const bool filterByType = !_showAllTypes && !_filamentType.empty();
    size_t visible = 0;
    size_t otherTypes = 0;
    for (SpoolmanSpoolRow* row : _rows)
    {
        const bool typeMatch = !filterByType || SpoolmanMaterialMatches(row->Spool().material, _filamentType);
        const bool searchMatch = SpoolmanSpoolMatches(row->Spool(), filter);
        row->Show(typeMatch && searchMatch);
        visible += typeMatch && searchMatch ? 1 : 0;
        otherTypes += !typeMatch && searchMatch ? 1 : 0;
    }

    const wxString type = FromUtf8(_filamentType);
    wxString status;
    if (visible == 0 && otherTypes > 0)
        status = wxString::Format(_L("No %s spools found. %d spools of other types are hidden; use the filter button to show them."),
                                  type, static_cast<int>(otherTypes));
    else if (visible == 0)
        status = _L("No spools match the search.");
    else if (otherTypes > 0)
        status = wxString::Format(_L("Showing %s spools, %d of other types hidden."), type, static_cast<int>(otherTypes));
    ShowStatus(status);
    _listPanel->FitInside();
    _listPanel->Layout();
}

void SpoolmanDialog::SelectSpool(int spoolId)
{
    _selectedSpoolId = spoolId;
    _selection = SpoolmanSpool();
    for (SpoolmanSpoolRow* row : _rows)
    {
        const bool selected = spoolId > 0 && row->Spool().id == spoolId;
        row->SetSelected(selected);
        if (selected)
            _selection = row->Spool();
    }
    UpdatePreview();
}

void SpoolmanDialog::UpdatePreview()
{
    const bool hasSelection = _selection.id > 0;
    _previewBitmap->SetBitmap(MakePreviewBitmap(hasSelection ? SpoolColour(_selection) : _currentColor, FromDIP(60)));
    _nameLabel->SetLabel(hasSelection ? FromUtf8(_selection.DisplayName()) : _L("No spool selected"));
    _detailLabel->SetLabel(hasSelection ? SpoolDetails(_selection, true) : _L("Pick a spool to use its color for this filament."));
    if (_okButton != nullptr)
        _okButton->Enable(hasSelection);
    Layout();
}

void SpoolmanDialog::ShowStatus(const wxString& status)
{
    if (_statusLabel->GetLabel() == status && _statusLabel->IsShown() == !status.empty())
        return;
    _statusLabel->SetLabel(status);
    _statusLabel->Wrap(std::max(FromDIP(200), GetClientSize().GetWidth() - FromDIP(32)));
    _statusLabel->Show(!status.empty());
    Layout();
}

void SpoolmanDialog::on_dpi_changed(const wxRect&)
{
    for (SpoolmanSpoolRow* row : _rows)
        row->SetMinSize(wxSize(-1, FromDIP(48)));
    for (SpoolmanIconToggle* toggle : { _typeFilterToggle, _sortByIdToggle, _sortByRecentToggle, _refreshButton, _openButton })
        toggle->Rescale();
    UpdatePreview();
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
