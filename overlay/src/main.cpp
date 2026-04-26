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

#include <exception_wrap.hpp>
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
            renderer->drawString("https://github.com/averne/Fizeau",     false, x, y + 170, 18, (0xffff));
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

enum class PeriodOverride : std::uint8_t {
    Dynamic = 0,
    Day     = 1,
    Night   = 2,
};

// Sentinel times used to encode period overrides directly in the profile's
// dusk/dawn schedule so the sysmodule's own clock logic enforces the override
// without any sysmodule changes.
//
// Force Day:   dawn_begin=00:00, dawn_end=00:00, dusk_begin=23:59, dusk_end=23:59
//   → day interval is_in_interval(ts, 00:00, 23:59) is true for 00:00–23:58 (all day)
//   → zero-length dusk and dawn windows never fire
//
// Force Night: all four times = 00:00
//   → every is_in_interval check is against a zero-length interval → always false
//   → falls through to the else-branch which is Night
//
// Dynamic:     restore the saved real times; sysmodule behaves normally
static constexpr Time OVERRIDE_DAY_DUSK_BEGIN = {23, 59, 0};
static constexpr Time OVERRIDE_DAY_DUSK_END   = {23, 59, 0};
static constexpr Time OVERRIDE_DAY_DAWN_BEGIN = {0,  0,  0};
static constexpr Time OVERRIDE_DAY_DAWN_END   = {0,  0,  0};

static constexpr Time OVERRIDE_NIGHT_DUSK_BEGIN = {0, 0, 0};
static constexpr Time OVERRIDE_NIGHT_DUSK_END   = {0, 0, 0};
static constexpr Time OVERRIDE_NIGHT_DAWN_BEGIN = {0, 0, 0};
static constexpr Time OVERRIDE_NIGHT_DAWN_END   = {0, 0, 0};

struct ProfilePeriodState {
    PeriodOverride override = PeriodOverride::Dynamic;
    // Original dusk/dawn times saved when the user enables Day or Night.
    // Restored to the profile when switching back to Dynamic.
    // Persisted in period_overrides.ini so they survive reboots.
    Time real_dusk_begin = {};
    Time real_dusk_end   = {};
    Time real_dawn_begin = {};
    Time real_dawn_end   = {};
};

// ── TimeStepTrackBar ──────────────────────────────────────────────────────────
// Inherits from StepTrackBar with 25 steps so the slider has a tick mark for
// every whole hour from 00:00 to 24:00 inclusive.  StepTrackBar's draw code
// places tick 0 at the far left and tick (numSteps-1) at exactly
// baseX + width - 1 (far right), and the handle position uses the same
// scaled maxValue used by input handling, so the handle reaches the right
// edge cleanly when the user lands on the last tick.
//
// usingNamedStepTrackbar=true activates the V2 draw path that renders
// m_selection (our HH:00 string) on the right, matching the profile bar style.
//
// Note: 24 hourly *segments* between 25 *ticks* — both 00:00 and 24:00 must
// be reachable, so there are 25 step positions.  The listener fires with the
// step index 0..24, which is the hour directly.
class TimeStepTrackBar : public tsl::elm::StepTrackBar {
public:
    static constexpr std::size_t kNumSteps = 25;   // 00:00 … 24:00 inclusive
    static constexpr int         kMaxHour  = static_cast<int>(kNumSteps) - 1;

    explicit TimeStepTrackBar(const std::string &label)
        : tsl::elm::StepTrackBar(/*icon=*/"", /*numSteps=*/kNumSteps,
                                 /*usingNamedStepTrackbar=*/true,
                                 /*useV2Style=*/true, label) {
        // libtesla quirk: TrackBar declares `u8 m_numSteps = 101` and
        // StepTrackBar redeclares its own `u8 m_numSteps = 1`, shadowing
        // the base.  StepTrackBar's constructor only initialises its own
        // copy, so TrackBar::draw — which reads the base's m_numSteps —
        // keeps using the default 101, drawing 101 tick marks and using
        // maxValue=100 for the handle (capping it at 96% with our step
        // size of 4).  Force the base copy to match.
        this->TrackBar::m_numSteps = kNumSteps;
        this->m_selection = "00:00";
    }

    // Keep m_selection current when repositioned programmatically.
    virtual void setProgress(u16 hour) override {
        tsl::elm::StepTrackBar::setProgress(hour);
        this->m_selection = hour_to_hhmm(hour);
    }

    // Shadow the non-virtual base so callers on the concrete type get a
    // wrapper that keeps m_selection in sync on every user interaction.
    // The base StepTrackBar fires the listener with getProgress() (the step
    // index 0..kMaxHour), which is the hour directly.
    void setValueChangedListener(std::function<void(u16)> listener) {
        tsl::elm::TrackBar::setValueChangedListener([this, listener](u16 hour) {
            this->m_selection = hour_to_hhmm(hour);
            listener(hour);
        });
    }

    // Helpers shared with FizeauOverlayGui ────────────────────────────────────
    static std::string hour_to_hhmm(int hour) {
        char buf[6];
        std::snprintf(buf, sizeof(buf), "%02d:00", std::clamp(hour, 0, kMaxHour));
        return buf;
    }

    static int time_to_hour(Time t) {
        // Truncate any minutes — the slider has hourly resolution only.
        return std::clamp((int)t.h, 0, kMaxHour);
    }

    static Time hour_to_time(int hour) {
        return { (std::uint8_t)std::clamp(hour, 0, kMaxHour), 0, 0 };
    }
};
// ─────────────────────────────────────────────────────────────────────────────

// ── DynamicProfileTrackBar ───────────────────────────────────────────────────
// NamedStepTrackBar's constructor only accepts std::initializer_list<std::string>,
// which can't be sized at runtime.  This thin subclass forwards to the base with
// a one-element placeholder, then overwrites m_stepDescriptions and the step
// count from a runtime vector.  Keeps the V2 inline label/value style and tick
// marks identical to the original profile bar.
//
// Both copies of m_numSteps are kept in sync (StepTrackBar shadows TrackBar's,
// see TimeStepTrackBar comment) — NamedStepTrackBar's own draw() only reads the
// StepTrackBar copy, but writing both costs nothing and prevents surprises.
class DynamicProfileTrackBar : public tsl::elm::NamedStepTrackBar {
public:
    DynamicProfileTrackBar(std::vector<std::string> labels, const std::string &title)
        : tsl::elm::NamedStepTrackBar(/*icon=*/"",
                                      /*stepDescriptions=*/{ "" },   // placeholder, replaced below
                                      /*useV2Style=*/true,
                                      title,
                                      /*unlocked=*/true) {
        this->m_stepDescriptions = std::move(labels);
        const u8 n = static_cast<u8>(this->m_stepDescriptions.size());
        this->m_numSteps           = n;     // StepTrackBar::m_numSteps
        this->TrackBar::m_numSteps = n;     // base copy (libtesla shadow quirk)
        if (!this->m_stepDescriptions.empty())
            this->m_selection = this->m_stepDescriptions.front();
    }
};
// ─────────────────────────────────────────────────────────────────────────────

class FizeauOverlayGui: public tsl::Gui {
public:
    // forced_profile: if valid, open that profile directly instead of the APM-active one.
    //                 Used so the profile bar can reconstruct the GUI on the same profile
    //                 after a write/read cycle. Pass FizeauProfileId_Invalid (default) for
    //                 normal startup behaviour (auto-detect from APM mode).
    FizeauOverlayGui(FizeauProfileId forced_profile = FizeauProfileId_Invalid)
        : allow_high_temp(false), apply_counter(0), pending_apply(false) {
        this->rc = fizeauInitialize();
        if (R_FAILED(rc))
            return;

        this->config.read();

        // Ensure config.ini always has all 4 profile sections.
        // Appends defaults for any missing ones (ini + sysmodule).
        this->pad_config_to_four_profiles();

        // Determine how many profiles the user actually defined in config.ini.
        // The sysmodule always has FizeauProfileId_Total slots, but only the
        // contiguous prefix from [profile1] is exposed in the UI.
        this->num_profiles = count_profiles_in_config();

        // Defensive clamp: handheld_profile / docked_profile in config.ini may
        // refer to slots beyond what's actually defined (or be Invalid if the
        // top-level keys are absent).  Snap them to profile1 so open_profile
        // below has a real target.
        if (this->config.internal_profile >= this->num_profiles)
            this->config.internal_profile = FizeauProfileId_Profile1;
        if (this->config.external_profile >= this->num_profiles)
            this->config.external_profile = FizeauProfileId_Profile1;

        // Read the actual active state from the system
        if (this->rc = fizeauGetIsActive(&this->config.active); R_FAILED(this->rc))
            return;

        if (this->rc = apmGetPerformanceMode(&this->perf_mode); R_FAILED(this->rc))
            return;

        FizeauProfileId target_profile;
        if (forced_profile != FizeauProfileId_Invalid) {
            target_profile = forced_profile;
        } else {
            target_profile = (this->perf_mode == ApmPerformanceMode_Normal)
                ? config.internal_profile : config.external_profile;
        }

        if (this->rc = this->config.open_profile(target_profile); R_FAILED(this->rc))
            return;

        this->load_period_overrides();

        // For Dynamic profiles: the sysmodule has the real times from config.ini.
        // Capture them now so we can restore them if the user later switches back
        // from Day/Night to Dynamic.
        auto &ps = this->period_states[target_profile];
        if (ps.override == PeriodOverride::Dynamic) {
            ps.real_dusk_begin = this->config.profile.dusk_begin;
            ps.real_dusk_end   = this->config.profile.dusk_end;
            ps.real_dawn_begin = this->config.profile.dawn_begin;
            ps.real_dawn_end   = this->config.profile.dawn_end;
        }
        // For Day/Night profiles: the sysmodule already has override times from
        // config.ini (they were written there last session).  load_period_overrides()
        // already populated ps.real_*.  Nothing to push.

        this->is_day = this->compute_is_day();
        this->allow_high_temp =
            (this->is_day ? this->config.profile.day_settings.temperature
                          : this->config.profile.night_settings.temperature) > D65_TEMP;
    }

    virtual ~FizeauOverlayGui() {
        // Flush any pending slider changes so the sysmodule has the latest data
        // before we read all profiles back in config.write().
        if (this->pending_apply) {
            this->config.apply();
        }

        // Persist the override states and real times to disk.
        this->save_period_overrides();

        // Write config.ini.  Config::make() iterates all 4 profiles via
        // open_profile() which calls fizeauGetProfile() for each one.  Because
        // we keep the sysmodule's copy of each profile in sync with our override
        // state (set_period_override patches/unpatches the times and pushes via
        // fizeauSetProfile), the sysmodule already holds the correct dusk/dawn
        // times for every profile.  config.write() therefore serialises exactly
        // the right values: override times for Day/Night profiles (so the
        // sysmodule honours them on the next reboot) and real times for Dynamic
        // profiles.
        this->config.write();

        fizeauExit();
    }

    // Returns whether we're currently in the day period, respecting per-profile override.
    bool compute_is_day() const {
        auto id = this->config.cur_profile_id;
        if (id < FizeauProfileId_Total) {
            switch (this->period_states[id].override) {
                case PeriodOverride::Day:   return true;
                case PeriodOverride::Night: return false;
                default: break;
            }
        }
        // Dynamic: check real times against the clock.
        // (this->config.profile may hold override times if an override is active
        // for this profile, but we only reach here when override == Dynamic, so
        // the profile holds real times.)
        return Clock::is_in_interval(this->config.profile.dawn_end, this->config.profile.dusk_begin);
    }

    // Refresh all slider/button positions from current profile & is_day state.
    void refresh_sliders() {
        this->is_day = this->compute_is_day();

        // Update period button label
        if (this->period_button) {
            auto id = this->config.cur_profile_id;
            auto ov = (id < FizeauProfileId_Total) ? this->period_states[id].override : PeriodOverride::Dynamic;
            const char *lbl = (ov == PeriodOverride::Day) ? "Day"
                            : (ov == PeriodOverride::Night) ? "Night"
                            : "Dynamic";
            this->period_button->setValue(lbl);
        }

        this->allow_high_temp =
            (this->is_day ? this->config.profile.day_settings.temperature
                          : this->config.profile.night_settings.temperature) > D65_TEMP;

        this->temp_slider->setProgress(
            ((this->is_day ? this->config.profile.day_settings.temperature
                           : this->config.profile.night_settings.temperature) - MIN_TEMP)
            * 100 / ((this->allow_high_temp ? MAX_TEMP : D65_TEMP) - MIN_TEMP));

        this->sat_slider->setProgress(
            ((this->is_day ? this->config.profile.day_settings.saturation
                           : this->config.profile.night_settings.saturation) - MIN_SAT)
            * 100 / (MAX_SAT - MIN_SAT));

        this->hue_slider->setProgress(
            ((this->is_day ? this->config.profile.day_settings.hue
                           : this->config.profile.night_settings.hue) - MIN_HUE)
            * 100 / (MAX_HUE - MIN_HUE));

        this->components_bar->setProgress(static_cast<u8>(this->config.profile.components));

        this->filter_bar->setProgress(
            (this->config.profile.filter == Component_None) ? 0
            : std::countr_zero(static_cast<std::uint32_t>(this->config.profile.filter)) + 1);

        this->contrast_slider->setProgress(
            ((this->is_day ? this->config.profile.day_settings.contrast
                           : this->config.profile.night_settings.contrast) - MIN_CONTRAST)
            * 100 / (MAX_CONTRAST - MIN_CONTRAST));

        this->gamma_slider->setProgress(
            ((this->is_day ? this->config.profile.day_settings.gamma
                           : this->config.profile.night_settings.gamma) - MIN_GAMMA)
            * 100 / (MAX_GAMMA - MIN_GAMMA));

        this->luma_slider->setProgress(
            ((this->is_day ? this->config.profile.day_settings.luminance
                           : this->config.profile.night_settings.luminance) - MIN_LUMA)
            * 100 / (MAX_LUMA - MIN_LUMA));

        auto &range = (this->is_day ? this->config.profile.day_settings.range
                                    : this->config.profile.night_settings.range);
        this->range_button->setValue(is_full(range) ? "Full" : "Limited");

        // Reposition time sliders (setProgress also updates the displayed HH:MM label)
        if (this->dawn_slider && this->dusk_slider) {
            auto id = this->config.cur_profile_id;
            if (id < FizeauProfileId_Total) {
                auto &ps = this->period_states[id];
                this->dawn_slider->setProgress(TimeStepTrackBar::time_to_hour(ps.real_dawn_begin));
                this->dusk_slider->setProgress(TimeStepTrackBar::time_to_hour(ps.real_dusk_begin));
            }
        }
    }

    static constexpr const char *overrides_path = "/config/fizeau/period_overrides.ini";
    static constexpr const char *config_ini_path = "/config/fizeau/config.ini";

    // Append any missing [profile2]..[profile4] sections to config.ini with
    // neutral default settings, and push those same defaults to the sysmodule
    // so they are live for the current session without requiring a reboot.
    // Profiles already present are never touched.  Does nothing if >= 4 exist.
    void pad_config_to_four_profiles() {
        std::size_t existing = count_profiles_in_config();
        if (existing >= FizeauProfileId_Total)
            return;

        // ── Append missing sections to config.ini ────────────────────────────
        FILE *fp = std::fopen(config_ini_path, "a");
        if (fp) {
            for (std::size_t i = existing + 1; i <= FizeauProfileId_Total; ++i) {
                std::fprintf(fp, "\n[profile%zu]\n",  i);
                std::fprintf(fp, "dusk_begin        = 20:00\n");
                std::fprintf(fp, "dusk_end          = 20:00\n");
                std::fprintf(fp, "dawn_begin        = 07:00\n");
                std::fprintf(fp, "dawn_end          = 07:00\n");
                std::fprintf(fp, "temperature_day   = 6500\n");
                std::fprintf(fp, "temperature_night = 6500\n");
                std::fprintf(fp, "saturation_day    = 1.000000\n");
                std::fprintf(fp, "saturation_night  = 1.000000\n");
                std::fprintf(fp, "hue_day           = 0.000000\n");
                std::fprintf(fp, "hue_night         = 0.000000\n");
                std::fprintf(fp, "components        = all\n");
                std::fprintf(fp, "filter            = none\n");
                std::fprintf(fp, "contrast_day      = 1.000000\n");
                std::fprintf(fp, "contrast_night    = 1.000000\n");
                std::fprintf(fp, "gamma_day         = 2.400000\n");
                std::fprintf(fp, "gamma_night       = 2.400000\n");
                std::fprintf(fp, "luminance_day     = 0.000000\n");
                std::fprintf(fp, "luminance_night   = 0.000000\n");
                std::fprintf(fp, "range_day         = 0.00-1.00\n");
                std::fprintf(fp, "range_night       = 0.00-1.00\n");
                std::fprintf(fp, "dimming_timeout   = 00:00\n");
            }
            std::fclose(fp);
        }

        // ── Push defaults to sysmodule for the current session ───────────────
        // Build a profile that matches exactly what we just wrote to disk.
        FizeauProfile def = {};
        def.day_settings   = Config::default_settings;
        def.night_settings = Config::default_settings;
        def.dusk_begin     = { 20, 0, 0 };
        def.dusk_end       = { 20, 0, 0 };
        def.dawn_begin     = {  7, 0, 0 };
        def.dawn_end       = {  7, 0, 0 };
        def.components     = Component_All;
        def.filter         = Component_None;
        def.dimming_timeout = {};

        for (std::size_t i = existing; i < FizeauProfileId_Total; ++i)
            fizeauSetProfile(static_cast<FizeauProfileId>(i), &def);
    }

    // Count contiguous [profileN] sections starting at N=1 in config.ini.
    // Returns at least 1 (so the rest of the code always has a valid profile
    // to open, even if the user's config.ini is missing or malformed).
    // Non-contiguous gaps (e.g. profile1 + profile3 with no profile2) stop
    // the count at the gap — every advertised profile must be a real one.
    static std::size_t count_profiles_in_config() {
        FILE *fp = std::fopen(config_ini_path, "r");
        if (!fp) return 1;

        std::array<bool, FizeauProfileId_Total> seen{};
        char line[128];
        while (std::fgets(line, sizeof(line), fp)) {
            int id;
            // Tolerates leading whitespace and trailing comments after the ']'.
            if (std::sscanf(line, " [profile%d]", &id) == 1 &&
                id >= 1 && id <= FizeauProfileId_Total) {
                seen[id - 1] = true;
            }
        }
        std::fclose(fp);

        std::size_t count = 0;
        for (bool s : seen) {
            if (!s) break;
            ++count;
        }
        return std::max<std::size_t>(count, 1);
    }

    void save_period_overrides() {
        FILE *fp = std::fopen(overrides_path, "w");
        if (!fp) return;
        for (std::size_t i = 0; i < this->num_profiles; ++i) {
            auto &ps = this->period_states[i];
            if (ps.override == PeriodOverride::Dynamic) {
                std::fprintf(fp, "profile%zu=dynamic\n", i + 1);
            } else {
                const char *ov_str = (ps.override == PeriodOverride::Day) ? "day" : "night";
                // Store real times so they can be restored when switching back to Dynamic
                std::fprintf(fp, "profile%zu=%s,%02d:%02d,%02d:%02d,%02d:%02d,%02d:%02d\n",
                    i + 1, ov_str,
                    (int)ps.real_dusk_begin.h, (int)ps.real_dusk_begin.m,
                    (int)ps.real_dusk_end.h,   (int)ps.real_dusk_end.m,
                    (int)ps.real_dawn_begin.h, (int)ps.real_dawn_begin.m,
                    (int)ps.real_dawn_end.h,   (int)ps.real_dawn_end.m);
            }
        }
        std::fclose(fp);
    }

    void load_period_overrides() {
        FILE *fp = std::fopen(overrides_path, "r");
        if (!fp) return;

        char line[128];
        while (std::fgets(line, sizeof(line), fp)) {
            int id;
            char ov_str[16];
            char db[8] = {}, de[8] = {}, ab[8] = {}, ae[8] = {};

            // Try full format first: profileN=override,dusk_begin,dusk_end,dawn_begin,dawn_end
            int n = std::sscanf(line, "profile%d=%15[^,],%7[^,],%7[^,],%7[^,],%7s",
                                &id, ov_str, db, de, ab, ae);
            if (n < 2 || id < 1 || id > (int)this->num_profiles)
                continue;

            auto idx = id - 1;
            auto &ps = this->period_states[idx];

            if      (std::strcmp(ov_str, "day")   == 0) ps.override = PeriodOverride::Day;
            else if (std::strcmp(ov_str, "night") == 0) ps.override = PeriodOverride::Night;
            else                                         ps.override = PeriodOverride::Dynamic;

            // Restore real times if we have them (non-Dynamic entry with full format)
            if (n == 6 && ps.override != PeriodOverride::Dynamic) {
                auto parse_t = [](const char *s) -> Time {
                    Time t = {}; int h = 0, m = 0;
                    std::sscanf(s, "%d:%d", &h, &m);
                    t.h = h; t.m = m;
                    return t;
                };
                ps.real_dusk_begin = parse_t(db);
                ps.real_dusk_end   = parse_t(de);
                ps.real_dawn_begin = parse_t(ab);
                ps.real_dawn_end   = parse_t(ae);
            }
        }
        std::fclose(fp);
    }

    // Apply the period override for 'id' to the profile currently loaded in
    // this->config.profile and push it to the sysmodule.
    //
    // Day/Night:  patches dusk_begin/end and dawn_begin/end to sentinel values
    //             that make the sysmodule's clock logic always pick the forced
    //             period (no settings mirroring, no sysmodule changes needed).
    // Dynamic:    restores the saved real dusk/dawn times.
    //
    // Because the patched times are pushed via fizeauSetProfile, the sysmodule
    // immediately starts using them.  When config.write() runs later it reads
    // them back via fizeauGetProfile and serialises them to config.ini, so the
    // sysmodule will honour the override on the next reboot as well.
    void set_period_override(FizeauProfileId id, PeriodOverride new_ov) {
        if (id >= FizeauProfileId_Total) return;

        auto &ps = this->period_states[id];

        // When leaving Dynamic, capture the current real times so we can
        // restore them if the user later switches back.
        if (ps.override == PeriodOverride::Dynamic && new_ov != PeriodOverride::Dynamic) {
            ps.real_dusk_begin = this->config.profile.dusk_begin;
            ps.real_dusk_end   = this->config.profile.dusk_end;
            ps.real_dawn_begin = this->config.profile.dawn_begin;
            ps.real_dawn_end   = this->config.profile.dawn_end;
        }

        ps.override = new_ov;

        switch (new_ov) {
            case PeriodOverride::Day:
                this->config.profile.dusk_begin = OVERRIDE_DAY_DUSK_BEGIN;
                this->config.profile.dusk_end   = OVERRIDE_DAY_DUSK_END;
                this->config.profile.dawn_begin = OVERRIDE_DAY_DAWN_BEGIN;
                this->config.profile.dawn_end   = OVERRIDE_DAY_DAWN_END;
                break;
            case PeriodOverride::Night:
                this->config.profile.dusk_begin = OVERRIDE_NIGHT_DUSK_BEGIN;
                this->config.profile.dusk_end   = OVERRIDE_NIGHT_DUSK_END;
                this->config.profile.dawn_begin = OVERRIDE_NIGHT_DAWN_BEGIN;
                this->config.profile.dawn_end   = OVERRIDE_NIGHT_DAWN_END;
                break;
            case PeriodOverride::Dynamic:
                this->config.profile.dusk_begin = ps.real_dusk_begin;
                this->config.profile.dusk_end   = ps.real_dusk_end;
                this->config.profile.dawn_begin = ps.real_dawn_begin;
                this->config.profile.dawn_end   = ps.real_dawn_end;
                break;
        }

        // Push the updated profile (with patched or restored times) to the
        // sysmodule so it takes effect immediately.
        this->config.apply();
    }

    // Thin wrapper kept for call-site clarity.  With the time-patching approach
    // there is no longer any settings mirroring to do — just push the profile.
    Result apply_with_override() {
        return this->config.apply();
    }

    // Switch to a different profile in-place, refreshing all slider positions.
    void switch_profile(FizeauProfileId new_id) {
        if (this->pending_apply) {
            this->config.apply();
            this->pending_apply = false;
            this->apply_counter = 0;
        }

        if (this->rc = this->config.open_profile(new_id); R_FAILED(this->rc))
            return;

        // If this profile is Dynamic, capture its real times from the sysmodule
        // (the sysmodule loaded them from config.ini on boot).  If it is Day/Night,
        // period_states[new_id].real_* was already populated by load_period_overrides().
        auto &ps = this->period_states[new_id];
        if (ps.override == PeriodOverride::Dynamic) {
            ps.real_dusk_begin = this->config.profile.dusk_begin;
            ps.real_dusk_end   = this->config.profile.dusk_end;
            ps.real_dawn_begin = this->config.profile.dawn_begin;
            ps.real_dawn_end   = this->config.profile.dawn_end;
        }

        // Tell the sysmodule to use this profile for the current display so
        // changes are immediately visible on screen.
        bool is_external = (this->perf_mode != ApmPerformanceMode_Normal);
        fizeauSetActiveProfileId(is_external, new_id);
        (is_external ? this->config.external_profile : this->config.internal_profile) = new_id;

        this->refresh_sliders();
    }

    virtual tsl::elm::Element *createUI() override {
        this->info_header = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer *renderer, s32 x, s32 y, s32 w, s32 h) {
            //renderer->drawString(format("Editing profile: %u", static_cast<std::uint32_t>(this->config.cur_profile_id) + 1).c_str(),
            //    false, x, y + 20, 20, (0xffff));
            renderer->drawString(format("In period: %s", this->is_day ? "day" : "night").c_str(),
                false, x, y + 20, 20, (0xffff));
        });

        // Profile selector — only meaningful when there's more than one profile
        // defined in config.ini.  With a single profile we omit the bar entirely
        // (creation is skipped, and the addItem() below is guarded too).
        // Switching flushes any pending apply, opens the new profile, and refreshes all sliders.
        if (this->num_profiles > 1) {
            std::vector<std::string> labels;
            labels.reserve(this->num_profiles);
            for (std::size_t i = 1; i <= this->num_profiles; ++i)
                labels.push_back(std::to_string(i));
            this->profile_bar = new DynamicProfileTrackBar(std::move(labels), "Display Profile");
            this->profile_bar->setProgress(static_cast<u8>(this->config.cur_profile_id));
            this->profile_bar->setValueChangedListener([this](u8 val) {
                auto new_id = static_cast<FizeauProfileId>(val);
                if (new_id != this->config.cur_profile_id)
                    this->switch_profile(new_id);
            });
        }

        this->active_button = new tsl::elm::ListItem("Correction State");
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

        // Period override — cycles Dynamic → Day → Night → Dynamic per profile
        this->period_button = new tsl::elm::ListItem("Period Mode");
        this->period_button->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_A) {
                auto id = this->config.cur_profile_id;
                if (id >= FizeauProfileId_Total)
                    return false;

                // Advance to next override state
                auto new_ov = static_cast<PeriodOverride>(
                    (static_cast<std::uint8_t>(this->period_states[id].override) + 1) % 3);

                this->set_period_override(id, new_ov);

                const char *lbl = (new_ov == PeriodOverride::Day)   ? "Day"
                                : (new_ov == PeriodOverride::Night) ? "Night"
                                :                                      "Dynamic";
                this->period_button->setValue(lbl);
                // Refresh sliders so is_day and all slider positions update immediately
                this->refresh_sliders();
                // Persist immediately so a crash / hard power-off still saves the state
                this->save_period_overrides();
                return true;
            }
            return false;
        });
        {
            auto id = this->config.cur_profile_id;
            auto ov = (id < FizeauProfileId_Total) ? this->period_states[id].override : PeriodOverride::Dynamic;
            this->period_button->setValue(
                (ov == PeriodOverride::Day) ? "Day" : (ov == PeriodOverride::Night) ? "Night" : "Dynamic");
        }

        // ── Dawn (Day Start) — 1-hour steps, Dynamic only ────────────────────
        this->dawn_slider = new TimeStepTrackBar("Day Start / Night End");
        {
            auto id = this->config.cur_profile_id;
            auto &ps = this->period_states[id < FizeauProfileId_Total ? id : 0];
            this->dawn_slider->setProgress(TimeStepTrackBar::time_to_hour(ps.real_dawn_begin));
        }
        this->dawn_slider->setValueChangedListener([this](u16 hour) {
            auto id = this->config.cur_profile_id;
            if (id >= FizeauProfileId_Total) return;
            auto &ps = this->period_states[id];
            if (ps.override != PeriodOverride::Dynamic) return;
            Time t = TimeStepTrackBar::hour_to_time(hour);
            ps.real_dawn_begin = ps.real_dawn_end = t;
            this->config.profile.dawn_begin = this->config.profile.dawn_end = t;
            this->pending_apply = true;
        });

        // ── Dusk (Day End) — 1-hour steps, Dynamic only ───────────────────────
        this->dusk_slider = new TimeStepTrackBar("Day End / Night Start");
        {
            auto id = this->config.cur_profile_id;
            auto &ps = this->period_states[id < FizeauProfileId_Total ? id : 0];
            this->dusk_slider->setProgress(TimeStepTrackBar::time_to_hour(ps.real_dusk_begin));
        }
        this->dusk_slider->setValueChangedListener([this](u16 hour) {
            auto id = this->config.cur_profile_id;
            if (id >= FizeauProfileId_Total) return;
            auto &ps = this->period_states[id];
            if (ps.override != PeriodOverride::Dynamic) return;
            Time t = TimeStepTrackBar::hour_to_time(hour);
            ps.real_dusk_begin = ps.real_dusk_end = t;
            this->config.profile.dusk_begin = this->config.profile.dusk_end = t;
            this->pending_apply = true;
        });

        this->reset_button = new tsl::elm::ListItem("Reset Settings");
        this->reset_button->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_A) {
                // Reset temperature
                (this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) = DEFAULT_TEMP;
                this->temp_slider->setProgress((DEFAULT_TEMP - MIN_TEMP) * 100 / ((this->allow_high_temp ? MAX_TEMP : D65_TEMP) - MIN_TEMP));
                
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
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                
                return true;
            }
            return false;
        });

        this->temp_slider = new tsl::elm::TrackBar("");
        this->temp_slider->setProgress(((this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) - MIN_TEMP)
            * 100 / ((this->allow_high_temp ? MAX_TEMP : D65_TEMP) - MIN_TEMP));
        this->temp_slider->setClickListener([&, this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->temp_slider->setProgress((DEFAULT_TEMP - MIN_TEMP) * 100 / ((this->allow_high_temp ? MAX_TEMP : D65_TEMP) - MIN_TEMP));
                (this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) = DEFAULT_TEMP;
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                triggerSettingsFeedback();
                return true;
            }
            return false;
        });
        this->temp_slider->setValueChangedListener([this](std::uint8_t val) {
            (this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature) =
                val * ((this->allow_high_temp ? MAX_TEMP : D65_TEMP) - MIN_TEMP) / 100 + MIN_TEMP;
            this->pending_apply = true;
        });

        this->sat_slider = new tsl::elm::TrackBar("");
        this->sat_slider->setProgress(((this->is_day ? this->config.profile.day_settings.saturation : this->config.profile.night_settings.saturation) - MIN_SAT)
            * 100 / (MAX_SAT - MIN_SAT));
        this->sat_slider->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_Y) {
                this->sat_slider->setProgress((DEFAULT_SAT - MIN_SAT) * 100 / (MAX_SAT - MIN_SAT));
                (this->is_day ? this->config.profile.day_settings.saturation : this->config.profile.night_settings.saturation) = DEFAULT_SAT;
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                triggerSettingsFeedback();
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
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                triggerSettingsFeedback();
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
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                triggerSettingsFeedback();
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
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                triggerSettingsFeedback();
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
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                triggerSettingsFeedback();
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
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                triggerSettingsFeedback();
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
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                triggerSettingsFeedback();
                return true;
            }
            return false;
        });
        this->luma_slider->setValueChangedListener([this](std::uint8_t val) {
            (this->is_day ? this->config.profile.day_settings.luminance : this->config.profile.night_settings.luminance) =
                val * (MAX_LUMA - MIN_LUMA) / 100 + MIN_LUMA;
            this->pending_apply = true;
        });

        this->range_button = new tsl::elm::ListItem("Color Range");
        this->range_button->setClickListener([this](std::uint64_t keys) {
            if (keys & HidNpadButton_A) {
                auto &range = (this->is_day ? this->config.profile.day_settings.range : this->config.profile.night_settings.range);
                if (is_full(range))
                    range = DEFAULT_LIMITED_RANGE;
                else
                    range = DEFAULT_RANGE;
                this->range_button->setValue(is_full(range) ? "Full" : "Limited");
                this->rc = this->apply_with_override();
                this->pending_apply = false;
                this->apply_counter = 0;
                return true;
            }
            return false;
        });
        this->range_button->setValue(is_full(this->is_day ? this->config.profile.day_settings.range : this->config.profile.night_settings.range) ? "Full" : "Limited");

        this->temp_header       = new tsl::elm::CategoryHeader("Temperature");
        this->sat_header        = new tsl::elm::CategoryHeader("Saturation");
        this->hue_header        = new tsl::elm::CategoryHeader("Hue");
        this->components_header = new tsl::elm::CategoryHeader("Components");
        this->filter_header     = new tsl::elm::CategoryHeader("Filter");
        this->contrast_header   = new tsl::elm::CategoryHeader("Contrast");
        this->gamma_header      = new tsl::elm::CategoryHeader("Gamma");
        this->luma_header       = new tsl::elm::CategoryHeader("Luminance");

        auto* list = new tsl::elm::List();

        //list->addItem(this->info_header, 60);
        this->display_settings_header = new tsl::elm::CategoryHeader("Display Settings");
        list->addItem(this->display_settings_header);
        list->addItem(this->active_button);
        list->addItem(this->reset_button);
        if (this->profile_bar)
            list->addItem(this->profile_bar);
        this->daylight_header = new tsl::elm::CategoryHeader("Daylight Cycle");
        list->addItem(this->daylight_header);
        list->addItem(this->period_button);
        list->addItem(this->dawn_slider);
        list->addItem(this->dusk_slider);
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
        
        auto* frame = new tsl::elm::OverlayFrame("Fizeau", VERSION);
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

        this->is_day = this->compute_is_day();

        // Poll display mode every ~18 frames (~300ms at 60fps)
        this->display_mode_poll_counter++;
        if (this->display_mode_poll_counter >= 18) {
            this->display_mode_poll_counter = 0;
            apmGetPerformanceMode(&this->perf_mode);
        }
        this->display_settings_header->setValue(
            this->perf_mode == ApmPerformanceMode_Normal ? "Handheld" : "Docked", tsl::onTextColor);

        // Apply changes every 3 frames (~50ms at 60fps, ~33ms at 90fps)
        if (this->pending_apply) {
            this->apply_counter++;
            if (this->apply_counter >= 3) {
                Result apply_rc = this->apply_with_override();
                // Don't let a single failed apply kill the overlay
                // Just log it and continue
                if (R_FAILED(apply_rc)) {
                    LOG("Failed to apply config: %#x\n", apply_rc);
                }
                this->pending_apply = false;
                this->apply_counter = 0;
            }
        }

        this->temp_header->setValue(format("%u°K",
            this->is_day ? this->config.profile.day_settings.temperature : this->config.profile.night_settings.temperature), tsl::onTextColor);
        this->daylight_header->setValue(this->is_day ? "Day" : "Night", tsl::onTextColor);
        this->sat_header->setValue(format("%.2f",
            this->is_day ? this->config.profile.day_settings.saturation  : this->config.profile.night_settings.saturation), tsl::onTextColor);
        this->hue_header->setValue(format("%.2f",
            this->is_day ? this->config.profile.day_settings.hue         : this->config.profile.night_settings.hue), tsl::onTextColor);
        this->contrast_header->setValue(format("%.2f",
            this->is_day ? this->config.profile.day_settings.contrast    : this->config.profile.night_settings.contrast), tsl::onTextColor);
        this->gamma_header->setValue(format("%.2f",
            this->is_day ? this->config.profile.day_settings.gamma       : this->config.profile.night_settings.gamma), tsl::onTextColor);
        this->luma_header->setValue(format("%.2f",
            this->is_day ? this->config.profile.day_settings.luminance   : this->config.profile.night_settings.luminance), tsl::onTextColor);
    }

    Config &get_config() {
        return this->config;
    }

private:
    Result rc;
    bool is_day;
    bool allow_high_temp;
    ApmPerformanceMode perf_mode = ApmPerformanceMode_Normal;
    Config config = {};
    tsl::elm::CustomDrawer      *info_header;
    tsl::elm::NamedStepTrackBar *profile_bar    = nullptr;
    tsl::elm::ListItem          *active_button;
    tsl::elm::ListItem          *period_button  = nullptr;
    TimeStepTrackBar            *dawn_slider    = nullptr;
    TimeStepTrackBar            *dusk_slider    = nullptr;
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
    tsl::elm::CategoryHeader *daylight_header = nullptr;
    tsl::elm::CategoryHeader *display_settings_header = nullptr;
    int display_mode_poll_counter = 0;
    
    // Frame-based throttling (simpler than time-based)
    int apply_counter;
    bool pending_apply;

    // Per-profile period state: override enum + original dusk/dawn times
    std::array<ProfilePeriodState, FizeauProfileId_Total> period_states = {};

    // How many profile slots (1..FizeauProfileId_Total) are actually defined
    // in config.ini.  Determines whether the profile bar is shown and how many
    // steps it has.  Computed once in the constructor.
    std::size_t num_profiles = 1;
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