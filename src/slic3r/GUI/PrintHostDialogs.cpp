///|/ Copyright (c) Prusa Research 2018 - 2023 Oleksandra Iushchenko @YuSanka, Lukáš Matěna @lukasmatena, David Kocík @kocikdav, Vojtěch Bubník @bubnikv, Lukáš Hejl @hejllukas, Enrico Turri @enricoturri1966, Vojtěch Král @vojtechkral
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "PrintHostDialogs.hpp"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>

#include <wx/frame.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/listbox.h>
#include <wx/spinctrl.h>
#include <wx/dataview.h>
#include <wx/wupdlock.h>
#include <wx/debug.h>
#include <wx/msgdlg.h>

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "NotificationManager.hpp"
#include "ExtraRenderers.hpp"
#include "format.hpp"
#include "../Utils/BambuLan.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

static const char *CONFIG_KEY_PATH  = "printhost_path";
static const char *CONFIG_KEY_GROUP = "printhost_group";
static const char* CONFIG_KEY_STORAGE = "printhost_storage";
static const char *CONFIG_KEY_BAMBU_USE_AMS = "bambu_lan_use_ams";
static const char *CONFIG_KEY_BAMBU_AMS_MAPPING = "bambu_lan_ams_mapping";
static const char *CONFIG_KEY_BAMBU_BED_TYPE = "bambu_lan_bed_type";
static const char *CONFIG_KEY_BAMBU_BED_LEVELING = "bambu_lan_bed_leveling";
static const char *CONFIG_KEY_BAMBU_FLOW_CALI = "bambu_lan_flow_cali";
static const char *CONFIG_KEY_BAMBU_VIBRATION_CALI = "bambu_lan_vibration_cali";
static const char *CONFIG_KEY_BAMBU_LAYER_INSPECT = "bambu_lan_layer_inspect";
static const char *CONFIG_KEY_BAMBU_TIMELAPSE = "bambu_lan_timelapse";

PrintHostSendDialog::PrintHostSendDialog(const fs::path &path, PrintHostPostUploadActions post_actions, const wxArrayString &groups, const wxArrayString& storage_paths, const wxArrayString& storage_names, bool bambu_lan)
    : MsgDialog(static_cast<wxWindow*>(wxGetApp().mainframe), _L("Send G-Code to printer host"), _L("Upload to Printer Host with the following filename:"), 0) // Set style = 0 to avoid default creation of the "OK" button. 
                                                                                                                                                               // All buttons will be added later in this constructor 
    , txt_filename(new wxTextCtrl(this, wxID_ANY))
    , combo_groups(!groups.IsEmpty() ? new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, groups, wxCB_READONLY) : nullptr)
    , combo_storage(storage_names.GetCount() > 1 ? new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, storage_names, wxCB_READONLY) : nullptr)
    , post_upload_action(PrintHostPostUploadAction::None)
    , m_paths(storage_paths)
    , m_bambu_lan(bambu_lan)
{
#ifdef __APPLE__
    txt_filename->OSXDisableAllSmartSubstitutions();
#endif
    const AppConfig *app_config = wxGetApp().app_config;

    auto *label_dir_hint = new wxStaticText(this, wxID_ANY, _L("Use forward slashes ( / ) as a directory separator if needed."));
    label_dir_hint->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());

    content_sizer->Add(txt_filename, 0, wxEXPAND);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);
    
    if (combo_groups != nullptr) {
        // Repetier specific: Show a selection of file groups.
        auto *label_group = new wxStaticText(this, wxID_ANY, _L("Group"));
        content_sizer->Add(label_group);
        content_sizer->Add(combo_groups, 0, wxBOTTOM, 2*VERT_SPACING);        
        wxString recent_group = from_u8(app_config->get("recent", CONFIG_KEY_GROUP));
        if (! recent_group.empty())
            combo_groups->SetValue(recent_group);
    }

    if (combo_storage != nullptr) {
        // PrusaLink specific: User needs to choose a storage
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ":");
        content_sizer->Add(label_group);
        content_sizer->Add(combo_storage, 0, wxBOTTOM, 2 * VERT_SPACING);
        combo_storage->SetValue(storage_names.front());
        wxString recent_storage = from_u8(app_config->get("recent", CONFIG_KEY_STORAGE));
        if (!recent_storage.empty())
            combo_storage->SetValue(recent_storage); 
    } else if (storage_names.GetCount() == 1){
        // PrusaLink specific: Show which storage has been detected.
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ": " + storage_names.front());
        content_sizer->Add(label_group);
        m_preselected_storage = storage_paths.front();
    }

    if (m_bambu_lan) {
        auto *label_bambu = new wxStaticText(this, wxID_ANY, _L("Bambu Lab print options"));
        label_bambu->SetFont(label_bambu->GetFont().Bold());
        content_sizer->Add(label_bambu, 0, wxTOP, VERT_SPACING);

        bambu_use_ams = new wxCheckBox(this, wxID_ANY, _L("Use AMS"));
        bambu_use_ams->SetValue(app_config->get_bool("recent", CONFIG_KEY_BAMBU_USE_AMS));
        content_sizer->Add(bambu_use_ams, 0, wxTOP, VERT_SPACING);

        auto *label_ams_mapping = new wxStaticText(this, wxID_ANY, _L("AMS mapping"));
        bambu_ams_mapping = new wxTextCtrl(this, wxID_ANY, from_u8(app_config->get("recent", CONFIG_KEY_BAMBU_AMS_MAPPING)));
        bambu_ams_mapping->SetToolTip(_L("Optional raw AMS mapping array, for example [0,1,2,3]. Leave empty for the printer default."));
        content_sizer->Add(label_ams_mapping, 0, wxTOP, VERT_SPACING);
        content_sizer->Add(bambu_ams_mapping, 0, wxEXPAND);

        wxArrayString bed_choices;
        bed_choices.Add(_L("Printer default"));
        bed_choices.Add(_L("Textured PEI Plate"));
        bed_choices.Add(_L("Cool Plate"));
        bed_choices.Add(_L("Engineering Plate"));
        bed_choices.Add(_L("High Temperature Plate"));
        bambu_bed_type = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, bed_choices);
        long bed_selection = 0;
        const std::string recent_bed_type = app_config->get("recent", CONFIG_KEY_BAMBU_BED_TYPE);
        if (!recent_bed_type.empty())
            from_u8(recent_bed_type).ToLong(&bed_selection);
        if (bed_selection < 0 || bed_selection >= long(bed_choices.size()))
            bed_selection = 0;
        bambu_bed_type->SetSelection(int(bed_selection));
        content_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Build plate")), 0, wxTOP, VERT_SPACING);
        content_sizer->Add(bambu_bed_type, 0, wxEXPAND);

        bambu_bed_leveling = new wxCheckBox(this, wxID_ANY, _L("Bed leveling"));
        bambu_flow_cali = new wxCheckBox(this, wxID_ANY, _L("Flow calibration"));
        bambu_vibration_cali = new wxCheckBox(this, wxID_ANY, _L("Vibration calibration"));
        bambu_layer_inspect = new wxCheckBox(this, wxID_ANY, _L("First layer inspection"));
        bambu_timelapse = new wxCheckBox(this, wxID_ANY, _L("Timelapse"));
        bambu_bed_leveling->SetValue(app_config->get("recent", CONFIG_KEY_BAMBU_BED_LEVELING).empty() ? true : app_config->get_bool("recent", CONFIG_KEY_BAMBU_BED_LEVELING));
        bambu_flow_cali->SetValue(app_config->get_bool("recent", CONFIG_KEY_BAMBU_FLOW_CALI));
        bambu_vibration_cali->SetValue(app_config->get_bool("recent", CONFIG_KEY_BAMBU_VIBRATION_CALI));
        bambu_layer_inspect->SetValue(app_config->get_bool("recent", CONFIG_KEY_BAMBU_LAYER_INSPECT));
        bambu_timelapse->SetValue(app_config->get_bool("recent", CONFIG_KEY_BAMBU_TIMELAPSE));
        content_sizer->Add(bambu_bed_leveling, 0, wxTOP, VERT_SPACING);
        content_sizer->Add(bambu_flow_cali);
        content_sizer->Add(bambu_vibration_cali);
        content_sizer->Add(bambu_layer_inspect);
        content_sizer->Add(bambu_timelapse, 0, wxBOTTOM, 2 * VERT_SPACING);
    }


    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/') {
        recent_path += '/';
    }
    const auto recent_path_len = recent_path.Length();
    recent_path += path.filename().wstring();
    wxString stem(path.stem().wstring());
    const auto stem_len = stem.Length();

    txt_filename->SetValue(recent_path);

    if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
        m_valid_suffix = recent_path.substr(extension_start);
    // .gcode suffix control
    auto validate_path = [this](const wxString &path) -> bool {
        if (!path.Lower().EndsWith(m_valid_suffix.Lower())) {
            MessageDialog msg_wingow(this, wxString::Format(_L("Upload filename doesn't end with \"%s\". Do you wish to continue?"), m_valid_suffix), wxString(SLIC3R_APP_NAME), wxYES | wxNO);
            return msg_wingow.ShowModal() == wxID_YES;
        }
        return true;
    };

    auto* btn_ok = add_button(wxID_OK, true, _L("Upload"));
    btn_ok->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
        if (validate_path(txt_filename->GetValue())) {
            post_upload_action = PrintHostPostUploadAction::None;
            EndDialog(wxID_OK);
        }
    });
    txt_filename->SetFocus();
    
    if (post_actions.has(PrintHostPostUploadAction::QueuePrint)) {
        auto* btn_print = add_button(wxID_ADD, false, _L("Upload to Queue"));
        btn_print->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
            if (validate_path(txt_filename->GetValue())) {
                post_upload_action = PrintHostPostUploadAction::QueuePrint;
                EndDialog(wxID_OK);
            }
            });
    }

    if (post_actions.has(PrintHostPostUploadAction::StartPrint)) {
        auto* btn_print = add_button(wxID_YES, false, _L("Upload and Print"));
        btn_print->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
            if (validate_path(txt_filename->GetValue())) {
                post_upload_action = PrintHostPostUploadAction::StartPrint;
                EndDialog(wxID_OK);
            }
        });
    }

    if (post_actions.has(PrintHostPostUploadAction::StartSimulation)) {
        // Using wxID_MORE as a button identifier to be different from the other buttons, wxID_MORE has no other meaning here.
        auto* btn_simulate = add_button(wxID_MORE, false, _L("Upload and Simulate"));
        btn_simulate->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
            if (validate_path(txt_filename->GetValue())) {
                post_upload_action = PrintHostPostUploadAction::StartSimulation;
                EndDialog(wxID_OK);
            }        
        });
    }

    add_button(wxID_CANCEL);
    finalize();

#ifdef __linux__
    // On Linux with GTK2 when text control lose the focus then selection (colored background) disappears but text color stay white
    // and as a result the text is invisible with light mode
    // see https://github.com/prusa3d/PrusaSlicer/issues/4532
    // Workaround: Unselect text selection explicitly on kill focus
    txt_filename->Bind(wxEVT_KILL_FOCUS, [this](wxEvent& e) {
        e.Skip();
        txt_filename->SetInsertionPoint(txt_filename->GetLastPosition());
    }, txt_filename->GetId());
#endif /* __linux__ */

    Bind(wxEVT_SHOW, [=](const wxShowEvent &) {
        // Another similar case where the function only works with EVT_SHOW + CallAfter,
        // this time on Mac.
        CallAfter([=]() {
            txt_filename->SetInsertionPoint(0);
            txt_filename->SetSelection(recent_path_len, recent_path_len + stem_len);
        });
    });
}

fs::path PrintHostSendDialog::filename() const
{
    return into_path(txt_filename->GetValue());
}

PrintHostPostUploadAction PrintHostSendDialog::post_action() const
{
    return post_upload_action;
}

std::string PrintHostSendDialog::group() const
{
     if (combo_groups == nullptr) {
         return "";
     } else {
         wxString group = combo_groups->GetValue();
         return into_u8(group);
    }
}

std::string PrintHostSendDialog::storage() const
{
    if (!combo_storage)
        return GUI::format("%1%", m_preselected_storage);
    if (combo_storage->GetSelection() < 0 || combo_storage->GetSelection() >= int(m_paths.size()))
        return {};
    return into_u8(m_paths[combo_storage->GetSelection()]);
}

std::string PrintHostSendDialog::data_json() const
{
    if (!m_bambu_lan)
        return {};

    std::ostringstream out;
    out << "bambu_use_ams=" << (bambu_use_ams != nullptr && bambu_use_ams->GetValue() ? "1" : "0") << '\n';
    out << "bambu_ams_mapping=" << (bambu_ams_mapping != nullptr ? into_u8(bambu_ams_mapping->GetValue()) : "") << '\n';
    out << "bambu_bed_type=" << (bambu_bed_type != nullptr ? bambu_bed_type->GetSelection() : 0) << '\n';
    out << "bambu_bed_leveling=" << (bambu_bed_leveling != nullptr && bambu_bed_leveling->GetValue() ? "1" : "0") << '\n';
    out << "bambu_flow_cali=" << (bambu_flow_cali != nullptr && bambu_flow_cali->GetValue() ? "1" : "0") << '\n';
    out << "bambu_vibration_cali=" << (bambu_vibration_cali != nullptr && bambu_vibration_cali->GetValue() ? "1" : "0") << '\n';
    out << "bambu_layer_inspect=" << (bambu_layer_inspect != nullptr && bambu_layer_inspect->GetValue() ? "1" : "0") << '\n';
    out << "bambu_timelapse=" << (bambu_timelapse != nullptr && bambu_timelapse->GetValue() ? "1" : "0") << '\n';
    return out.str();
}

void PrintHostSendDialog::EndModal(int ret)
{
    if (ret == wxID_OK) {
        // Persist path and print settings
        wxString path = txt_filename->GetValue();
        int last_slash = path.Find('/', true);
		if (last_slash == wxNOT_FOUND)
			path.clear();
		else
            path = path.SubString(0, last_slash);
                
		AppConfig *app_config = wxGetApp().app_config;
		app_config->set("recent", CONFIG_KEY_PATH, into_u8(path));

        if (combo_groups != nullptr) {
            wxString group = combo_groups->GetValue();
            app_config->set("recent", CONFIG_KEY_GROUP, into_u8(group));
        }
        if (combo_storage != nullptr) {
            wxString storage = combo_storage->GetValue();
            app_config->set("recent", CONFIG_KEY_STORAGE, into_u8(storage));
        }
        if (m_bambu_lan) {
            app_config->set("recent", CONFIG_KEY_BAMBU_USE_AMS, bambu_use_ams != nullptr && bambu_use_ams->GetValue() ? "1" : "0");
            app_config->set("recent", CONFIG_KEY_BAMBU_AMS_MAPPING, bambu_ams_mapping != nullptr ? into_u8(bambu_ams_mapping->GetValue()) : "");
            app_config->set("recent", CONFIG_KEY_BAMBU_BED_TYPE, std::to_string(bambu_bed_type != nullptr ? bambu_bed_type->GetSelection() : 0));
            app_config->set("recent", CONFIG_KEY_BAMBU_BED_LEVELING, bambu_bed_leveling != nullptr && bambu_bed_leveling->GetValue() ? "1" : "0");
            app_config->set("recent", CONFIG_KEY_BAMBU_FLOW_CALI, bambu_flow_cali != nullptr && bambu_flow_cali->GetValue() ? "1" : "0");
            app_config->set("recent", CONFIG_KEY_BAMBU_VIBRATION_CALI, bambu_vibration_cali != nullptr && bambu_vibration_cali->GetValue() ? "1" : "0");
            app_config->set("recent", CONFIG_KEY_BAMBU_LAYER_INSPECT, bambu_layer_inspect != nullptr && bambu_layer_inspect->GetValue() ? "1" : "0");
            app_config->set("recent", CONFIG_KEY_BAMBU_TIMELAPSE, bambu_timelapse != nullptr && bambu_timelapse->GetValue() ? "1" : "0");
        }
    }

    MsgDialog::EndModal(ret);
}

wxDEFINE_EVENT(EVT_PRINTHOST_PROGRESS, PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_ERROR,    PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_CANCEL,   PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_INFO,  PrintHostQueueDialog::Event);

class BambuLanControlDialog : public DPIDialog
{
public:
    BambuLanControlDialog(wxWindow *parent, DynamicPrintConfig *config)
        : DPIDialog(parent, wxID_ANY, _L("Bambu Lab LAN Control"), wxDefaultPosition, wxSize(760, 560), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_host(std::make_unique<BambuLan>(config))
    {
        auto *topsizer = new wxBoxSizer(wxVERTICAL);

        auto *button_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *btn_refresh_status = new wxButton(this, wxID_ANY, _L("Refresh Status"));
        auto *btn_refresh_files = new wxButton(this, wxID_ANY, _L("Refresh Files"));
        auto *btn_pause = new wxButton(this, wxID_ANY, _L("Pause"));
        auto *btn_resume = new wxButton(this, wxID_ANY, _L("Resume"));
        auto *btn_stop = new wxButton(this, wxID_ANY, _L("Stop"));
        button_sizer->Add(btn_refresh_status, 0, wxRIGHT, 6);
        button_sizer->Add(btn_refresh_files, 0, wxRIGHT, 6);
        button_sizer->AddStretchSpacer();
        button_sizer->Add(btn_pause, 0, wxRIGHT, 6);
        button_sizer->Add(btn_resume, 0, wxRIGHT, 6);
        button_sizer->Add(btn_stop, 0);

        m_status = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        m_files = new wxListBox(this, wxID_ANY);

        auto *temp_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_nozzle_temp = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1), wxSP_ARROW_KEYS, 0, 360, 220);
        m_bed_temp = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1), wxSP_ARROW_KEYS, 0, 120, 60);
        m_chamber_temp = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(80, -1), wxSP_ARROW_KEYS, 0, 65, 0);
        auto *btn_nozzle_temp = new wxButton(this, wxID_ANY, _L("Set Nozzle"));
        auto *btn_bed_temp = new wxButton(this, wxID_ANY, _L("Set Bed"));
        auto *btn_chamber_temp = new wxButton(this, wxID_ANY, _L("Set Chamber"));
        temp_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Nozzle")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        temp_sizer->Add(m_nozzle_temp, 0, wxRIGHT, 4);
        temp_sizer->Add(btn_nozzle_temp, 0, wxRIGHT, 10);
        temp_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Bed")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        temp_sizer->Add(m_bed_temp, 0, wxRIGHT, 4);
        temp_sizer->Add(btn_bed_temp, 0, wxRIGHT, 10);
        temp_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Chamber")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        temp_sizer->Add(m_chamber_temp, 0, wxRIGHT, 4);
        temp_sizer->Add(btn_chamber_temp, 0);

        auto *speed_sizer = new wxBoxSizer(wxHORIZONTAL);
        wxArrayString speed_choices;
        speed_choices.Add(_L("Silent"));
        speed_choices.Add(_L("Standard"));
        speed_choices.Add(_L("Sport"));
        speed_choices.Add(_L("Ludicrous"));
        m_speed = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, speed_choices);
        m_speed->SetSelection(1);
        auto *btn_speed = new wxButton(this, wxID_ANY, _L("Set Speed"));
        speed_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Print speed")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        speed_sizer->Add(m_speed, 0, wxRIGHT, 4);
        speed_sizer->Add(btn_speed, 0);
        speed_sizer->AddStretchSpacer();

        auto *gcode_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_gcode = new wxTextCtrl(this, wxID_ANY);
        auto *btn_gcode = new wxButton(this, wxID_ANY, _L("Send G-code"));
        gcode_sizer->Add(m_gcode, 1, wxRIGHT, 4);
        gcode_sizer->Add(btn_gcode, 0);

        auto *file_button_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto *btn_print_file = new wxButton(this, wxID_ANY, _L("Print Selected File"));
        auto *btn_delete_file = new wxButton(this, wxID_ANY, _L("Delete Selected File"));
        file_button_sizer->Add(btn_print_file, 0, wxRIGHT, 6);
        file_button_sizer->Add(btn_delete_file, 0);

        topsizer->Add(button_sizer, 0, wxEXPAND | wxALL, 10);
        topsizer->Add(temp_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        topsizer->Add(speed_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        topsizer->Add(gcode_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        topsizer->Add(new wxStaticText(this, wxID_ANY, _L("Status JSON")), 0, wxLEFT | wxRIGHT, 10);
        topsizer->Add(m_status, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        topsizer->Add(new wxStaticText(this, wxID_ANY, _L("SD card files")), 0, wxLEFT | wxRIGHT, 10);
        topsizer->Add(m_files, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        topsizer->Add(file_button_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        auto *close_sizer = new wxBoxSizer(wxHORIZONTAL);
        close_sizer->AddStretchSpacer();
        close_sizer->Add(new wxButton(this, wxID_CANCEL, _L("Close")));
        topsizer->Add(close_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        SetSizer(topsizer);
        Layout();
        CentreOnParent();

        btn_refresh_status->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_status(); });
        btn_refresh_files->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_files(); });
        btn_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_print_command("pause"); });
        btn_resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_print_command("resume"); });
        btn_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (wxMessageBox(_L("Stop the active print?"), _L("Bambu Lab LAN Control"), wxYES_NO | wxICON_WARNING, this) == wxYES)
                send_print_command("stop");
        });
        btn_print_file->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { print_selected_file(); });
        btn_delete_file->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { delete_selected_file(); });
        btn_nozzle_temp->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_nozzle_temp(); });
        btn_bed_temp->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_bed_temp(); });
        btn_chamber_temp->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_chamber_temp(); });
        btn_speed->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { set_speed(); });
        btn_gcode->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_gcode(); });
    }

private:
    void on_dpi_changed(const wxRect &suggested_rect) override
    {
        SetSize(suggested_rect.GetSize());
        Layout();
    }

    void show_error_message(const std::string &error)
    {
        show_error(this, wxString::FromUTF8(error.c_str()));
    }

    std::string selected_file() const
    {
        const int selection = m_files->GetSelection();
        return selection == wxNOT_FOUND ? std::string() : into_u8(m_files->GetString(selection));
    }

    void refresh_status()
    {
        wxBusyCursor wait;
        std::string status;
        std::string error;
        if (!m_host->request_status(status, error)) {
            show_error_message(error);
            return;
        }
        m_status->SetValue(wxString::FromUTF8(status.c_str()));
    }

    void refresh_files()
    {
        wxBusyCursor wait;
        std::vector<std::string> files;
        std::string error;
        if (!m_host->list_sdcard(files, error)) {
            show_error_message(error);
            return;
        }
        m_files->Clear();
        for (const std::string &file : files)
            m_files->Append(wxString::FromUTF8(file.c_str()));
    }

    void send_print_command(const std::string &command)
    {
        wxBusyCursor wait;
        std::string error;
        if (!m_host->send_print_command(command, error)) {
            show_error_message(error);
            return;
        }
        refresh_status();
    }

    void run_control_command(const std::function<bool(std::string&)> &fn)
    {
        wxBusyCursor wait;
        std::string error;
        if (!fn(error)) {
            show_error_message(error);
            return;
        }
        refresh_status();
    }

    void set_nozzle_temp()
    {
        run_control_command([this](std::string &error) { return m_host->set_nozzle_temp(m_nozzle_temp->GetValue(), error); });
    }

    void set_bed_temp()
    {
        run_control_command([this](std::string &error) { return m_host->set_bed_temp(m_bed_temp->GetValue(), error); });
    }

    void set_chamber_temp()
    {
        run_control_command([this](std::string &error) { return m_host->set_chamber_temp(m_chamber_temp->GetValue(), error); });
    }

    void set_speed()
    {
        run_control_command([this](std::string &error) { return m_host->set_print_speed(m_speed->GetSelection() + 1, error); });
    }

    void send_gcode()
    {
        const std::string gcode = into_u8(m_gcode->GetValue());
        if (gcode.empty())
            return;
        run_control_command([this, &gcode](std::string &error) { return m_host->send_gcode_line(gcode, error); });
    }

    void print_selected_file()
    {
        const std::string file = selected_file();
        if (file.empty())
            return;
        if (wxMessageBox(wxString::Format(_L("Start printing %s?"), wxString::FromUTF8(file.c_str())), _L("Bambu Lab LAN Control"), wxYES_NO | wxICON_QUESTION, this) != wxYES)
            return;

        wxBusyCursor wait;
        std::string error;
        if (!m_host->start_sdcard_file(file, std::string(), error)) {
            show_error_message(error);
            return;
        }
        refresh_status();
    }

    void delete_selected_file()
    {
        const std::string file = selected_file();
        if (file.empty())
            return;
        if (wxMessageBox(wxString::Format(_L("Delete %s from the printer SD card?"), wxString::FromUTF8(file.c_str())), _L("Bambu Lab LAN Control"), wxYES_NO | wxICON_WARNING, this) != wxYES)
            return;

        wxBusyCursor wait;
        std::string error;
        if (!m_host->delete_sdcard_file(file, error)) {
            show_error_message(error);
            return;
        }
        refresh_files();
    }

    std::unique_ptr<BambuLan> m_host;
    wxTextCtrl *m_status { nullptr };
    wxListBox *m_files { nullptr };
    wxSpinCtrl *m_nozzle_temp { nullptr };
    wxSpinCtrl *m_bed_temp { nullptr };
    wxSpinCtrl *m_chamber_temp { nullptr };
    wxChoice *m_speed { nullptr };
    wxTextCtrl *m_gcode { nullptr };
};

void show_bambu_lan_control_dialog(wxWindow *parent)
{
    DynamicPrintConfig *config = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config();
    if (config == nullptr) {
        show_error(parent, _L("Select a physical printer configured as Bambu Lab LAN first."));
        return;
    }
    const auto *host_type = config->option<ConfigOptionEnum<PrintHostType>>("host_type");
    if (host_type == nullptr || host_type->value != htBambuLan) {
        show_error(parent, _L("The selected physical printer is not configured as Bambu Lab LAN."));
        return;
    }
    if (config->opt_string("print_host").empty() || config->opt_string("printhost_apikey").empty() || config->opt_string("printhost_user").empty()) {
        show_error(parent, _L("Bambu Lab LAN control requires Hostname/IP, API key/LAN access code, and Username/serial number."));
        return;
    }

    BambuLanControlDialog dlg(parent, config);
    dlg.ShowModal();
}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id)
    : wxEvent(winid, eventType)
    , job_id(job_id)
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, int progress)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , progress(progress)
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, wxString error)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , status(std::move(error))
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, wxString tag, wxString status)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , tag(std::move(tag))
    , status(std::move(status))
{}

wxEvent *PrintHostQueueDialog::Event::Clone() const
{
    return new Event(*this);
}

PrintHostQueueDialog::PrintHostQueueDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Print host upload queue"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , on_progress_evt(this, EVT_PRINTHOST_PROGRESS, &PrintHostQueueDialog::on_progress, this)
    , on_error_evt(this, EVT_PRINTHOST_ERROR, &PrintHostQueueDialog::on_error, this)
    , on_cancel_evt(this, EVT_PRINTHOST_CANCEL, &PrintHostQueueDialog::on_cancel, this)
    , on_info_evt(this, EVT_PRINTHOST_INFO, &PrintHostQueueDialog::on_info, this)
{
    const auto em = GetTextExtent("m").x;

    auto *topsizer = new wxBoxSizer(wxVERTICAL);

    std::vector<int> widths;
    widths.reserve(7);
    if (!load_user_data(UDT_COLS, widths)) {
        widths.clear();
        for (size_t i = 0; i < 7; i++)
            widths.push_back(-1);
    }

    job_list = new wxDataViewListCtrl(this, wxID_ANY);

    // MSW DarkMode: workaround for the selected item in the list
    auto append_text_column = [this](const wxString& label, int width, wxAlignment align = wxALIGN_LEFT,
                                     int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE) {
#ifdef _WIN32
            job_list->AppendColumn(new wxDataViewColumn(label, new TextRenderer(), job_list->GetColumnCount(), width, align, flags));
#else
            job_list->AppendTextColumn(label, wxDATAVIEW_CELL_INERT, width, align, flags);
#endif
    };

    // Note: Keep these in sync with Column
    append_text_column(_L("ID"), widths[0]);
    job_list->AppendProgressColumn(_L("Progress"),      wxDATAVIEW_CELL_INERT, widths[1], wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    append_text_column(_L("Status"),widths[2]);
    append_text_column(_L("Host"),  widths[3]);
    append_text_column(_CTX(L_CONTEXT("Size", "OfFile"), "OfFile"), widths[4]);
    append_text_column(_L("Filename"),      widths[5]);
    append_text_column(_L("Message"), widths[6]);
    //append_text_column(_L("Error Message"), -1, wxALIGN_CENTER, wxDATAVIEW_COL_HIDDEN);
 
    auto *btnsizer = new wxBoxSizer(wxHORIZONTAL);
    btn_cancel = new wxButton(this, wxID_DELETE, _L("Cancel selected"));
    btn_cancel->Disable();
    btn_error = new wxButton(this, wxID_ANY, _L("Show error message"));
    btn_error->Disable();
    // Note: The label needs to be present, otherwise we get accelerator bugs on Mac
    auto *btn_close = new wxButton(this, wxID_CANCEL, _L("Close"));
    btnsizer->Add(btn_cancel, 0, wxRIGHT, SPACING);
    btnsizer->Add(btn_error, 0);
    btnsizer->AddStretchSpacer();
    btnsizer->Add(btn_close);

    topsizer->Add(job_list, 1, wxEXPAND | wxBOTTOM, SPACING);
    topsizer->Add(btnsizer, 0, wxEXPAND);
    SetSizer(topsizer);

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(job_list);

    std::vector<int> size;
    SetSize(load_user_data(UDT_SIZE, size) ? wxSize(size[0] * em, size[1] * em) : wxSize(HEIGHT * em, WIDTH * em));

    Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        OnSize(evt); 
        save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
     });
    
    std::vector<int> pos;
    if (load_user_data(UDT_POSITION, pos))
        SetPosition(wxPoint(pos[0], pos[1]));

    Bind(wxEVT_MOVE, [this](wxMoveEvent& evt) {
        save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
    });

    job_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) { on_list_select(); });

    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int selected = job_list->GetSelectedRow();
        if (selected == wxNOT_FOUND) { return; }

        const JobState state = get_state(selected);
        if (state < ST_ERROR) {
            GUI::wxGetApp().printhost_job_queue().cancel(selected);
        }
    });

    btn_error->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int selected = job_list->GetSelectedRow();
        if (selected == wxNOT_FOUND) { return; }
        GUI::show_error(nullptr, job_list->GetTextValue(selected, COL_ERRORMSG));
    });
}

void PrintHostQueueDialog::append_job(const PrintHostJob &job)
{
    wxCHECK_RET(!job.empty(), "PrintHostQueueDialog: Attempt to append an empty job");

    wxVector<wxVariant> fields;
    fields.push_back(wxVariant(wxString::Format("%d", job_list->GetItemCount() + 1)));
    fields.push_back(wxVariant(0));
    fields.push_back(wxVariant(_L("Enqueued")));
    fields.push_back(wxVariant(job.printhost->get_host()));
    boost::system::error_code ec;
    boost::uintmax_t size_i = boost::filesystem::file_size(job.upload_data.source_path, ec);
    std::stringstream stream;
    if (ec) {
        stream << "unknown";
        size_i = 0;
        BOOST_LOG_TRIVIAL(error) << ec.message();
    } else 
        stream << std::fixed << std::setprecision(2) << ((float)size_i / 1024 / 1024) << "MB";
    fields.push_back(wxVariant(stream.str()));
    fields.push_back(wxVariant(from_path(job.upload_data.upload_path)));
    fields.push_back(wxVariant(""));
    job_list->AppendItem(fields, static_cast<wxUIntPtr>(ST_NEW));
    // Both strings are UTF-8 encoded.
    upload_names.emplace_back(job.printhost->get_host(), job.upload_data.upload_path.string());

    wxGetApp().notification_manager()->push_upload_job_notification(job_list->GetItemCount(), (float)size_i / 1024 / 1024, job.upload_data.upload_path.string(), job.printhost->get_notification_host());
}

void PrintHostQueueDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    const int& em = em_unit();

    msw_buttons_rescale(this, em, { wxID_DELETE, wxID_CANCEL, btn_error->GetId() });

    SetMinSize(wxSize(HEIGHT * em, WIDTH * em));

    Fit();
    Refresh();

    save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
}

void PrintHostQueueDialog::on_sys_color_changed()
{
#ifdef _WIN32
    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(job_list);
#endif
}

PrintHostQueueDialog::JobState PrintHostQueueDialog::get_state(int idx)
{
    wxCHECK_MSG(idx >= 0 && idx < job_list->GetItemCount(), ST_ERROR, "Out of bounds access to job list");
    return static_cast<JobState>(job_list->GetItemData(job_list->RowToItem(idx)));
}

void PrintHostQueueDialog::set_state(int idx, JobState state)
{
    wxCHECK_RET(idx >= 0 && idx < job_list->GetItemCount(), "Out of bounds access to job list");
    job_list->SetItemData(job_list->RowToItem(idx), static_cast<wxUIntPtr>(state));

    switch (state) {
        case ST_NEW:        job_list->SetValue(_L("Enqueued"), idx, COL_STATUS); break;
        case ST_PROGRESS:   job_list->SetValue(_L("Uploading"), idx, COL_STATUS); break;
        case ST_ERROR:      job_list->SetValue(_L("Error"), idx, COL_STATUS); break;
        case ST_CANCELLING: job_list->SetValue(_L("Cancelling"), idx, COL_STATUS); break;
        case ST_CANCELLED:  job_list->SetValue(_L("Cancelled"), idx, COL_STATUS); break;
        case ST_COMPLETED:  job_list->SetValue(_L("Completed"), idx, COL_STATUS); break;
    }
    // This might be ambigous call, but user data needs to be saved time to time
    save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
}

void PrintHostQueueDialog::on_list_select()
{
    int selected = job_list->GetSelectedRow();
    if (selected != wxNOT_FOUND) {
        const JobState state = get_state(selected);
        btn_cancel->Enable(state < ST_ERROR);
        btn_error->Enable(state == ST_ERROR);
        Layout();
    } else {
        btn_cancel->Disable();
    }
}

void PrintHostQueueDialog::on_progress(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    if (evt.progress < 100) {
        set_state(evt.job_id, ST_PROGRESS);
        job_list->SetValue(wxVariant(evt.progress), evt.job_id, COL_PROGRESS);
    } else {
        set_state(evt.job_id, ST_COMPLETED);
        job_list->SetValue(wxVariant(100), evt.job_id, COL_PROGRESS);
    }

    on_list_select();

    if (evt.progress > 0)
    {
        wxVariant nm, hst;
        job_list->GetValue(nm, evt.job_id, COL_FILENAME);
        job_list->GetValue(hst, evt.job_id, COL_HOST);
        const std::string& nm_str = into_u8(nm.GetString());
        const std::string& hst_str = into_u8(hst.GetString());
        wxGetApp().notification_manager()->set_upload_job_notification_percentage(evt.job_id + 1, nm_str, hst_str, evt.progress / 100.f);
    }
}

void PrintHostQueueDialog::on_error(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    set_state(evt.job_id, ST_ERROR);

    auto errormsg = format_wxstr("%1%\n%2%", _L("Error uploading to print host") + ":", evt.status);
    job_list->SetValue(wxVariant(0), evt.job_id, COL_PROGRESS);
    job_list->SetValue(wxVariant(errormsg), evt.job_id, COL_ERRORMSG);    // Stashes the error message into a hidden column for later

    on_list_select();

    GUI::show_error(nullptr, errormsg);

    wxVariant nm, hst;
    job_list->GetValue(nm, evt.job_id, COL_FILENAME);
    job_list->GetValue(hst, evt.job_id, COL_HOST);
    wxGetApp().notification_manager()->upload_job_notification_show_error(evt.job_id + 1, into_u8(nm.GetString()), into_u8(hst.GetString()));
}

void PrintHostQueueDialog::on_cancel(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    set_state(evt.job_id, ST_CANCELLED);
    job_list->SetValue(wxVariant(0), evt.job_id, COL_PROGRESS);

    on_list_select();

    wxVariant nm, hst;
    job_list->GetValue(nm, evt.job_id, COL_FILENAME);
    job_list->GetValue(hst, evt.job_id, COL_HOST);
    wxGetApp().notification_manager()->upload_job_notification_show_canceled(evt.job_id + 1, into_u8(nm.GetString()), into_u8(hst.GetString()));
}

void PrintHostQueueDialog::on_info(Event& evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");
    
    if (evt.tag == L"resolve") {
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_HOST);
        wxGetApp().notification_manager()->set_upload_job_notification_host(evt.job_id + 1, into_u8(evt.status));
    } else if (evt.tag == L"complete") {
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_ERRORMSG);
        wxGetApp().notification_manager()->set_upload_job_notification_completed(evt.job_id + 1);
        wxGetApp().notification_manager()->set_upload_job_notification_status(evt.job_id + 1, into_u8(evt.status));
    } else if(evt.tag == L"complete_with_warning"){
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_ERRORMSG);
        wxGetApp().notification_manager()->set_upload_job_notification_completed_with_warning(evt.job_id + 1);
        wxGetApp().notification_manager()->set_upload_job_notification_status(evt.job_id + 1, into_u8(evt.status));
    } else if (evt.tag == L"set_complete_off") {
        wxGetApp().notification_manager()->set_upload_job_notification_comp_on_100(evt.job_id + 1, false);
    } else if (evt.tag == L"prusaconnect_printer_address") {
        wxGetApp().notification_manager()->set_upload_job_notification_hypertext(evt.job_id + 1
            , [evt](wxEvtHandler *) {
                wxGetApp().mainframe->show_connect_tab(into_u8(evt.status));
                return false ;
            }
            );
    }
}

void PrintHostQueueDialog::get_active_jobs(std::vector<std::pair<std::string, std::string>>& ret)
{
    int ic = job_list->GetItemCount();
    for (int i = 0; i < ic; i++)
    {
        auto item = job_list->RowToItem(i);
        auto data = job_list->GetItemData(item);
        JobState st = static_cast<JobState>(data);
        if(st == JobState::ST_NEW || st == JobState::ST_PROGRESS)
            ret.emplace_back(upload_names[i]);       
    }
}
void PrintHostQueueDialog::save_user_data(int udt)
{
    const auto em = GetTextExtent("m").x;
    auto *app_config = wxGetApp().app_config;
    if (udt & UserDataType::UDT_SIZE) {
        
        app_config->set("print_host_queue_dialog_height", std::to_string(this->GetSize().x / em));
        app_config->set("print_host_queue_dialog_width", std::to_string(this->GetSize().y / em));
    }
    if (udt & UserDataType::UDT_POSITION)
    {
        app_config->set("print_host_queue_dialog_x", std::to_string(this->GetPosition().x));
        app_config->set("print_host_queue_dialog_y", std::to_string(this->GetPosition().y));
    }
    if (udt & UserDataType::UDT_COLS)
    {
        for (size_t i = 0; i < job_list->GetColumnCount() - 1; i++)
        {
            app_config->set("print_host_queue_dialog_column_" + std::to_string(i), std::to_string(job_list->GetColumn(i)->GetWidth()));
        }
    }    
}
bool PrintHostQueueDialog::load_user_data(int udt, std::vector<int>& vector)
{
    auto* app_config = wxGetApp().app_config;
    auto hasget = [app_config](const std::string& name, std::vector<int>& vector)->bool {
        if (app_config->has(name)) {
            std::string val = app_config->get(name);
            if (!val.empty() || val[0]!='\0') {
                vector.push_back(std::stoi(val));
                return true;
            }
        }
        return false;
    };
    if (udt & UserDataType::UDT_SIZE) {
        if (!hasget("print_host_queue_dialog_height",vector))
            return false;
        if (!hasget("print_host_queue_dialog_width", vector))
            return false;
    }
    if (udt & UserDataType::UDT_POSITION)
    {
        if (!hasget("print_host_queue_dialog_x", vector))
            return false;
        if (!hasget("print_host_queue_dialog_y", vector))
            return false;
    }
    if (udt & UserDataType::UDT_COLS)
    {
        for (size_t i = 0; i < 7; i++)
        {
            if (!hasget("print_host_queue_dialog_column_" + std::to_string(i), vector))
                return false;
        }
    }
    return true;
}
}}
