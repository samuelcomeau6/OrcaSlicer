#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/Spoolman.hpp"

#include <memory>
#include <string>
#include <vector>

class wxScrolledWindow;
class wxStaticBitmap;
class wxStaticText;
class TextInput;

namespace Slic3r
{

class Http;

namespace GUI
{

class SpoolmanSpoolRow;
class SpoolmanIconToggle;

/**
 * @brief Dialog for assigning a Spoolman spool to a filament slot.
 *
 * Styled after FilamentColorDialog. By default only spools whose material matches
 * @p filamentType are listed; a filter toggle shows every type. ShowModal() returns
 * wxID_OK when a spool was chosen, wxID_MORE when the user asked for the regular
 * color picker instead and wxID_CANCEL otherwise.
 */
class SpoolmanDialog : public DPIDialog
{
public:
    SpoolmanDialog(wxWindow* parent, const std::string& baseUrl, const std::string& filamentType, int currentSpoolId,
                   const FilamentColor& currentColor);
    ~SpoolmanDialog() override;

    /**
     * @brief The chosen spool; only meaningful after ShowModal() returned wxID_OK.
     */
    const SpoolmanSpool& Selection() const { return _selection; }

private:
    void BuildUi();
    void LoadSpools();
    void OnSpoolsLoaded(std::vector<SpoolmanSpool> spools);
    void OnLoadFailed(const std::string& error);
    void RebuildRows();
    void SetSortOrder(SpoolmanSortOrder order);
    void UpdateToolbar();
    void ApplyFilter();
    void SelectSpool(int spoolId);
    void UpdatePreview();
    void ShowStatus(const wxString& status);
    void on_dpi_changed(const wxRect& suggestedRect) override;

private:
    // Lets background HTTP callbacks find out whether the dialog still exists.
    struct AliveToken
    {
        SpoolmanDialog* dialog { nullptr };
    };

    std::string _baseUrl;
    std::string _filamentType;
    bool _showAllTypes { false };
    SpoolmanSortOrder _sortOrder { SpoolmanSortOrder::Id };
    int _selectedSpoolId { 0 };
    FilamentColor _currentColor;
    SpoolmanSpool _selection;
    std::vector<SpoolmanSpool> _spools;
    std::vector<SpoolmanSpoolRow*> _rows;
    std::shared_ptr<AliveToken> _alive;
    std::shared_ptr<Http> _request;

    wxStaticBitmap* _previewBitmap { nullptr };
    wxStaticText* _nameLabel { nullptr };
    wxStaticText* _detailLabel { nullptr };
    TextInput* _searchInput { nullptr };
    SpoolmanIconToggle* _typeFilterToggle { nullptr };
    SpoolmanIconToggle* _sortByIdToggle { nullptr };
    SpoolmanIconToggle* _sortByRecentToggle { nullptr };
    SpoolmanIconToggle* _refreshButton { nullptr };
    SpoolmanIconToggle* _openButton { nullptr };
    wxScrolledWindow* _listPanel { nullptr };
    wxStaticText* _statusLabel { nullptr };
    wxWindow* _okButton { nullptr };
};

} // namespace GUI
} // namespace Slic3r
