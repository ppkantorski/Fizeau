// Copyright (c) 2024 averne
//
// This file is part of Fizeau.
//
// Fizeau is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// Fizeau is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Fizeau.  If not, see <http://www.gnu.org/licenses/>.

#define NDEBUG
#define STBTT_STATIC
#define TESLA_INIT_IMPL

#include <tesla.hpp>
#include <common.hpp>

#ifdef DEBUG
TwiliPipe g_twlPipe;
#endif

namespace fz {

namespace {

template <typename ...Args>
std::string format(const std::string_view &fmt, Args &&...args) {
    std::string str(std::snprintf(nullptr, 0, fmt.data(), args...) + 1, 0);
    std::snprintf(str.data(), str.capacity(), fmt.data(), args...);
    return str;
}

bool is_full(const ColorRange &range) {
    return (range.lo == MIN_RANGE) && (range.hi == MAX_RANGE);
}

} // namespace


// ========================================
// ServiceInactiveGui Class
// ========================================
class ServiceInactiveGui: public tsl::Gui {
public:
    ServiceInactiveGui() { }
    
    virtual ~ServiceInactiveGui() {
        tsl::Overlay::get()->close();
    }
    
    virtual tsl::elm::Element *createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Fizeau", VERSION, false);
        
        auto* drawer = new tsl::elm::CustomDrawer([](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString("Fizeau system module is not active.", false, x + 16, y +  80, 20, (0xffff));
            renderer->drawString("Enable the system module and",        false, x + 16, y + 110, 20, (0xffff));
            renderer->drawString("reboot your device.",                 false, x + 16, y + 130, 20, (0xffff));
        });

        frame->setContent(drawer);
        
        #if USING_WIDGET_DIRECTIVE
        frame->m_showWidget = true;
        #endif
        
        return frame;
    }
};


// ========================================
// ErrorGui Class
// ========================================
class ErrorGui: public tsl::Gui {
public:
    ErrorGui(Result rc): rc(rc) { }
    
    virtual ~ErrorGui() {
        tsl::Overlay::get()->close();
    }
    
    virtual tsl::elm::Element *createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Fizeau", VERSION, false);
        
        auto* drawer = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString(format("%#x (%04d-%04d)", this->rc, R_MODULE(this->rc) + 2000, R_DESCRIPTION(this->rc)).c_str(),
                                                                         false, x, y +  50, 20, (0xffff));
            renderer->drawString("An error occurred",                    false, x, y +  80, 20, (0xffff));
            renderer->drawString("Please make sure you are using the",   false, x, y + 110, 20, (0xffff));
            renderer->drawString("latest release.",                      false, x, y + 130, 20, (0xffff));
            renderer->drawString("Otherwise, make an issue on github:",  false, x, y + 150, 20, (0xffff));
            renderer->drawString("https://www.github.com/averne/Fizeau", false, x, y + 170, 18, (0xffff));
        });

        frame->setContent(drawer);
        
        #if USING_WIDGET_DIRECTIVE
        frame->m_showWidget = true;
        #endif
        
        return frame;
    }

private:
    Result rc;
};

// ========================================
// FizeauOverlayGui Class
// ========================================
class FizeauOverlayGui: public tsl::Gui {
public:
    FizeauOverlayGui() : apply_counter(0), pending_apply(false) {
        this->rc = fizeauInitialize();
        if (R_FAILED(rc))
            return;

        this->config.read();

        // Read the actual active state from the system
        if (this->rc = fizeauGetIsActive(&this->config.active); R_FAILED(this->rc))
            return;

        ApmPerformanceMode perf_mode;
        if (this->rc = apmGetPerformanceMode(&perf_mode); R_FAILED(this->rc))
            return;

        if (this->rc = this->config.open_profile(perf_mode == ApmPerformanceMode_Normal ?
                                     config.internal_profile : config.external_profile); R_FAILED(this->rc))
            return;

        this->is_day = Clock::is_in_interval(this->config.profile.dawn_begin, this->config.profile.dusk_begin);
    }

    virtual ~FizeauOverlayGui() {
        // Apply any pending changes before exit
        if (this->pending_apply) {
            this->config.apply();
        }
        this->config.write();
        fizeauExit();
    }

    virtual tsl::elm::Element *createUI() override {
        this->info_header = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
            renderer->drawString(format("Editing profile: %u", static_cast<std::uint32_t>(this->config.cur_profile_id) + 1).c_str(),
                false, x, y + 20, 20, (0xffff));
            renderer->drawString(format("In period: %s", this->is_day ? "day" : "night").c_str(),
                false, x, y + 45, 20, (0xffff));
        });

        bool enable_extra_hot_temps = false;
        if ((this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) > D65_TEMP)
            enable_extra_hot_temps = true;

        this->active_button = new tsl::elm::ListItem("Correction active");
        this->active_button->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_A) {
                this->config.active ^= 1;
                this->rc = fizeauSetIsActive(this->config.active);
                this->active_button->setValue(this->config.active ? "Active": "Inactive");
                return true;
            }
            return false;
        });
        this->active_button->setValue(this->config.active ? "Active": "Inactive");

        this->reset_button = new tsl::elm::ListItem("Reset settings");
        this->reset_button->setClickListener([this, enable_extra_hot_temps](std::uint64_t keys) mutable {
            if (keys & HidNpadButton_A) {
                // Reset temperature
                (this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) = DEFAULT_TEMP;
                this->temp_slider->setProgress((DEFAULT_TEMP - MIN_TEMP) * 100 / ((enable_extra_hot_temps ? MAX_TEMP : D65_TEMP) - MIN_TEMP));
                
                // Reset saturation
                (this->is_day ? this->config.profile.day_settings.saturation : this->config.profile.night_settings.saturation) = DEFAULT_SAT;
                this->sat_slider->setProgress((DEFAULT_SAT - MIN_SAT) * 100 / (MAX_SAT - MIN_SAT));
                
                // Reset hue
                (this->is_day ? this->config.profile.day_settings.hue : this->config.profile.night_settings.hue) = DEFAULT_HUE;
                this->hue_slider->setProgress((DEFAULT_HUE - MIN_HUE) * 100 / (MAX_HUE - MIN_HUE));
                
                // Reset components
                this->config.profile.components = Component_All;
                this->components_bar->setProgress(Component_All);
                
                // Reset filter
                this->config.profile.filter = Component_None;
                this->filter_bar->setProgress(Component_None);
                
                // Reset contrast
                (this->is_day ? this->config.profile.day_settings.contrast : this->config.profile.night_settings.contrast) = DEFAULT_CONTRAST;
                this->contrast_slider->setProgress((DEFAULT_CONTRAST - MIN_CONTRAST) * 100 / (MAX_CONTRAST - MIN_CONTRAST));
                
                // Reset gamma
                (this->is_day ? this->config.profile.day_settings.gamma : this->config.profile.night_settings.gamma) = DEFAULT_GAMMA;
                this->gamma_slider->setProgress((DEFAULT_GAMMA - MIN_GAMMA) * 100 / (MAX_GAMMA - MIN_GAMMA));
                
                // Reset luminance
                (this->is_day ? this->config.profile.day_settings.luminance : this->config.profile.night_settings.luminance) = DEFAULT_LUMA;
                this->luma_slider->setProgress((DEFAULT_LUMA - MIN_LUMA) * 100 / (MAX_LUMA - MIN_LUMA));
                
                // Reset color range
                auto &range = (this->is_day ? this->config.profile.day_settings.range : this->config.profile.night_settings.range);
                range = DEFAULT_RANGE;
                this->range_button->setValue(is_full(range) ? "Full" : "Limited");
                
                // Apply all reset values immediately
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                
                return true;
            }
            return false;
        });

        this->temp_slider = new tsl::elm::TrackBar("");
        this->temp_slider->setProgress(((this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) - MIN_TEMP)
            * 100 / ((enable_extra_hot_temps ? MAX_TEMP : D65_TEMP) - MIN_TEMP));
        this->temp_slider->setClickListener([&, this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->temp_slider->setProgress((DEFAULT_TEMP - MIN_TEMP) * 100 / ((enable_extra_hot_temps ? MAX_TEMP : D65_TEMP) - MIN_TEMP));
                (this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) = DEFAULT_TEMP;
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->temp_slider->setValueChangedListener([this, enable_extra_hot_temps](std::uint8_t val) {
            (this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) =
                val * ((enable_extra_hot_temps ? MAX_TEMP : D65_TEMP) - MIN_TEMP) / 100 + MIN_TEMP;
            this->pending_apply = true;
        });

        this->sat_slider = new tsl::elm::TrackBar("");
        this->sat_slider->setProgress(((this->is_day ? this->config.profile.day_settings.saturation : this->config.profile.night_settings.saturation) - MIN_SAT)
            * 100 / (MAX_SAT - MIN_SAT));
        this->sat_slider->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->sat_slider->setProgress((DEFAULT_SAT - MIN_SAT) * 100 / (MAX_SAT - MIN_SAT));
                (this->is_day ? this->config.profile.day_settings.saturation : this->config.profile.night_settings.saturation) = DEFAULT_SAT;
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->sat_slider->setValueChangedListener([this](std::uint8_t val) {
            (this->is_day ? this->config.profile.day_settings.saturation : this->config.profile.night_settings.saturation) =
                val * (MAX_SAT - MIN_SAT) / 100 + MIN_SAT;
            this->pending_apply = true;
        });

        this->hue_slider = new tsl::elm::TrackBar("");
        this->hue_slider->setProgress(((this->is_day ? this->config.profile.day_settings.hue : this->config.profile.night_settings.hue) - MIN_HUE)
            * 100 / (MAX_HUE - MIN_HUE));
        this->hue_slider->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->hue_slider->setProgress((DEFAULT_HUE - MIN_HUE) * 100 / (MAX_HUE - MIN_HUE));
                (this->is_day ? this->config.profile.day_settings.hue : this->config.profile.night_settings.hue) = DEFAULT_HUE;
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->hue_slider->setValueChangedListener([this](std::uint8_t val) {
            (this->is_day ? this->config.profile.day_settings.hue : this->config.profile.night_settings.hue) =
                val * (MAX_HUE - MIN_HUE) / 100 + MIN_HUE;
            this->pending_apply = true;
        });

        this->components_bar = new tsl::elm::NamedStepTrackBar("", { "None", "R", "G", "RG", "B", "RB", "GB", "All" });
        this->components_bar->setProgress(static_cast<u8>(this->config.profile.components));
        this->components_bar->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->components_bar->setProgress(Component_All);
                this->config.profile.components = Component_All;
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->components_bar->setValueChangedListener([this](u8 val) {
            this->config.profile.components = static_cast<Component>(val);
            this->pending_apply = true;
        });

        this->filter_bar = new tsl::elm::NamedStepTrackBar("", { "None", "Red", "Green", "Blue" });
        this->filter_bar->setProgress((this->config.profile.filter == Component_None) ? 0 : std::countr_zero(static_cast<std::uint32_t>(this->config.profile.filter)) + 1);
        this->filter_bar->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->filter_bar->setProgress(Component_None);
                this->config.profile.filter = Component_None;
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->filter_bar->setValueChangedListener([this](u8 val) {
            this->config.profile.filter = static_cast<Component>(static_cast<Component>(val ? BIT(val - 1) : val));
            this->pending_apply = true;
        });

        this->contrast_slider = new tsl::elm::TrackBar("");
        this->contrast_slider->setProgress(((this->is_day ? this->config.profile.day_settings.contrast : this->config.profile.night_settings.contrast) - MIN_CONTRAST)
            * 100 / (MAX_CONTRAST - MIN_CONTRAST));
        this->contrast_slider->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->contrast_slider->setProgress((DEFAULT_CONTRAST - MIN_CONTRAST) * 100 / (MAX_CONTRAST - MIN_CONTRAST));
                (this->is_day ? this->config.profile.day_settings.contrast : this->config.profile.night_settings.contrast) = DEFAULT_CONTRAST;
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->contrast_slider->setValueChangedListener([this](std::uint8_t val) {
            (this->is_day ? this->config.profile.day_settings.contrast : this->config.profile.night_settings.contrast) =
                val * (MAX_CONTRAST - MIN_CONTRAST) / 100 + MIN_CONTRAST;
            this->pending_apply = true;
        });

        this->gamma_slider = new tsl::elm::TrackBar("");
        this->gamma_slider->setProgress(((this->is_day ? this->config.profile.day_settings.gamma : this->config.profile.night_settings.gamma) - MIN_GAMMA)
            * 100 / (MAX_GAMMA - MIN_GAMMA));
        this->gamma_slider->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->gamma_slider->setProgress((DEFAULT_GAMMA - MIN_GAMMA) * 100 / (MAX_GAMMA - MIN_GAMMA));
                (this->is_day ? this->config.profile.day_settings.gamma : this->config.profile.night_settings.gamma) = DEFAULT_GAMMA;
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->gamma_slider->setValueChangedListener([this](std::uint8_t val) {
            (this->is_day ? this->config.profile.day_settings.gamma : this->config.profile.night_settings.gamma) =
                val * (MAX_GAMMA - MIN_GAMMA) / 100 + MIN_GAMMA;
            this->pending_apply = true;
        });

        this->luma_slider = new tsl::elm::TrackBar("");
        this->luma_slider->setProgress(((this->is_day ? this->config.profile.day_settings.luminance : this->config.profile.night_settings.luminance) - MIN_LUMA)
            * 100 / (MAX_LUMA - MIN_LUMA));
        this->luma_slider->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->luma_slider->setProgress((DEFAULT_LUMA - MIN_LUMA) * 100 / (MAX_LUMA - MIN_LUMA));
                (this->is_day ? this->config.profile.day_settings.luminance : this->config.profile.night_settings.luminance) = DEFAULT_LUMA;
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->luma_slider->setValueChangedListener([this](std::uint8_t val) {
            (this->is_day ? this->config.profile.day_settings.luminance : this->config.profile.night_settings.luminance) =
                val * (MAX_LUMA - MIN_LUMA) / 100 + MIN_LUMA;
            this->pending_apply = true;
        });

        this->range_button = new tsl::elm::ListItem("Color range");
        this->range_button->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_A) {
                auto &range = (this->is_day ? this->config.profile.day_settings.range : this->config.profile.night_settings.range);
                if (is_full(range))
                    range = DEFAULT_LIMITED_RANGE;
                else
                    range = DEFAULT_RANGE;
                this->range_button->setValue(is_full(range) ? "Full" : "Limited");
                this->rc = this->config.apply();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->range_button->setValue(is_full(this->is_day ? this->config.profile.day_settings.range : this->config.profile.night_settings.range) ? "Full" : "Limited");

        this->temp_header       = new tsl::elm::CategoryHeader("");
        this->sat_header        = new tsl::elm::CategoryHeader("");
        this->hue_header        = new tsl::elm::CategoryHeader("");
        this->components_header = new tsl::elm::CategoryHeader("Components");
        this->filter_header     = new tsl::elm::CategoryHeader("Filter");
        this->contrast_header   = new tsl::elm::CategoryHeader("");
        this->gamma_header      = new tsl::elm::CategoryHeader("");
        this->luma_header       = new tsl::elm::CategoryHeader("");

        auto* frame = new tsl::elm::OverlayFrame("Fizeau", VERSION);
        auto* list = new tsl::elm::List();

        list->addItem(this->info_header, 60);
        list->addItem(this->active_button);
        list->addItem(this->reset_button);
        list->addItem(this->temp_header);
        list->addItem(this->temp_slider);
        list->addItem(this->sat_header);
        list->addItem(this->sat_slider);
        list->addItem(this->hue_header);
        list->addItem(this->hue_slider);
        list->addItem(this->components_header);
        list->addItem(this->components_bar);
        list->addItem(this->filter_header);
        list->addItem(this->filter_bar);
        list->addItem(this->contrast_header);
        list->addItem(this->contrast_slider);
        list->addItem(this->gamma_header);
        list->addItem(this->gamma_slider);
        list->addItem(this->luma_header);
        list->addItem(this->luma_slider);
        list->addItem(this->range_button);
        
        frame->setContent(list);
        
        #if USING_WIDGET_DIRECTIVE
        frame->m_showWidget = true;
        #endif
        
        return frame;
    }

    virtual void update() override {
        // Only switch to error GUI for critical initialization errors
        if (R_FAILED(this->rc) && this->config.cur_profile_id == FizeauProfileId_Invalid)
            tsl::changeTo<ErrorGui>(this->rc);

        this->is_day = Clock::is_in_interval(this->config.profile.dawn_begin, this->config.profile.dusk_begin);

        // Apply changes every 3 frames (~50ms at 60fps, ~33ms at 90fps)
        if (this->pending_apply) {
            this->apply_counter++;
            if (this->apply_counter >= 3) {
                Result apply_rc = this->config.apply();
                // Don't let a single failed apply kill the overlay
                // Just log it and continue
                if (R_FAILED(apply_rc)) {
                    LOG("Failed to apply config: %#x\n", apply_rc);
                }
                this->pending_apply = false;
                this->apply_counter = 0;
            }
        }

        this->temp_header->setText(format("Temperature: %u°K",
            this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature));
        this->sat_header->setText(format("Saturation: %.2f",
            this->is_day ? this->config.profile.day_settings.saturation  : this->config.profile.night_settings.saturation));
        this->hue_header->setText(format("Hue: %.2f",
            this->is_day ? this->config.profile.day_settings.hue         : this->config.profile.night_settings.hue));
        this->contrast_header->setText(format("Contrast: %.2f",
            this->is_day ? this->config.profile.day_settings.contrast    : this->config.profile.night_settings.contrast));
        this->gamma_header->setText(format("Gamma: %.2f",
            this->is_day ? this->config.profile.day_settings.gamma       : this->config.profile.night_settings.gamma));
        this->luma_header->setText(format("Luminance: %.2f",
            this->is_day ? this->config.profile.day_settings.luminance   : this->config.profile.night_settings.luminance));
    }

    Config &get_config() {
        return this->config;
    }

private:
    Result rc;
    bool is_day;
    Config config = {};
    tsl::elm::CustomDrawer      *info_header;
    tsl::elm::ListItem          *active_button;
    tsl::elm::ListItem          *reset_button;
    tsl::elm::TrackBar          *temp_slider;
    tsl::elm::TrackBar          *sat_slider;
    tsl::elm::TrackBar          *hue_slider;
    tsl::elm::NamedStepTrackBar *components_bar;
    tsl::elm::NamedStepTrackBar *filter_bar;
    tsl::elm::TrackBar          *contrast_slider;
    tsl::elm::TrackBar          *gamma_slider;
    tsl::elm::TrackBar          *luma_slider;
    tsl::elm::ListItem          *range_button;
    tsl::elm::CategoryHeader *temp_header, *sat_header, *hue_header,
        *components_header, *filter_header, *contrast_header, *gamma_header, *luma_header;
    
    // Frame-based throttling (simpler than time-based)
    int apply_counter;
    bool pending_apply;
};

} // namespace fz

// ========================================
// Main Overlay Class
// ========================================
class FizeauOverlay: public tsl::Overlay {
private:
    bool serviceActive = true;
    
public:
    virtual void initServices() override {
#ifdef DEBUG
        twiliInitialize();
        twiliCreateNamedOutputPipe(&g_twlPipe, "fzovlout");
#endif
        apmInitialize();

        bool is_active = false;
        Result rc = fizeauIsServiceActive(&is_active);
        
        if (R_FAILED(rc) || !is_active) {
            serviceActive = false;
            return;
        }
        
        fz::Clock::initialize();
    }
    
    virtual void exitServices() override {
        apmExit();
#ifdef DEBUG
        twiliClosePipe(&g_twlPipe);
        twiliExit();
#endif
    }
    
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        if (!serviceActive) {
            // Return a specific GUI for inactive service
            return initially<fz::ServiceInactiveGui>();
        }
        
        return initially<fz::FizeauOverlayGui>();
    }
    
    virtual void onShow() override { }
    
    virtual void onHide() override { }
};

// ========================================
// Main Entry Point
// ========================================
int main(int argc, char **argv) {
    LOG("Starting overlay\n");
    
    return tsl::loop<FizeauOverlay>(argc, argv);
}
