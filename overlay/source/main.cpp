// FujinNX Tesla overlay: live temperatures + fan curve presets.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace {

constexpr const char* CONFIG_DIR    = "sdmc:/config/fujinnx";
constexpr const char* CONFIG_PATH   = "sdmc:/config/fujinnx/config.ini";
constexpr const char* STATUS_PATH   = "sdmc:/config/fujinnx/status.txt";

struct Point { int temp; int duty; };

struct Preset {
    const char* name;
    const char* note;
    Point points[6];
    int count;
};

const Preset PRESETS[] = {
    {"Quiet",    "fan stays low until ~42C", {{35, 20}, {42, 40}, {47, 70}, {52, 100}}, 4},
    {"Balanced", "100% at 50C",              {{30, 20}, {35, 35}, {40, 55}, {45, 80}, {50, 100}}, 5},
    {"Cool",     "100% at 45C (default)",    {{30, 20}, {35, 30}, {40, 60}, {43, 80}, {45, 100}}, 5},
    {"Max",      "fan always 100%",          {{30, 100}, {31, 100}}, 2},
};

const char* SOURCES[] = {"soc", "skin", "max"};
const char* SOURCE_LABELS[] = {"SoC", "Skin", "Hottest"};
const int MIN_DUTY_STEPS[] = {0, 10, 20, 30, 40, 50};

// Settings kept across preset changes.
struct Settings {
    int source = 2;     // index into SOURCES
    int min_duty = 20;
    int ramp_down = 3;
    int failsafe = 75;
};

Settings loadSettings() {
    Settings s;
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f)
        return s;
    char line[128];
    bool inSettings = false;
    while (fgets(line, sizeof(line), f)) {
        char* semi = strchr(line, ';');
        if (semi)
            *semi = 0;
        if (line[0] == '[') {
            inSettings = !strncmp(line, "[settings]", 10);
            continue;
        }
        char* eq = strchr(line, '=');
        if (!inSettings || !eq)
            continue;
        *eq = 0;
        const char* val = eq + 1;
        while (*val == ' ')
            val++;
        if (!strcmp(line, "source")) {
            s.source = !strncmp(val, "soc", 3) ? 0 : (!strncmp(val, "skin", 4) ? 1 : 2);
        } else if (!strcmp(line, "min_duty")) {
            s.min_duty = atoi(val);
        } else if (!strcmp(line, "ramp_down")) {
            s.ramp_down = atoi(val);
        } else if (!strcmp(line, "failsafe_temp")) {
            s.failsafe = atoi(val);
        }
    }
    fclose(f);
    return s;
}

void saveConfig(const Settings& s, const Preset& p) {
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f)
        return;
    fprintf(f, "; FujinNX fan curve. Edit here or with the Tesla overlay.\n");
    fprintf(f, "[settings]\n");
    fprintf(f, "source=%s\n", SOURCES[s.source]);
    fprintf(f, "min_duty=%d\n", s.min_duty);
    fprintf(f, "ramp_down=%d\n", s.ramp_down);
    fprintf(f, "failsafe_temp=%d\n", s.failsafe);
    fprintf(f, "[curve]\n");
    for (int i = 0; i < p.count; i++)
        fprintf(f, "%d=%d\n", p.points[i].temp, p.points[i].duty);
    fclose(f);
}

// Rewrites only the [settings] block, keeping the curve points as they are.
void saveSettingsKeepCurve(const Settings& s) {
    std::string curve;
    FILE* f = fopen(CONFIG_PATH, "r");
    if (f) {
        char line[128];
        bool inCurve = false;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '[')
                inCurve = !strncmp(line, "[curve]", 7);
            else if (inCurve)
                curve += line;
        }
        fclose(f);
    }
    if (curve.empty()) {
        saveConfig(s, PRESETS[2]);
        return;
    }
    f = fopen(CONFIG_PATH, "w");
    if (!f)
        return;
    fprintf(f, "; FujinNX fan curve. Edit here or with the Tesla overlay.\n");
    fprintf(f, "[settings]\nsource=%s\nmin_duty=%d\nramp_down=%d\nfailsafe_temp=%d\n[curve]\n%s",
            SOURCES[s.source], s.min_duty, s.ramp_down, s.failsafe, curve.c_str());
    fclose(f);
}

int readFanDuty(bool& ok) {
    ok = false;
    FILE* f = fopen(STATUS_PATH, "r");
    if (!f)
        return 0;
    char line[64];
    int duty = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "duty=", 5)) {
            duty = atoi(line + 5);
            ok = true;
        }
    }
    fclose(f);
    return duty;
}

std::string fmtTemp(s32 mc) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d.%d C", mc / 1000, (mc % 1000) / 100);
    return buf;
}

} // namespace

class GuiMain : public tsl::Gui {
public:
    GuiMain() : m_settings(loadSettings()) {}

    virtual tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("FujinNX", "Fan curve");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Live"));
        m_skin = new tsl::elm::ListItem("Skin");
        m_fan  = new tsl::elm::ListItem("Fan");
        list->addItem(m_skin);
        list->addItem(m_fan);

        list->addItem(new tsl::elm::CategoryHeader("Curve preset (A to apply)"));
        for (const Preset& p : PRESETS) {
            auto item = new tsl::elm::ListItem(p.name, p.note);
            item->setClickListener([this, &p](u64 keys) {
                if (keys & HidNpadButton_A) {
                    saveConfig(m_settings, p);
                    return true;
                }
                return false;
            });
            list->addItem(item);
        }

        list->addItem(new tsl::elm::CategoryHeader("Settings (A to change)"));
        m_source = new tsl::elm::ListItem("Temperature source");
        m_source->setValue(SOURCE_LABELS[m_settings.source]);
        m_source->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                m_settings.source = (m_settings.source + 1) % 3;
                m_source->setValue(SOURCE_LABELS[m_settings.source]);
                saveSettingsKeepCurve(m_settings);
                return true;
            }
            return false;
        });
        list->addItem(m_source);

        m_minDuty = new tsl::elm::ListItem("Minimum fan");
        m_minDuty->setValue(std::to_string(m_settings.min_duty) + " %");
        m_minDuty->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                constexpr int n = sizeof(MIN_DUTY_STEPS) / sizeof(MIN_DUTY_STEPS[0]);
                int next = MIN_DUTY_STEPS[0];
                for (int i = 0; i < n; i++) {
                    if (MIN_DUTY_STEPS[i] > m_settings.min_duty) {
                        next = MIN_DUTY_STEPS[i];
                        break;
                    }
                }
                m_settings.min_duty = next;
                m_minDuty->setValue(std::to_string(next) + " %");
                saveSettingsKeepCurve(m_settings);
                return true;
            }
            return false;
        });
        list->addItem(m_minDuty);

        frame->setContent(list);
        return frame;
    }

    virtual void update() override {
        // Refresh about once a second (the overlay runs at ~60 fps).
        if (m_frames++ % 60 != 0)
            return;
        s32 skin = 0;
        m_skin->setValue(R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin)) ? fmtTemp(skin) : "n/a");
        bool ok;
        int duty = readFanDuty(ok);
        m_fan->setValue(ok ? std::to_string(duty) + " %" : "sysmodule not running");
    }

private:
    Settings m_settings;
    u32 m_frames = 0;
    tsl::elm::ListItem *m_skin = nullptr, *m_fan = nullptr;
    tsl::elm::ListItem *m_source = nullptr, *m_minDuty = nullptr;
};

class FujinOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        // libtesla opens the fs service but does not mount the SD card;
        // without this, fopen fails and mkdir crashes the overlay loader.
        fsdevMountSdmc();
        tcInitialize();
    }
    virtual void exitServices() override {
        tcExit();
        fsdevUnmountDevice("sdmc");
    }
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiMain>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<FujinOverlay>(argc, argv);
}
