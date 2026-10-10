// FujinNX sysmodule: applies a user-defined temperature -> fan duty curve.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#define INNER_HEAP_SIZE 0x40000

#define CONFIG_DIR    "sdmc:/config/fujinnx"
#define CONFIG_PATH   CONFIG_DIR "/config.ini"
#define STATUS_PATH   CONFIG_DIR "/status.txt"

#define FAN_DEVICE_CODE 0x3D000001u
#define MAX_POINTS 16

typedef enum { SRC_SOC, SRC_SKIN, SRC_MAX } TempSource;

typedef struct {
    int temp;  // degrees C
    int duty;  // percent 0..100
} CurvePoint;

typedef struct {
    TempSource source;
    int min_duty;       // never drop below this while running
    int ramp_down;      // max percent the duty may fall per second
    int failsafe_temp;  // at/above this the fan is forced to 100%
    int num_points;
    CurvePoint points[MAX_POINTS];
} Config;

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();

    // Services must be opened while the sm session is still up.
    rc = tcInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);
    rc = tsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);
    rc = fanInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    smExit();
}

void __appExit(void)
{
    fanExit();
    tsExit();
    tcExit();
    fsdevUnmountAll();
    fsExit();
}

#ifdef __cplusplus
}
#endif

// Default curve: spins up earlier and harder than stock.
static void config_defaults(Config* c)
{
    static const CurvePoint def[] = {
        {30, 20}, {35, 30}, {40, 60}, {43, 80}, {45, 100},
    };
    c->source = SRC_MAX;
    c->min_duty = 20;
    c->ramp_down = 3;
    c->failsafe_temp = 75;
    c->num_points = sizeof(def) / sizeof(def[0]);
    memcpy(c->points, def, sizeof(def));
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void config_write_default(void)
{
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f)
        return;
    Config c;
    config_defaults(&c);
    fprintf(f, "; FujinNX fan curve. Edit here or with the Tesla overlay.\n");
    fprintf(f, "[settings]\n");
    fprintf(f, "source=max            ; soc | skin | max\n");
    fprintf(f, "min_duty=%d            ; fan never goes below this percent\n", c.min_duty);
    fprintf(f, "ramp_down=%d           ; max percent the fan may slow per second\n", c.ramp_down);
    fprintf(f, "failsafe_temp=%d      ; at or above this temperature the fan is forced to 100\n", c.failsafe_temp);
    fprintf(f, "[curve]               ; temperature_C=fan_percent\n");
    for (int i = 0; i < c.num_points; i++)
        fprintf(f, "%d=%d\n", c.points[i].temp, c.points[i].duty);
    fclose(f);
}

static void trim(char* s)
{
    char* semi = strchr(s, ';');
    if (semi)
        *semi = 0;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
}

static int cmp_points(const void* a, const void* b)
{
    return ((const CurvePoint*)a)->temp - ((const CurvePoint*)b)->temp;
}

// Returns true if a valid config was loaded; otherwise c keeps defaults.
static bool config_load(Config* c)
{
    config_defaults(c);
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f)
        return false;

    Config n;
    config_defaults(&n);
    n.num_points = 0;
    int section = 0; // 1 = settings, 2 = curve
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        char* p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            continue;
        if (*p == '[') {
            section = !strncmp(p, "[settings]", 10) ? 1 : (!strncmp(p, "[curve]", 7) ? 2 : 0);
            continue;
        }
        char* eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = 0;
        char* val = eq + 1;
        trim(p);
        while (*val == ' ')
            val++;
        if (section == 1) {
            if (!strcmp(p, "source"))
                n.source = !strncmp(val, "soc", 3) ? SRC_SOC : (!strncmp(val, "skin", 4) ? SRC_SKIN : SRC_MAX);
            else if (!strcmp(p, "min_duty"))
                n.min_duty = clampi(atoi(val), 0, 100);
            else if (!strcmp(p, "ramp_down"))
                n.ramp_down = clampi(atoi(val), 1, 100);
            else if (!strcmp(p, "failsafe_temp"))
                n.failsafe_temp = clampi(atoi(val), 50, 90);
        } else if (section == 2 && n.num_points < MAX_POINTS) {
            n.points[n.num_points].temp = clampi(atoi(p), 0, 120);
            n.points[n.num_points].duty = clampi(atoi(val), 0, 100);
            n.num_points++;
        }
    }
    fclose(f);

    if (n.num_points < 2)
        return false;
    qsort(n.points, n.num_points, sizeof(CurvePoint), cmp_points);
    *c = n;
    return true;
}

// Linear interpolation between curve points, flat outside the ends.
static int curve_duty(const Config* c, int temp_mc)
{
    const CurvePoint* p = c->points;
    int n = c->num_points;
    if (temp_mc <= p[0].temp * 1000)
        return p[0].duty;
    if (temp_mc >= p[n - 1].temp * 1000)
        return p[n - 1].duty;
    for (int i = 1; i < n; i++) {
        if (temp_mc <= p[i].temp * 1000) {
            int t0 = p[i - 1].temp * 1000, t1 = p[i].temp * 1000;
            if (t1 == t0)
                return p[i].duty;
            return p[i - 1].duty + (int)((s64)(p[i].duty - p[i - 1].duty) * (temp_mc - t0) / (t1 - t0));
        }
    }
    return 100;
}

static void write_status(int temp_mc, int duty)
{
    FILE* f = fopen(STATUS_PATH, "w");
    if (!f)
        return;
    fprintf(f, "duty=%d\ntemp_mc=%d\n", duty, temp_mc);
    fclose(f);
}

static time_t file_mtime(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void set_fan(FanController* fan, int duty)
{
    fanControllerSetRotationSpeedLevel(fan, clampi(duty, 0, 100) / 100.0f);
}

// Reads the SoC temperature in milli-C, using whichever ts API this firmware has.
static bool read_soc_milli_c(TsSession* session, bool session_ok, s32* out_mc)
{
    if (session_ok) {
        float t = 0.0f;
        if (R_SUCCEEDED(tsSessionGetTemperature(session, &t))) {
            *out_mc = (s32)(t * 1000.0f);
            return true;
        }
        return false;
    }
    return R_SUCCEEDED(tsGetTemperatureMilliC(TsLocation_External, out_mc));
}

int main(int argc, char* argv[])
{
    Result rc;

    // Services were opened in __appInit; just grab the fan controller.
    FanController fan;
    rc = fanOpenController(&fan, FAN_DEVICE_CODE);
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    // On 10.0.0+ the per-location session API is the reliable way to read ts.
    TsSession soc_session;
    bool soc_session_ok = false;
    if (hosversionAtLeast(10, 0, 0))
        soc_session_ok = R_SUCCEEDED(tsOpenSession(&soc_session, TsDeviceCode_LocationExternal));

    Config cfg;
    if (!file_mtime(CONFIG_PATH))
        config_write_default();
    config_load(&cfg);
    time_t cfg_mtime = file_mtime(CONFIG_PATH);

    // Boot marker: lets the overlay distinguish "launched" from "never ran".
    write_status(0, 100);

    tcDisableFanControl();

    int cur = 100;          // start high until we have a reading
    int last_written = -1;
    int last_temp_written = -1000000;
    u64 tick = 0;

    for (;;) {
        // Hot-reload the config when the overlay or the user changes it.
        if (tick % 2 == 0) {
            time_t m = file_mtime(CONFIG_PATH);
            if (m && m != cfg_mtime) {
                config_load(&cfg);
                cfg_mtime = m;
            }
        }

        s32 soc = 0, skin = 0;
        bool ok_soc = read_soc_milli_c(&soc_session, soc_session_ok, &soc);
        bool ok_skin = R_SUCCEEDED(tcGetSkinTemperatureMilliC(&skin));

        int temp_mc = 0;
        bool have = false;
        if (cfg.source == SRC_SOC && ok_soc) { temp_mc = soc; have = true; }
        else if (cfg.source == SRC_SKIN && ok_skin) { temp_mc = skin; have = true; }
        else if (cfg.source == SRC_MAX && (ok_soc || ok_skin)) {
            temp_mc = ok_soc && (!ok_skin || soc > skin) ? soc : skin;
            have = true;
        }

        int target;
        if (!have) {
            target = 100; // can't read the sensors: be safe
        } else if ((ok_soc && soc >= cfg.failsafe_temp * 1000) || (ok_skin && skin >= cfg.failsafe_temp * 1000)) {
            target = 100;
        } else {
            target = curve_duty(&cfg, temp_mc);
            if (target < cfg.min_duty)
                target = cfg.min_duty;
        }

        // Speed up immediately, slow down gradually.
        if (target >= cur)
            cur = target;
        else
            cur = (cur - target > cfg.ramp_down) ? cur - cfg.ramp_down : target;

        set_fan(&fan, cur);

        // Only touch the SD card when something meaningfully changed.
        if (cur != last_written || (temp_mc - last_temp_written > 2000 || last_temp_written - temp_mc > 2000)) {
            write_status(temp_mc, cur);
            last_written = cur;
            last_temp_written = temp_mc;
        }

        svcSleepThread(1000000000ULL);
        tick++;
    }

    // Unreachable while running; kept so an early exit hands the fan back to the OS.
    tcEnableFanControl();
    if (soc_session_ok)
        tsSessionClose(&soc_session);
    fanControllerClose(&fan);
    return 0;
}
