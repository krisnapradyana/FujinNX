// FujinNX Tesla overlay: live readings from the sysmodule, presets, settings.
// The overlay never touches sensors or the fan itself; the sysmodule is the
// single reader/writer and publishes its state in status.txt.
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
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

// Bounds for the simple two-point curve.
constexpr int LOW_TEMP_MIN = 20, HIGH_TEMP_MAX = 70;

// Settings kept across preset changes.
struct Settings {
    int source = 2;     // index into SOURCES
    int min_duty = 20;
    int ramp_down = 3;
    int failsafe = 75;
};

void trimEol(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
}

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

// Curve points as written in config.ini (in file order).
std::vector<Point> loadCurve() {
    std::vector<Point> pts;
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f)
        return pts;
    char line[128];
    bool inCurve = false;
    while (fgets(line, sizeof(line), f)) {
        char* semi = strchr(line, ';');
        if (semi)
            *semi = 0;
        if (line[0] == '[') {
            inCurve = !strncmp(line, "[curve]", 7);
            continue;
        }
        char* eq = strchr(line, '=');
        if (!inCurve || !eq)
            continue;
        *eq = 0;
        pts.push_back({atoi(line), atoi(eq + 1)});
    }
    fclose(f);
    return pts;
}

void saveConfigPoints(const Settings& s, const Point* pts, int count) {
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
    for (int i = 0; i < count; i++)
        fprintf(f, "%d=%d\n", pts[i].temp, pts[i].duty);
    fclose(f);
}

void saveConfig(const Settings& s, const Preset& p) {
    saveConfigPoints(s, p.points, p.count);
}

// Rewrites only the [settings] block, keeping the curve points as they are.
void saveSettingsKeepCurve(const Settings& s) {
    std::vector<Point> pts = loadCurve();
    if (pts.size() < 2) {
        saveConfig(s, PRESETS[2]);
        return;
    }
    saveConfigPoints(s, pts.data(), (int)pts.size());
}

// What the sysmodule last published.
struct Status {
    bool running = false;
    int duty = 0;
    std::string state, soc, skin;
};

std::string fmtTemp(const std::string& field) {
    // field is either "<milli-C>" or "err 0x...".
    if (field.empty())
        return "n/a";
    if (!strncmp(field.c_str(), "err", 3))
        return field;
    int mc = atoi(field.c_str());
    char buf[24];
    snprintf(buf, sizeof(buf), "%d.%d C", mc / 1000, (mc % 1000) / 100);
    return buf;
}

Status readStatus() {
    Status st;
    FILE* f = fopen(STATUS_PATH, "r");
    if (!f)
        return st;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        trimEol(l);
        if (!l.compare(0, 5, "duty=")) {
            st.duty = atoi(l.c_str() + 5);
            st.running = true;
        } else if (!l.compare(0, 6, "state=")) {
            st.state = l.substr(6);
        } else if (!l.compare(0, 4, "soc=")) {
            st.soc = l.substr(4);
        } else if (!l.compare(0, 5, "skin=")) {
            st.skin = l.substr(5);
        }
    }
    fclose(f);
    return st;
}

} // namespace

class GuiMain : public tsl::Gui {
public:
    GuiMain() : m_settings(loadSettings()) {
        // Simple-curve endpoints shown in the settings: first and last point.
        std::vector<Point> pts = loadCurve();
        if (pts.size() >= 2) {
            m_lowTemp = pts.front().temp;
            m_highTemp = pts.back().temp;
        }
    }

    virtual tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("FujinNX", "Fan curve");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Live"));
        m_soc   = new tsl::elm::ListItem("SoC");
        m_skin  = new tsl::elm::ListItem("Skin");
        m_fan   = new tsl::elm::ListItem("Fan");
        m_state = new tsl::elm::ListItem("Sysmodule");
        list->addItem(m_soc);
        list->addItem(m_skin);
        list->addItem(m_fan);
        list->addItem(m_state);

        list->addItem(new tsl::elm::CategoryHeader("Curve preset (A to apply)"));
        for (const Preset& p : PRESETS) {
            auto item = new tsl::elm::ListItem(p.name, p.note);
            item->setClickListener([this, &p](u64 keys) {
                if (keys & HidNpadButton_A) {
                    saveConfig(m_settings, p);
                    m_lowTemp = p.points[0].temp;
                    m_highTemp = p.points[p.count - 1].temp;
                    refreshSimpleRows();
                    return true;
                }
                return false;
            });
            list->addItem(item);
        }

        list->addItem(new tsl::elm::CategoryHeader("Simple curve (A = up, Y = down)"));
        m_lowRow = new tsl::elm::ListItem("Fan starts at", "min fan below this");
        m_lowRow->setClickListener([this](u64 keys) {
            int step = (keys & HidNpadButton_A) ? 1 : (keys & HidNpadButton_Y) ? -1 : 0;
            if (!step)
                return false;
            int next = m_lowTemp + step;
            if (next < LOW_TEMP_MIN || next >= m_highTemp)
                return true;
            m_lowTemp = next;
            saveSimpleCurve();
            return true;
        });
        list->addItem(m_lowRow);

        m_highRow = new tsl::elm::ListItem("100% fan at", "linear ramp between the two");
        m_highRow->setClickListener([this](u64 keys) {
            int step = (keys & HidNpadButton_A) ? 1 : (keys & HidNpadButton_Y) ? -1 : 0;
            if (!step)
                return false;
            int next = m_highTemp + step;
            if (next > HIGH_TEMP_MAX || next <= m_lowTemp)
                return true;
            m_highTemp = next;
            saveSimpleCurve();
            return true;
        });
        list->addItem(m_highRow);
        refreshSimpleRows();

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
        Status st = readStatus();
        m_soc->setValue(st.running ? fmtTemp(st.soc) : "n/a");
        m_skin->setValue(st.running ? fmtTemp(st.skin) : "n/a");
        m_fan->setValue(st.running ? std::to_string(st.duty) + " %" : "n/a");
        m_state->setValue(!st.running ? "not running" : (st.state.empty() ? "running" : st.state));
    }

private:
    // Two-point curve: minimum fan at the low temperature, 100% at the high one.
    void saveSimpleCurve() {
        Point pts[2] = {{m_lowTemp, m_settings.min_duty}, {m_highTemp, 100}};
        saveConfigPoints(m_settings, pts, 2);
        refreshSimpleRows();
    }

    void refreshSimpleRows() {
        if (m_lowRow)
            m_lowRow->setValue(std::to_string(m_lowTemp) + " C");
        if (m_highRow)
            m_highRow->setValue(std::to_string(m_highTemp) + " C");
    }

    Settings m_settings;
    int m_lowTemp = 30, m_highTemp = 45;
    u32 m_frames = 0;
    tsl::elm::ListItem *m_soc = nullptr, *m_skin = nullptr, *m_fan = nullptr, *m_state = nullptr;
    tsl::elm::ListItem *m_lowRow = nullptr, *m_highRow = nullptr;
    tsl::elm::ListItem *m_source = nullptr, *m_minDuty = nullptr;
};

class FujinOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        // libtesla opens the fs service but does not mount the SD card;
        // without this, fopen fails and mkdir crashes the overlay loader.
        fsdevMountSdmc();
    }
    virtual void exitServices() override {
        fsdevUnmountDevice("sdmc");
    }
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiMain>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<FujinOverlay>(argc, argv);
}
