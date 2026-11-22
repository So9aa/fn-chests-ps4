// fn_chests - GoldHEN plugin for Fortnite CUSA07022 / CUSA07669 (own EZFN server, offline use).
// Calls FortCheatManager functions through UObject::ProcessEvent, on the game thread (Detour on ProcessEvent).
// Triggers: pad combos (L1 + D-pad) and text commands on TCP 9099 (fn_cmd.bat). Log: /data/GoldHEN/fn_chests_log.txt
// Build: OpenOrbis toolchain + GoldHEN Plugins SDK, same flow as the BO2 plugins (BUILD_FN_CHESTS.bat).
// Never run on a console by the author of this file; see README "What can still go wrong".
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <orbis/libkernel.h>
#include <orbis/Net.h>
#include <orbis/UserService.h>
#include <orbis/_types/pad.h>
#include "plugin_common.h"

attr_public const char *g_pluginName = "fn_chests";
attr_public const char *g_pluginDesc = "Fortnite FortCheatManager caller (private server)";
attr_public const char *g_pluginAuth = "B33";
#define BUILD_VER "v133"
#define BUILD_TAG "BUILD-V133 (smooth ESP: learned level actor walk, 100 Hz positions, nearest-only drawing)"
#define BOOT_PATH "/data/GoldHEN/fn_chests_boot.txt"     // one letter per stage of plugin_load
attr_public uint32_t g_pluginVersion = 0x00001B10;

// ---- config ---------------------------------------------------------------------------------------------
#define LOG_PATH        "/data/GoldHEN/fn_chests_log.txt"
// Runtime copies: a console that never ran this plugin may not have /data/GoldHEN writable (or at all),
// so bootstrap_fs() picks the first writable location at load time and everything else follows it.
static char g_log_path[96] = "/data/GoldHEN/fn_chests_log.txt";
static char g_cfg_path[96] = "/data/GoldHEN/fn_chests_cfg.txt";
static char g_fs_base[96]  = "/data/GoldHEN";        // the GoldHEN folder this console actually uses
#define CMD_PORT        9099
#define START_DELAY_US  (30u * 1000000u)      // wait before patching the game code (engine init)
#define QUEUE_SIZE      16
#define PAD_POLL_MS     16

// ---- phase breadcrumbs ---------------------------------------------------------------------------
// The pad reader runs on its own thread and prints a line every ~second. It also prints the phase the
// GAME thread is in, so if the title dies the last line of the log says whether it died inside our HUD
// hook, our tick, a game stub call, ... instead of leaving us with a silent log.
#define PH_IDLE 0
#define PH_PE 1
#define PH_TICK 2
#define PH_MATCH 3
#define PH_WORLD 4
#define PH_ESP 5
#define PH_TOUR 6
#define PH_UI 7
#define PH_HUD 8
#define PH_DRAW 9
#define PH_STUB 10
static const char *const PH_NAMES[] = { "idle", "pe", "tick", "match", "world", "esp", "tour", "ui", "hud", "draw", "stub" };
// ---- remote bisect flags: /data/GoldHEN/fn_chests_cfg.txt ------------------------------------------
// One flag per line (or comma separated), read once at plugin start. Lets individual parts be switched
// off without a rebuild while a crash is being hunted:
//   off      - install nothing at all (the plugin is completely inert, like not loaded)
//   nocvars  - never touch console cvars (no Graphics presets)
//   nohud    - never patch the AHUD PostRender vtable (no menu / ESP / watermark)
//   nopad    - never patch the libScePad stubs (no R1+Square menu, no tapout)
//   nogvp    - never redirect the GetPlayerViewPoint vtable slot (no native silent aim)
#define CFG_PATH "/data/GoldHEN/fn_chests_cfg.txt"
static void logf_(const char *fmt, ...);
static void aim_resolve_fire(void);
static int find_methods_on(uint64_t cls, const char *const *names, uint64_t *out, int n);
static int cfg_off = 0, cfg_nocvars = 0, cfg_nohud = 0, cfg_nopad = 0, cfg_nogvp = 0;
static int g_supported_title = 0;               // CUSA07022 / CUSA07669: the two Fortnite regions
static void cfg_load(void) {
    char buf[512];
    int fd = sceKernelOpen(g_cfg_path, 0x0000, 0);
    if (fd < 0) return;
    int n = sceKernelRead(fd, buf, sizeof buf - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *t = buf; *t; ) {
        while (*t == ' ' || *t == '\n' || *t == '\r' || *t == ',' || *t == '\t') t++;
        if (*t == '#') { while (*t && *t != '\n') t++; continue; }   // comment line in the generated config
        char *e = t;
        while (*e && *e != ' ' && *e != '\n' && *e != '\r' && *e != ',' && *e != '\t') e++;
        int len = (int)(e - t);
        if (len == 3 && !strncmp(t, "off", 3)) cfg_off = 1;
        else if (len == 7 && !strncmp(t, "nocvars", 7)) cfg_nocvars = 1;
        else if (len == 5 && !strncmp(t, "nohud", 5)) cfg_nohud = 1;
        else if (len == 5 && !strncmp(t, "nopad", 5)) cfg_nopad = 1;
        else if (len == 5 && !strncmp(t, "nogvp", 5)) cfg_nogvp = 1;
        t = e;
    }
    logf_("cfg: off=%d nocvars=%d nohud=%d nopad=%d nogvp=%d", cfg_off, cfg_nocvars, cfg_nohud, cfg_nopad, cfg_nogvp);
}
static volatile int g_phase = PH_IDLE;
static void ph(int i) { g_phase = i; }
// last ProcessEvent calls (function + self) so a stall report can say what the game thread was doing
#define PE_RING 8
static struct { uint64_t fn, self; } g_pe_ring[PE_RING];
static volatile uint32_t g_pe_ri = 0, g_pe_ctr = 1;

// build-specific addresses (eboot.bin, fixed module addresses). Regen: dump_objects.bat + probe_live.bat, see notes/fn_findings.txt
#define A_PROCESSEVENT      0xfaadbcULL     // UObject::ProcessEvent (vtable slot 72), verified live
#define A_GENGINE_A         0x154f3fa8ULL   // GEngine global candidates (probe_live)
#define A_GENGINE_B         0x15524688ULL
#define A_CLS_FORTENGINE    0x128bcbe0ULL   // UClass objects (static module data)
#define A_CLS_GAMEINSTANCE  0x128d4550ULL
#define A_CLS_CHEATMANAGER  0x13144eb8ULL
#define ENGINE_SCAN_SIZE    0x4000          // GameInstance pointer lives inside UEngine (not in the first 0x1000)
#define OBJ_CLASS           0x10            // UObject::ClassPrivate
#define GI_LOCALPLAYERS     0x38            // GameInstance.LocalPlayers (TArray: data, num)
#define LP_CONTROLLER       0x30            // LocalPlayer.PlayerController
#define A_GOBJECTS          0x15494740ULL   // chunked FUObjectItem array: +0 chunk table, 24-byte items, 65536 per chunk
#define A_CLS_CONTAINER     0x1250a7c0ULL   // BuildingContainer
#define A_CLS_INTERACTION   0x125f0be8ULL   // FortControllerComponent_Interaction (owns ServerAttemptInteract)
#define A_FN_GETPAWN        0x112d98d8ULL   // Controller.K2_GetPawn
#define A_FN_GETLOC         0x111a3c10ULL   // Actor.K2_GetActorLocation
#define A_GETPLAYERVIEWPOINT 0x10ed8ec8ULL  // AController::GetPlayerViewPoint (native address unusable on this build)
#define A_HITNOTIFY       0x12cb4be8ULL  // AFortPawn::OnDisplayHitNotify (native)
#define A_FN_GETROT         0x111a3d60ULL   // Actor.K2_GetActorRotation
#define A_FN_TELEPORT       0x111a5a48ULL   // Actor.K2_TeleportTo(DestLocation, DestRotation) -> bool
#define A_FN_SERVERCHEAT    0x13165910ULL   // ServerCheat(FString Msg)
#define A_FN_GETVEHICLE     0x130dff00ULL   // FortPlayerPawn.GetVehicleActor() -> AActor*
#define A_FN_INTERACT       0x125ed848ULL   // FortControllerComponent_Interaction.ServerAttemptInteract(ReceivingActor, ...)
#define VEH_WATER_LEVEL     0x0e94ULL   // AFortAthenaVehicle.WaterLevel (float, world Z of the surface)
#define VEH_WATER_OVERLAP   0x0e98ULL   // AFortAthenaVehicle.WaterOverlapCount (int32, 0 = hull is dry)
#define OBJ_OUTER           0x20
#define MAX_CONT            4096
#define STEP_MS             250
#define TP_WAIT_MS          700
#define INT_WAIT_MS         2500            // 2.5 s per chest: stay on the container while the server runs the search
#define AURA_RADIUS         320.0           // game units (~3 m)
#define AURA_SCAN_MS        10000
#define AURA_TICK_MS        300
#define TOUR_STEP_MS        200             // dwell at a chest before the next leg (continuous flight); 'speed <ms>'
#define TOUR_ARRIVE_MS      700             // max time to get the pawn onto the target
#define TOUR_PIN_MS         25              // re-send the teleport this often until it holds
#define TOUR_NEAR           150.0           // close enough to interact
#define TOUR_RANGE          600.0           // max distance from tour point to accept a re-found chest
#define TP_LIFT             120.0           // teleport this far above the container
#define FLY_STEP_MS         16              // fly: one movement step this often (~60 Hz, smooth)
#define FLY_STEP_UU         3000.0          // fly: safety cap per step
#define FLY_SPEED_UU        5500.0          // fly: constant speed, units/second (no easing, dead straight)
#define FLY_NEAR            120.0           // fly: close enough to stop (no snap teleport at all)
#define FLY_SETTLE_MS       200             // after the flight: let the interact land, then keep flying
#define ACTOR_ROOT          0x1b0           // AActor.RootComponent
#define SCENE_REL_LOC       0x138           // USceneComponent.RelativeLocation (FVector, 3 doubles)
#define PC_CHEATMANAGER     0x3e0           // PlayerController.CheatManager. FILL: dedotto (CheatManagerClass a +0x3d8 e' confermato); 'status' lo mostra

typedef struct { const char *name; uint64_t addr; int on_controller; uint32_t pad_combo; int sp; } Fn;   // sp != 0: handled by the plugin itself (no cheat call)
// UFunction objects from objects_dump.txt. All assumed to take no parameters.
static const Fn FNS[] = {
    { "enablecheats", 0x114a1a08ULL, 1, 0 },                                    // PlayerController.EnableCheats
    { "tpchest",      0, 0, ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_UP, 1 },   // own impl: teleport to nearest not-yet-visited container  // TeleportToNearestUnsearchedContainer
    { "chests",       0, 0, 0, 2 },  // own impl: visit + interact with every container
    { "probe",        0, 0, 0, 3 },
    { "stop",         0, 0, 0, 4 },
    { "reset",        0, 0, 0, 5 },
    { "trace",        0, 0, 0, 6 },
    { "chest1",       0, 0, 0, 7 },
    { "near",         0, 0, 0, 8 },     // interact with the nearest chest without teleporting
    { "sc",           0, 0, 0, 9 },
    { "radius",       0, 0, 0, 11 },
    { "tour",         0, 0, ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R3, 12 },
    { "speed",        0, 0, 0, 13 },
    { "menu",         0, 0, ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_SQUARE, 14 }, // R1+Square: open/close the menu
    { "aura",         0, 0, ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1, 10 },   // toggle: auto-open every chest within AURA_RADIUS while you walk     // ServerCheat(<text>) on the server     // teleport to the nearest unvisited chest, interact once, stay there  // OpenAllContainersAndWriteLootEconomy
    { "god",          0x13141ee0ULL, 0, 0 },  // ToggleUnlimitedHealth
    { "ammo",         0x131404a0ULL, 0, 0 },  // ToggleInfiniteAmmo
    { "stamina",      0x13140740ULL, 0, 0 },                                    // ToggleInfiniteStamina
    { "freecam",      0x1313fe80ULL, 0, 0 },                                    // ToggleFreeCamera
    { "buildmats",    0x13141e00ULL, 0, 0 },                                    // ToggleUnlimitedBuildMaterials
    { "nocooldown",   0x131410e0ULL, 0, 0 },                                    // ToggleNoCoolDown
    { "durability",   0x13140660ULL, 0, 0 },                                    // ToggleInfiniteDurability
    { "consumables",  0x13140580ULL, 0, 0 },                                    // ToggleInfiniteConsumables
    { "gofast",       0x1311e628ULL, 0, 0 },                                    // GoFast
    { "clearfog",     0x1310f9b8ULL, 0, 0 },                                    // ClearMapFog
    { "cannon",       0, 0, 0, 15 },                                            // toggle: only teleport while seated in a cannon
    { "perf",         0, 0, 0, 16 },                                            // toggle the graphics/perf preset (console-unlocker style cvars)
    { "esp",          0, 0, 0, 17 },                                            // toggle the cannon ESP (boxes + distance)
    { "pesp",         0, 0, 0, 19 },                                            // toggle the player ESP (name + BOT/PLAYER)
    { "vesp",         0, 0, 0, 20 },                                            // toggle the vehicle ESP (box + tower)
    { "instant",      0, 0, 0, 21 },                                            // toggle INSTANT teleport mode (no flight)
    { "hpbar",        0, 0, 0, 22 },                                            // toggle player health/shield bars
    { "skel",         0, 0, 0, 23 },                                            // toggle the player skeleton
    { "chestesp",     0, 0, 0, 24 },                                            // toggle chest ESP
    { "snap",         0, 0, 0, 25 },                                            // toggle snaplines
    { "aim",          0, 0, 0, 26 },                                            // toggle the L2 aimbot
    { "aimpart",      0, 0, 0, 27 },                                            // cycle the aimbot body part
    { "aimfov",       0, 0, 0, 28 },                                            // cycle the aimbot FOV
    { "aimsmooth",    0, 0, 0, 32 },                                            // cycle the aimbot smoothing
    { "fov",          0, 0, 0, 33 },                                            // cycle the camera FOV
    { "gotocannon",   0, 0, 0, 18 },                                            // fly to the nearest cannon (no gravity, no snap)
    { "kickguard",     0, 0, 0, 34 },                                            // toggle the anti cannon kick (auto re-seat)
    { "waterguard",    0, 0, 0, 35 },                                            // toggle the water guard (hold the cannon above water)
    { "seatlock",      0, 0, 0, 36 },                                            // toggle the seat lock (block every ejection call)
    { "tower",         0, 0, 0, 37 },                                            // own impl: hold the cannon above every player (fly or instant)
    { "towerheight",   0, 0, 0, 38 },                                            // tower: how far above the player the cannon sits
    { "ghost",         0, 0, 0, 39 },                                            // hide the ridden cannon (OFF by default)
};
#define NFNS (sizeof FNS / sizeof FNS[0])

// ---- plumbing -------------------------------------------------------------------------------------------
static void logf_(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf - 1, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    int fd = sceKernelOpen(g_log_path, 0x0001 | 0x0200 | 0x0008, 0666);   // WRONLY | CREAT | APPEND
    if (fd < 0) return;
    sceKernelWrite(fd, buf, n);
    sceKernelClose(fd);
}
static uint64_t now_ms(void) { return sceKernelGetProcessTime() / 1000; }

// ---- first-run filesystem bootstrap ---------------------------------------------------------------------
// A .sprx copied onto a console that never ran this plugin must not need anything prepared by hand, so on
// load it creates the directories it writes into, picks the first writable log location (with a fallback if
// /data is read-only), rotates a log that grew too large, and writes a documented default config file once.
// Everything here is best-effort: a failure only costs a log line, never the plugin.
static const char CFG_DEFAULT[] =
    "# fn_chests plugin options - one flag per line. '#' starts a comment; an empty file means all defaults.\n"
    "#   off      - install nothing at all (the plugin is completely inert, like not loaded)\n"
    "#   nocvars  - never touch console cvars (no Graphics presets)\n"
    "#   nohud    - never patch the AHUD PostRender vtable (no menu / ESP / watermark)\n"
    "#   nopad    - never patch the libScePad stubs (no R1+Square menu, no tapout)\n"
    "#   nogvp    - never redirect the GetPlayerViewPoint vtable slot (no native silent aim)\n"
    "# Delete this file to have it recreated on the next boot.\n";
static int readable(uint64_t p, uint64_t n);        // defined with the object walk, used to check argv
static int fs_try(const char *p, int trunc) {
    int fd = sceKernelOpen(p, 0x0001 | 0x0200 | (trunc ? 0x0400 : 0x0008), 0666);   // WRONLY|CREAT|(TRUNC|APPEND)
    if (fd < 0) return 0;
    sceKernelClose(fd);
    return 1;
}
// on the console /data/GoldHEN and /data/goldhen are distinct paths: keep both, so a plugins.ini written for
// either of them keeps working after this build is dropped in
static void fs_mkdirs(void) {
    sceKernelMkdir("/data", 0777);
    sceKernelMkdir("/data/GoldHEN", 0777);
    sceKernelMkdir("/data/GoldHEN/plugins", 0777);
    sceKernelMkdir("/data/goldhen", 0777);
    sceKernelMkdir("/data/goldhen/plugins", 0777);
    sceKernelMkdir("/user", 0777);
    sceKernelMkdir("/user/data", 0777);
    sceKernelMkdir("/user/data/GoldHEN", 0777);
    sceKernelMkdir("/user/data/GoldHEN/plugins", 0777);
}
// One line per stage of plugin_load. The GoldHEN loader gives the plugin no console of its own, so if the
// game dies before the first log line this file is the only trace of how far the plugin got.
static void boot_mark(char stage) {
    int fd = sceKernelOpen(BOOT_PATH, 0x0001 | 0x0200 | 0x0008, 0666);           // WRONLY|CREAT|APPEND
    if (fd < 0) fd = sceKernelOpen("/data/goldhen/fn_chests_boot.txt", 0x0001 | 0x0200 | 0x0008, 0666);
    if (fd < 0) return;
    char b[2]; b[0] = stage; b[1] = '\n';
    sceKernelWrite(fd, b, 2);
    sceKernelClose(fd);
}
// Last stage letter written by a PREVIOUS start (0 if the file is missing or empty). 'F' means that start
// reached the end of plugin_load; anything else means it died while loading.
static int boot_last_stage(char *out) {
    int fd = sceKernelOpen(BOOT_PATH, 0x0000, 0);
    if (fd < 0) return 0;                                  // first start with this build
    char buf[256];
    int64_t end = sceKernelLseek(fd, 0, 2);
    if (end <= 0) { sceKernelClose(fd); return 0; }
    int64_t from = end > (int64_t)sizeof buf ? end - (int64_t)sizeof buf : 0;
    sceKernelLseek(fd, from, 0);
    int r = (int)sceKernelRead(fd, buf, sizeof buf);
    sceKernelClose(fd);
    if (r <= 0) return 0;
    int i = r - 1;
    while (i >= 0 && (buf[i] == '\n' || buf[i] == '\r')) i--;   // last non-empty line
    if (i < 0) return 0;
    *out = buf[i];                                              // lines are one letter: "A\n"
    return 1;
}
// v126: the crash-loop marker belongs to one build. A marker left by an older sprx (or a copy dropped over
// an existing one) must not make the new build inert on its first start.
static int boot_same_build(void) {
    int fd = sceKernelOpen(BOOT_PATH, 0x0000, 0);
    if (fd < 0) fd = sceKernelOpen("/data/goldhen/fn_chests_boot.txt", 0x0000, 0);
    if (fd < 0) return 0;
    char b[96];
    int r = (int)sceKernelRead(fd, b, sizeof b - 1);
    sceKernelClose(fd);
    if (r <= 0) return 0;
    b[r] = 0;
    int n = 0;
    while (n < r && b[n] != '\n' && b[n] != '\r') n++;
    b[n] = 0;
    return !strcmp(b, BUILD_TAG);
}
static void boot_reset(void) {
    int fd = sceKernelOpen(BOOT_PATH, 0x0001 | 0x0200 | 0x0400, 0666);      // WRONLY|CREAT|TRUNC
    if (fd < 0) fd = sceKernelOpen("/data/goldhen/fn_chests_boot.txt", 0x0001 | 0x0200 | 0x0400, 0666);
    if (fd < 0) return;
    sceKernelWrite(fd, BUILD_TAG "\n", sizeof(BUILD_TAG));   // tag + newline (sizeof includes the NUL)
    sceKernelClose(fd);
}
// "dir" + "name" into dst, no stdio at all: this runs on the GoldHEN loader thread, before anything else
static void path_join(char *dst, int n, const char *dir, const char *name) {
    int d = 0, k = 0;
    while (dir[d] && d < n - 1) { dst[d] = dir[d]; d++; }
    while (name[k] && d + k < n - 1) { dst[d + k] = name[k]; k++; }
    dst[d + k] = 0;
}
static void bootstrap_fs(void) {
    boot_mark('a');
    fs_mkdirs();
    boot_mark('b');
    boot_mark('c');
    const char *dirs[6]; int nd = 0;
    // The old path is the default again: everything this plugin has always written lives in /data/GoldHEN.
    // The lowercase spelling is only used when the uppercase folder is not writable, so a console that spells
    // it /data/goldhen still works - the file is never moved away from the path it has always had.
    // /user/data/GoldHEN is where GoldHEN actually lives on this console (/data/GoldHEN is the same folder,
    // it is just the shorter spelling), so that spelling is tried first and everything else follows.
    dirs[nd++] = "/user/data/GoldHEN";
    dirs[nd++] = "/data/GoldHEN";
    dirs[nd++] = "/user/data/goldhen";
    dirs[nd++] = "/data/goldhen";
    dirs[nd++] = "/data";
    dirs[nd++] = "/dev_hdd0/tmp";
    char cand[128]; int chosen = -1;
    for (int i = 0; i < nd; i++) {
        if (strlen(dirs[i]) > 80) continue;
        path_join(cand, (int)sizeof cand, dirs[i], "/fn_chests_log.txt");
        if (!fs_try(cand, 0)) continue;
        chosen = i;
        path_join(g_log_path, (int)sizeof g_log_path, dirs[i], "/fn_chests_log.txt");
        path_join(g_cfg_path, (int)sizeof g_cfg_path, dirs[i], "/fn_chests_cfg.txt");
        path_join(g_fs_base, (int)sizeof g_fs_base, dirs[i], "");
        break;
    }
    boot_mark(chosen < 0 ? 'z' : 'd');      // z = no folder was writable at all
    int fell = (chosen > 3);                // 0..3 = a GoldHEN folder, 4+ = /data, /dev_hdd0/tmp or the plugin's own dir
    int fd = sceKernelOpen(g_log_path, 0x0001 | 0x0200 | 0x0008, 0666);   // rotate a log that grew too big
    if (fd >= 0) {
        int64_t sz = sceKernelLseek(fd, 0, 2);
        sceKernelClose(fd);
        if (sz > (16 << 20)) {
            if (fs_try(g_log_path, 1)) logf_("fs: log rotated (was %ld MB)", (long)(sz >> 20));
        }
    }
    boot_mark('e');
    logf_("fs: base %s", g_fs_base);
    logf_("fs: log %s%s", g_log_path, fell ? " (fallback - neither GoldHEN folder is writable)" : "");
    fd = sceKernelOpen(g_cfg_path, 0x0000, 0);               // documented default config, only when missing
    if (fd >= 0) { sceKernelClose(fd); boot_mark('f'); return; }
    fd = sceKernelOpen(g_cfg_path, 0x0001 | 0x0200 | 0x0400, 0666);
    if (fd < 0) { logf_("fs: cannot create %s", g_cfg_path); return; }
    sceKernelWrite(fd, CFG_DEFAULT, sizeof CFG_DEFAULT - 1);
    sceKernelClose(fd);
    logf_("fs: wrote default config %s", g_cfg_path);
    boot_mark('f');
}

// ---- per-frame cost instrumentation (v80) ---------------------------------------------------------------
// RDTSC on the PS4 Jaguar is good enough to compare menu/ESP cost against the frame budget. Counters are
// raw cycles; ui_stats_log derives MHz from cycles/ms between two reports, so no clock assumption is needed.
static inline uint64_t rcyc(void) { return __builtin_ia32_rdtsc(); }
static volatile uint64_t g_cy_menu, g_cy_menu_max, g_cy_esp, g_cy_esp_max, g_cy_scan, g_cy_scan_max;
static volatile uint32_t g_n_menu, g_n_esp, g_n_scan;
static uint64_t g_stat_cyc0, g_stat_ms0, g_stat_draw0, g_stat_frame0;

static int writable(uint64_t p, uint64_t n) {          // is this range safe to store into?
    void *st = 0, *en = 0; int prot = 0;
    if (sceKernelQueryMemoryProtection((void *)p, &st, &en, &prot) != 0 || (uint64_t)st > p) return 0;
    return (prot & 2) && (uint64_t)en - p >= n;
}
static int readable(uint64_t p, uint64_t n) {
    while (n) {
        void *st = 0, *en = 0; int prot = 0;
        if (sceKernelQueryMemoryProtection((void *)p, &st, &en, &prot) != 0 || (uint64_t)st > p || !(prot & 1)) return 0;
        uint64_t left = (uint64_t)en - p;
        if (!left) return 0;          // a zero-length range would spin this loop forever (game frozen, not crashed)
        if (left >= n) return 1;
        n -= left; p = (uint64_t)en;
    }
    return 1;
}
static uint64_t rd64(uint64_t a) { return readable(a, 8) ? *(volatile uint64_t *)a : 0; }
static uint64_t frd64(uint64_t a);                 // cached-range read, defined with the object walk
static int fast_ok(uint64_t p, uint64_t n);        // cached readable-range check, same place
static int plausible(uint64_t p) { return (p >= 0x400000ULL && p < 0x16000000ULL) || (p >= 0x1000000000ULL && p < 0x4000000000ULL); }
// The Athena island fits well inside these bounds. Anything outside is a misread pointer, not a chest:
// teleporting there is what dropped the player out of the map.
static int in_world(double x, double y, double z) {
    if (!(x == x && y == y && z == z)) return 0;
    if (x == 0 && y == 0 && z == 0) return 0;
    double ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    return ax < 400000.0 && ay < 400000.0 && z > -60000.0 && z < 60000.0;
}

static inline int ld(volatile int *v) { return __atomic_load_n(v, __ATOMIC_ACQUIRE); }
static inline void st(volatile int *v, int x) { __atomic_store_n(v, x, __ATOMIC_RELEASE); }

// ---- game objects ---------------------------------------------------------------------------------------
typedef void (*pe_t)(void *self, void *fn, void *params);
static Detour g_det;
static void *g_stub = 0;                 // trampoline into the original ProcessEvent
static volatile int g_hooked = 0, g_busy = 0;
static uint64_t g_pc = 0, g_eng = 0;

static int resolve_engine(void) {
    if (g_eng && rd64(g_eng + OBJ_CLASS)) return 1;
    const uint64_t globals[2] = { A_GENGINE_A, A_GENGINE_B };
    for (int i = 0; i < 2; i++) {
        uint64_t e = rd64(globals[i]);
        if (plausible(e) && rd64(e + OBJ_CLASS) == A_CLS_FORTENGINE) { g_eng = e; return 1; }
    }
    return 0;
}

static int resolve_pc(void) {
    static int rp_n = 0;
    if (rp_n < 3) logf_("resolve_pc: enter %d (eng %lx pc %lx)", ++rp_n, g_eng, g_pc);
    // Always re-walk the chain: the PlayerController is replaced when a match is loaded, and a stale
    // pointer must never reach ProcessEvent (that crashed the title during map travel).
    if (!resolve_engine()) { if (!g_pc) logf_("GEngine not found at %lx / %lx", A_GENGINE_A, A_GENGINE_B); return 0; }
    uint64_t eng = g_eng;
    uint64_t gi = 0;                                   // re-walk every call: v56 gave up caching, keep it that way
    if (!readable(eng, ENGINE_SCAN_SIZE)) { if (!g_pc) logf_("engine %lx not readable", eng); return 0; }
    for (uint64_t o = 0; o < ENGINE_SCAN_SIZE; o += 8) {
        uint64_t p = frd64(eng + o);
        if (plausible(p) && frd64(p + OBJ_CLASS) == A_CLS_GAMEINSTANCE) { gi = p; break; }
    }
    if (!gi) { if (!g_pc) logf_("GameInstance not found inside engine %lx", eng); return 0; }
    uint64_t data = rd64(gi + GI_LOCALPLAYERS);
    if (!plausible(data)) return 0;
    uint64_t lp = rd64(data);
    if (!plausible(lp)) return 0;
    uint64_t pc = rd64(lp + LP_CONTROLLER);
    if (!plausible(pc) || !rd64(pc + OBJ_CLASS)) { if (!g_pc) logf_("no PlayerController yet"); return 0; }
    if (pc != g_pc) {
        g_pc = pc;
        logf_("chain ok: engine %lx gi %lx lp %lx pc %lx", eng, gi, lp, pc);
    }
    return 1;
}

static void call_pe(uint64_t self, uint64_t fn) {
    uint8_t params[64] __attribute__((aligned(16)));
    memset(params, 0, sizeof params);
    ((pe_t)g_stub)((void *)self, (void *)fn, params);
}


// Locate the CheatManager inside the controller: exact class match first, then the fixed guess.
static uint64_t find_cm(void) {
    uint64_t v = rd64(g_pc + PC_CHEATMANAGER);       // confirmed live: CheatClass at +3d8, CheatManager at +3e0
    if (plausible(v) && plausible(rd64(v + OBJ_CLASS))) return v;   // instance class is a subclass of FortCheatManager
    return 0;
}
static void dump_pc_words(void) {
    for (uint64_t o = 0x3a0; o < 0x420; o += 8) {
        uint64_t p = rd64(g_pc + o);
        logf_("pc+%lx = %lx (class %lx)", o, p, plausible(p) ? rd64(p + OBJ_CLASS) : 0);
    }
}


// ---- own chest logic ------------------------------------------------------------------------------------
static int g_super_off = 0x40;           // UStruct::SuperStruct, verified/searched in find_super_off()
static int g_dbl = 1;                    // FVector = 3 doubles (UE5) or 3 floats; decided from a live read
static uint64_t g_visited[512]; static int g_nvis = 0;

// cached readable-range read (the scan touches ~700k objects).
// The cache is only trusted inside the game tick that created it (g_fast_gen is bumped once per tick): a range
// that was mapped a second ago proves nothing - the game returns freed heap to the OS all the time, and reading
// through a stale cached range is precisely how a walk over ~300k objects faults (crash of 2026-11-20).
static volatile uint32_t g_fast_gen = 1;
static uint64_t g_rs = 0, g_re = 0; static uint32_t g_rs_gen = 0;
static int fast_ok(uint64_t p, uint64_t n) {
    if (g_rs_gen == g_fast_gen && p >= g_rs && p + n <= g_re) return 1;
    void *st_ = 0, *en = 0; int prot = 0;
    if (sceKernelQueryMemoryProtection((void *)p, &st_, &en, &prot) != 0 || (uint64_t)st_ > p || !(prot & 1)) return 0;
    g_rs = (uint64_t)st_; g_re = (uint64_t)en; g_rs_gen = g_fast_gen;
    return p + n <= g_re;
}
static uint64_t g_esp_world;                        // world identity, set by world_guard (defined further down)
// GObjects item array: 65536 entries of 24 bytes (1.5 MB) per chunk. The game owns that memory and can free it
// while we walk it - or while a resumable ESP walk is paused between two ticks - so each 16 KB page is verified
// as the walk crosses it (one query per page, not one unchecked 1.5 MB range per chunk).
static int items_ok(uint64_t chunk, int i) {
    uint64_t it = chunk + (uint64_t)i * 24;
    uint64_t page = it & ~0xffffULL;
    uint64_t span = 0x10000ULL - (it & 0xffffULL);              // to the end of the page
    uint64_t left = 65536ULL * 24 - (uint64_t)i * 24;           // to the end of the array
    return readable(page, span < left ? span : left);
}
static uint64_t frd64(uint64_t a) { return fast_ok(a, 8) ? *(volatile uint64_t *)a : 0; }
// An actor that is safe to hand to ProcessEvent: valid object, valid class, not pending kill / already destroyed.
static int actor_alive(uint64_t a) {
    if (!plausible(a)) return 0;
    uint64_t cls = frd64(a + OBJ_CLASS);
    if (!plausible(cls)) return 0;
    return !(frd64(a + 8) & 0x18000ULL);
}

static int is_sub_of(uint64_t cls, uint64_t base) {
    for (int i = 0; i < 40 && plausible(cls); i++) {
        if (cls == base) return 1;
        cls = frd64(cls + g_super_off);
    }
    return 0;
}
static void find_super_off(void) {       // locate SuperStruct using the live CheatManager's class chain
    uint64_t cm = find_cm(); if (!cm) return;
    uint64_t cls = rd64(cm + OBJ_CLASS);
    for (int o = 0x30; o <= 0x70; o += 8) {
        uint64_t c = cls; int hit = 0;
        for (int i = 0; i < 6 && plausible(c); i++) { if (c == A_CLS_CHEATMANAGER) { hit = 1; break; } c = rd64(c + o); }
        if (hit) { g_super_off = o; logf_("SuperStruct offset = %x", o); return; }
    }
    logf_("SuperStruct offset not found, keeping %x", g_super_off);
}


#define A_NAMEPOOL 0x15444240ULL
static int fname_str(uint32_t idx, char *out, int max) {      // FNamePool entry -> ascii (no number suffix)
    uint64_t blk = rd64(A_NAMEPOOL + 0x10 + 8ULL * (idx >> 16));
    if (!plausible(blk)) return 0;
    uint64_t e = blk + (uint64_t)(idx & 0xFFFF) * 2;
    if (!readable(e, 2)) return 0;
    uint16_t h = *(volatile uint16_t *)e; int ln = h >> 6;
    if (!ln || ln >= max || (h & 1)) return 0;
    if (!readable(e + 2, (uint64_t)ln)) return 0;
    memcpy(out, (const void *)(e + 2), ln); out[ln] = 0;
    return ln;
}
static int class_name(uint64_t cls, char *out, int max) {
    if (!plausible(cls)) return 0;
    uint64_t v = rd64(cls + 0x18);
    return fname_str((uint32_t)v, out, max);
}
static uint64_t g_fc[96]; static signed char g_fv[96]; static int g_nfc = 0;
static int class_wanted(uint64_t cls) {                        // chests only (no ammo boxes)
    for (int i = 0; i < g_nfc; i++) if (g_fc[i] == cls) return g_fv[i];
    char nm[96]; int ok = 0;
    if (class_name(cls, nm, sizeof nm)) ok = strstr(nm, "Chest") != NULL;
    if (g_nfc < 96) { g_fc[g_nfc] = cls; g_fv[g_nfc++] = ok; }
    return ok;
}
// direct-mapped cache: the object walk visits ~300k objects, and the SuperStruct walk per object was the
// biggest cost of a scan (every 10 s it froze the game thread for several frames).
static uint64_t g_ck[4096]; static signed char g_cv[4096];
static int class_is_container(uint64_t cls) {
    unsigned h = (unsigned)(((cls >> 4) ^ (cls >> 13)) & 4095u);
    if (g_ck[h] == cls) return g_cv[h];
    int ok = is_sub_of(cls, A_CLS_CONTAINER) && class_wanted(cls);
    g_ck[h] = cls; g_cv[h] = (signed char)ok;
    return ok;
}

// ---- trace: log interaction-related ProcessEvent calls while the player opens a chest by hand ------------
static volatile int g_trace = 0; static int g_trace_n = 0;
static uint64_t g_tf[1024]; static signed char g_tv[1024];
static int trace_fn_wanted(uint64_t fn) {
    unsigned h = (unsigned)((fn >> 4) ^ (fn >> 14)) & 1023;
    if (g_tf[h] == fn) return g_tv[h];
    char nm[96]; int ok = 0;
    if (plausible(fn) && fname_str((uint32_t)rd64(fn + 0x18), nm, sizeof nm))
        ok = (strstr(nm, "Interact") || strstr(nm, "Search") || strstr(nm, "Container") || strstr(nm, "OpenChest") || strstr(nm, "Loot")) && !strstr(nm, "GetMinDistance") && !strstr(nm, "GetMin");
    g_tf[h] = fn; g_tv[h] = ok;
    return ok;
}
static uint8_t g_tc[1024];
static void trace_call(uint64_t self, uint64_t fn, uint8_t *params) {
    if (g_trace_n >= 300 || !trace_fn_wanted(fn)) return;
    unsigned h = (unsigned)((fn >> 4) ^ (fn >> 14)) & 1023;
    if (g_tc[h] >= 6) return; g_tc[h]++;
    g_trace_n++;
    char fnm[96] = "?", cnm[96] = "?";
    fname_str((uint32_t)rd64(fn + 0x18), fnm, sizeof fnm);
    if (plausible(self)) class_name(rd64(self + OBJ_CLASS), cnm, sizeof cnm);
    uint64_t q[4] = {0,0,0,0}; if (readable((uint64_t)params, 32)) memcpy(q, params, 32);
    logf_("TRACE %s on %s(%lx): %lx %lx %lx %lx", fnm, cnm, self, q[0], q[1], q[2], q[3]);
}
// learned ServerAttemptInteract call (captured when the player opens a real chest)
static uint64_t g_tpl[8]; static int g_have_tpl = 1, g_comp_off = 0x1b0, g_learn_n = 0;
static void learn_interact(uint64_t self, uint8_t *params) {
    if (g_learn_n >= 12) return; g_learn_n++;
    uint64_t q[8]; for (int i = 0; i < 8; i++) q[i] = *(uint64_t *)(params + i * 8);
    char nm[96] = "?"; if (plausible(q[0])) class_name(rd64(q[0] + OBJ_CLASS), nm, sizeof nm);
    logf_("LEARN interact self %lx actor %lx (%s) p: %lx %lx %lx %lx %lx %lx", self, q[0], nm, q[1], q[2], q[3], q[4], q[5], q[6]);
    if (!plausible(q[0]) || !class_wanted(rd64(q[0] + OBJ_CLASS))) return;     // only chests teach us
    memcpy(g_tpl, q, sizeof q);
    if (plausible(q[1]) && readable(q[0], 0x1000))
        for (int o = 0; o < 0x1000; o += 8) if (*(volatile uint64_t *)(q[0] + o) == q[1]) { g_comp_off = o; logf_("LEARN component is at actor+%x", o); break; }
    if (g_comp_off < 0) logf_("LEARN component pointer not found inside the actor (it is probably a subobject; comp class %lx)", plausible(q[1]) ? rd64(q[1] + OBJ_CLASS) : 0);
}

typedef struct { uint64_t obj; double x, y, z; uint64_t cls; uint8_t open; uint64_t open_next; } Cont;
static Cont g_cont[MAX_CONT]; static int g_ncont = 0;
static uint64_t g_interact = 0;          // interaction component of our controller

static void call_pe2(uint64_t self, uint64_t fn, void *params) { ((pe_t)g_stub)((void *)self, (void *)fn, params); }

static int get_vec(uint64_t actor, uint64_t fn, double *o) {
    uint8_t p[96] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    call_pe2(actor, fn, p);
    double *d = (double *)p; float *f = (float *)p;
    int dbl_ok = 1;
    for (int i = 0; i < 3; i++) if (!(d[i] == d[i]) || d[i] > 1e8 || d[i] < -1e8 || (d[i] != 0 && d[i] < 1e-6 && d[i] > -1e-6)) dbl_ok = 0;
    g_dbl = dbl_ok;
    if (dbl_ok) { o[0] = d[0]; o[1] = d[1]; o[2] = d[2]; } else { o[0] = f[0]; o[1] = f[1]; o[2] = f[2]; }
    return 1;
}
// Read an actor's position from AActor.RootComponent->RelativeLocation instead of calling
// K2_GetActorLocation through ProcessEvent for every container (that was thousands of events per scan).
// Calibrated against the engine on the first use; falls back to ProcessEvent if the layout does not match.
static int g_pos_fast = -1;
static int frdvec3(uint64_t a, double *o) {
    if (!fast_ok(a, 24)) return 0;
    double x = *(volatile double *)(a), y = *(volatile double *)(a + 8), z = *(volatile double *)(a + 16);
    if (!(x == x && y == y && z == z) || x < -1e8 || x > 1e8 || y < -1e8 || y > 1e8 || z < -1e8 || z > 1e8) return 0;
    o[0] = x; o[1] = y; o[2] = z;
    return 1;
}
// memory path only: used by the 30 Hz refresh, so a failing actor can never trigger a ProcessEvent per tick
static int actor_pos_mem(uint64_t actor, double *o) {
    if (g_pos_fast == 0) return 0;
    uint64_t root = frd64(actor + ACTOR_ROOT);
    double v[3];
    if (plausible(root) && frdvec3(root + SCENE_REL_LOC, v)) {
        o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
        return 1;
    }
    return 0;
}
static int actor_pos(uint64_t actor, double *o) {
    if (g_pos_fast != 0) {
        uint64_t root = frd64(actor + ACTOR_ROOT);
        double v[3];
        if (plausible(root) && frdvec3(root + SCENE_REL_LOC, v)) {
            if (g_pos_fast < 0) {                              // one-time calibration
                double p[3]; get_vec(actor, A_FN_GETLOC, p);
                double dx = p[0]-v[0], dy = p[1]-v[1], dz = p[2]-v[2], d = dx*dx+dy*dy+dz*dz;
                g_pos_fast = (d < 4.0);
                logf_("pos: root->RelativeLocation %s K2_GetActorLocation (root %lx, d=%.2f)",
                      g_pos_fast ? "matches" : "does NOT match", root, __builtin_sqrt(d));
                if (g_pos_fast) { o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; return 1; }
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; return 1;
            }
            o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
            return 1;
        }
    }
    get_vec(actor, A_FN_GETLOC, o);
    return 1;
}
static int looks_like_pawn(uint64_t p) {
    if (!plausible(p)) return 0;
    uint64_t cls = rd64(p + OBJ_CLASS);
    if (!plausible(cls)) return 0;
    char nm[96];
    if (!class_name(cls, nm, sizeof nm)) return 0;
    if (strstr(nm, "Controller") || strstr(nm, "Component") || strstr(nm, "State")) return 0;
    return strstr(nm, "Pawn") || strstr(nm, "Character") || strstr(nm, "Player") || strstr(nm, "Vehicle");
}
// pawn from the controller fields only (no resolve_pc / no engine walk): safe to call every tick
static uint64_t pawn_fast(void) {
    if (!g_pc) return 0;
    uint64_t pw = frd64(g_pc + 0x2a8);
    if (!looks_like_pawn(pw)) pw = frd64(g_pc + 0x310);
    return looks_like_pawn(pw) ? pw : 0;
}
static uint64_t get_pawn(void) {
    // AController::Pawn (0x2a8) / APlayerController::AcknowledgedPawn (0x310). Plain field reads only:
    // a nested ProcessEvent(K2_GetPawn) on a controller that the map travel just destroyed crashed the title.
    uint64_t pw = g_pc ? rd64(g_pc + 0x2a8) : 0;
    if (!looks_like_pawn(pw)) pw = g_pc ? rd64(g_pc + 0x310) : 0;
    if (!looks_like_pawn(pw)) {
        if (resolve_pc() && g_pc) {                      // controller from a previous map?
            pw = rd64(g_pc + 0x2a8);
            if (!looks_like_pawn(pw)) pw = rd64(g_pc + 0x310);
        }
    }
    if (looks_like_pawn(pw)) {
        static int src = 0;
        if (!src) { src = 1; char nm[64] = "?"; class_name(rd64(pw + OBJ_CLASS), nm, sizeof nm); logf_("pawn: %lx (%s) read from pc fields", pw, nm); }
        return pw;
    }
    static uint64_t last_note = 0;                        // in the lobby there is no pawn: say why, slowly
    if (g_pc && now_ms() - last_note > 5000) {
        last_note = now_ms();
        char n1[64] = "?", n2[64] = "?";
        uint64_t p1 = rd64(g_pc + 0x2a8), p2 = rd64(g_pc + 0x310);
        if (plausible(p1)) class_name(rd64(p1 + OBJ_CLASS), n1, sizeof n1);
        if (plausible(p2)) class_name(rd64(p2 + OBJ_CLASS), n2, sizeof n2);
        logf_("pawn: none (pc %lx +2a8 %lx %s, +310 %lx %s)", g_pc, p1, n1, p2, n2);
    }
    return 0;
}

// walk GObjects: collect BuildingContainer instances, find our interaction component
static void scan_objects(void) {
    g_ncont = 0; g_interact = 0;
    g_fast_gen++;                                       // this walk gets its own readable-range cache generation
    uint64_t chunks = rd64(A_GOBJECTS);
    if (!plausible(chunks)) { logf_("GObjects chunk table not readable"); return; }
    // snapshot the table first: the game can grow/reallocate it while we are in the middle of the walk
    uint64_t ctab[64];
    for (int ci = 0; ci < 64; ci++) ctab[ci] = rd64(chunks + (uint64_t)ci * 8);
    uint64_t world = g_esp_world;
    for (int ci = 0; ci < 64; ci++) {
        uint64_t chunk = ctab[ci];
        if (!plausible(chunk)) break;
        uint64_t cur_page = 0;
        for (int i = 0; i < 65536; i++) {
            const uint8_t *it = (const uint8_t *)(chunk + (uint64_t)i * 24);
            uint64_t pg = (uint64_t)it & ~0xffffULL;
            if (pg != cur_page) { if (!items_ok(chunk, i)) break; cur_page = pg; }   // freed under us: stop this chunk
            uint64_t q0 = *(const uint64_t *)it;
            if (!q0 || (q0 & 0x1020000000000000ULL)) continue;
            uint64_t obj = ((q0 & 0x3fff00000000ULL) | *(const uint32_t *)(it + 8)) << 3;
            if (!plausible(obj)) continue;
            uint64_t cls = frd64(obj + OBJ_CLASS);
            if (!plausible(cls)) continue;
            if (cls == A_CLS_INTERACTION) { if (frd64(obj + OBJ_OUTER) == g_pc) g_interact = obj; continue; }
            if (g_ncont < MAX_CONT && class_is_container(cls)) { g_cont[g_ncont].obj = obj; g_cont[g_ncont].cls = cls; g_cont[g_ncont].open = 0; g_cont[g_ncont].open_next = 0; g_ncont++; }
        }
        if (world && world != g_esp_world) { g_ncont = 0; g_interact = 0; return; }   // world replaced mid-walk
    }
}
static void fill_positions(void) {
    int n = 0;
    for (int i = 0; i < g_ncont; i++) {
        if (frd64(g_cont[i].obj + OBJ_CLASS) != g_cont[i].cls) continue;    // freed / retyped object
        double v[3];
        actor_pos(g_cont[i].obj, v);
        if (!in_world(v[0], v[1], v[2])) continue;                          // garbage / off-map read
        g_cont[n] = g_cont[i]; g_cont[n].x = v[0]; g_cont[n].y = v[1]; g_cont[n].z = v[2]; n++;
    }
    g_ncont = n;
}
static int was_visited(uint64_t o) { for (int i = 0; i < g_nvis; i++) if (g_visited[i] == o) return 1; return 0; }
static int nearest(double *me, int skip_visited) {
    int best = -1; double bd = 1e300;
    for (int i = 0; i < g_ncont; i++) {
        if (skip_visited && was_visited(g_cont[i].obj)) continue;
        double dx = g_cont[i].x - me[0], dy = g_cont[i].y - me[1], dz = g_cont[i].z - me[2], d = dx*dx + dy*dy + dz*dz;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static uint64_t get_vehicle(uint64_t pawn) {
    if (!actor_alive(pawn)) return 0;
    uint8_t p[32] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    call_pe2(pawn, A_FN_GETVEHICLE, p);
    uint64_t v = *(uint64_t *)p;
    return actor_alive(v) ? v : 0;
}

// ---- cannon gate -----------------------------------------------------------------------------------------
// Fortnite's teleport only works reliably while the local player is seated in a cannon (the plugin moves the
// vehicle the player is riding). Before every teleport we verify that the player really is in a cannon; if
// not, the running sequence pauses and resumes automatically as soon as the player is back on a cannon.
static int g_need_cannon = 1;          // toggled from the menu / the 'cannon' command
static int g_cannon_paused = 0;        // sequence is waiting for a cannon
static uint64_t g_cannon_note = 0;
// graphics/perf preset (see perf_apply): declared here because the menu renders it
static int g_perf = 1, g_perf_on_done = 0; static uint64_t g_perf_pc = 0;

static int is_cannon(uint64_t actor) {
    if (!plausible(actor)) return 0;
    char nm[96];
    if (!class_name(frd64(actor + OBJ_CLASS), nm, sizeof nm)) return 0;
    return strstr(nm, "Cannon") != NULL;     // AFortMountedCannon, AFortAthenaSKPushCannon, BP_Cannon_* ...
}
static int in_cannon(uint64_t pawn) {
    if (!plausible(pawn)) return 0;
    uint64_t veh = get_vehicle(pawn);        // Controller/Pawn.GetVehicleActor()
    if (veh && is_cannon(veh)) return 1;
    return is_cannon(pawn);                  // push cannons keep the character as the outer actor
}
// cached: the hover now calls this every 16 ms, and GetVehicleActor is a ProcessEvent - 5x/second is plenty
static uint64_t g_ck_t = 0, g_ck_pawn = 0; static int g_ck_ok = 0;
static int cannon_ok(uint64_t pawn) {
    if (!g_need_cannon) { g_cannon_paused = 0; return 1; }
    uint64_t now = now_ms();
    int ok;
    if (pawn && pawn == g_ck_pawn && now - g_ck_t < 200) ok = g_ck_ok;
    else { ok = in_cannon(pawn); g_ck_pawn = pawn; g_ck_t = now; g_ck_ok = ok; }
    if (ok) {
        if (g_cannon_paused) {
            g_cannon_paused = 0;
            logf_("cannon: seated again, resuming");
        }
        return 1;
    }
    if (!g_cannon_paused || now_ms() - g_cannon_note > 10000) {
        g_cannon_paused = 1; g_cannon_note = now_ms();
        logf_("cannon: player is not in a cannon -> paused");
    }
    return 0;
}
static int g_tp_logged = 0;
static uint64_t g_tc_pawn = 0, g_tc_tgt = 0, g_tc_t = 0;
// Facing is captured ONCE per run (per ridden actor) and re-sent unchanged with every teleport. It is never
// re-read between legs, so the cannon cannot snap to a new yaw mid-run: the rotation stays exactly the one it
// had when the run started, for the whole flight and every hover.
static double g_tp_rot[3]; static int g_tp_rot_ok = 0;
static uint64_t g_rot_actor = 0;
static int g_rotlog = 0;
static int rot_sane(const double *r) {
    return r[0] == r[0] && r[1] == r[1] && r[2] == r[2] &&
           r[0] > -89.9 && r[0] < 89.9 && r[1] > -1e5 && r[1] < 1e5 && r[2] > -180.1 && r[2] < 180.1;
}
static uint64_t teleport_target(uint64_t pawn) {          // the actor that must move: the cannon when seated
    uint64_t veh = get_vehicle(pawn);
    return (veh && actor_alive(veh) && veh != pawn) ? veh : pawn;
}
static void hold_rotation(uint64_t pawn) {
    if (!actor_alive(pawn)) return;
    uint64_t tgt = teleport_target(pawn);
    if (tgt == pawn && g_tp_rot_ok && g_rot_actor && g_rot_actor != pawn)
        return;                                           // GetVehicleActor failed for one frame: keep the cannon facing
    if (g_tp_rot_ok && tgt == g_rot_actor) return;        // already frozen for this actor: DO NOT re-read it
    double r[3];
    if (!get_vec(tgt, A_FN_GETROT, r) || !rot_sane(r)) return;      // unreadable: keep the last good facing
    g_tp_rot[0] = r[0]; g_tp_rot[1] = r[1]; g_tp_rot[2] = r[2];
    g_tp_rot_ok = 1; g_rot_actor = tgt;
    if (g_rotlog++ < 8) logf_("fly: rotation frozen %.0f %.0f %.0f (%s)", r[0], r[1], r[2], tgt == pawn ? "pawn" : "cannon");
}
static void rotation_forget(void) { g_tp_rot_ok = 0; g_rot_actor = 0; }
// gravity off while flying: ACharacter.CharacterMovement (0x2f0) -> UCharacterMovementComponent.GravityScale (0x198)
static uint64_t g_grav_comp = 0; static float g_grav_saved = 1.0f; static int g_grav_zeroed = 0;
static void fly_gravity(uint64_t pawn, int zero) {
    if (zero) {
        if (!actor_alive(pawn)) return;
        uint64_t cm = frd64(pawn + 0x2f0);
        if (!plausible(cm) || !readable(cm + 0x198, 4)) return;
        if (g_grav_zeroed && g_grav_comp != cm) {                // the pawn was replaced: give the old one its gravity back
            if (plausible(g_grav_comp) && readable(g_grav_comp + 0x198, 4))
                *(volatile float *)(g_grav_comp + 0x198) = g_grav_saved;
            g_grav_zeroed = 0; g_grav_comp = 0;
        }
        if (!g_grav_zeroed) {
            g_grav_saved = *(volatile float *)(cm + 0x198);
            g_grav_comp = cm; g_grav_zeroed = 1;
            logf_("fly: pawn gravity 0 (comp %lx, was %.2f)", cm, g_grav_saved);
        }
        *(volatile float *)(cm + 0x198) = 0.0f;
    } else if (g_grav_zeroed) {
        if (plausible(g_grav_comp) && readable(g_grav_comp + 0x198, 4))
            *(volatile float *)(g_grav_comp + 0x198) = g_grav_saved;
        logf_("fly: pawn gravity restored (%.2f)", g_grav_saved);
        g_grav_zeroed = 0; g_grav_comp = 0;
    }
}
// The cannon is a vehicle (not a Character), so its fall comes from physics: SetEnableGravity(false) on the
// root primitive is what makes "vehicle flying" possible. Resolved by name once a match is live.
static uint64_t g_fn_setgrav = 0, g_fn_zerovel = 0, g_fn_angvel = 0, g_prim_cls = 0;
static uint64_t g_nograv[2]; static int g_nograv_n = 0;
static void prim_call(uint64_t actor, uint64_t fn, int byte) {
    if (!fn || !actor_alive(actor)) return;
    uint64_t root = frd64(actor + ACTOR_ROOT);
    if (!plausible(root) || !plausible(frd64(root + OBJ_CLASS))) return;
    uint8_t p[48] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    p[0] = (uint8_t)byte;
    call_pe2(root, fn, p);
}
static void fly_nograv(uint64_t actor, int on) {
    if (!actor_alive(actor)) return;
    for (int i = 0; i < g_nograv_n; i++) if (g_nograv[i] == actor) {
        if (!on) { prim_call(actor, g_fn_setgrav, 1); g_nograv[i] = g_nograv[--g_nograv_n]; logf_("fly: gravity back on %lx", actor); }
        return;
    }
    if (on && g_nograv_n < 2) {
        prim_call(actor, g_fn_setgrav, 0);
        prim_call(actor, g_fn_zerovel, 0);                       // zeroed params = velocity 0, no add
        prim_call(actor, g_fn_angvel, 0);                        // and no residual spin: the frozen facing holds
        g_nograv[g_nograv_n++] = actor;
        logf_("fly: vehicle gravity off + velocity 0 (%lx)", actor);
    }
}
static void fly_nograv_all(int on) {
    while (g_nograv_n) {
        uint64_t a = g_nograv[--g_nograv_n];
        prim_call(a, g_fn_setgrav, on);
    }
}
// noclip: SetActorEnableCollision(false) on the pawn and the cannon while the run is flying, so walls,
// terrain and props cannot stop or push the flight. Restored at the end / on stop.
static uint64_t g_fn_noclip = 0;
static uint64_t g_noclip[4]; static int g_noclip_n = 0;
static void actor_bool(uint64_t a, uint64_t fn, int v) {
    if (!fn || !actor_alive(a)) return;
    uint8_t p[16] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    p[0] = (uint8_t)v;
    call_pe2(a, fn, p);
}
static void fly_noclip(uint64_t a, int on) {
    if (!a || !g_fn_noclip || !actor_alive(a)) return;
    for (int i = 0; i < g_noclip_n; i++) if (g_noclip[i] == a) {
        if (!on) { actor_bool(a, g_fn_noclip, 1); g_noclip[i] = g_noclip[--g_noclip_n]; logf_("fly: collision back on %lx", a); }
        return;
    }
    if (on && g_noclip_n < 4) {
        actor_bool(a, g_fn_noclip, 0);
        g_noclip[g_noclip_n++] = a;
        logf_("fly: noclip %lx", a);
    }
}
static void fly_noclip_all(void) {
    while (g_noclip_n) {
        uint64_t a = g_noclip[--g_noclip_n];
        actor_bool(a, g_fn_noclip, 1);
    }
}
// after a map load the old pawn and cannon are gone: forget them instead of writing into freed memory
static void fly_forget(void) {
    if (g_grav_zeroed) logf_("fly: new match, forgot the old pawn gravity");
    g_grav_zeroed = 0; g_grav_comp = 0;
    g_nograv_n = 0; g_noclip_n = 0;
}
static int g_tmode;                                       // 0 = chest tour, 1 = plain goto (defined with the tour)
static int teleport_pawn(uint64_t pawn, double x, double y, double z) {
    if (!in_world(x, y, z)) {                                       // never jump to a garbage/off-map point
        static int n = 0;
        if (n++ < 6) logf_("teleport: refused off-map target %.0f %.0f %.0f", x, y, z);
        return 0;
    }
    uint64_t t = now_ms();
    if (pawn != g_tc_pawn || t - g_tc_t > 250 || !actor_alive(g_tc_tgt)) {
        g_tc_pawn = pawn;                                           // re-resolve the ridden actor 4x/second, never per step
        uint64_t tgt = teleport_target(pawn);
        if (tgt != g_tc_tgt) {                                      // switched cannon: follow the NEW one
            g_tc_tgt = tgt;
            if (tgt != pawn && tgt != g_rot_actor) {                // only a new VEHICLE re-captures the facing;
                g_tp_rot_ok = 0; hold_rotation(pawn);               // a one-frame pawn fallback never does
            }
        }
        g_tc_t = t;
        if (g_tp_logged < 12) {
            g_tp_logged++;
            char tn[96] = "?"; class_name(rd64(g_tc_tgt + OBJ_CLASS), tn, sizeof tn);
            logf_("teleport target: %s %lx %s", g_tc_tgt == pawn ? "pawn" : "VEHICLE", g_tc_tgt, tn);
        }
    }
    if (!actor_alive(g_tc_tgt)) return 0;                           // actor died between resolve and use: skip this tick
    if (g_tc_tgt == pawn && g_need_cannon && g_tmode == 0) return 0;   // never drag the bare pawn out of the cannon
    if (!g_tp_rot_ok) {
        hold_rotation(pawn);                                        // never send a zeroed rotation: that is what spun the view
        if (!g_tp_rot_ok) return 0;                                 // facing not readable yet: wait a tick instead
    }
    uint8_t p[96] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    if (g_dbl) { double *d = (double *)p; d[0] = x; d[1] = y; d[2] = z; d[3] = g_tp_rot[0]; d[4] = g_tp_rot[1]; d[5] = g_tp_rot[2]; }
    else { float *f = (float *)p; f[0] = (float)x; f[1] = (float)y; f[2] = (float)z; f[3] = (float)g_tp_rot[0]; f[4] = (float)g_tp_rot[1]; f[5] = (float)g_tp_rot[2]; }
    call_pe2(g_tc_tgt, A_FN_TELEPORT, p);
    return 1;
}
// One flight step towards (x,y,z) from the virtual position g_flypos: CONSTANT speed, straight line.
// There is no easing and no snap teleport, and the residual velocity of the character is zeroed on every
// step, so nothing (gravity, physics drift, server correction) bends the line.
static double g_flypos[3]; static int g_flypos_ok = 0; static uint64_t g_fly_t = 0;
static void fly_hold(uint64_t pawn) {
    // Only the mounted character's movement velocity is zeroed, and that is a plain memory write (free).
    // The cannon used to get SetPhysicsLinearVelocity(0) 20x/s here: that is a ProcessEvent + physics wake-up
    // on the very actor we are seated in, and every one of them is a chance for the game to break the seat.
    // Position AND rotation are re-sent every flight step / hover tick instead, which pins the cannon anyway.
    if (!plausible(pawn)) return;
    uint64_t cm = frd64(pawn + 0x2f0);                           // UCharacterMovementComponent
    if (!plausible(cm) || !fast_ok(cm + 0xd0, 24)) return;
    *(volatile double *)(cm + 0xd0) = 0.0;                       // UMovementComponent.Velocity (FVector, doubles)
    *(volatile double *)(cm + 0xd8) = 0.0;
    *(volatile double *)(cm + 0xe0) = 0.0;
}
static int fly_step(uint64_t pawn, double x, double y, double z, double near) {
    uint64_t t = now_ms();
    if (!g_flypos_ok) { get_vec(pawn, A_FN_GETLOC, g_flypos); g_flypos_ok = 1; g_fly_t = t; }
    double dt = (t - g_fly_t) / 1000.0; g_fly_t = t;
    if (dt <= 0.0) dt = 0.016;
    if (dt > 0.20) dt = 0.20;
    double dx = x - g_flypos[0], dy = y - g_flypos[1], dz = z - g_flypos[2];
    double d = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
    if (d <= near) return 1;                                     // arrived: stop exactly here, no jump
    double step = FLY_SPEED_UU * dt;                             // same distance every tick = smooth
    if (step < 4.0) step = 4.0;
    if (step > FLY_STEP_UU) step = FLY_STEP_UU;
    if (step > d) step = d;
    double f = step / d;
    double nx = g_flypos[0] + dx*f, ny = g_flypos[1] + dy*f, nz = g_flypos[2] + dz*f;
    if (!teleport_pawn(pawn, nx, ny, nz)) return 0;               // not sent (facing not ready): keep the plan
    g_flypos[0] = nx; g_flypos[1] = ny; g_flypos[2] = nz;
    fly_hold(pawn);
    return 0;
}
static void fly_sync(uint64_t pawn) { get_vec(pawn, A_FN_GETLOC, g_flypos); g_flypos_ok = 1; g_fly_t = now_ms(); }
static void fly_resync(uint64_t pawn) {                          // only if the real pawn drifted far from the plan
    double real[3]; get_vec(pawn, A_FN_GETLOC, real);
    if (!in_world(real[0], real[1], real[2])) return;
    double dx = real[0] - g_flypos[0], dy = real[1] - g_flypos[1], dz = real[2] - g_flypos[2];
    if (dx*dx + dy*dy + dz*dz > 400.0*400.0) { g_flypos[0] = real[0]; g_flypos[1] = real[1]; g_flypos[2] = real[2]; g_fly_t = now_ms(); }
}
static void interact(uint64_t actor) {
    if (!g_interact) { logf_("no interaction component (outer == controller) found"); return; }
    uint64_t comp = rd64(actor + g_comp_off);                             // learned live: component at actor+1b0 for the chest classes
    if (!plausible(comp)) { logf_("interact: no component at %lx+%x, skipped", actor, g_comp_off); return; }
    uint8_t p[64] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    *(uint64_t *)p = actor;                                               // ReceivingActor
    *(uint64_t *)(p + 8) = comp;                                          // InteractComponent
    p[16] = 2;                                                            // ETInteractionType seen on real chest opens
    call_pe2(g_interact, A_FN_INTERACT, p);                               // OptionalObjectData = 0, InteractionBeingAttempted = 0
}
// ---- chest checker: never waste a teleport (or 2.5 s) on a container that is already open ----------------
static uint64_t g_fn_searched = 0;            // ABuildingContainer::IsAlreadySearched (resolved once in a live match)
static int chest_open(uint64_t obj) {
    if (!plausible(obj)) return 1;
    if (frd64(obj + 8) & 0x18000ULL) return 1;                            // pending kill / destroyed
    if (!g_fn_searched) return 0;
    uint8_t p[16] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    call_pe2(obj, g_fn_searched, p);
    return p[0] != 0;
}

// 'chests' and 'tour' both run through the fly engine below (do_tour_start / tour_tick)
static void do_probe(void) {
    find_super_off();
    uint64_t pawn = get_pawn();
    double v[3] = {0,0,0}; if (pawn) get_vec(pawn, A_FN_GETLOC, v);
    logf_("probe: pawn %lx loc %f %f %f (%s), in_cannon %d (check %s)", pawn, v[0], v[1], v[2], g_dbl ? "double" : "float", in_cannon(pawn), g_need_cannon ? "ON" : "OFF");
    scan_objects(); int raw = g_ncont; fill_positions();
    logf_("probe: containers %d (with position %d), interaction comp %lx", raw, g_ncont, g_interact);
    for (int i = 0; i < g_nfc; i++) { char nm[96] = "?"; class_name(g_fc[i], nm, sizeof nm); logf_("  class %lx %s wanted=%d", g_fc[i], nm, g_fv[i]); if (i > 40) break; }
    for (int i = 0; i < g_ncont && i < 6; i++) logf_("  #%d obj %lx class %lx at %.0f %.0f %.0f", i, g_cont[i].obj, g_cont[i].cls, g_cont[i].x, g_cont[i].y, g_cont[i].z);
}
static void do_tour_start(int limit, int ret_home);
static int level_walk_now(int containers, int max_ms);   // v132: synchronous level walk for explicit commands
static void do_tpchest(void) { do_tour_start(1, 0); }                 // one chest, flown to (never a snap teleport)
static void do_chests_start(int limit) { do_tour_start(limit, 1); }   // 'chests' = tour + go back home

static double g_aura_r = AURA_RADIUS;
static int g_aura = 0; static uint64_t g_aura_next = 0, g_aura_scan = 0, g_aura_retry = 0, g_aura_log = 0; static int g_aura_n = 0;
static void aura_tick(void) {
    uint64_t t = now_ms();
    if (t < g_aura_next) return;
    g_aura_next = t + AURA_TICK_MS;
    static int dbg = 0;
    int trace = dbg < 8;
    if (dbg < 1000000) dbg++;
    if (trace) logf_("aura: tick");
    uint64_t pawn = get_pawn(); if (!pawn) { if (trace) logf_("aura: no pawn"); return; }
    if (!g_ncont || t - g_aura_scan > AURA_SCAN_MS) {
        if (t < g_aura_retry) return;              // last walk found nothing usable: do not repeat it every tick
        int raw = 0;
        if (!level_walk_now(1, 50)) scan_objects();
        raw = g_ncont;
        fill_positions();
        g_aura_scan = t;
        // The walk reads ~300k game objects and is the most delicate thing this plugin does. When it found no
        // usable container it used to repeat on EVERY 300 ms tick; that is what ended up touching a page the
        // game had just returned to the OS. Nothing usable -> retry in 5 s, and log at most once per 5 s.
        g_aura_retry = t + (g_ncont ? AURA_SCAN_MS : 5000);
        if (trace) logf_("aura: scan -> %d containers (%d usable)", raw, g_ncont);
        if (t - g_aura_log > 5000) { g_aura_log = t; logf_("aura: %d containers, %d with position", raw, g_ncont); }
    }
    double me[3]; get_vec(pawn, A_FN_GETLOC, me);
    int best = -1; double bd = g_aura_r * g_aura_r;
    for (int i = 0; i < g_ncont; i++) {
        if (was_visited(g_cont[i].obj)) continue;
        double dx = g_cont[i].x - me[0], dy = g_cont[i].y - me[1], dz = g_cont[i].z - me[2], d = dx*dx + dy*dy + dz*dz;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return;
    Cont *c = &g_cont[best];
    if (!plausible(c->obj) || rd64(c->obj + OBJ_CLASS) != c->cls || (rd64(c->obj + 8) & 0x18000)) { if (g_nvis < 512) g_visited[g_nvis++] = c->obj; return; }
    if (trace) logf_("aura: interact %lx at %.0f", c->obj, __builtin_sqrt(bd));
    interact(c->obj);
    if (trace) logf_("aura: interact returned");
    if (g_nvis >= 512) g_nvis = 0;
    g_visited[g_nvis++] = c->obj;
    g_aura_n++;
    if (g_aura_n <= 60) logf_("aura: %lx at %.0f (radius %.0f)", c->obj, __builtin_sqrt(bd), g_aura_r);
}
static int g_tauto; static void tour_tick(void);
static int g_esp = 0;                          // cannon ESP on/off
static int g_esp_plr = 0, g_esp_veh = 0;       // player ESP (name + bot flag) / vehicle ESP (box + tower)
static int g_esp_hp = 1, g_esp_skel = 1, g_esp_chest = 0, g_esp_snap = 0;   // health bars / skeleton / chests / snaplines
static void esp_tick(void);
static void fov_tick(void);
static void esp_draw(uint64_t cv);
static void world_guard(void);
static void match_quick(void);
static void life_gate(void);
static void suspend_all(const char *why);
static int ui_wanted(void);
static int ui_mark(void);
static int install_hud_hook(void);
static uint64_t find_hud(void);
static int g_hud_hooked;
static uint64_t hud_try_ms;
// Everything gameplay-related (ESP, tour, aura, canvas drawing) only runs in a live match with an alive,
// non-spectator local pawn. In the lobby, while dead, or during map travel the features are suspended and
// re-armed automatically when the pawn is back.
static int g_ingame = 0; static uint64_t g_pawn_lost = 0;
#define CTRL_ROT  0x2e0                         // AController::ControlRotation (3 doubles)
#define AIM_PARTS 4
static const char *const AIM_PART_NAME[AIM_PARTS] = { "HEAD", "CHEST", "PELVIS", "NEAREST" };
static int g_aim = 0, g_aim_part = 0, g_aim_fov = 12, g_aim_smooth = 1;   // head, responsive by default
static int g_silent = 0, g_silent_part = 0, g_silent_fov = 6;   // v126: silent aim removed - never set, kept 0
static uint64_t g_aim_write_last = 0;
static uint64_t g_fn_gpvp = 0, g_fn_hitnotify = 0;  // GetPlayerViewPoint / OnDisplayHitNotify UFunctions
static uint64_t g_gpvp_hits = 0, g_hitnotify_hits = 0;
static int g_fov_chg = 0, g_fov_orig = 0, g_fov_done = 0, g_fov_logged = 0;   // camera FOV changer
static volatile uint64_t g_pad_r2_until = 0;    // keep silent aim alive ~120 ms after the trigger
static uint64_t g_viewpt_calls = 0, g_viewpt_hits = 0;
static uint64_t g_aim_lock_ps = 0, g_aim_lock_until = 0;
static int g_aim_was = 0;
static int g_hold_on = 0;                       // last aim rotation must keep winning over the game's input
static double g_hold_rot[2];
static double g_silent_prev[3], g_silent_ret_from[3];
static int g_silent_sw = 0, g_silent_returning = 0;
static uint64_t g_silent_ret_start = 0;
static int g_hp_bar_logged = 0;
static int g_aim_locks = 0;                      // v125: aim-lock acquisition log budget (per match)
static uint64_t g_fn_dmg = 0;                    // AFortPlayerController::ClientAddDamageNumber UFunction
static uint64_t g_fn_los = 0;                    // AController::LineOfSightTo UFunction (aimbot visibility)
static int g_los_tries = 0, g_los_log = 0;
static int g_dmg_tries = 0, g_dmg_hits = 0;
static int g_skel_logged = 0;                    // skeleton draw log is once per match, never per frame
typedef void (*hn_t)(void *self, float dmg, uint8_t crit, uint8_t fatal, uint8_t shield, uint64_t actor, void *tags, double lx, double ly, double lz);
static hn_t g_hn_orig = 0;
static Detour g_det_hn;
typedef void (*viewpt_t)(void *self, double *loc, double *rot);
static viewpt_t g_viewpt_orig = 0;
// v97: the real silent-aim hook. AFortPlayerController::GetPlayerViewPoint is the virtual the shot
// trace asks for its view; it sits in vtable slot 0x7e8 (verified in the CUSA07022 memory dump:
// APlayerController 0x298078e, FortPlayerController 0x9374f20). The rendered camera does NOT go
// through it - ULocalPlayer::GetViewPoint copies APLayerCameraManager's CameraCachePrivate directly -
// so rewriting only the out rotation leaves the screen exactly where the player is looking.
typedef void (*gvp_nat_t)(void *self, double *loc, double *rot);
static gvp_nat_t g_gvp_nat_orig = 0;
static uint64_t g_gvp_nat_fn = 0, g_gvp_nat_calls = 0;
static int g_view_ok = 0, g_view_bad = 0;       // slot verified once per process
static int g_view_ro = 0;                       // call sites whose rotation is not writable
static void gvp_nat_hook(void *self, double *loc, double *rot);
static void install_gvp_native(void);
static Detour g_det_vp;
static double g_silent_rot[2];                  // pitch/yaw applied only inside ProcessEvent
static double g_silent_pt[3];                   // world position of the silent-aim target
static uint64_t g_fn_firerot = 0, g_fn_projvals = 0, g_fn_projvals_tgt = 0;
static uint64_t g_fn_pawn_tgt = 0, g_fn_pawn_defrot = 0, g_fn_cam_tgt = 0;
static volatile uint32_t g_shot_hits[6];
static int g_fire_stage = 0;                    // 0..3: resolve the shot-path UFunctions one walk per tick
static uint64_t g_fire_try = 0;
static int g_silent_ok = 0, g_silent_applied = 0;
static uint64_t g_pcm = 0;                      // APlayerCameraManager (cached by cam_cache_read)
static int g_pov_off = -1;                      // winning CameraCachePrivate offset inside +0x1550
static uint64_t g_fn_suc = 0;                   // APlayerController::ServerUpdateCamera
static int g_suc_tries = 0;
static volatile int g_pad_l2 = 0, g_pad_r2 = 0; // live trigger state, sampled by pad_tick
static int g_aim_logged = 0, g_silent_logged = 0;
static int g_silent_bad = 0, g_suc_seen = 0;

// virtual button press (the game's own input path): used to re-enter the cannon after a game-side ejection
static void pad_inject(uint32_t btn, int ms);
static void pad_inject_cancel(void);
static uint64_t g_reseat_next = 0; static int g_reseat_n = 0;
static void reseat_tick(void) {
    if (g_reseat_n >= 8) return;
    uint64_t t = now_ms();
    if (t < g_reseat_next) return;
    g_reseat_next = t + 1800;
    g_reseat_n++;
    pad_inject(ORBIS_PAD_BUTTON_SQUARE, 180);
    logf_("tour: re-seat attempt %d (Square)", g_reseat_n);
}
static void tour_stop_gravity(uint64_t pawn);
static void water_guard_tick(void);
static void seatlock_tick(void);
// Everything that touches the cannon (gravity, collision, velocity, position/rotation) belongs to an ACTIVE
// run only. When no run is flying - stopped, finished, never started - the cannon is a plain vehicle again
// and the plugin writes nothing to it, so driving it by hand behaves exactly like vanilla.
// ---- cannon ghost: hide the cannon we are riding ---------------------------------------------------------
// SetActorHiddenInGame on the VEHICLE actor only (never on the pawn: push cannons keep the character as
// the outer actor, and hiding that would hide the player). No collision change is made, so the cannon
// keeps behaving exactly as before - it just no longer renders in front of the camera while riding/flying.
static uint64_t g_fn_hide = 0;                                 // AActor::SetActorHiddenInGame
static uint64_t g_ghost = 0;                                   // cannon currently hidden by us
static uint64_t g_ghost_t = 0;
static int g_ghost_on = 1;                                     // v126: ON by default - the ridden cannon is hidden

static void cannon_ghost_restore(void) {
    if (!g_ghost) return;
    if (actor_alive(g_ghost) && g_fn_hide) actor_bool(g_ghost, g_fn_hide, 0);
    logf_("cannon: visible again %lx", g_ghost);
    g_ghost = 0;
}
static void cannon_ghost_tick(void) {
    if (!g_ghost_on) { if (g_ghost) cannon_ghost_restore(); return; }   // v126: toggle with "ghost"
    uint64_t t = now_ms();
    if (t - g_ghost_t < 300) return;
    g_ghost_t = t;
    if (!g_ingame) { cannon_ghost_restore(); return; }
    uint64_t pawn = pawn_fast();
    uint64_t veh = pawn ? get_vehicle(pawn) : 0;
    if (!veh || veh == pawn || !is_cannon(veh)) { cannon_ghost_restore(); return; }
    if (veh == g_ghost) return;
    cannon_ghost_restore();
    if (!g_fn_hide) return;
    actor_bool(veh, g_fn_hide, 1);
    g_ghost = veh;
    logf_("cannon: hidden in game %lx", veh);
}

// ---- anti cannon kick: put the local player straight back in the seat --------------------------------
// The title ejects the rider on its own in a few situations that this plugin provokes (seat validation after
// a teleport, the physics wake-up of a hidden/no-clip vehicle, damage while flying). Fighting the ejection
// function would only desync the client from the server, so the seat is reclaimed through the game's own
// input path instead: while the cannon is still next to the player, Square is injected until the game seats
// him again. It only runs while a cannon was occupied a moment ago, gives up after 8 s or when the player
// walks away, and stands down immediately when the player presses Square himself (he wants to get out).
static int g_kickguard = 1;                       // menu / 'kickguard' command
static uint64_t g_kg_veh = 0;                     // cannon we are watching
static uint64_t g_kg_seen = 0;                    // last time the player was actually seated in it
static uint64_t g_kg_next = 0;                    // next Square injection
static uint64_t g_kg_t = 0;                       // tick throttle
static uint64_t g_kg_pawn = 0;
static volatile uint64_t g_pad_sq_ms = 0;         // last real Square press, sampled by pad_tick
static int g_kg_tries = 0, g_kg_logged = 0, g_kg_out = 0;

static void kick_guard_forget(void) {
    g_kg_veh = 0; g_kg_seen = 0; g_kg_tries = 0; g_kg_logged = 0;
}
static void kick_guard_tick(void) {
    if (!g_kickguard) { if (g_kg_veh) kick_guard_forget(); return; }
    uint64_t t = now_ms();
    if (t - g_kg_t < 200) return;                 // 5 Hz is enough: seat loss is reported within a frame or two
    g_kg_t = t;
    if (!g_ingame) { kick_guard_forget(); return; }
    uint64_t pawn = pawn_fast();
    if (!pawn || !actor_alive(pawn)) { kick_guard_forget(); return; }
    if (pawn != g_kg_pawn) { kick_guard_forget(); g_kg_pawn = pawn; }
    uint64_t veh = get_vehicle(pawn);             // Controller/Pawn.GetVehicleActor()
    if (!veh && is_cannon(pawn)) veh = pawn;      // push cannons keep the character as the outer actor
    if (veh && is_cannon(veh)) {                  // seated: remember which cannon to reclaim
        if (g_kg_veh != veh) {
            kick_guard_forget();
            logf_("kick: guarding cannon %lx", veh);
        }
        g_kg_veh = veh; g_kg_seen = t; g_kg_tries = 0;
        if (g_kg_out) { g_kg_out = 0; logf_("kick: seated again in cannon %lx", veh); }
        return;
    }
    if (!g_kg_veh) return;
    if (!actor_alive(g_kg_veh) || !is_cannon(g_kg_veh)) { kick_guard_forget(); return; }
    if (t - g_kg_seen > 8000) {                   // the player really left (or died): stop pushing Square
        logf_("kick: out of the cannon for 8 s - standing down");
        kick_guard_forget(); return;
    }
    if (g_pad_sq_ms && t - g_pad_sq_ms < 1500) {  // the player pressed Square himself: let him out
        logf_("kick: Square by the player - standing down");
        kick_guard_forget(); return;
    }
    double a2[3], b2[3];
    if (actor_pos(pawn, a2) && actor_pos(g_kg_veh, b2)) {
        double dx = a2[0] - b2[0], dy = a2[1] - b2[1], dz = a2[2] - b2[2];
        if (dx * dx + dy * dy + dz * dz > 1200.0 * 1200.0) { kick_guard_forget(); return; }   // walked away
    }
    if (t < g_kg_next) return;
    if (g_kg_tries >= 40) {
        logf_("kick: gave up after %d attempts", g_kg_tries);
        kick_guard_forget(); return;
    }
    g_kg_next = t + 250;
    g_kg_tries++;
    g_kg_out = 1;
    if (!g_kg_logged) { g_kg_logged = 1; logf_("kick: ejected - re-seating via Square (cannon %lx)", g_kg_veh); }
    pad_inject(ORBIS_PAD_BUTTON_SQUARE, 150);
}
static void flight_guard(void) {
    if (g_tauto) return;
    if (!g_grav_zeroed && !g_nograv_n && !g_noclip_n) return;
    logf_("fly: no run active - gravity/collision restored (pawn %lx, veh %d, noclip %d)", get_pawn(), g_nograv_n, g_noclip_n);
    tour_stop_gravity(0);
}
static void auto_tick(void) {
    ph(PH_TICK);
    life_gate();                                        // death / lobby: suspend before anything else
    match_quick();
    ph(PH_WORLD);
    world_guard();
    ph(PH_TICK);
    // The HUD hook is removed while nothing needs drawing (see hud_render_hook); put it back the moment a
    // feature asks for it again. Only this branch costs a clock read, and only while it is unhooked.
    if (!g_hud_hooked && ui_wanted()) {
        uint64_t n = now_ms();
        if (n - hud_try_ms > 250) {
            hud_try_ms = n;
            if (resolve_pc()) { find_hud(); install_hud_hook(); }
        }
    }
    if (!g_ingame) { ph(PH_IDLE); return; }             // lobby / dead / travelling: no ESP, no aim, no tour
    cannon_ghost_tick();
    kick_guard_tick();
    water_guard_tick();
    seatlock_tick();
    flight_guard();
    if (g_fov_chg || (g_fov_orig && !g_fov_done)) fov_tick();   // also run once to restore on OFF
    if (g_esp || g_esp_plr || g_esp_veh || g_esp_chest || g_aim) { ph(PH_ESP); esp_tick(); }
    if (g_tauto) { ph(PH_TOUR); tour_tick(); ph(PH_IDLE); return; }
    if (g_aura) { ph(PH_ESP); aura_tick(); }
    ph(PH_IDLE);
}


// ---- tour: fly to every chest, open it, and keep flying ------------------------------------------------
// There is no snap teleport anywhere in the tour: every move is a flight step (see fly_step), the pawn's
// gravity and the vehicle's physics gravity are switched off for the WHOLE run, and both are restored once
// the run ends. The cannon is never "parked": after a chest is opened the run keeps holding the flight
// (gravity off, collisions off) for one short dwell and then goes straight on to the next chest, so being
// on the cannon never turns the flight off. Every chest is checked with ABuildingContainer::IsAlreadySearched
// before we spend the trip.
static double g_tour[512][3]; static uint64_t g_tobj[512], g_tcls[512];
static uint64_t g_tstep = TOUR_STEP_MS;
static int g_tn = 0, g_ti = 0, g_tphase = 0, g_tauto = 0, g_topen = 0, g_tskip = 0, g_tret = 0;
static int g_tmode = 0;                     // 0 = chest tour, 1 = plain goto (cannon), 2 = player tower
static uint64_t g_tnext = 0, g_tchest = 0;
static int g_instant = 0;                   // 1 = instant teleport mode: no flight, one physics teleport per chest
static int g_tp_tries = 0;                  // instant mode: teleport attempts for the current chest (max 8)
static double g_thome[3]; static int g_have_home = 0; static int g_flyn = 0; static uint64_t g_tpawn = 0;
static double g_last_good[3]; static int g_have_good = 0;
// hover: after arriving at a chest the pawn keeps "flying" in place (gravity still off, velocity pinned to 0)
// until the next chest starts streaming in, so it never sinks, slides or falls off the chest between stops.
static double g_hover[3]; static int g_hover_ok = 0; static uint64_t g_hover_next = 0;
// ---- tower: the same engine, but the targets are players ------------------------------------------------
// Every leg parks the cannon g_tower_h units straight above one player, nearest first, and the dwell keeps
// tracking that player so the cannon stays above them while they move. FLY / INSTANT and the stop time are
// the chest tour's own settings, so both modes behave exactly like the chest run.
#define TOWER_MAX   64
#define TOWER_H_DEF 900                           // default height above the player (uu)
static int g_tower = 0, g_tower_h = TOWER_H_DEF;  // run armed / height above the player
static uint64_t g_track = 0;                      // player pawn the current dwell is sitting above
static char g_tnm[TOWER_MAX][26];                 // target names, copied when the list is built
static void players_refresh(void);                // the ESP owns the player list and its live positions
static void players_positions(void);
static void tower_build(uint64_t pawn);           // defined with the ESP, where Plr/g_plr live
static void do_tower_start(void);
static void do_tower_stop(void);
static void tour_stop_gravity(uint64_t pawn);
// ---- water guard: keep the cannon out of the water -------------------------------------------------------
// AFortAthenaVehicle throws the rider out (and kills the hull) once it sits in water deep enough - the
// vehicle blueprint gets AFortAthenaVehicle::OnWaterTooDeep() for that. Letting that happen and only
// re-seating afterwards is pointless when the cannon is already gone, so the hull is kept dry instead.
// Two SDK-true offsets and two native helpers make the state readable without guessing:
//   WaterLevel        @0x0e94 (float)  world Z of the water surface the hull overlaps
//   WaterOverlapCount @0x0e98 (int32)  0 = hull not in water
//   IsInWater() / GetWaterLevel()      resolved by name on the cannon class (fallback when the fields are 0)
// While the local player rides a cannon that reports water, the hull - and the rider attached to it - is
// teleported just above the surface; the flight/hover height is raised in the same tick so nothing drags
// it back in. Nothing is written while the cannon is dry, and the whole guard can be switched off.
static int g_waterguard = 1;
static uint64_t g_wg_t = 0, g_wg_log = 0, g_wg_veh = 0, g_wg_rt = 0;
static uint64_t g_fn_isinwater = 0, g_fn_waterlvl = 0, g_fn_waterdeep = 0;
static int g_wg_res = 0, g_wg_logx = 0, g_wg_lift = 0;

static int veh_water_state(uint64_t veh, double *surf) {
    if (fast_ok(veh + VEH_WATER_LEVEL, 8)) {                 // cheap path: the vehicle's own bookkeeping
        uint64_t w = frd64(veh + VEH_WATER_LEVEL);
        float wl; memcpy(&wl, &w, 4);
        int32_t cnt = (int32_t)(uint32_t)(w >> 32);
        if (wl == wl && wl > -60000.f && wl < 60000.f && cnt > 0) { *surf = (double)wl; return 1; }
    }
    if (g_fn_isinwater) {                                    // fallback: ask the vehicle
        uint8_t p[32] __attribute__((aligned(16))); memset(p, 0, sizeof p);
        call_pe2(veh, g_fn_isinwater, p);
        if (!p[0]) return 0;
        if (g_fn_waterlvl) {
            uint8_t q[32] __attribute__((aligned(16))); memset(q, 0, sizeof q);
            call_pe2(veh, g_fn_waterlvl, q);
            float f; memcpy(&f, q, 4);
            if (f == f && f > -60000.f && f < 60000.f) { *surf = (double)f; return 1; }
        }
        *surf = 0.0; return 1;
    }
    return 0;
}
static void water_guard_tick(void) {
    if (!g_waterguard) { g_wg_veh = 0; g_wg_lift = 0; return; }
    uint64_t t = now_ms();
    if (t - g_wg_t < 200) return;                             // 5 Hz: the hull state only changes while moving
    g_wg_t = t;
    if (!g_ingame) { g_wg_veh = 0; g_wg_lift = 0; return; }
    uint64_t pawn = pawn_fast();
    if (!pawn) return;
    uint64_t veh = get_vehicle(pawn);                         // Controller/Pawn.GetVehicleActor()
    if (!veh || veh == pawn || !is_cannon(veh) || !actor_alive(veh)) { g_wg_veh = 0; g_wg_lift = 0; return; }
    g_wg_veh = veh;                                           // pe_hook swallows OnWaterTooDeep for this actor
    if (!g_wg_res && t - g_wg_rt > 1500) {                     // one GObjects walk per 1.5 s until it lands
        g_wg_rt = t;
        const char *nm[3] = { "IsInWater", "GetWaterLevel", "OnWaterTooDeep" };
        uint64_t o[3] = { 0, 0, 0 };
        find_methods_on(frd64(veh + OBJ_CLASS), nm, o, 3);
        g_fn_isinwater = o[0]; g_fn_waterlvl = o[1]; g_fn_waterdeep = o[2];
        g_wg_res = (g_fn_isinwater || g_fn_waterdeep) ? 1 : 0;
        logf_("water: guard armed (IsInWater %lx, level %lx, tooDeep %lx)", g_fn_isinwater, g_fn_waterlvl, g_fn_waterdeep);
    }
    double surf = 0;
    if (!veh_water_state(veh, &surf)) { g_wg_lift = 0; return; }
    double p[3];
    if (!actor_pos(veh, p)) return;
    double want = surf + 320.0;                               // hull above the surface, rider comes with it
    if (p[2] >= want) return;
    if (!g_wg_lift || t - g_wg_log > 2000) {
        g_wg_log = t; g_wg_lift = 1;
        logf_("water: lifting cannon out of the water (surface %.0f, hull %.0f -> %.0f)", surf, p[2], want);
    }
    if (g_flypos_ok && g_flypos[2] < want) g_flypos[2] = want;
    if (g_hover_ok && g_hover[2] < want) g_hover[2] = want;    // otherwise the next hover tick pulls it back in
    teleport_pawn(pawn, p[0], p[1], want);
}
// ---- seat lock: nothing the game calls may take the local player out of the cannon ------------------------
// The water guard only covers the one water path. This is the blanket version: every UFunction this title
// uses to eject a rider is resolved by name once per cannon and swallowed while the player is seated.
//   vehicle side : EjectAllPlayers, EjectPlayerPawn(InPawn), EjectSeat(Index), OnPawnExitVehicle(Pawn, ...)
//   pawn side    : ServerOnExitVehicle, ServerVehicleEjectForZiplining
//   controller   : ServerAttemptExitVehicle
// Only calls that target the cannon we ride / our own pawn / our own controller are touched; every other
// vehicle in the match keeps its exact engine behaviour. The player keeps control: a real Square press opens
// a 1.5 s window in which the exit calls are let through (the guard's own injected Square never counts,
// because g_pad_sq_ms is only sampled from the untouched pad state).
#define SL_VEH_N   4
#define SL_PAWN_N  2
#define SL_PC_N    1
static int g_seatlock = 1;
static uint64_t g_sl_t = 0, g_sl_veh_act = 0, g_sl_pawn_act = 0, g_sl_pc_act = 0;
static uint64_t g_sl_veh_fn[SL_VEH_N], g_sl_pawn_fn[SL_PAWN_N], g_sl_pc_fn[SL_PC_N];
static int g_sl_stage = 0, g_sl_blocks = 0;
static const char *const SL_VEH_NAMES[SL_VEH_N] = { "EjectAllPlayers", "EjectPlayerPawn", "EjectSeat", "OnPawnExitVehicle" };
static const char *const SL_PAWN_NAMES[SL_PAWN_N] = { "ServerOnExitVehicle", "ServerVehicleEjectForZiplining" };
static const char *const SL_PC_NAMES[SL_PC_N] = { "ServerAttemptExitVehicle" };

static int sl_has(const uint64_t *v, int n, uint64_t fn) {
    for (int i = 0; i < n; i++) if (v[i] && v[i] == fn) return 1;
    return 0;
}
// called for every ProcessEvent: a handful of compares, a clock read only when the function matches
static int seatlock_block(uint64_t self, uint64_t fn) {
    if (!g_seatlock || !self || !fn) return 0;
    int hit = 0;
    if (g_sl_veh_act && self == g_sl_veh_act && sl_has(g_sl_veh_fn, SL_VEH_N, fn)) hit = 1;
    else if (g_sl_pawn_act && self == g_sl_pawn_act && sl_has(g_sl_pawn_fn, SL_PAWN_N, fn)) hit = 1;
    else if (g_sl_pc_act && self == g_sl_pc_act && sl_has(g_sl_pc_fn, SL_PC_N, fn)) hit = 1;
    if (!hit) return 0;
    if (g_pad_sq_ms && now_ms() - g_pad_sq_ms < 1500) return 0;   // he pressed Square himself: let him out
    if (g_sl_blocks < 24) { g_sl_blocks++; logf_("seat: blocked ejection call on %lx (fn %lx)", self, fn); }
    return 1;
}
static void seatlock_tick(void) {
    if (!g_seatlock) { g_sl_veh_act = g_sl_pawn_act = g_sl_pc_act = 0; g_sl_stage = 0; return; }
    uint64_t t = now_ms();
    if (t - g_sl_t < 250) return;
    g_sl_t = t;
    if (!g_ingame) { g_sl_veh_act = g_sl_pawn_act = g_sl_pc_act = 0; g_sl_stage = 0; return; }
    uint64_t pawn = pawn_fast();
    uint64_t veh = pawn ? get_vehicle(pawn) : 0;
    if (!pawn || !veh || veh == pawn || !is_cannon(veh) || !actor_alive(veh)) {
        g_sl_veh_act = g_sl_pawn_act = g_sl_pc_act = 0; g_sl_stage = 0; return;
    }
    g_sl_veh_act = veh; g_sl_pawn_act = pawn; g_sl_pc_act = g_pc;
    if (g_sl_stage >= 3) return;
    static uint64_t next = 0;
    if (t < next) return;                       // one GObjects walk per 700 ms, never a stall
    next = t + 700;
    if (g_sl_stage == 0) {                      // cannon: the four ejection entry points
        uint64_t o[SL_VEH_N] = { 0, 0, 0, 0 };
        find_methods_on(frd64(veh + OBJ_CLASS), SL_VEH_NAMES, o, SL_VEH_N);
        for (int i = 0; i < SL_VEH_N; i++) g_sl_veh_fn[i] = o[i];
        g_sl_stage = 1;
        return;
    }
    if (g_sl_stage == 1) {                      // our pawn: the two exit RPCs
        uint64_t o[SL_PAWN_N] = { 0, 0 };
        find_methods_on(frd64(pawn + OBJ_CLASS), SL_PAWN_NAMES, o, SL_PAWN_N);
        for (int i = 0; i < SL_PAWN_N; i++) g_sl_pawn_fn[i] = o[i];
        g_sl_stage = 2;
        return;
    }
    {                                           // our controller: the "let me out" request
        uint64_t o[SL_PC_N] = { 0 };
        if (g_pc) find_methods_on(frd64(g_pc + OBJ_CLASS), SL_PC_NAMES, o, SL_PC_N);
        g_sl_pc_fn[0] = o[0];
    }
    g_sl_stage = 3;
    logf_("seat: lock armed (veh %lx/%lx/%lx/%lx pawn %lx/%lx pc %lx)", g_sl_veh_fn[0], g_sl_veh_fn[1],
          g_sl_veh_fn[2], g_sl_veh_fn[3], g_sl_pawn_fn[0], g_sl_pawn_fn[1], g_sl_pc_fn[0]);
}
static void tour_hover(uint64_t t) {
    if (!g_hover_ok || !g_tauto) return;
    if (t < g_hover_next) return;
    g_hover_next = t + FLY_STEP_MS;                                  // every tick (~60 Hz), not "only on drift"
    uint64_t pawn = get_pawn();
    if (!pawn || pawn != g_tpawn) return;
    static uint64_t hov_bad_t = 0;
    if (!cannon_ok(pawn)) {                                          // a short GetVehicleActor miss is NOT "left the
        if (!hov_bad_t) hov_bad_t = t;                               // cannon": while a chest streams in it can fail
        if (t - hov_bad_t < 1200) return;                            // for a moment - keep flying straight through
        hov_bad_t = 0;
        g_hover_ok = 0;
        tour_stop_gravity(pawn);                                     // really out: gravity + collisions back
        logf_("tour: hover off (not in a cannon)");
        return;
    }
    hov_bad_t = 0;
    if (g_tmode == 2 && g_track) {                                   // tower: stay above the player, not the spot
        double v[3];
        if (actor_alive(g_track) && actor_pos(g_track, v) && in_world(v[0], v[1], v[2])) {
            g_hover[0] = v[0]; g_hover[1] = v[1]; g_hover[2] = v[2] + (double)g_tower_h;
        } else g_track = 0;                                          // died mid-dwell: hold the last spot
    }
    fly_hold(pawn);                                                  // kill any velocity the engine added
    teleport_pawn(pawn, g_hover[0], g_hover[1], g_hover[2]);          // ALWAYS set position AND rotation while in the
                                                                     // cannon: the game can never drift or unseat us
}

static void tour_recover(uint64_t pawn) {                            // emergency: back to the last known-good spot
    if (g_have_good) { logf_("tour: recovering to last good %.0f %.0f %.0f", g_last_good[0], g_last_good[1], g_last_good[2]); teleport_pawn(pawn, g_last_good[0], g_last_good[1], g_last_good[2]); }
    else if (g_have_home) { logf_("tour: recovering to home"); teleport_pawn(pawn, g_thome[0], g_thome[1], g_thome[2]); }
}
static void tour_stop_gravity(uint64_t pawn) {
    fly_gravity(pawn, 0);                                            // character gravity
    fly_nograv_all(1);                                               // vehicle physics gravity
    fly_noclip_all();                                                // collisions back on
}
static void do_tour_start(int limit, int ret_home) {
    find_super_off();
    fly_gravity(0, 0); fly_nograv_all(1); fly_noclip_all();          // a previous run may have left physics off
    uint64_t pawn = get_pawn(); if (!pawn) { logf_("tour: no pawn"); return; }
    get_vec(pawn, A_FN_GETLOC, g_thome);
    g_have_home = in_world(g_thome[0], g_thome[1], g_thome[2]);
    if (!g_have_home) logf_("tour: home position not readable, the run will not return home");
    g_tret = ret_home && g_have_home;
    g_have_good = 0;
    g_track = 0;
    g_flyn = 0;
    g_flypos_ok = 0;
    g_hover_ok = 0;
    g_tmode = 0;
    rotation_forget(); g_reseat_n = 0; g_reseat_next = 0;
    if (!level_walk_now(1, 80)) { scan_objects(); fill_positions(); }
    uint64_t saved[512]; int ns = g_nvis; memcpy(saved, g_visited, sizeof saved); if (!limit) g_nvis = 0;
    double cur[3] = { g_thome[0], g_thome[1], g_thome[2] }; int n = 0;
    for (int k = 0; k < g_ncont && n < 512 && (!limit || n < limit); k++) {
        int b = nearest(cur, 1); if (b < 0) break;
        g_tour[n][0] = g_cont[b].x; g_tour[n][1] = g_cont[b].y; g_tour[n][2] = g_cont[b].z;
        g_tobj[n] = g_cont[b].obj; g_tcls[n] = g_cont[b].cls; n++;
        if (g_nvis < 512) g_visited[g_nvis++] = g_cont[b].obj;
        cur[0] = g_cont[b].x; cur[1] = g_cont[b].y; cur[2] = g_cont[b].z;
    }
    if (!limit) { memcpy(g_visited, saved, sizeof saved); g_nvis = ns; }
    g_tn = n; g_ti = 0; g_tphase = 0; g_topen = 0; g_tskip = 0; g_tauto = n > 0; g_tnext = now_ms();
    g_tpawn = pawn;
    logf_("tour: %d chests, %s mode (%.0f ms at each chest)%s", n, g_instant ? "INSTANT teleport" : "continuous flight",
          g_tstep / 1.0, ret_home ? ", back home" : "");
    if (!g_have_tpl) logf_("tour: no interact call learned yet - open one chest by hand first");
}
static void tour_tick(void) {
    uint64_t t = now_ms();
    if (t < g_tnext) { tour_hover(t); return; }
    uint64_t pawn = get_pawn();
    if (!pawn) { g_tauto = 0; g_tower = 0; g_tmode = 0; g_track = 0; tour_stop_gravity(0); logf_("tour: pawn lost, stopped"); return; }
    if (pawn != g_tpawn) {                                           // respawned / new pawn: never fly from the old spot
        logf_("tour: pawn changed (%lx -> %lx), resyncing", g_tpawn, pawn);
        g_tpawn = pawn; g_flypos_ok = 0; g_flyn = 0; g_tphase = 0; g_have_good = 0; g_hover_ok = 0;
        fly_gravity(pawn, 1);
        g_tnext = t; return;
    }

    if (g_ti >= g_tn && g_tauto && g_tmode == 2 && g_tower && g_tphase != 4) {   // tower: a pass is done, refresh and go again
        tower_build(pawn);
        if (g_tn) {
            static uint64_t pass_log = 0;
            if (t - pass_log > 5000) { pass_log = t; logf_("tower: pass done, %d players again", g_tn); }
            g_tnext = t; return;
        }
        g_tower = 0;                                                            // nobody left: go home
        if (g_tret && g_have_home) { g_tauto = 1; g_ti = g_tn; g_tphase = 4; g_tnext = t; logf_("tower: no players left, flying home"); return; }
    }
    if (g_ti >= g_tn && g_tauto && g_tret && g_tphase != 4 && g_tmode != 1) {   // last target: fly home instead of jumping
        g_tphase = 4;
        fly_sync(pawn);
        g_tnext = t;
        if (g_tmode == 2) logf_("tower: run done, flying home");
        else logf_("tour: %d chests done, flying home", g_topen);
        return;
    }
    if (g_ti >= g_tn) {
        if (g_tphase == 4 && g_instant) {                            // instant mode: jump home, no flight
            uint64_t hv = get_vehicle(pawn);
            fly_gravity(pawn, 1);
            if (hv) fly_nograv(hv, 1);
            fly_noclip(pawn, 1);
            if (hv) fly_noclip(hv, 1);
            fly_hold(pawn);
            teleport_pawn(pawn, g_thome[0], g_thome[1], g_thome[2] + TP_LIFT);
            g_flypos[0] = g_thome[0]; g_flypos[1] = g_thome[1]; g_flypos[2] = g_thome[2] + TP_LIFT; g_flypos_ok = 1;
            logf_("tour: home (instant)");
        }
        else if (g_tphase == 4) {
            if (!fly_step(pawn, g_thome[0], g_thome[1], g_thome[2], 90.0)) {
                if (++g_flyn % 10 == 0) fly_resync(pawn);
                g_tnext = t + FLY_STEP_MS; return;
            }
            logf_("tour: home");
        }
        g_tphase = 0; g_tauto = 0; g_tower = 0; g_track = 0; g_hover_ok = 0;
        tour_stop_gravity(pawn);                                     // always give gravity back
        if (g_tret && g_have_home) logf_("%s back at the start position (%.0f %.0f %.0f)",
                                         g_tmode == 2 ? "tower:" : "tour:", g_thome[0], g_thome[1], g_thome[2]);
        if (g_tmode == 1) { logf_("gotocannon: arrived (%.0f %.0f %.0f)", g_flypos[0], g_flypos[1], g_flypos[2]);  g_tmode = 0; return; }
        if (g_tmode == 2) logf_("tower: done, %d players visited%s", g_tn, g_tret ? ", back home" : "");
        else logf_("tour: done, %d opened, %d already open, %d points%s", g_topen, g_tskip, g_tn, g_tret ? ", back home" : "");
        g_tmode = 0;
        return;
    }
    if (g_tphase != 4 && g_tmode != 1 && !cannon_ok(pawn)) {       // paused: try to seat the player again ourselves
        reseat_tick();
        if (g_grav_zeroed || g_nograv_n || g_noclip_n) tour_stop_gravity(pawn);   // not flying: cannon back to normal
        g_tnext = t + 800; return;
    }
    g_reseat_n = 0;
    g_track = 0;                                                     // a new leg: stop tracking the last player
    double *p = g_tour[g_ti];
    uint64_t o = g_tobj[g_ti];
    if (g_tmode == 2 && g_tphase == 0) {                             // tower: chase the player, not the stale point
        double v[3];
        if (!o || !actor_alive(o) || !actor_pos(o, v) || !in_world(v[0], v[1], v[2])) { g_ti++; g_tnext = t; return; }
        p[0] = v[0]; p[1] = v[1]; p[2] = v[2] + (double)g_tower_h - TP_LIFT;
    }

    if (g_tphase == 0) {                                             // validate the point, then the chest checker
        g_tchest = t;
        if (g_tmode == 1) {                                          // plain goto: no chest, no cannon gate
            if (!in_world(p[0], p[1], p[2])) { g_ti++; g_tnext = t; return; }
            fly_sync(pawn); g_flyn = 0; g_hover_ok = 0;
            hold_rotation(pawn);                                     // keep the facing this leg started with
            fly_gravity(pawn, 1);
            uint64_t v = get_vehicle(pawn);
            if (v) fly_nograv(v, 1);
            fly_noclip(pawn, 1);
            if (v) fly_noclip(v, 1);
            g_tp_tries = 0;
            g_tphase = 1; g_tnext = t; return;
        }
        if (g_tmode != 2) {                                          // chest-only checks
            if (!(plausible(o) && rd64(o + OBJ_CLASS) == g_tcls[g_ti] && !(rd64(o + 8) & 0x18000)) || !in_world(p[0], p[1], p[2])) {
                // The actor streamed out while we were far away. Re-finding it means a full GObjects walk, which is a
                // visible hitch in the middle of the flight - the one thing that used to make the tour stutter. Skip
                // the point instead; the run has 100+ of them and the chest is gone from the stream anyway.
                o = 0;
            }
            if (!o || !in_world(p[0], p[1], p[2])) { logf_("tour: %d/%d no chest near point", g_ti + 1, g_tn); g_ti++; g_tnext = t; return; }
            if (chest_open(o)) {                                     // already looted: skip it without a detour
                g_tobj[g_ti] = 0; g_tskip++; g_ti++; g_tnext = t;
                if (g_tskip <= 8 || (g_tskip % 25) == 0) logf_("tour: %d/%d chest %lx already open, skipped (%d so far)", g_ti, g_tn, o, g_tskip);
                return;
            }
        }
        fly_sync(pawn); g_flyn = 0; g_hover_ok = 0;
        hold_rotation(pawn);                                         // keep the facing this leg started with
        if (!in_world(g_flypos[0], g_flypos[1], g_flypos[2])) {
            logf_("tour: %d/%d pawn position unreadable, skipped", g_ti + 1, g_tn);
            g_ti++; g_tnext = t + 200; return;
        }
        if (g_tmode != 2) {                                          // chest-only sanity checks
            if (p[2] < -2500.0) {                                    // chest under the island floor: teleporting there kills the player
                logf_("tour: %d/%d chest at %.0f z is under the map, skipped", g_ti + 1, g_tn, p[2]);
                g_ti++; g_tnext = t; return;
            }
            if (p[2] < g_flypos[2] - 8000.0) {                       // chest far below us: almost certainly under the map
                logf_("tour: %d/%d chest %.0f uu below the player, skipped", g_ti + 1, g_tn, g_flypos[2] - p[2]);
                g_ti++; g_tnext = t; return;
            }
        }
        fly_gravity(pawn, 1);                                        // character gravity off
        uint64_t veh = get_vehicle(pawn);
        if (veh) fly_nograv(veh, 1);                                 // vehicle physics gravity off + velocity 0
        fly_noclip(pawn, 1);                                         // no collision walls/terrain while flying
        if (veh) fly_noclip(veh, 1);
        g_tp_tries = 0;
        g_tphase = 1; g_tnext = t; return;
    }

    if (g_tphase == 1 && g_instant) {                                // instant mode: straight physics teleport
        if (g_tp_tries >= 8) {                                       // the game refused 8 times: give up on this chest
            logf_("tour: %d/%d instant teleport refused, skipped", g_ti + 1, g_tn);
            tour_recover(pawn);
            g_ti++; g_tphase = 0; g_tnext = t; return;
        }
        g_tp_tries++;
        // the same physics state the flight uses: no gravity on the character, no gravity + no collisions on
        // the cannon, no residual velocity. Re-asserted every attempt because the game can reset them.
        fly_gravity(pawn, 1);
        uint64_t iv = get_vehicle(pawn);
        if (iv) { fly_nograv(iv, 1); fly_noclip(iv, 1); }
        fly_noclip(pawn, 1);
        fly_hold(pawn);
        teleport_pawn(pawn, p[0], p[1], p[2] + TP_LIFT);              // position AND frozen rotation, physics teleport
        g_flypos[0] = p[0]; g_flypos[1] = p[1]; g_flypos[2] = p[2] + TP_LIFT; g_flypos_ok = 1; g_fly_t = t;
        g_tphase = 2; g_tnext = t + 90;                               // verify the landing on the next step
        return;
    }

    if (g_tphase == 1) {                                             // fly: one short hop per step, no snap
        if (t - g_tchest > 15000) {                                  // teleports rejected (no cannon / anti-cheat): drop it
            logf_("tour: %d/%d fly timed out at %.0f %.0f %.0f", g_ti + 1, g_tn, p[0], p[1], p[2]);
            tour_recover(pawn);
            g_ti++; g_tphase = 0; g_tnext = t; return;
        }
        if (fly_step(pawn, p[0], p[1], p[2] + TP_LIFT, 90.0)) {
            g_tphase = 2; g_tnext = t + FLY_SETTLE_MS; return;
        }
        if (++g_flyn % 10 == 0) {
            fly_resync(pawn);
            static uint64_t fl_bad_t = 0;
            if (g_tmode == 0 && g_need_cannon && !cannon_ok(pawn)) {  // ejected mid-flight: freeze the run (do not keep
                if (!fl_bad_t) fl_bad_t = t;                          // teleporting a cannon nobody rides)
                if (t - fl_bad_t < 700) { g_tnext = t + FLY_STEP_MS; return; }   // give a streaming hiccup time
                fl_bad_t = 0;
                logf_("tour: ejected mid-flight - pausing to re-enter the cannon");   // teleporting a cannon nobody rides)
                g_tphase = 0; g_hover_ok = 0; g_flyn = 0; g_flypos_ok = 0;
                tour_stop_gravity(pawn);
                g_tnext = t + 400;
                return;
            }
        }
        g_tnext = t + FLY_STEP_MS; return;
    }

    {                                                                 // phase 2: verify the landing, then open it
        double pp[3]; get_vec(pawn, A_FN_GETLOC, pp);
        double dx = p[0] - pp[0], dy = p[1] - pp[1], dz = (p[2] + TP_LIFT) - pp[2];
        double d = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
        int under = (pp[2] < p[2] - 1200.0) || (pp[2] < -3000.0 && p[2] > -1000.0);   // fell through the floor
        if (!in_world(pp[0], pp[1], pp[2]) || d > 6000.0 || under) {
            if (g_instant && g_tp_tries < 8) { g_tphase = 1; g_tnext = t + 60; return; }   // retry, like the PC tool
            logf_("tour: %d/%d bad landing (pawn %.0f %.0f %.0f, chest %.0f %.0f %.0f, %.0f uu%s)",
                  g_ti + 1, g_tn, pp[0], pp[1], pp[2], p[0], p[1], p[2], d, under ? ", UNDER THE MAP" : "");
            tour_recover(pawn);
            g_ti++; g_tphase = 0; g_tnext = t + 400; return;
        }
        g_last_good[0] = pp[0]; g_last_good[1] = pp[1]; g_last_good[2] = pp[2]; g_have_good = 1;
        // keep flying: gravity stays off, the cannon keeps its noclip, and hover pins position AND rotation
        // every tick - the run never "lands" (landing is what un-flew the cannon and ejected the player).
        g_hover[0] = pp[0]; g_hover[1] = pp[1]; g_hover[2] = pp[2]; g_hover_ok = 1; g_hover_next = 0;
        if (g_tmode == 2) g_track = o;                               // stay above this player while we dwell
    }
    if (o && g_tmode != 2) { interact(o); g_topen++; }
    if (g_tmode == 1) {                                              // cannon goto: finish right away
        g_ti++; g_tphase = 0; g_tnext = t; return;
    }
    if (g_ti < 12 || (g_ti % 20) == 0) {
        if (g_tmode == 2) logf_("tower: %d/%d above '%s' (+%d uu)", g_ti + 1, g_tn, g_tnm[g_ti], g_tower_h);
        else logf_("tour: %d/%d %s %lx", g_ti + 1, g_tn, o ? "opened" : "no chest", o);
    }
    g_ti++; g_tphase = 0;
    g_tnext = t + g_tstep;                                           // short dwell only, then fly on
    if (g_tnext < t + 40) g_tnext = t + 40;
}
static char g_sc_text[128];
static void do_near(void) {
    find_super_off();
    uint64_t pawn = get_pawn(); if (!pawn) { logf_("near: no pawn"); return; }
    double me[3]; get_vec(pawn, A_FN_GETLOC, me);
    if (!level_walk_now(1, 50)) { scan_objects(); fill_positions(); }
    int b = nearest(me, 0);
    if (b < 0) { logf_("near: no container"); return; }
    double dx = g_cont[b].x - me[0], dy = g_cont[b].y - me[1], dz = g_cont[b].z - me[2];
    logf_("near: nearest %lx at distance %.0f (xy %.0f z %.0f)", g_cont[b].obj, __builtin_sqrt(dx*dx + dy*dy + dz*dz), __builtin_sqrt(dx*dx + dy*dy), dz);
    interact(g_cont[b].obj);
    logf_("near: interact sent");
}
static void do_servercheat(void) {
    if (!g_sc_text[0]) { logf_("sc: empty"); return; }
    static uint16_t w[128];
    int n = 0; for (; g_sc_text[n] && n < 120; n++) w[n] = (uint16_t)(uint8_t)g_sc_text[n];
    w[n] = 0;
    uint8_t p[32] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    *(uint64_t *)p = (uint64_t)w; *(int32_t *)(p + 8) = n + 1; *(int32_t *)(p + 12) = 128;
    call_pe2(g_pc, A_FN_SERVERCHEAT, p);
    logf_("sc: sent '%s'", g_sc_text);
}
static int g_menu = 0, g_msel = 0;
static int ui_wanted(void);

// ---- in-game canvas menu (AHUD::ReceiveDrawHUD) ----------------------------------------------------------
// A plugin cannot create a GPU overlay, but Fortnite's HUD is a Blueprint event that is executed through
// ProcessEvent every frame and draws onto a UCanvas. The ProcessEvent detour below watches for that
// UFUNCTION (matched by its FName index) and then draws the menu with UCanvas::K2_DrawLine / K2_DrawBox /
// K2_DrawText. Font, canvas and the canvas functions are all resolved from live objects, so no build
// specific address is needed for the drawing itself.
#define OBJ_NAME          0x18
#define USTRUCT_CHILDREN  0x48
#define UFIELD_NEXT       0x28
#define HUD_CANVAS        0x2b8            // AHUD.canvas (scanned for if the layout moved)
#define ENG_SMALLFONT     0x50             // UEngine.SmallFont
typedef struct { double x, y; } V2;
typedef struct { float r, g, b, a; } LC;

static uint32_t g_draw_idx = 0;            // FName index of "ReceiveDrawHUD" (0 = hook off)
static int g_canvas_off = HUD_CANVAS;
static uint64_t g_canvas = 0, g_font = 0;
static uint64_t g_cfn_text = 0, g_cfn_box = 0, g_cfn_line = 0;
static int g_draw_n = 0, g_draw_ok = 0, g_in_draw = 0;
static uint64_t g_menu_since = 0;
static volatile uint64_t g_tapout_until = 0;   // keep the game deaf for a moment after the menu closes
static volatile uint32_t g_pad_neutralised = 0;// pad reads the game did not see while the menu was open

static int streqi(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == 0 && *b == 0;
}
static int obj_name(uint64_t o, char *out, int max) { return fname_str((uint32_t)rd64(o + OBJ_NAME), out, max); }
static int obj_is_class(uint64_t o, const char *want) {
    char nm[64];
    return plausible(o) && class_name(rd64(o + OBJ_CLASS), nm, sizeof nm) && streqi(nm, want);
}

// FName index for a string: scan the name pool blocks (entries are 2-byte aligned, 2-byte header + chars).
static uint32_t find_fname_index(const char *name) {
    size_t L = strlen(name);
    for (int b = 0; b < 256; b++) {
        uint64_t blk = rd64(A_NAMEPOOL + 0x10 + (uint64_t)b * 8);
        if (!plausible(blk) || !readable(blk, 0x20000)) break;
        const uint8_t *p = (const uint8_t *)blk;
        for (uint32_t o = 0; o + 2 + L < 0x20000; o += 2) {
            uint16_t h = *(const uint16_t *)(p + o);
            if ((h & 1) || (h >> 6) != (int)L) continue;
            if (memcmp(p + o + 2, name, L) == 0) return (uint32_t)((b << 16) | (o >> 1));
        }
    }
    return 0;
}
// fallback: find an object by its FName in GObjects (only used if the class walk fails)
static uint64_t scan_object_named(const char *want) {
    uint64_t chunks = rd64(A_GOBJECTS);
    if (!plausible(chunks)) return 0;
    for (int ci = 0; ci < 64; ci++) {
        uint64_t chunk = rd64(chunks + ci * 8);
        if (!plausible(chunk) || !readable(chunk, 65536ULL * 24)) break;
        for (int i = 0; i < 65536; i++) {
            const uint8_t *it = (const uint8_t *)(chunk + (uint64_t)i * 24);
            uint64_t q0 = *(const uint64_t *)it;
            if (!q0 || (q0 & 0x1020000000000000ULL)) continue;
            uint64_t obj = ((q0 & 0x3fff00000000ULL) | *(const uint32_t *)(it + 8)) << 3;
            if (!plausible(obj) || !fast_ok(obj + OBJ_NAME, 8)) continue;
            char nm[64];
            if (!fname_str((uint32_t)frd64(obj + OBJ_NAME), nm, sizeof nm)) continue;
            if (strcmp(nm, want) == 0) return obj;
        }
    }
    return 0;
}
static uint32_t scan_function_fname(const char *want) {
    uint64_t obj = scan_object_named(want);
    if (!obj) return 0;
    uint32_t idx = (uint32_t)frd64(obj + OBJ_NAME);
    logf_("menu: %s object %lx idx %u", want, obj, idx);
    return idx;
}
static uint64_t find_font(void) {
    if (!g_eng && !resolve_engine()) return 0;
    const uint64_t offs[6] = { 0x70, ENG_SMALLFONT, 0x30, 0x90, 0xb0, 0xd0 };
    for (int i = 0; i < 6; i++) { uint64_t f = rd64(g_eng + offs[i]); if (obj_is_class(f, "Font")) { logf_("menu: font at engine+%lx", offs[i]); return f; } }
    for (uint64_t o = 0x28; o < 0x400; o += 8) { uint64_t f = rd64(g_eng + o); if (obj_is_class(f, "Font")) { logf_("menu: font at engine+%lx (scan)", o); return f; } }
    return 0;
}
static uint64_t find_canvas(uint64_t hud) {
    uint64_t c = rd64(hud + g_canvas_off);
    if (obj_is_class(c, "canvas")) return c;
    for (uint64_t o = 0x28; o < 0x400; o += 8) {
        uint64_t p = rd64(hud + o);
        if (obj_is_class(p, "canvas")) { g_canvas_off = (int)o; logf_("menu: canvas at hud+%lx", o); return p; }
    }
    return 0;
}
static int g_fn_scan_done = 0;
static void resolve_canvas_fns(uint64_t canvas) {
    if (g_cfn_text && g_cfn_line) return;
    uint64_t cls = rd64(canvas + OBJ_CLASS);
    if (!plausible(cls)) return;
    uint64_t f = rd64(cls + USTRUCT_CHILDREN);
    for (int n = 0; f && n < 4000; n++) {
        char nm[64];
        if (obj_name(f, nm, sizeof nm)) {
            if (!strcmp(nm, "K2_DrawText")) g_cfn_text = f;
            else if (!strcmp(nm, "K2_DrawBox")) g_cfn_box = f;
            else if (!strcmp(nm, "K2_DrawLine")) g_cfn_line = f;
            if (g_cfn_text && g_cfn_line) return;
        }
        uint64_t nx = rd64(f + UFIELD_NEXT);
        if (nx == f) break;
        f = nx;
    }
    if (!g_fn_scan_done) {                       // class layout not as expected: search all objects once
        g_fn_scan_done = 1;
        if (!g_cfn_text) g_cfn_text = scan_object_named("K2_DrawText");
        if (!g_cfn_box) g_cfn_box = scan_object_named("K2_DrawBox");
        if (!g_cfn_line) g_cfn_line = scan_object_named("K2_DrawLine");
        logf_("menu: canvas functions children %lx -> text %lx box %lx line %lx", rd64(cls + USTRUCT_CHILDREN), g_cfn_text, g_cfn_box, g_cfn_line);
    }
}

static void c_line(double x1, double y1, double x2, double y2, double th, LC c) {
    uint8_t p[0x38] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    V2 a = { x1, y1 }, b = { x2, y2 };
    *(V2 *)(p + 0x00) = a; *(V2 *)(p + 0x10) = b;
    *(float *)(p + 0x20) = (float)th; *(LC *)(p + 0x24) = c;
    call_pe2(g_canvas, g_cfn_line, p);
}
static void c_box(double x, double y, double w, double h, double th, LC c) {
    uint8_t p[0x38] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    V2 a = { x, y }, b = { w, h };
    *(V2 *)(p + 0x00) = a; *(V2 *)(p + 0x10) = b;
    *(float *)(p + 0x20) = (float)th; *(LC *)(p + 0x24) = c;
    call_pe2(g_canvas, g_cfn_box, p);
}
static void c_text(const char *s, double x, double y, double scale, LC c, LC sh) {
    static uint16_t wbuf[256];
    int n = 0; while (s[n] && n < 250) { wbuf[n] = (uint16_t)(uint8_t)s[n]; n++; } wbuf[n] = 0;
    uint8_t p[0x88] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    *(uint64_t *)(p + 0x00) = g_font;
    *(uint64_t *)(p + 0x08) = (uint64_t)wbuf; *(int32_t *)(p + 0x10) = n + 1; *(int32_t *)(p + 0x14) = 256;
    *(double *)(p + 0x18) = x; *(double *)(p + 0x20) = y;
    *(double *)(p + 0x28) = scale; *(double *)(p + 0x30) = scale;
    *(LC *)(p + 0x38) = c;
    *(float *)(p + 0x48) = 0.0f;
    *(LC *)(p + 0x4c) = sh;
    *(double *)(p + 0x60) = 1.0; *(double *)(p + 0x68) = 1.0;
    p[0x70] = 0; p[0x71] = 0; p[0x72] = 0;
    call_pe2(g_canvas, g_cfn_text, p);
}

static const Fn *find_fn(const char *n);
#define MENU_N 28
static void menu_item(int i, char *out, int n) {
    switch (i) {
    case 0: snprintf(out, n, "Aura: %s", g_aura ? "ON" : "OFF"); break;
    case 1: snprintf(out, n, "Tour all chests: %s", g_tauto && g_tmode == 0 ? "ON" : "OFF"); break;
    case 2: snprintf(out, n, "Tour mode: %s", g_instant ? "INSTANT" : "FLY"); break;
    case 3: snprintf(out, n, "Stop time: %.1f s", g_tstep / 1000.0); break;
    case 4: snprintf(out, n, "Teleport to nearest chest"); break;
    case 5: snprintf(out, n, "Graphics: %s", g_perf == 0 ? "OFF" : (g_perf == 1 ? "PERF" : "LOW")); break;
    case 6: snprintf(out, n, "Cannon ESP: %s", g_esp ? "ON" : "OFF"); break;
    case 7: snprintf(out, n, "Player ESP: %s", g_esp_plr ? "ON" : "OFF"); break;
    case 8: snprintf(out, n, "Vehicle ESP: %s", g_esp_veh ? "ON" : "OFF"); break;
    case 9: snprintf(out, n, "Fly to nearest cannon"); break;
    case 10: snprintf(out, n, "God mode"); break;
    case 11: snprintf(out, n, "Infinite ammo"); break;
    case 12: snprintf(out, n, "Stop everything"); break;
    case 13: snprintf(out, n, "Player health bars: %s", g_esp_hp ? "ON" : "OFF"); break;
    case 14: snprintf(out, n, "Player skeleton: %s", g_esp_skel ? "ON" : "OFF"); break;
    case 15: snprintf(out, n, "Chest ESP: %s", g_esp_chest ? "ON" : "OFF"); break;
    case 16: snprintf(out, n, "Snaplines: %s", g_esp_snap ? "ON" : "OFF"); break;
    case 17: snprintf(out, n, "Aimbot (hold L2): %s", g_aim ? "ON" : "OFF"); break;
    case 18: snprintf(out, n, "Aimbot part: %s", AIM_PART_NAME[g_aim_part]); break;
    case 19: snprintf(out, n, "Aimbot FOV: %d deg", g_aim_fov); break;
    case 20: snprintf(out, n, g_aim_smooth ? "Aimbot smooth: %d" : "Aimbot smooth: INSTANT", g_aim_smooth); break;
    case 21: snprintf(out, n, g_fov_chg ? "Camera FOV: %d" : "Camera FOV: OFF", g_fov_chg); break;
    case 22: snprintf(out, n, "Anti cannon kick: %s", g_kickguard ? "ON" : "OFF"); break;
    case 23: snprintf(out, n, "Water guard: %s", g_waterguard ? "ON" : "OFF"); break;
    case 24: snprintf(out, n, "Seat lock: %s", g_seatlock ? "ON" : "OFF"); break;
    case 25: snprintf(out, n, "Tower over players: %s", (g_tauto && g_tmode == 2) ? "ON" : "OFF"); break;
    case 26: snprintf(out, n, "Cannon ghost: %s", g_ghost_on ? "ON (hidden)" : "OFF (visible)"); break;
    default: snprintf(out, n, "Close menu"); break;
    }
}
static void menu_render(uint64_t canvas) {
    g_canvas = canvas;
    int sw = *(volatile int *)(canvas + 0x40), sh = *(volatile int *)(canvas + 0x44);
    if (sw < 640 || sw > 7680) { sw = 1920; sh = 1080; }
    if (sh < 360 || sh > 4320) sh = 1080;
    double S = sw / 1920.0; if (S < 0.5) S = 0.5; if (S > 2.0) S = 2.0;
    const double w = 430.0 * S, x = 40.0 * S, y = 70.0 * S;
    const double h = (54.0 + MENU_N * 26.0 + 34.0) * S;
    LC bg = { 0.04f, 0.04f, 0.06f, 0.90f }, br = { 0.92f, 0.18f, 0.18f, 1.0f };
    LC tb = { 0.60f, 0.07f, 0.09f, 1.0f }, sel = { 0.85f, 0.18f, 0.20f, 1.0f };
    LC wtx = { 0.98f, 0.98f, 0.98f, 1.0f }, dim = { 0.70f, 0.70f, 0.70f, 1.0f };
    LC shd = { 0.0f, 0.0f, 0.0f, 1.0f };

    c_line(x, y + h / 2, x + w, y + h / 2, h, bg);                  // panel
    c_line(x, y + 18 * S, x + w, y + 18 * S, 36 * S, tb);           // title bar
    for (int i = 0; i < MENU_N; i++)
        if (i == g_msel) c_line(x + 4 * S, y + (54 + i * 26 + 12) * S, x + w - 4 * S, y + (54 + i * 26 + 12) * S, 23 * S, sel);
    c_box(x, y, w, h, 2 * S, br);                                   // border

    char line[80], title[64];
    snprintf(title, sizeof title, ">,< // funcult   %d/%d", g_msel + 1, MENU_N);
    c_text(title, x + 12 * S, y + 8 * S, 1.15 * S, wtx, shd);
    for (int i = 0; i < MENU_N; i++) {
        menu_item(i, line, sizeof line);
        char row[96];
        snprintf(row, sizeof row, "%c %s", i == g_msel ? '>' : ' ', line);
        c_text(row, x + 12 * S, y + (54 + i * 26 + 5) * S, 1.10 * S, i == g_msel ? wtx : dim, shd);
    }
    c_text("UP/DOWN move   X select   O close", x + 12 * S, y + (54 + MENU_N * 26 + 8) * S, 0.95 * S, dim, shd);
}
static void banner_render(uint64_t canvas) {
    g_canvas = canvas;
    int sw = *(volatile int *)(canvas + 0x40);
    if (sw < 640 || sw > 7680) sw = 1920;
    double S = sw / 1920.0; if (S < 0.5) S = 0.5; if (S > 2.0) S = 2.0;
    LC bg = { 0.45f, 0.05f, 0.05f, 0.92f }, br = { 0.95f, 0.25f, 0.25f, 1.0f }, wtx = { 1, 1, 1, 1 }, shd = { 0, 0, 0, 1 };
    double x = 40 * S, y = 40 * S, w = 360 * S, h = 34 * S;
    c_line(x, y + h / 2, x + w, y + h / 2, h, bg);
    c_box(x, y, w, h, 2 * S, br);
    c_text("PAUSED - get in a cannon", x + 10 * S, y + 7 * S, 1.1 * S, wtx, shd);
}
static void draw_ui_on_canvas(uint64_t canvas) {
    g_canvas = canvas;                               // K2_Draw* always need THIS frame's canvas
    if (!g_font) g_font = find_font();
    if (!g_font) { if (g_draw_n < 8) logf_("menu: no Font object (engine %lx)", g_eng); return; }
    resolve_canvas_fns(canvas);
    if (!g_cfn_text || !g_cfn_line) return;
    if (g_menu) menu_render(canvas);
    if (g_esp || g_esp_plr || g_esp_veh || g_esp_chest) esp_draw(canvas);
    if (g_cannon_paused && g_need_cannon) banner_render(canvas);
    g_draw_ok++;
}
static void hud_draw(uint64_t hud) {
    if (!ui_wanted()) return;
    int n = g_draw_n++;
    uint64_t canvas = find_canvas(hud);
    if (!canvas) { if (n < 8) logf_("menu: hud %lx has no canvas", hud); return; }
    if (n < 8) logf_("menu: draw hud %lx canvas %lx", hud, canvas);
    draw_ui_on_canvas(canvas);
}

// ---- v28: text menu through UKismetSystemLibrary::PrintString ------------------------------------------
// The shipping HUD never dispatches AHUD::ReceiveDrawHUD (v27 log proof), so the canvas path above stays
// silent. PrintString is the engine's own "print string" node: it feeds the on-screen debug message list,
// which is gated by two globals the shipping build keeps cleared. Flip them back on and push one
// multi-line message every ~250 ms. If Fortnite ever draws through a UCanvas we also piggyback on that
// canvas object (see pe_hook), which turns the same text menu into boxes/lines for free.
#define UI_FLAG_ENABLED  0x15504c28ULL      // checked by PrintString before adding a screen message
#define UI_FLAG_AUX      0x15504c20ULL      // second flag PrintString tests right after
static uint64_t g_ps_fn = 0;                // PrintString UFunction object
static uint32_t g_ps_name = 0, g_ps_key = 0;
static int g_ps_state = 0, g_ps_ok = 0, g_ui_flags = 0;
static uint64_t g_ps_next = 0, g_ps_calls = 0;
static uint64_t g_canvas_cls = 0, g_canvas_events = 0, g_canvas_last = 0;
static uint64_t g_hud_obj = 0, g_hud_cls = 0;
static int g_hud_idle_frames = 0;
static uint16_t g_ui_wbuf[800];

static void ui_unlock_messages(void) {
    if (g_ui_flags) return;
    g_ui_flags = 1;
    int a = -1, b = -1;
    if (readable(UI_FLAG_ENABLED, 1)) { a = *(volatile uint8_t *)UI_FLAG_ENABLED; *(volatile uint8_t *)UI_FLAG_ENABLED = 1; }
    if (readable(UI_FLAG_AUX, 1)) { b = *(volatile uint8_t *)UI_FLAG_AUX; *(volatile uint8_t *)UI_FLAG_AUX = 1; }
    logf_("ui: screen message flags were %d/%d, set to 1/1", a, b);
}

static uint64_t find_object_with_name(uint32_t idx, const char *cls) {
    uint64_t chunks = rd64(A_GOBJECTS);
    if (!plausible(chunks)) return 0;
    for (int ci = 0; ci < 64; ci++) {
        uint64_t chunk = rd64(chunks + ci * 8);
        if (!plausible(chunk) || !readable(chunk, 65536ULL * 24)) break;
        for (int i = 0; i < 65536; i++) {
            const uint8_t *it = (const uint8_t *)(chunk + (uint64_t)i * 24);
            uint64_t q0 = *(const uint64_t *)it;
            if (!q0 || (q0 & 0x1020000000000000ULL)) continue;
            uint64_t obj = ((q0 & 0x3fff00000000ULL) | *(const uint32_t *)(it + 8)) << 3;
            if (!plausible(obj) || !fast_ok(obj + OBJ_NAME, 8)) continue;
            if ((uint32_t)frd64(obj + OBJ_NAME) != idx) continue;
            if (!cls || obj_is_class(obj, cls)) return obj;
        }
    }
    return 0;
}

static uint64_t find_object_by_text(const char *want, const char *cls) {
    uint64_t chunks = rd64(A_GOBJECTS);
    if (!plausible(chunks)) return 0;
    for (int ci = 0; ci < 64; ci++) {
        uint64_t chunk = rd64(chunks + ci * 8);
        if (!plausible(chunk) || !readable(chunk, 65536ULL * 24)) break;
        for (int i = 0; i < 65536; i++) {
            const uint8_t *it = (const uint8_t *)(chunk + (uint64_t)i * 24);
            uint64_t q0 = *(const uint64_t *)it;
            if (!q0 || (q0 & 0x1020000000000000ULL)) continue;
            uint64_t obj = ((q0 & 0x3fff00000000ULL) | *(const uint32_t *)(it + 8)) << 3;
            if (!plausible(obj) || !fast_ok(obj + OBJ_NAME, 8)) continue;
            char nm[64];
            if (!obj_name(obj, nm, sizeof nm) || strcmp(nm, want)) continue;
            if (!cls || obj_is_class(obj, cls)) return obj;
        }
    }
    return 0;
}

// ---- chest checker: ABuildingContainer::IsAlreadySearched resolved by name --------------------------------
// The UFunction is not at a fixed address in this build, so it is looked up once the match is live: a Function
// object with that name whose outer class derives from BuildingContainer. Called through ProcessEvent it
// returns the bAlreadySearched flag the server keeps for every container.
static uint64_t find_class_fn(const char *want, uint64_t base) {
    uint32_t idx = find_fname_index(want);
    if (!idx) return 0;
    uint64_t chunks = rd64(A_GOBJECTS);
    if (!plausible(chunks)) return 0;
    for (int ci = 0; ci < 64; ci++) {
        uint64_t chunk = rd64(chunks + ci * 8);
        if (!plausible(chunk) || !readable(chunk, 65536ULL * 24)) break;
        for (int i = 0; i < 65536; i++) {
            const uint8_t *it = (const uint8_t *)(chunk + (uint64_t)i * 24);
            uint64_t q0 = *(const uint64_t *)it;
            if (!q0 || (q0 & 0x1020000000000000ULL)) continue;
            uint64_t obj = ((q0 & 0x3fff00000000ULL) | *(const uint32_t *)(it + 8)) << 3;
            if (!plausible(obj) || !fast_ok(obj + OBJ_NAME, 8)) continue;
            if ((uint32_t)frd64(obj + OBJ_NAME) != idx) continue;
            if (!obj_is_class(obj, "Function")) continue;
            if (!is_sub_of(frd64(obj + OBJ_OUTER), base)) continue;
            return obj;
        }
    }
    return 0;
}

// ---- perf: the graphics preset the console unlocker applies (fn.gfx perf) --------------------------------
// UKismetSystemLibrary::ExecuteConsoleCommand is also resolved by name (its address is not in the dump).
static uint64_t g_fn_exec = 0;            // ExecuteConsoleCommand UFunction

static const char *PERF_ON[] = {
    "t.MaxFPS 60", "r.ScreenPercentage 100", "r.DynamicRes.OperationMode 1",
    "r.DynamicRes.MinScreenPercentage 55", "r.DynamicRes.MaxScreenPercentage 100",
    "r.ViewDistanceScale 0.7", "r.DetailMode 2", "r.MotionBlurQuality 0", "r.DefaultFeature.MotionBlur 0",
    "r.DepthOfFieldQuality 0", "r.BloomQuality 0", "r.SSR.Quality 0", "r.VolumetricFog 0",
    "r.VolumetricCloud 0", "r.LightShaftQuality 0", "r.ContactShadows 0", "r.CapsuleShadows 0",
    "r.Shadow.MaxResolution 1024", "r.Shadow.CSM.MaxCascades 2", "r.MaxAnisotropy 2",
    "r.SkeletalMeshLODBias 1", "r.StaticMeshLODDistanceScale 1.5", "r.LandscapeLODDistributionScale 0.5",
    "r.Streaming.MipBias 1", "r.RefractionQuality 0", "r.SceneColorFringeQuality 0", "r.DisableDistortion 1",
    "r.ParticleLightQuality 0", "r.EyeAdaptationQuality 0", "grass.Enable 0", "foliage.DensityScale 0",
    "fx.Niagara.QualityLevel 0", "sg.ShadowQuality 1", "sg.EffectsQuality 1", "sg.PostProcessQuality 1",
    "sg.FoliageQuality 0", "sg.ReflectionQuality 0", "sg.GlobalIlluminationQuality 0", "sg.LandscapeQuality 1",
    "r.Tonemapper.Sharpen 0", "r.LensFlareQuality 0", "r.Tonemapper.Quality 1",
};
static const char *PERF_OFF[] = {
    // absolute defaults: every cvar PERF/LOW touches is put back here, so toggling the menu repeatedly
    // can never leave the game in a mixed state (that is what made it feel laggy after several toggles)
    "t.MaxFPS 0", "r.ScreenPercentage 100", "r.DynamicRes.OperationMode 1",
    "r.DynamicRes.MinScreenPercentage 100", "r.DynamicRes.MaxScreenPercentage 100",
    "r.ViewDistanceScale 1", "r.DetailMode 2", "r.MotionBlurQuality 4", "r.DefaultFeature.MotionBlur 1",
    "r.DepthOfFieldQuality 2", "r.BloomQuality 5", "r.SSR.Quality 3", "r.VolumetricFog 1",
    "r.VolumetricCloud 1", "r.LightShaftQuality 1", "r.ContactShadows 1", "r.CapsuleShadows 1",
    "r.Shadow.MaxResolution 2048", "r.Shadow.CSM.MaxCascades 4", "r.MaxAnisotropy 4",
    "r.SkeletalMeshLODBias 0", "r.StaticMeshLODDistanceScale 1", "r.LandscapeLODDistributionScale 1",
    "r.Streaming.MipBias 0", "r.RefractionQuality 1", "r.SceneColorFringeQuality 1", "r.DisableDistortion 0",
    "r.ParticleLightQuality 1", "r.EyeAdaptationQuality 1", "grass.Enable 1", "foliage.DensityScale 1",
    "fx.Niagara.QualityLevel 3", "sg.ShadowQuality 3", "sg.EffectsQuality 3", "sg.PostProcessQuality 3",
    "sg.FoliageQuality 3", "sg.ReflectionQuality 3", "sg.GlobalIlluminationQuality 3", "sg.LandscapeQuality 3",
    // the texture settings (this is all OFF should ever have to repair)
    "sg.TextureQuality 3", "sg.ViewDistanceQuality 3", "r.MaterialQualityLevel 1",
    "r.PostProcessAAQuality 6", "r.AmbientOcclusionLevels 3", "r.Shadow.DistanceScale 1",
    "r.DistanceFieldShadowing 1", "r.DistanceFieldAO 1", "r.Tonemapper.Sharpen 1",
    "r.DefaultFeature.AntiAliasing 2", "r.LensFlareQuality 1", "r.Tonemapper.Quality 3",
    "r.DefaultFeature.AutoExposure 1", "r.Streaming.LimitPoolSizeToVRAM 0",
};
static const char *PERF_LOW[] = {
    "t.MaxFPS 60", "r.ScreenPercentage 50", "r.DynamicRes.OperationMode 0",
    "r.DynamicRes.MinScreenPercentage 50", "r.DynamicRes.MaxScreenPercentage 50",
    "r.ViewDistanceScale 0.35", "r.DetailMode 0", "r.MaterialQualityLevel 0",
    "r.MotionBlurQuality 0", "r.DefaultFeature.MotionBlur 0", "r.DepthOfFieldQuality 0",
    "r.BloomQuality 0", "r.AmbientOcclusionLevels 0", "r.SSR.Quality 0",
    "r.VolumetricFog 0", "r.VolumetricCloud 0", "r.LightShaftQuality 0",
    "r.ContactShadows 0", "r.CapsuleShadows 0", "r.ParticleLightQuality 0",
    "r.EyeAdaptationQuality 0", "r.Shadow.MaxResolution 256", "r.Shadow.CSM.MaxCascades 1",
    "r.Shadow.DistanceScale 0.5", "r.DistanceFieldShadowing 0", "r.DistanceFieldAO 0",
    "r.SkeletalMeshLODBias 3", "r.StaticMeshLODDistanceScale 3", "r.LandscapeLODDistributionScale 0",
    "r.Streaming.MipBias 2", "r.MaxAnisotropy 0", "r.RefractionQuality 0",
    "r.SceneColorFringeQuality 0", "r.DisableDistortion 1", "r.Tonemapper.Sharpen 0",
    "r.PostProcessAAQuality 0", "grass.Enable 0", "foliage.DensityScale 0",
    "fx.Niagara.QualityLevel 0", "sg.ShadowQuality 0", "sg.EffectsQuality 0",
    "sg.PostProcessQuality 0", "sg.FoliageQuality 0", "sg.ReflectionQuality 0",
    "sg.GlobalIlluminationQuality 0", "sg.LandscapeQuality 0", "sg.TextureQuality 1",
    "sg.ViewDistanceQuality 0", "r.LensFlareQuality 0", "r.Tonemapper.Quality 0",
    "r.DefaultFeature.AutoExposure 0",
};

static void exec_cmd(uint64_t world, const char *cmd) {
    if (!g_fn_exec || !g_pc) return;
    static uint16_t w[160];
    int n = 0; while (cmd[n] && n < 150) { w[n] = (uint16_t)(uint8_t)cmd[n]; n++; } w[n] = 0;
    uint8_t p[48] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    *(uint64_t *)(p + 0x00) = world;                 // WorldContextObject
    *(uint64_t *)(p + 0x08) = (uint64_t)w;           // Command (FString data)
    *(int32_t  *)(p + 0x10) = n + 1;
    *(int32_t  *)(p + 0x14) = 160;
    *(uint64_t *)(p + 0x18) = 0;                     // SpecificPlayer
    call_pe2(g_pc, g_fn_exec, p);
}
static void perf_run(const char **arr, size_t n) {
    for (size_t i = 0; i < n; i++) exec_cmd(g_pc, arr[i]);
}
static void perf_apply(void) {
    if (cfg_off || cfg_nocvars) { static int once = 0; if (!once) { once = 1; logf_("perf: skipped (cfg)"); } return; }
    if (!g_fn_exec) { logf_("perf: no ExecuteConsoleCommand (%s not applied)", g_perf == 0 ? "OFF" : (g_perf == 1 ? "PERF" : "LOW")); return; }
    const char **arr; size_t n; const char *label;
    if (g_perf == 0)      { arr = PERF_OFF; n = sizeof PERF_OFF / sizeof PERF_OFF[0]; label = "OFF"; }
    else if (g_perf == 1) { arr = PERF_ON;  n = sizeof PERF_ON  / sizeof PERF_ON[0];  label = "PERF"; }
    else                  { arr = PERF_LOW; n = sizeof PERF_LOW / sizeof PERF_LOW[0]; label = "LOW"; }
    // Always start from the untouched defaults. Toggling OFF -> LOW -> PERF -> OFF used to leave cvars from
    // the previous preset behind (min/max dynamic res, texture quality, detail mode), and that mixed state is
    // what makes the game feel laggy/ugly after a few toggles.
    if (g_perf != 0) perf_run(PERF_OFF, sizeof PERF_OFF / sizeof PERF_OFF[0]);
    perf_run(arr, n);
    logf_("perf: %s (%u cvars applied, defaults reset first)", label, (unsigned)n);
}

// Resolve the two runtime functions once a live match exists (needs the GameInstance + SuperStruct chain).
static void resolve_extras(void) {
    static int super_try = 0, chest_try = 0, exec_try = 0, grav_try = 0;
    if (!super_try) { find_super_off(); super_try = 1; }
    if ((!g_fn_setgrav || !g_fn_zerovel || !g_fn_noclip || !g_fn_angvel) && grav_try < 5) {
        grav_try++;
        if (!g_fn_setgrav) g_fn_setgrav = find_object_by_text("SetEnableGravity", "Function");
        if (!g_fn_zerovel) g_fn_zerovel = find_object_by_text("SetPhysicsLinearVelocity", "Function");
        if (!g_fn_noclip) g_fn_noclip = find_object_by_text("SetActorEnableCollision", "Function");
        if (!g_fn_angvel) g_fn_angvel = find_object_by_text("SetPhysicsAngularVelocityInDegrees", "Function");
        if (!g_fn_hide) g_fn_hide = find_object_by_text("SetActorHiddenInGame", "Function");
        logf_("fly: gravity %lx, linvel %lx, angvel %lx, noclip %lx (%s)", g_fn_setgrav, g_fn_zerovel, g_fn_angvel, g_fn_noclip,
              (g_fn_setgrav && g_fn_zerovel) ? "vehicle gravity control ok" : (grav_try < 5 ? "retrying" : "partial"));
    }
    if (!g_fn_searched && chest_try < 5) {
        chest_try++;
        g_fn_searched = find_class_fn("IsAlreadySearched", A_CLS_CONTAINER);
        if (!g_fn_searched) g_fn_searched = find_class_fn("BP_IsAlreadySearched", A_CLS_CONTAINER);
        logf_("chest: IsAlreadySearched %lx (checker %s)", g_fn_searched, g_fn_searched ? "active" : (chest_try < 5 ? "retrying" : "inactive"));
    }
    if (!g_fn_exec && exec_try < 5) {
        exec_try++;
        g_fn_exec = find_object_by_text("ExecuteConsoleCommand", "Function");
        logf_("perf: ExecuteConsoleCommand %lx (%s)", g_fn_exec, g_fn_exec ? "convar access ok" : (exec_try < 5 ? "retrying" : "not found"));
    }
}

// ---- cannon ESP + fly-to-cannon ---------------------------------------------------------------------------
// The cannons are ordinary AActor instances whose class name contains "Cannon". They are collected with the
// same GObjects walk the containers use. The projection is computed here, in plain math, from the live
// APlayerCameraManager (location + rotation + FOV) - no native code address and no Project UFunction is called,
// because the native ProjectWorldToScreen crashed the title the moment the ESP ran. Screen positions are cached
// on the game thread at ~8 Hz; the render hook only draws boxes.
// ---- ESP: cannons, players (name + bot flag) and every vehicle (box + tower beacon) -----------------------
// The projection is plain math from the live APlayerCameraManager (location + rotation + FOV); no native code
// and no Project UFunction is called (calling that crashed the title). The actor lists come from ONE GObjects
// walk when the ESP is switched on and then at most every 30 s; every other tick only re-reads cached
// positions, so the overlay never hitches. Players are NOT walked: they come from
// UWorld->GameState->PlayerArray (a handful of reads, so a joining player shows up within 400 ms).
#define MAX_ENT 128
#define MAX_PLR 100
typedef struct { uint64_t obj; double x, y, z; char nm[20]; } Ent;
static Ent g_cann[MAX_ENT]; static int g_ncann = 0;
static Ent g_veh[MAX_ENT];  static int g_nveh = 0;
typedef struct {
    uint64_t ps, pawn, seen;
    double x, y, z;
    char name[26];
    int named, bot, bot_known, local, have, logged;
    int alive, hpk, hp_live;                         // alive: pawn alive and health > 0; hpk: hp/hpmax valid; hp_live: seen with hp > 0
    float hp, hpmax, sh, shmax;
    float hp_est, sh_est;                           // damage-tracked estimate when the title does not replicate HP
    float d2;                                       // v130: squared distance to the local player
    unsigned char hp_guess;                         // 1 = bar comes from hit notifications, not the health set
    unsigned char hp_init;                          // estimate has been seeded (0 HP is a valid value)
    unsigned char vis_ok, vis;                      // v126: cached LineOfSightTo result for the aimbot
    uint64_t vis_t;
    uint64_t hp_next;                                // next health poll for this player
    double bone[8][3];                               // skeleton points in world space
    unsigned bones_ok;                               // bit per bone
    uint64_t bone_next;
} Plr;
static Plr g_plr[MAX_PLR]; static int g_nplr = 0;
static uint64_t g_cann_scan = 0, g_cann_cls = 0, g_veh_cls = 0, g_ps_cls = 0;
static uint64_t g_ps_cache[128]; static int g_nps_cache = 0;       // APlayerState objects seen by the last walk
static int g_plr_logs = 0;
static double g_esp_me[3]; static int g_esp_me_ok = 0;
static double g_cam_loc[3], g_cam_rot[3], g_cam_me[3];
static float g_cam_fov = 90.0f; static int g_cam_ok = 0;
static uint64_t g_fn_camloc = 0, g_fn_camrot = 0, g_fn_fov = 0;
static int g_cam_try = 0, g_cam_logged = 0, g_esp_logged = 0;
static uint64_t g_esp_upd = 0, g_esp_pos = 0, g_esp_cur = 0;

static int strstri(const char *h, const char *needle) {
    size_t L = strlen(needle);
    if (!L) return 1;
    for (; *h; h++) {
        size_t i = 0;
        for (; i < L && h[i]; i++) {
            char a = h[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == L) return 1;
    }
    return 0;
}
// "FortAthenaAntelopeVehicle" -> "Antelope", "BP_Cannon_C" -> "Cannon"
static void short_name(const char *cls, char *out, int max) {
    char t[96];
    snprintf(t, sizeof t, "%s", cls);
    char *p = t;
    if (!strncmp(p, "BP_", 3)) p += 3;
    if (!strncmp(p, "FortAthena", 10)) p += 10;
    else if (!strncmp(p, "Fort", 4)) p += 4;
    int L = (int)strlen(p);
    if (L > 2 && p[L - 2] == '_' && p[L - 1] == 'C') { p[L - 2] = 0; L -= 2; }
    if (L > 7 && !strcmp(p + L - 7, "Vehicle")) p[L - 7] = 0;
    snprintf(out, max, "%s", p[0] ? p : cls);
}
// class object of the first superclass called `want` (name compare, ~10 levels): used ONCE to learn the
// AActor UClass pointer from the local controller, so the ESP never has to do a second GObjects walk.
static uint64_t class_in_chain_named(uint64_t cls, const char *want) {
    char nm[96];
    for (int i = 0; i < 48 && plausible(cls); i++) {
        if (class_name(cls, nm, sizeof nm) && !strcmp(nm, want)) return cls;
        cls = frd64(cls + g_super_off);
    }
    return 0;
}
// one SuperStruct walk tells us what a class is: 1 = AActor, 2 = AFortAthenaVehicle, 4 = APlayerState
static int cls_kind(uint64_t cls) {
    int k = 0;
    for (int i = 0; i < 48 && plausible(cls); i++) {
        if (cls == g_cann_cls) k |= 1;
        else if (g_veh_cls && cls == g_veh_cls) k |= 2;
        else if (g_ps_cls && cls == g_ps_cls) k |= 4;
        cls = frd64(cls + g_super_off);
    }
    return k;
}
// One SuperStruct walk + one name read per distinct class - never per object. The walk visits ~700k object
// headers; before this cache every one of them re-walked the class chain and re-copied the class name, and
// that was the one-second freeze on every scan.
typedef struct { uint64_t cls; unsigned char kind, cannon, veh, junk, vlike, ok; } ClsC;
static ClsC g_clsc[2048];
static ClsC *classify(uint64_t cls) {
    unsigned h = (unsigned)(((cls >> 4) ^ (cls >> 13)) & 511u) * 4u;   // 4-way set, no thrash between hot classes
    unsigned slot = h;
    for (unsigned i = 0; i < 4; i++) {
        ClsC *e = &g_clsc[h + i];
        if (e->cls == cls) return e;                               // hit (or a cached miss: cls == 0 never matches)
        if (!e->ok && !e->cls) slot = h + i;                        // first free slot wins
    }
    ClsC *e = &g_clsc[slot];
    char nm[96];
    e->cls = 0; e->ok = 0; e->kind = e->cannon = e->veh = e->junk = e->vlike = 0;
    if (!class_name(cls, nm, sizeof nm)) return e;                 // retry on the next object of this class
    e->kind = (unsigned char)cls_kind(cls);
    e->cannon = (unsigned char)(strstri(nm, "Cannon") && !strstri(nm, "Ball") && !strstri(nm, "Weapon"));
    e->junk = (unsigned char)(strstri(nm, "Projectile") || strstri(nm, "Preview") || strstri(nm, "Anim") ||
                              strstri(nm, "Spawner") || strstri(nm, "Manager") || strstri(nm, "Component"));
    e->vlike = (unsigned char)(strstri(nm, "Vehicle") || strstri(nm, "Car") || strstri(nm, "Truck") ||
                               strstri(nm, "Boat") || strstri(nm, "Chopper"));
    e->veh = (unsigned char)(!e->cannon && ((e->kind & 2) != 0));
    e->cls = cls; e->ok = 1;
    return e;
}
static int dup_in(const Ent *l, int n, const double *v) {
    for (int k = 0; k < n; k++) {
        double dx = l[k].x - v[0], dy = l[k].y - v[1], dz = l[k].z - v[2];
        if (dx*dx + dy*dy + dz*dz < 200.0*200.0) return 1;
    }
    return 0;
}
// positions only, no GObjects walk: cheap enough for every ESP tick
static void refresh_esp(void) {
    int n = 0;
    for (int i = 0; i < g_ncann; i++) {
        uint64_t o = g_cann[i].obj;
        if (!plausible(o) || !frd64(o + OBJ_CLASS) || (frd64(o + 8) & 0x18000)) continue;
        double v[3];
        if (actor_pos_mem(o, v) && in_world(v[0], v[1], v[2])) {   // a failed read keeps the last good position
            g_cann[i].x = v[0]; g_cann[i].y = v[1]; g_cann[i].z = v[2];
        }
        g_cann[n++] = g_cann[i];
    }
    g_ncann = n;
    n = 0;
    for (int i = 0; i < g_nveh; i++) {
        uint64_t o = g_veh[i].obj;
        if (!plausible(o) || !frd64(o + OBJ_CLASS) || (frd64(o + 8) & 0x18000)) continue;
        double v[3];
        if (actor_pos_mem(o, v) && in_world(v[0], v[1], v[2])) {
            g_veh[i].x = v[0]; g_veh[i].y = v[1]; g_veh[i].z = v[2];
        }
        g_veh[n++] = g_veh[i];
    }
    g_nveh = n;
}
// The GObjects walk (~4M headers) is the only expensive part of the ESP. It is now RESUMABLE: a few
// milliseconds per game tick, spread over ~20 frames, and the previous lists stay published until the new
// walk finishes. No frame ever stalls, and the ESP never blanks out between scans.
#define SCAN_BUDGET_MS 2
static int g_scan_active = 0, g_scan_ci = 0, g_scan_i = 0;
static uint64_t g_scan_chunks = 0;
static int g_sc_nc = 0, g_sc_nv = 0, g_sc_nps = 0, g_sc_learned = 0;
static Ent g_sc_cann[MAX_ENT], g_sc_veh[MAX_ENT];
static uint64_t g_sc_ps[128];

// v131: every actor the ESP needs is already in the level actor arrays, so the GObjects table (4.2 million
// headers) is no longer walked for the normal case. From the CUSA07022 v05.27 SDK dump:
//   AActor.Outer -> ULevel, ULevel.OwningWorld @0x0c8 -> UWorld, UWorld.PersistentLevel @0x030,
//   UWorld.Levels @0x1c0 (TArray<ULevel*>).
// ULevel.Actors itself is NOT typed in the dump (it sits in a MISSED OFFSET pad), so its offset is learned
// from live memory and verified - a wrong slot is rejected. The walk is resumable (about 1 ms per slice),
// stages into the same g_sc_* buffers as the old walk, and keeps the published lists live until it finishes.
// The old walk stays as the fallback.
static int g_lw_active = 0, g_lw_containers = 0, g_lw_fail = 0, g_lw_actors = 0, g_lw_log = 0;
static uint64_t g_lw_lv[512]; static int g_lw_nl = 0, g_lw_li = 0, g_lw_i = 0;
static int g_lw_ncont = 0;
static Cont g_lw_cont[MAX_CONT];
static int g_lvl_off = 0;                                   // learned ULevel.Actors offset (0 = not learned)
static uint64_t g_lvl_off_world = 0;
static int walk_wanted(void) { return g_esp || g_esp_veh || g_esp_chest; }

static void scan_obj(uint64_t obj, uint64_t cls, ClsC *ce) {
    if (ce->kind & 4) {                                            // APlayerState: cache for the player ESP
        if (g_sc_nps >= 128) return;
        for (int k = 0; k < g_sc_nps; k++) if (g_sc_ps[k] == obj) return;
        g_sc_ps[g_sc_nps++] = obj;
        return;
    }
    if (ce->junk) return;
    if (!g_veh_cls && ce->vlike) {                                 // learn AFortAthenaVehicle from a real one
        g_veh_cls = class_in_chain_named(cls, "FortAthenaVehicle");
        if (g_veh_cls) {
            logf_("esp: vehicle base class %lx", g_veh_cls);
            g_sc_learned = 1;
            memset(g_clsc, 0, sizeof g_clsc);                      // re-classify the types already passed
            ce->veh = 1;
        }
    }
    int cannon = ce->cannon;
    int veh = ce->veh && !cannon;
    if (!cannon && !veh) return;
    double v[3]; actor_pos(obj, v);
    if (!in_world(v[0], v[1], v[2])) return;
    if (cannon) {
        if (g_sc_nc >= MAX_ENT || dup_in(g_sc_cann, g_sc_nc, v)) return;
        Ent *e = &g_sc_cann[g_sc_nc++];
        e->obj = obj; e->x = v[0]; e->y = v[1]; e->z = v[2];
        snprintf(e->nm, sizeof e->nm, "Cannon");
    } else {
        if (g_sc_nv >= MAX_ENT || dup_in(g_sc_veh, g_sc_nv, v)) return;
        Ent *e = &g_sc_veh[g_sc_nv++];
        e->obj = obj; e->x = v[0]; e->y = v[1]; e->z = v[2];
        char cn[96] = "Vehicle", sn[28];
        if (class_name(cls, cn, sizeof cn)) short_name(cn, sn, sizeof sn); else sn[0] = 0;
        snprintf(e->nm, sizeof e->nm, "%s", sn[0] ? sn : "Vehicle");
    }
}
// Process up to SCAN_BUDGET_MS of entries. Returns 1 when the walk is complete.
static int scan_step(void) {
    uint64_t t0 = now_ms();
    g_fast_gen++;                                       // every 2 ms slice starts with a fresh cache generation
    while (g_scan_ci < 64) {
        uint64_t chunk = frd64(g_scan_chunks + (uint64_t)g_scan_ci * 8);
        if (!plausible(chunk) || !fast_ok(chunk, 65536ULL * 24)) return 1;
        const uint8_t *base = (const uint8_t *)chunk;
        uint64_t cur_page = 0;
        for (; g_scan_i < 65536; g_scan_i++) {
            const uint8_t *it = base + (uint64_t)g_scan_i * 24;
            uint64_t pg = (uint64_t)it & ~0xffffULL;               // resumable walk: re-checked on every resume
            if (pg != cur_page) { if (!items_ok(chunk, g_scan_i)) return 1; cur_page = pg; }
            uint64_t q0 = *(const uint64_t *)it;
            if (q0 && !(q0 & 0x1020000000000000ULL)) {
                uint64_t obj = ((q0 & 0x3fff00000000ULL) | *(const uint32_t *)(it + 8)) << 3;
                if (plausible(obj) && fast_ok(obj + OBJ_NAME, 8)) {
                    uint64_t cls = frd64(obj + OBJ_CLASS);
                    if (plausible(cls) && !(frd64(obj + 8) & 0x18000)) {
                        ClsC *ce = classify(cls);
                        if (ce->ok && (ce->kind & 1)) scan_obj(obj, cls, ce);
                    }
                }
            }
            // Time budget checked every 1024 entries instead of 8192: a single batch was measured at ~8 ms,
            // which is exactly the kind of frame-time lump the overlay must not create.
            if ((g_scan_i & 1023) == 0 && now_ms() - t0 >= SCAN_BUDGET_MS) return 0;
        }
        g_scan_i = 0; g_scan_ci++;
    }
    return 1;
}
static void scan_publish(int force) {
    memcpy(g_cann, g_sc_cann, sizeof(Ent) * (size_t)g_sc_nc); g_ncann = g_sc_nc;
    memcpy(g_veh,  g_sc_veh,  sizeof(Ent) * (size_t)g_sc_nv); g_nveh  = g_sc_nv;
    memcpy(g_ps_cache, g_sc_ps, sizeof(uint64_t) * (size_t)g_sc_nps); g_nps_cache = g_sc_nps;
    g_scan_active = 0;
    g_cann_scan = now_ms();
    if (force || g_sc_learned)
        logf_("esp: %d cannons, %d vehicles, %d player states (actor %lx, veh %lx, ps %lx)",
              g_sc_nc, g_sc_nv, g_sc_nps, g_cann_cls, g_veh_cls, g_ps_cls);
    g_sc_learned = 0;
}
// ---- v131: direct level walk -----------------------------------------------------------------------------
// ULevel.Actors is not typed by the dumper, so find the TArray<AActor*> slot that really is the actor list:
// the persistent level must contain our own controller and/or the game state, and almost every entry must
// dereference to a plausible object. A wrong candidate fails one of those and is skipped.
static int lvl_learn_actor_off(uint64_t plvl, uint64_t world) {
    static const int cand[] = { 0x250, 0x248, 0x258, 0x260, 0x268, 0x270, 0x278, 0x280,
                                0x288, 0x290, 0x298, 0x2a0, 0x2a8, 0x2b0 };
    uint64_t gs = frd64(world + 0x1a8);                                // UWorld.GameState
    for (unsigned k = 0; k < sizeof cand / sizeof cand[0]; k++) {
        int off = cand[k];
        if (!fast_ok(plvl + off, 0x10)) continue;
        uint64_t arr = frd64(plvl + off);
        int32_t num = *(volatile int32_t *)(plvl + off + 8);
        int32_t max = *(volatile int32_t *)(plvl + off + 12);
        if (!plausible(arr) || num < 1 || num > 300000 || max < num) continue;
        if (!readable(arr, (uint64_t)num * 8)) continue;
        int lim = num < 20000 ? num : 20000;                           // never probe a huge array for long
        int good = 0, hit = 0;
        for (int i = 0; i < lim; i++) {
            uint64_t o = *(volatile uint64_t *)(arr + (uint64_t)i * 8);
            if (!plausible(o) || !fast_ok(o + OBJ_CLASS, 8)) continue;
            if (!plausible(*(volatile uint64_t *)(o + OBJ_CLASS))) continue;
            good++;
            if (o == g_pc || (gs && o == gs)) hit = 1;
        }
        if (good < 3 || good * 4 < lim * 3 || !hit) continue;
        g_lvl_off = off; g_lvl_off_world = world;
        logf_("esp: level actors at +0x%x (%d actors, %d checked, %d usable)", off, num, lim, good);
        return 1;
    }
    return 0;
}
static int level_walk_start(int containers) {
    if (!g_pc) return 0;
    if (!g_cann_cls) g_cann_cls = class_in_chain_named(frd64(g_pc + OBJ_CLASS), "Actor");
    if (!g_cann_cls) return 0;                                         // controller not up yet: next tick
    if (!g_ps_cls) {                                                   // APlayerState base class
        uint64_t lps = frd64(g_pc + 0x270);
        if (plausible(lps)) g_ps_cls = class_in_chain_named(frd64(lps + OBJ_CLASS), "PlayerState");
        if (g_ps_cls) logf_("esp: player state base class %lx", g_ps_cls);
    }
    uint64_t lvl = frd64(g_pc + OBJ_OUTER);                            // AActor.Outer = the ULevel
    if (!plausible(lvl) || !fast_ok(lvl + 0xc8, 8)) return 0;
    uint64_t world = frd64(lvl + 0xc8);                                // ULevel.OwningWorld
    if (!plausible(world) || !fast_ok(world + 0x30, 8)) return 0;
    uint64_t plvl = frd64(world + 0x30);                               // UWorld.PersistentLevel (typed in the dump)
    if (!plausible(plvl)) return 0;
    if (!g_lvl_off || g_lvl_off_world != world) {
        g_lvl_off = 0;
        if (!lvl_learn_actor_off(plvl, world)) {
            logf_("esp: cannot locate ULevel.Actors - using the object walk");
            return 0;
        }
    }
    // all levels when UWorld.Levels validates (it must contain the persistent level), otherwise that one only
    int nl = 0;
    if (fast_ok(world + 0x1c0, 0x10)) {
        uint64_t levels = frd64(world + 0x1c0);
        int32_t n = *(volatile int32_t *)(world + 0x1c8);
        if (plausible(levels) && n >= 1 && n <= 512 && readable(levels, (uint64_t)n * 8)) {
            for (int i = 0; i < n; i++) {
                uint64_t L = *(volatile uint64_t *)(levels + (uint64_t)i * 8);
                if (plausible(L)) g_lw_lv[nl++] = L;
            }
            int has_p = 0;
            for (int i = 0; i < nl; i++) if (g_lw_lv[i] == plvl) has_p = 1;
            if (!has_p) nl = 0;                                        // not a level list we understand
        }
    }
    if (!nl) { g_lw_lv[0] = plvl; nl = 1; }
    g_sc_nc = g_sc_nv = g_sc_nps = 0; g_sc_learned = 0;
    if (containers) g_lw_ncont = 0;
    g_lw_containers = containers;
    g_lw_nl = nl; g_lw_li = 0; g_lw_i = 0;
    g_lw_actors = 0; g_lw_active = 1;
    return 1;
}
static void level_walk_finish(void) {
    int cont = g_lw_containers, actors = g_lw_actors;
    g_lw_active = 0;
    if (!actors) {
        g_lw_fail++;
        logf_("esp: level walk found no actors (%d levels) - falling back to the object walk", g_lw_nl);
        return;
    }
    scan_publish(0);                                               // g_cann_scan = now
    if (cont) {
        memcpy(g_cont, g_lw_cont, sizeof(Cont) * (size_t)g_lw_ncont);
        g_ncont = g_lw_ncont;
        fill_positions();
    }
    if (g_lw_log < 3) {
        g_lw_log++;
        logf_("esp: level walk: %d levels, %d actors -> %d cannons, %d vehicles, %d states (%d chests)",
              g_lw_nl, actors, g_ncann, g_nveh, g_nps_cache, g_ncont);
    }
}
// one slice: 1 = the walk is complete
static int level_walk_step(void) {
    uint64_t t0 = now_ms();
    g_fast_gen++;                                                      // fresh readable-range cache per slice
    while (g_lw_li < g_lw_nl) {
        uint64_t L = g_lw_lv[g_lw_li];
        if (!plausible(L) || !fast_ok(L + g_lvl_off, 0x10)) { g_lw_li++; g_lw_i = 0; continue; }
        uint64_t a = frd64(L + g_lvl_off);                             // ULevel.Actors
        int32_t na = *(volatile int32_t *)(L + g_lvl_off + 8);
        if (!plausible(a) || na < 1 || na > 500000 || !readable(a, (uint64_t)na * 8)) { g_lw_li++; g_lw_i = 0; continue; }
        for (; g_lw_i < na; g_lw_i++) {
            // the array range is verified above and nothing can reallocate while we hold the game thread
            uint64_t obj = *(volatile uint64_t *)(a + (uint64_t)g_lw_i * 8);
            if (plausible(obj) && fast_ok(obj + OBJ_CLASS, 8)) {
                uint64_t cls = *(volatile uint64_t *)(obj + OBJ_CLASS);
                if (plausible(cls)) {
                    g_lw_actors++;
                    ClsC *ce = classify(cls);
                    if (ce->ok && (ce->kind & 1)) scan_obj(obj, cls, ce);
                    if (g_lw_containers && g_lw_ncont < MAX_CONT && class_is_container(cls)) {
                        Cont *c = &g_lw_cont[g_lw_ncont++];
                        c->obj = obj; c->cls = cls; c->x = c->y = c->z = 0.0; c->open = 0; c->open_next = 0;
                    }
                }
            }
            if ((g_lw_i & 127) == 0 && now_ms() - t0 >= 1) { g_lw_i++; return 0; }
        }
        g_lw_i = 0; g_lw_li++;
    }
    return 1;
}
// explicit commands (chest tour, nearest chest, aura, goto cannon) need the result NOW: drain the walk,
// with a hard cap so even a huge level list cannot freeze the game for more than a few frames
static int level_walk_now(int containers, int max_ms) {
    if (g_scan_active) g_scan_active = 0;                          // this command wins over a background walk
    if (!level_walk_start(containers)) return 0;
    uint64_t t0 = now_ms();
    while (g_lw_active) {
        if (level_walk_step()) break;
        if (now_ms() - t0 >= (uint64_t)max_ms) break;
    }
    level_walk_finish();                                           // publish complete OR partial, never nothing
    return 1;
}
static void scan_world(int force) {
    uint64_t t = now_ms();
    g_fast_gen++;                                       // a new ESP walk: verify ranges again
    if (g_scan_active) { if (scan_step()) scan_publish(force); return; }
    if (g_lw_active) return;                            // the level walk is running: pump it from esp_tick
    if (!force) {
        // v130: players come from GameState.PlayerArray and never need a walk at all. Cannons/vehicles/chests
        // are in the level actor lists, so even those only need a (cheap) refresh now and then.
        if (!walk_wanted()) return;
        uint64_t every = (g_esp || g_esp_veh) ? 10000ULL : 60000ULL;
        if (g_cann_scan && t - g_cann_scan < every) return;
    }
    if (g_lw_fail < 2 && level_walk_start(0)) {         // preferred path: no object-table walk
        if (force) {                                    // a command asked for fresh lists: finish the walk here
            uint64_t t0 = now_ms();
            while (g_lw_active) {
                if (level_walk_step()) break;
                if (now_ms() - t0 >= 50) break;
            }
            level_walk_finish();                        // publish complete OR partial
        }
        return;
    }
    if (!g_cann_cls && g_pc) g_cann_cls = class_in_chain_named(frd64(g_pc + OBJ_CLASS), "Actor");
    if (!g_cann_cls) return;                                           // controller not up yet: next tick
    if (!g_ps_cls && g_pc) {                                           // APlayerState base class
        uint64_t lps = frd64(g_pc + 0x270);
        if (plausible(lps)) g_ps_cls = class_in_chain_named(frd64(lps + OBJ_CLASS), "PlayerState");
        if (g_ps_cls) logf_("esp: player state base class %lx", g_ps_cls);
    }
    uint64_t chunks = frd64(A_GOBJECTS);
    if (!plausible(chunks)) return;
    g_scan_chunks = chunks; g_scan_ci = 0; g_scan_i = 0;
    g_sc_nc = g_sc_nv = g_sc_nps = 0;
    g_scan_active = 1;
    if (scan_step()) scan_publish(force);
}
// ProcessEvent helper: call a no-arg UFunction whose return value is the first thing in the param block
static int pe_vec3(uint64_t self, uint64_t fn, double *o) {
    if (!fn || !plausible(self)) return 0;
    uint8_t p[0x30] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    call_pe2(self, fn, p);
    o[0] = *(double *)(p + 0x00); o[1] = *(double *)(p + 0x08); o[2] = *(double *)(p + 0x10);
    return o[0] == o[0] && o[1] == o[1] && o[2] == o[2];
}
static int fn_matches(uint64_t fn, uint64_t obj) {                 // is this UFunction on obj's class?
    if (!fn || !obj) return 0;
    uint64_t fo = frd64(fn + OBJ_OUTER), oc = frd64(obj + OBJ_CLASS);
    if (!plausible(fo) || !plausible(oc)) return 0;
    return fo == oc || is_sub_of(oc, fo);
}
// ---- player ESP: APlayerState name + IsABot, position from the live pawn -------------------------------
// The list is merged from TWO sources: the GameState PlayerArray (always current) and the APlayerState
// objects cached by the GObjects walk (works even when the world chain is unreadable). Names and bot flags
// are read once per state; the POSITION is re-read from the pawn on every ESP tick, so boxes track moving
// players instead of stepping at 2.5 Hz.
static uint64_t g_fn_isbot = 0, g_fn_ps_pawn = 0;
static int g_isbot_try = 0, g_ps_pawn_try = 0;
static uint64_t g_ps_list[128]; static int g_nps_list = 0;
// FString { TCHAR* Data; int32 Num; int32 Max } -> ascii
static int read_fstring(uint64_t base, char *out, int max) {
    if (!readable(base, 16)) return 0;
    uint64_t d = rd64(base);
    int32_t num = *(volatile int32_t *)(base + 8);
    if (!plausible(d) || num < 1 || num > 48) return 0;
    if (!readable(d, (uint64_t)num * 2)) return 0;
    int n = 0;
    for (int i = 0; i < num - 1 && n < max - 1; i++) {
        uint16_t c = *(volatile uint16_t *)(d + (uint64_t)i * 2);
        if (c < 32 || c > 126) continue;                 // non-ascii / emoji: dropped
        out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}
// APlayerState::IsABot (native UFunction, resolved by name and verified against the object's class)
static int player_is_bot(uint64_t ps) {
    if (!g_fn_isbot && g_isbot_try < 3) {
        g_isbot_try++;
        g_fn_isbot = find_object_by_text("IsABot", "Function");
        logf_("esp: IsABot %lx (%s)", g_fn_isbot, g_fn_isbot ? "resolved" : "retrying");
    }
    if (!g_fn_isbot || !fn_matches(g_fn_isbot, ps)) return -1;
    uint8_t p[16] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    call_pe2(ps, g_fn_isbot, p);
    return p[0] ? 1 : 0;
}
// APlayerState.GetPawn: fallback when PawnPrivate has not been filled
static uint64_t player_pawn_pe(uint64_t ps) {
    if (!g_fn_ps_pawn && g_ps_pawn_try < 3) {
        g_ps_pawn_try++;
        g_fn_ps_pawn = find_object_by_text("GetPawn", "Function");
        logf_("esp: APlayerState.GetPawn %lx (%s)", g_fn_ps_pawn, g_fn_ps_pawn ? "resolved" : "retrying");
    }
    if (!g_fn_ps_pawn || !fn_matches(g_fn_ps_pawn, ps)) return 0;
    uint8_t p[16] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    call_pe2(ps, g_fn_ps_pawn, p);
    return *(uint64_t *)p;
}
// ---- v81: pawn health / death + skeleton ----------------------------------------------------------------
// Health and shield live in AFortPawn::HealthSet (0x1400). Every attribute of UFortHealthSet is a
// FFortGameplayAttributeData: BaseValue at +0x08 and CurrentValue at +0x0c (FFortGameplayAttributeData
// appends Minimum/Maximum/... after that base struct). Reading the set is two ordinary memory loads, so
// every tracked player's health can be polled on every tick without a single engine call.
// GetHealth/GetMaxHealth/GetShield/GetMaxShield/IsDead stay as the fallback for pawns whose attribute set
// is not readable yet (they are native UFunctions, resolved by name once per match).
#define ACTOR_MESH        0x2e8                  // ACharacter.Mesh (USkeletalMeshComponent*)
#define PAWN_HEALTHSET    0x1400                 // AFortPawn.HealthSet (UFortHealthSet*)
// AFortPlayerStateZone (the base of AFortPlayerStateAthena) keeps a REPLICATED copy of health/shield.
// This is the only place enemy health actually exists on a client of this title; the pawn HealthSet is 0.
#define PS_CURHEALTH      0xcec                  // 4-byte float, CPF_Net
#define PS_MAXHEALTH      0xcf0
#define PS_CURSHIELD      0xcf4
#define PS_MAXSHIELD      0xcf8
#define HS_HEALTH         0x48                   // UFortHealthSet.Health
#define HS_MAXHEALTH      0x70                   // UFortHealthSet.MaxHealth
#define HS_SHIELD         0x98                   // UFortHealthSet.Shield
#define HS_CURSHIELD      0xc0                   // UFortHealthSet.CurrentShield
#define ATTR_CUR          0x0c                   // FGameplayAttributeData.CurrentValue
enum { PFN_HP, PFN_HPMAX, PFN_SH, PFN_SHMAX, PFN_DEAD, PFN_N };
static uint64_t g_pfn[PFN_N];
static const char *const PFN_NAMES[PFN_N] = { "GetHealth", "GetMaxHealth", "GetShield", "GetMaxShield", "IsDead" };
static uint64_t g_fn_socket = 0;                  // USceneComponent::GetSocketLocation(FName) -> FVector
static uint64_t g_fn_bone = 0;                    // USkinnedMeshComponent::GetBoneLocationByName(FName, EBoneSpaces)
static int g_dbg_bone = 0, g_dbg_hp = 0, g_dbg_hp2 = 0, g_dbg_ps = 0;
static int g_fns_tried = 0;
static uint64_t g_bone_idx[8];                    // FName index per skeleton point (0 = not in this title)
static int g_bone_lay = -1;                       // parameter layout that validated against the pawn (-1 = untested)
static int g_probe_tries = 0;                     // bounded layout probes per match
static const char *const BONE_NAMES[8][3] = {     // first name that exists in the pool is used
    { "head", 0, 0 }, { "neck_01", "neck", 0 }, { "spine_03", "spine_02", "spine_01" }, { "pelvis", "hips", 0 },
    { "hand_l", "lowerarm_l", "upperarm_l" }, { "hand_r", "lowerarm_r", "upperarm_r" },
    { "foot_l", "calf_l", "thigh_l" }, { "foot_r", "calf_r", "thigh_r" },
};
static float f32_at(uint64_t a) { return fast_ok(a, 4) ? *(volatile float *)a : 0.0f; }
// Health/shield from AFortPlayerStateZone (the field the game's GetHealthAndShields reads). It is marked
// replicated in the SDK, so it is tried first; on this BR title it reads 0.0/0.0 for every player, in which
// case the caller falls back to the pawn set and finally to the damage-tracked estimate.
static int ps_health(uint64_t ps, float *hp, float *hpmax, float *sh, float *shmax) {
    *sh = *shmax = 0.0f;
    if (!plausible(ps) || !fast_ok(ps + PS_CURHEALTH, 0x10)) return 0;
    float h = f32_at(ps + PS_CURHEALTH), m = f32_at(ps + PS_MAXHEALTH);
    float cs = f32_at(ps + PS_CURSHIELD), ms = f32_at(ps + PS_MAXSHIELD);
    if (g_dbg_ps < 10) {
        g_dbg_ps++;
        logf_("esp: ps health raw %.1f/%.1f sh %.1f/%.1f (ps %lx)", h, m, cs, ms, ps);
    }
    if (!(h == h && m == m && cs == cs && ms == ms)) return 0;
    if (!(m >= 1.0f && m <= 20000.0f)) return 0;
    if (!(h >= -1.0f && h <= m * 2.0f)) return 0;
    if (h < 0.0f) h = 0.0f;
    *hp = h; *hpmax = m;
    if (cs >= 0.0f && cs <= 20000.0f) {
        *sh = cs;
        *shmax = (ms >= 1.0f && ms <= 20000.0f) ? ms : (cs > 0.5f ? 100.0f : 0.0f);
    }
    return 1;
}
// health + shield straight out of the attribute set. Returns 0 when the set is missing or reads as garbage,
// so the caller can fall back instead of drawing a wrong (possibly lethal-looking) value.
static int pawn_health(uint64_t pawn, float *hp, float *hpmax, float *sh, float *shmax) {
    *sh = *shmax = 0.0f;
    uint64_t hs = frd64(pawn + PAWN_HEALTHSET);
    if (!plausible(hs) || !fast_ok(hs + HS_HEALTH, 0xc8)) return 0;
    float h = f32_at(hs + HS_HEALTH + ATTR_CUR), m = f32_at(hs + HS_MAXHEALTH + ATTR_CUR);
    // A set can replicate Health while MaxHealth is still 0 (attribute sets are conditional): a sane
    // positive Health then means "full bar of the usual 100" instead of dropping the bar entirely.
    if (!(h == h && m == m)) return 0;
    if (m < 1.0f && h > 0.5f && h <= 1000.0f) m = 100.0f;
    if (m < 1.0f || m > 1.0e5f || h < 0.0f || h > m * 4.0f) return 0;
    *hp = h; *hpmax = m;
    float cs = f32_at(hs + HS_CURSHIELD + ATTR_CUR), ms = f32_at(hs + HS_SHIELD + ATTR_CUR);
    if (cs == cs && cs >= 0.0f && cs <= 1.0e4f && cs > 0.0f) {
        *sh = cs;
        *shmax = (ms == ms && ms >= 1.0f && ms <= 1.0e4f) ? ms : (cs > 100.0f ? cs : 100.0f);
    }
    return 1;
}
// first UFunction per name that can be called on cls (declared on cls or on one of its bases)
static int find_methods_on(uint64_t cls, const char *const *names, uint64_t *out, int n) {
    if (!plausible(cls)) return 0;
    uint32_t idx[8]; int want = 0;
    for (int i = 0; i < n && i < 8; i++) { idx[i] = find_fname_index(names[i]); if (idx[i]) want++; }
    if (!want) return 0;
    uint64_t chunks = rd64(A_GOBJECTS);
    if (!plausible(chunks)) return 0;
    int found = 0;
    for (int ci = 0; ci < 64; ci++) {
        uint64_t chunk = rd64(chunks + ci * 8);
        if (!plausible(chunk) || !readable(chunk, 65536ULL * 24)) break;
        for (int i = 0; i < 65536; i++) {
            const uint8_t *it = (const uint8_t *)(chunk + (uint64_t)i * 24);
            uint64_t q0 = *(const uint64_t *)it;
            if (!q0 || (q0 & 0x1020000000000000ULL)) continue;
            uint64_t o = ((q0 & 0x3fff00000000ULL) | *(const uint32_t *)(it + 8)) << 3;
            if (!plausible(o) || !fast_ok(o + OBJ_NAME, 8)) continue;
            uint32_t nm = (uint32_t)frd64(o + OBJ_NAME);
            for (int k = 0; k < n && k < 8; k++) {
                if (!idx[k] || nm != idx[k] || out[k]) continue;
                if (!obj_is_class(o, "Function")) continue;
                uint64_t outer = frd64(o + OBJ_OUTER);
                if (plausible(outer) && is_sub_of(cls, outer)) { out[k] = o; found++; }
            }
        }
    }
    return found;
}
static int resolve_esp_fns(uint64_t pawn) {
    static uint64_t next = 0;
    static int mesh_tried = 0;
    uint64_t t = now_ms();
    if (t < next) return 0;
    next = t + 3000;
    if (!plausible(pawn)) return 0;
    // one GObjects walk per group, at most 4 times each: a walk is ~1 s of game-thread time, so it must
    // never repeat for a name that simply does not exist in this title
    if (!g_pfn[PFN_HP] && g_fns_tried < 4) {
        g_fns_tried++;
        uint64_t cls = frd64(pawn + OBJ_CLASS);
        find_methods_on(cls, PFN_NAMES, g_pfn, PFN_N);
    }
    uint64_t mesh = frd64(pawn + ACTOR_MESH);
    if (plausible(mesh) && !g_fn_socket && !g_fn_bone && mesh_tried < 4) {
        mesh_tried++;
        uint64_t mcls = frd64(mesh + OBJ_CLASS);
        const char *sn[2] = { "GetBoneLocationByName", "GetSocketLocation" };
        uint64_t so[2] = { 0, 0 };
        find_methods_on(mcls, sn, so, 2);
        if (so[0]) g_fn_bone = so[0];
        if (so[1]) g_fn_socket = so[1];
    }
    for (int k = 0; k < 8; k++) if (!g_bone_idx[k]) {
        for (int j = 0; j < 3 && BONE_NAMES[k][j]; j++) {
            uint32_t bi = find_fname_index(BONE_NAMES[k][j]);
            if (bi) { g_bone_idx[k] = bi; break; }
        }
    }
    logf_("esp: pfns hp %lx max %lx sh %lx shmax %lx dead %lx | socket %lx bone %lx | bones %lu %lu %lu %lu %lu %lu %lu %lu",
          g_pfn[PFN_HP], g_pfn[PFN_HPMAX], g_pfn[PFN_SH], g_pfn[PFN_SHMAX], g_pfn[PFN_DEAD], g_fn_socket, g_fn_bone,
          (unsigned long)g_bone_idx[0], (unsigned long)g_bone_idx[1], (unsigned long)g_bone_idx[2], (unsigned long)g_bone_idx[3],
          (unsigned long)g_bone_idx[4], (unsigned long)g_bone_idx[5], (unsigned long)g_bone_idx[6], (unsigned long)g_bone_idx[7]);
    return 1;
}
// A no-argument scalar getter returns its value in the first bytes of the parameter block. The position of
// the float is still probed once, because a double return (large-world builds) would otherwise read as
// nonsense: layout 0 = f32 @0, 1 = f32 @4, 2 = f64 @0, 3 = f64 @8.
static int g_hp_layout = -1;
static int hp_parse(const uint8_t *p, int lay, float *out) {
    if (lay == 0) { float f; memcpy(&f, p, 4); *out = f; }
    else if (lay == 1) { float f; memcpy(&f, p + 4, 4); *out = f; }
    else if (lay == 2) { double d; memcpy(&d, p, 8); *out = (float)d; }
    else { double d; memcpy(&d, p + 8, 8); *out = (float)d; }
    return (*out == *out);
}
static int pe_call(uint64_t self, uint64_t fn, uint8_t *p, int sz) {
    if (!fn || !plausible(self) || !fn_matches(fn, self)) return 0;
    memset(p, 0, (size_t)sz);
    call_pe2(self, fn, p);
    return 1;
}
// -1 = cannot tell, 0 = alive, 1 = dead
static int pe_dead(uint64_t self) {
    if (!g_pfn[PFN_DEAD]) return -1;
    uint8_t p[0x10] __attribute__((aligned(16)));
    if (!pe_call(self, g_pfn[PFN_DEAD], p, sizeof p)) return -1;
    return p[0] ? 1 : 0;
}
// ---- skeleton -------------------------------------------------------------------------------------------
// Parameter-block layouts for "FName in, FVector out". UHT puts the return value last, so
// GetSocketLocation(FName) is {FName @0x00, FVector @0x08} and GetBoneLocationByName(FName, EBoneSpaces) is
// {FName @0x00, byte @0x08, FVector @0x10}. The other two entries cover the reversed order, and which one
// is right is decided by physics (both points must sit on the pawn) rather than by trusting the dump.
typedef struct { int bone; int fname_off; int ret_off; } BLay;
static const BLay BONE_LAYS[] = {
    { 0, 0x00, 0x08 }, { 1, 0x00, 0x10 }, { 0, 0x00, 0x10 }, { 1, 0x00, 0x08 },
    { 0, 0x18, 0x00 }, { 1, 0x18, 0x00 },
};
#define N_BONE_LAYS ((int)(sizeof BONE_LAYS / sizeof BONE_LAYS[0]))
static int pe_bone_lay(uint64_t mesh, uint32_t bone, int lay, double *o) {
    if (!bone || lay < 0 || lay >= N_BONE_LAYS || !plausible(mesh)) return 0;
    const BLay *L = &BONE_LAYS[lay];
    uint64_t fn = L->bone ? g_fn_bone : g_fn_socket;
    if (!fn || !fn_matches(fn, mesh)) return 0;
    uint8_t p[0x40] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    *(uint32_t *)(p + L->fname_off) = bone;           // FName: pool index in the low dword, number 0
    call_pe2(mesh, fn, p);
    const double *v = (const double *)(p + L->ret_off);
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return 0;
    o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
    return 1;
}
static int near_point(const double *b, const double *pos, double r) {
    double dx = b[0] - pos[0], dy = b[1] - pos[1], dz = b[2] - pos[2];
    return dx*dx + dy*dy + dz*dz <= r * r;
}
// try every layout once, keep the first one whose head and foot both land on this pawn and are apart
static void probe_bone_layout(uint64_t mesh, const double *pos) {
    double a[3] = { 0, 0, 0 }, b[3] = { 0, 0, 0 };
    g_bone_lay = -1;
    for (int lay = 0; lay < N_BONE_LAYS; lay++) {
        int oka = pe_bone_lay(mesh, (uint32_t)g_bone_idx[0], lay, a);
        int okb = pe_bone_lay(mesh, (uint32_t)g_bone_idx[7], lay, b);
        double dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
        int good = oka && okb && near_point(a, pos, 300.0) && near_point(b, pos, 300.0) &&
                   (dx*dx + dy*dy + dz*dz) > 400.0;
        if (g_dbg_bone < 8) {
            g_dbg_bone++;
            logf_("esp: bone layout %d (%s fname@%x ret@%x): head %.0f %.0f %.0f foot %.0f %.0f %.0f call %d/%d -> %s",
                  lay, BONE_LAYS[lay].bone ? "bone" : "socket", BONE_LAYS[lay].fname_off, BONE_LAYS[lay].ret_off,
                  a[0], a[1], a[2], b[0], b[1], b[2], oka, okb, good ? "USE" : "no");
        }
        if (good) { g_bone_lay = lay; return; }
    }
}

// GameState.PlayerArray + the walked states, deduped
static void ps_collect(void) {
    g_nps_list = 0;
    int from_array = 0;
    if (g_pc) {
        uint64_t lvl = frd64(g_pc + OBJ_OUTER);            // AActor.Outer = the ULevel
        if (plausible(lvl)) {
            uint64_t w = frd64(lvl + 0xc8);                // ULevel.OwningWorld
            if (plausible(w)) {
                uint64_t gs = frd64(w + 0x1a8);            // UWorld.GameState
                if (plausible(gs) && readable(gs + 0x280, 16)) {
                    uint64_t arr = frd64(gs + 0x280);      // AGameStateBase.PlayerArray
                    int32_t num = *(volatile int32_t *)(gs + 0x288);
                    if (plausible(arr) && num >= 0 && num <= 128 && readable(arr, (uint64_t)num * 8)) {
                        from_array = 1;
                        for (int i = 0; i < num && g_nps_list < 128; i++) {
                            uint64_t ps = rd64(arr + (uint64_t)i * 8);
                            if (!plausible(ps)) continue;
                            int dup = 0;
                            for (int k = 0; k < g_nps_list; k++) if (g_ps_list[k] == ps) { dup = 1; break; }
                            if (!dup) g_ps_list[g_nps_list++] = ps;
                        }
                    }
                }
            }
        }
    }
    if (from_array) return;                                // authoritative list
    // array unreadable: fall back to the states the last GObjects walk found
    for (int i = 0; i < g_nps_cache && g_nps_list < 128; i++) g_ps_list[g_nps_list++] = g_ps_cache[i];
}
static void players_refresh(void) {
    static uint64_t last = 0;
    uint64_t t = now_ms();
    if (t - last < 250) return;
    last = t;
    ps_collect();
    uint64_t myps = g_pc ? frd64(g_pc + 0x270) : 0;        // AController.PlayerState (local player)
    Plr out[MAX_PLR]; int n = 0;
    for (int i = 0; i < g_nps_list && n < MAX_PLR; i++) {
        uint64_t ps = g_ps_list[i];
        // a PlayerState from the previous match passes actor_alive but its class chain is gone: require the
        // full chain before any ProcessEvent, otherwise IsABot/GetPawn can call into freed memory
        if (!plausible(ps)) continue;
        if (g_ps_cls) { if (!(cls_kind(frd64(ps + OBJ_CLASS)) & 4)) continue; }
        else if (!actor_alive(ps)) continue;
        Plr e; memset(&e, 0, sizeof e); e.ps = ps;
        for (int k = 0; k < g_nplr; k++) if (g_plr[k].ps == ps) { e = g_plr[k]; e.ps = ps; break; }
        if (!e.named && read_fstring(ps + 0x2f8, e.name, sizeof e.name) > 0) e.named = 1;
        if (!e.bot_known) { int bo = player_is_bot(ps); if (bo >= 0) { e.bot = bo; e.bot_known = 1; } }
        if (!e.bot_known && e.named && strstri(e.name, "bot")) { e.bot = 1; e.bot_known = 1; }
        e.local = (ps == myps);
        e.seen = t;                                        // this state is listed right now
        if (!e.have) {                                     // first sight: resolve the pawn right here
            uint64_t pw = rd64(ps + 0x2e0);                // APlayerState.PawnPrivate
            if (!actor_alive(pw)) pw = player_pawn_pe(ps);
            if (actor_alive(pw)) {
                double v[3]; actor_pos(pw, v);
                if (in_world(v[0], v[1], v[2])) { e.pawn = pw; e.x = v[0]; e.y = v[1]; e.z = v[2]; e.have = 1; e.alive = 1; }
            }
        }
        // v81: a PlayerState stays in PlayerArray after the player dies, so an entry that keeps its old
        // position would keep its box forever. The pawn is the only thing that says "still in the match".
        if (e.have && (!actor_alive(e.pawn))) { e.have = 0; e.alive = 0; e.bones_ok = 0; e.hpk = 0; }
        if (!e.have) {                                     // no pawn yet: keep the entry, it appears as soon as one does
            out[n++] = e;
            continue;
        }
        if (!e.logged && g_plr_logs < 12) {
            e.logged = 1; g_plr_logs++;
            logf_("esp: player '%s' %s ps %lx pawn %lx at %.0f %.0f %.0f", e.named ? e.name : "?",
                  !e.bot_known ? "?" : (e.bot ? "BOT" : "PLAYER"), ps, e.pawn, e.x, e.y, e.z);
        }
        out[n++] = e;
    }
    memcpy(g_plr, out, sizeof(Plr) * (size_t)n); g_nplr = n;
}
// ---- tower: nearest-first list of live players, then the tour engine flies it --------------------
// Positions are refreshed right before the list is built, and every leg re-reads its player again (see
// tour_tick phase 0), so a moving target is always above its CURRENT position. g_tour[] holds the landing
// point in the tour's own convention: p[2] + TP_LIFT is where the pawn ends up, so a player at z lands at
// z + g_tower_h.
static void tower_build(uint64_t pawn) {
    players_refresh(); players_positions();
    double cur[3] = { g_thome[0], g_thome[1], g_thome[2] };
    if (g_flypos_ok) { cur[0] = g_flypos[0]; cur[1] = g_flypos[1]; cur[2] = g_flypos[2]; }
    else if (pawn) {
        double v[3];
        if (get_vec(pawn, A_FN_GETLOC, v) && in_world(v[0], v[1], v[2])) { cur[0] = v[0]; cur[1] = v[1]; cur[2] = v[2]; }
    }
    char used[128]; memset(used, 0, sizeof used);
    int n = 0;
    while (n < TOWER_MAX) {
        int b = -1; double bd = 1e30;
        for (int k = 0; k < g_nplr && k < (int)sizeof used; k++) {
            Plr *e = &g_plr[k];
            if (used[k] || !e->have || !e->alive || e->local) continue;
            if (!in_world(e->x, e->y, e->z)) continue;
            double dx = e->x - cur[0], dy = e->y - cur[1], dz = e->z - cur[2];
            double d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { bd = d; b = k; }
        }
        if (b < 0) break;
        used[b] = 1;
        Plr *e = &g_plr[b];
        g_tour[n][0] = e->x; g_tour[n][1] = e->y; g_tour[n][2] = e->z + (double)g_tower_h - TP_LIFT;
        g_tobj[n] = e->pawn; g_tcls[n] = 0;
        strncpy(g_tnm[n], e->named ? e->name : "?", sizeof g_tnm[0] - 1);
        g_tnm[n][sizeof g_tnm[0] - 1] = 0;
        cur[0] = e->x; cur[1] = e->y; cur[2] = e->z;
        n++;
    }
    g_tn = n; g_ti = 0; g_tphase = 0; g_topen = 0; g_tskip = 0; g_tauto = n > 0;
}
static void do_tower_start(void) {
    find_super_off();
    fly_gravity(0, 0); fly_nograv_all(1); fly_noclip_all();       // a previous run may have left physics off
    uint64_t pawn = get_pawn(); if (!pawn) { logf_("tower: no pawn"); return; }
    get_vec(pawn, A_FN_GETLOC, g_thome);
    g_have_home = in_world(g_thome[0], g_thome[1], g_thome[2]);
    if (!g_have_home) logf_("tower: home position not readable, the run will not return home");
    g_tret = g_have_home;
    g_have_good = 0; g_track = 0; g_flyn = 0; g_flypos_ok = 0; g_hover_ok = 0;
    g_tmode = 2; g_tower = 1;
    rotation_forget(); g_reseat_n = 0; g_reseat_next = 0;
    tower_build(pawn);
    g_tpawn = pawn; g_tnext = now_ms();
    if (!g_tn) { g_tower = 0; g_tmode = 0; tour_stop_gravity(pawn); logf_("tower: no players in the list yet - start it in a match"); }
    else logf_("tower: %d players, %d uu above, %s mode (%.0f ms at each player)%s", g_tn, g_tower_h,
               g_instant ? "INSTANT teleport" : "continuous flight", g_tstep / 1.0, g_tret ? ", back home" : "");
}
static void do_tower_stop(void) {
    if (!g_tauto) { logf_("tower: not running"); return; }
    g_tower = 0; g_track = 0; g_hover_ok = 0;
    if (g_tret && g_have_home) {                                  // fly (or jump) home like the chest run
        g_ti = g_tn; g_tphase = 4; g_tnext = now_ms();
        logf_("tower: stopping - %s home", g_instant ? "teleporting" : "flying");
        return;
    }
    uint64_t pawn = get_pawn();
    g_tauto = 0; g_ti = g_tn; g_tphase = 0;
    tour_stop_gravity(pawn);
    logf_("tower: stopped");
}
static void esp_on_hit(uint64_t self, uint8_t *params);
// Native hit notification: the UFunction is never called through ProcessEvent on this build, so hook its
// native entry. Signature from the SDK: void(float, bool, bool, bool, AActor*, const FGameplayTagContainer&, FVector).
// On x64: this/rdi, float/xmm0, three bools/rsi-rdx, actor/r8, tags/r9, FVector(3 doubles)/xmm1-xmm3.
static void hn_hook(void *self, float dmg, uint8_t crit, uint8_t fatal, uint8_t shield, uint64_t actor, void *tags, double lx, double ly, double lz) {
    if (g_hn_orig) g_hn_orig(self, dmg, crit, fatal, shield, actor, tags, lx, ly, lz);
    if (g_hitnotify_hits < 5) logf_("esp: native hit notify self %lx actor %lx dmg %.1f shield %d", (uint64_t)self, actor, dmg, shield);
    uint8_t p[0x48]; memset(p, 0, sizeof p);
    *(float *)p = dmg; *(uint64_t *)(p + 8) = actor; p[6] = (uint8_t)shield;
    esp_on_hit((uint64_t)self, p);
}
// Shared estimator: this title never replicates enemy health, so start at 100/100 and apply the damage
// the game itself confirms. shield_first is the normal case (damage hits shield before HP).
static void hp_apply(Plr *p, float dmg, int shield_first) {
    if (!p || !(dmg > 0.0f && dmg < 10000.0f)) return;
    if (!p->hp_init) { p->hp_est = 100.0f; p->sh_est = 100.0f; p->hp_init = 1; }
    if (shield_first && p->sh_est > 0.0f) {
        float left = p->sh_est - dmg;
        if (left >= 0.0f) { p->sh_est = left; return; }
        p->sh_est = 0.0f;
        dmg = -left;
    }
    p->hp_est -= dmg;
    if (p->hp_est < 0.0f) p->hp_est = 0.0f;
}
// OnDisplayHitNotify(float DamageDealt, ..., AActor* HitActor, ..., bool bShieldHit, ...): the SHOOTER's
// pawn receives this when its shot lands. Enemy HealthSet values are 0 on this title, so the only enemy HP
// we can ever know is the damage we ourselves deal. Start everyone at 100 HP / 100 shield and subtract.
static void esp_on_hit(uint64_t self, uint8_t *params) {
    if (!params || !readable((uint64_t)params, 0x48)) return;
    float dmg = *(volatile float *)params;
    if (!(dmg > 0.0f && dmg < 10000.0f)) return;
    uint64_t victim = frd64((uint64_t)params + 8);
    if (!plausible(victim) || victim == get_pawn()) return;
    int shield = params[6] ? 1 : 0;
    for (int i = 0; i < g_nplr; i++) {
        Plr *p = &g_plr[i];
        if (p->pawn != victim || p->local) continue;
        hp_apply(p, dmg, shield);
        if (g_hitnotify_hits++ < 4)
            logf_("esp: hit %.0f%s on '%s' (self %lx) -> est %.0f hp / %.0f sh", dmg, shield ? " shield" : "",
                  p->name, self, p->hp_est, p->sh_est);
        return;
    }
}
// AFortPlayerController::ClientAddDamageNumber(float Damage, AActor* DamagedActor, ...) is the RPC the
// server sends to the shooter's client for every confirmed hit. Unlike OnDisplayHitNotify it is a net
// function, so it always arrives through ProcessEvent on the local controller - that makes it the one
// reliable damage feed on this build. It also gives the exact number, so the bars track real damage.
static void esp_on_damage(uint8_t *params) {
    if (!params || !readable((uint64_t)params, 0x50)) return;
    float dmg = *(volatile float *)params;
    if (!(dmg > 0.0f && dmg < 10000.0f)) return;
    uint64_t victim = frd64((uint64_t)params + 8);
    if (!plausible(victim)) return;
    for (int i = 0; i < g_nplr; i++) {
        Plr *p = &g_plr[i];
        if (p->pawn != victim || p->local) continue;
        hp_apply(p, dmg, 1);
        if (g_dmg_hits++ < 6)
            logf_("esp: damage %.0f on '%s' -> est %.0f hp / %.0f sh", dmg, p->name, p->hp_est, p->sh_est);
        return;
    }
}
// v130: one skeleton fetch = 8 ProcessEvent calls. Only the eight nearest players get one, at 20 Hz; the
// old code refreshed every player within 90 m at 10 Hz, which is thousands of calls per second.
static void bones_update(Plr *p, uint64_t t) {
    p->bone_next = t + 50;
    uint64_t mesh = frd64(p->pawn + ACTOR_MESH);
    unsigned ok = 0;
    if (plausible(mesh)) {
        if (g_bone_lay < 0 && g_probe_tries < 4) {
            double me[3] = { p->x, p->y, p->z };
            g_probe_tries++;
            probe_bone_layout(mesh, me);
        }
        if (g_bone_lay >= 0) {
            for (int k = 0; k < 8; k++) {
                double b[3];
                if (!g_bone_idx[k] || !pe_bone_lay(mesh, (uint32_t)g_bone_idx[k], g_bone_lay, b)) continue;
                double dx = b[0] - p->x, dy = b[1] - p->y, dz = b[2] - p->z;
                if (dx*dx + dy*dy + dz*dz > 300.0*300.0) continue;   // not this pawn's skeleton
                p->bone[k][0] = b[0]; p->bone[k][1] = b[1]; p->bone[k][2] = b[2];
                ok |= 1u << k;
            }
        }
    }
    p->bones_ok = ok;
}
// positions on every ESP tick (memory reads only), so the boxes follow moving players
static void players_positions(void) {
    static uint64_t last = 0, pe_next = 0;
    uint64_t t = now_ms();
    if (t - last < 10) return;                                  // v130: 100 Hz, one sample per render frame
    last = t;
    for (int i = 0; i < g_nplr; i++) {
        Plr *p = &g_plr[i];
        if (!p->seen || t - p->seen > 1200) continue;       // not in the current player list: do not touch it
        uint64_t pw = frd64(p->ps + 0x2e0);
        if (!actor_alive(pw) && t >= pe_next) { pw = player_pawn_pe(p->ps); pe_next = t + 300; }
        if (!actor_alive(pw)) {                             // pawn gone (death / respawn): drop the box NOW
            p->have = 0; p->alive = 0; p->bones_ok = 0; p->vis_ok = 0; p->d2 = 0.0f;
            continue;
        }
        if (p->pawn && p->pawn != pw) {                    // new pawn (respawn): drop the old estimate
            p->hp_init = 0; p->hp_est = 100.0f; p->sh_est = 100.0f; p->vis_ok = 0;
        }
        double v[3]; actor_pos(pw, v);
        if (!in_world(v[0], v[1], v[2])) continue;
        p->pawn = pw; p->x = v[0]; p->y = v[1]; p->z = v[2]; p->have = 1; p->seen = t;
        // v81: health + shield. The attribute set costs two memory reads, so a nearby player's health is
        // exact on every tick. Distant players are skipped - nothing is drawn there and it keeps the
        // ProcessEvent fallback cheap. GetHealth/IsDead are only used when the set is not readable.
        double ddx = p->x - g_esp_me[0], ddy = p->y - g_esp_me[1], ddz = p->z - g_esp_me[2];
        double far2 = ddx*ddx + ddy*ddy + ddz*ddz;
        p->d2 = (float)far2;
        if (g_esp_me_ok && far2 > 20000.0 * 20000.0) { p->hpk = 0; p->bones_ok = 0; p->alive = 1; continue; }
        if (p->hp_next <= t) {
            p->hp_next = t + 150;
            float hp = 0, hpmax = 0, sh = 0, shmax = 0;
            int got = ps_health(p->ps, &hp, &hpmax, &sh, &shmax);   // replicated: the real value
            if (!got) got = pawn_health(pw, &hp, &hpmax, &sh, &shmax);
            if (!got && !p->local && g_dbg_hp < 9 && g_dbg_hp2 < 3) {
                g_dbg_hp2++;
                uint64_t hs = frd64(pw + PAWN_HEALTHSET);
                logf_("esp: hp unavailable '%s' (ps %.1f/%.1f, pawn hs %lx readable %d)",
                      p->name, f32_at(p->ps + PS_CURHEALTH), f32_at(p->ps + PS_MAXHEALTH),
                      hs, fast_ok(hs + HS_HEALTH, 0xc8));
            }
            if (got && g_dbg_hp < 2) {
                g_dbg_hp++;
                uint64_t hs = frd64(pw + PAWN_HEALTHSET);
                logf_("esp: health set %lx -> %.1f/%.1f shield %.1f/%.1f (raw %.1f/%.1f)",
                      hs, hp, hpmax, sh, shmax, f32_at(hs + HS_HEALTH + ATTR_CUR), f32_at(hs + HS_MAXHEALTH + ATTR_CUR));
            }
            if (!got && g_pfn[PFN_HP] && fn_matches(g_pfn[PFN_HP], pw)) {
                uint8_t hr[0x10] __attribute__((aligned(16))), mr[0x10] __attribute__((aligned(16)));
                int hok = pe_call(pw, g_pfn[PFN_HP], hr, sizeof hr);
                int mok = pe_call(pw, g_pfn[PFN_HPMAX], mr, sizeof mr);
                if (hok && mok) {
                    if (g_hp_layout < 0) {
                        for (int L = 0; L < 4; L++) {
                            float h, m;
                            if (hp_parse(hr, L, &h) && hp_parse(mr, L, &m) && m >= 1.0f && h >= -1.0f && h <= m * 3.0f) { g_hp_layout = L; break; }
                        }
                        if (g_dbg_hp < 3) {
                            g_dbg_hp++;
                            float h0, h4, m0, m4;
                            memcpy(&h0, hr, 4); memcpy(&h4, hr + 4, 4); memcpy(&m0, mr, 4); memcpy(&m4, mr + 4, 4);
                            logf_("esp: health fns hp@0 %.1f hp@4 %.1f max@0 %.1f max@4 %.1f -> layout %d", h0, h4, m0, m4, g_hp_layout);
                        }
                    }
                    if (g_hp_layout >= 0) {
                        hp_parse(hr, g_hp_layout, &hp); hp_parse(mr, g_hp_layout, &hpmax);
                        got = 1;
                        uint8_t sr[0x10] __attribute__((aligned(16))), xr[0x10] __attribute__((aligned(16)));
                        if (pe_call(pw, g_pfn[PFN_SH], sr, sizeof sr) && pe_call(pw, g_pfn[PFN_SHMAX], xr, sizeof xr) &&
                            hp_parse(sr, g_hp_layout, &sh) && hp_parse(xr, g_hp_layout, &shmax) && shmax >= 1.0f) {}
                        else { sh = 0; shmax = 0; }
                    }
                }
            }
            if (!got && g_esp_hp) {
                if (!p->hp_init) { p->hp_est = 100.0f; p->sh_est = 100.0f; p->hp_init = 1; }
                p->hp = p->hp_est; p->hpmax = 100.0f;
                p->sh = p->sh_est; p->shmax = p->sh_est > 0.0f ? 100.0f : 0.0f;
                p->hpk = 1; p->hp_guess = 1;
            } else p->hp_guess = 0;
            if (got) {
                p->hp = hp; p->hpmax = hpmax; p->sh = sh; p->shmax = shmax; p->hpk = 1;
                if (hp > 0.5f) { p->alive = 1; p->hp_live = 1; }
                // Only a pawn that HAS reported a real HP value can be declared dead by a 0 reading. If the
                // title does not replicate enemy health (it reads 0 for everyone), every box would otherwise
                // vanish at once - the pawn check above is the authority in that case.
                else if (p->hp_live) { p->alive = 0; p->have = 0; p->bones_ok = 0; continue; }
            } else {
                p->alive = 1;
                if (p->hp_live && pe_dead(pw) == 1) { p->alive = 0; p->have = 0; p->bones_ok = 0; continue; }
            }
        }
    }
    // v130: skeletons for the eight nearest players (nearest first), each at 20 Hz
    if (g_esp_skel && (g_fn_bone || g_fn_socket)) {
        int pick[8], np = 0;
        for (int i = 0; i < g_nplr; i++) {
            Plr *p = &g_plr[i];
            if (!p->have || !p->alive || p->local) continue;
            if (!(p->d2 > 0.0f) || p->d2 > 9000.0f * 9000.0f) continue;
            int j = np;
            while (j > 0 && g_plr[pick[j-1]].d2 > p->d2) { if (j < 8) pick[j] = pick[j-1]; j--; }
            if (j < 8) { pick[j] = i; if (np < 8) np++; }
        }
        for (int k = 0; k < np; k++) {
            Plr *p = &g_plr[pick[k]];
            if (t >= p->bone_next) bones_update(p, t);
        }
        if (g_aim_lock_ps) {                                   // the locked target always keeps its skeleton
            for (int i = 0; i < g_nplr; i++) {
                Plr *p = &g_plr[i];
                if (p->ps == g_aim_lock_ps && p->have && p->alive && t >= p->bone_next) bones_update(p, t);
            }
        }
    }
}
// FMinimalViewInfo inside FCameraCacheEntry: POV can start at +0x00 or +0x08 (float TimeStamp first),
// so both offsets are tried. Location 0x00/24, Rotation 0x18/24, FOV 0x30/float.
static int try_pov(uint64_t base, int off, double *loc, double *rot, float *fov) {
    if (!readable(base + off, 0x38)) return 0;
    const volatile uint8_t *b = (const volatile uint8_t *)(base + off);
    double lx = *(const volatile double *)(b + 0x00), ly = *(const volatile double *)(b + 0x08), lz = *(const volatile double *)(b + 0x10);
    double rp = *(const volatile double *)(b + 0x18), ry = *(const volatile double *)(b + 0x20), rr = *(const volatile double *)(b + 0x28);
    float f = *(const volatile float *)(b + 0x30);
    if (!in_world(lx, ly, lz)) return 0;
    if (!(f > 40.0f && f < 130.0f)) return 0;
    if (!(rp > -89.5 && rp < 89.5)) return 0;
    if (!(ry > -720.0 && ry < 720.0)) return 0;
    if (g_esp_me_ok) {                                              // the camera is always close to the pawn
        double dx = lx - g_esp_me[0], dy = ly - g_esp_me[1], dz = lz - g_esp_me[2];
        if (dx*dx + dy*dy + dz*dz > 3000.0*3000.0) return 0;
    }
    loc[0] = lx; loc[1] = ly; loc[2] = lz;
    rot[0] = rp; rot[1] = ry; rot[2] = rr;
    *fov = f;
    return 1;
}
static void camera_update(uint64_t pawn) {
    g_cam_ok = 0;
    const char *src = "none";
    uint64_t pcm = 0;
    if (g_pc && readable(g_pc + 0x320, 8)) pcm = frd64(g_pc + 0x320);

    // 1) direct read of APlayerCameraManager.CameraCachePrivate (0x1550) - no call at all
    if (plausible(pcm) && readable(pcm + 0x1550, 0x60)) {
        for (int off = 0; off <= 0x10 && !g_cam_ok; off += 8) {
            double l[3], r[3]; float f = 0;
            if (try_pov(pcm + 0x1550, off, l, r, &f)) {
                g_cam_loc[0] = l[0]; g_cam_loc[1] = l[1]; g_cam_loc[2] = l[2];
                g_cam_rot[0] = r[0]; g_cam_rot[1] = r[1]; g_cam_rot[2] = r[2];
                g_cam_fov = f; g_cam_ok = 1; g_pcm = pcm; g_pov_off = off; src = off ? "camera cache +8" : "camera cache";
            }
        }
    }
    // 2) GetCameraLocation / GetCameraRotation / GetFOVAngle through ProcessEvent (resolved by name)
    if (!g_cam_ok) {
        if ((!g_fn_camloc || !g_fn_camrot) && g_cam_try < 3 && g_pc) {
            g_cam_try++;
            if (!g_fn_camloc) g_fn_camloc = find_object_by_text("GetCameraLocation", "Function");
            if (!g_fn_camrot) g_fn_camrot = find_object_by_text("GetCameraRotation", "Function");
            if (!g_fn_fov) g_fn_fov = find_object_by_text("GetFOVAngle", "Function");
            logf_("esp: camera fns loc %lx rot %lx fov %lx", g_fn_camloc, g_fn_camrot, g_fn_fov);
        }
        double l[3], r[3];
        if (plausible(pcm) && fn_matches(g_fn_camloc, pcm) && fn_matches(g_fn_camrot, pcm) &&
            pe_vec3(pcm, g_fn_camloc, l) && pe_vec3(pcm, g_fn_camrot, r) && in_world(l[0], l[1], l[2])) {
            if (g_fn_fov && fn_matches(g_fn_fov, pcm)) {
                uint8_t p[0x10] __attribute__((aligned(16))); memset(p, 0, sizeof p);
                call_pe2(pcm, g_fn_fov, p);
                float f = *(float *)p;
                if (f > 30.0f && f < 140.0f) g_cam_fov = f;
            }
            g_cam_loc[0] = l[0]; g_cam_loc[1] = l[1]; g_cam_loc[2] = l[2];
            g_cam_rot[0] = r[0]; g_cam_rot[1] = r[1]; g_cam_rot[2] = r[2];
            g_cam_ok = 1; src = "camera fns";
        }
    }
    // 3) last resort: pawn position + pawn rotation
    if (!g_cam_ok && pawn) {
        get_vec(pawn, A_FN_GETLOC, g_cam_loc);
        get_vec(pawn, A_FN_GETROT, g_cam_rot);
        g_cam_ok = in_world(g_cam_loc[0], g_cam_loc[1], g_cam_loc[2]);
        src = "pawn";
    }
    if (!g_cam_logged && g_cam_ok && pawn) {
        g_cam_logged = 1;
        double dx = g_cam_loc[0] - g_esp_me[0], dy = g_cam_loc[1] - g_esp_me[1], dz = g_cam_loc[2] - g_esp_me[2];
        logf_("esp: camera %.0f %.0f %.0f yaw %.0f pitch %.0f fov %.0f (%s, %.0f uu from pawn)",
              g_cam_loc[0], g_cam_loc[1], g_cam_loc[2], g_cam_rot[1], g_cam_rot[0], g_cam_fov, src,
              __builtin_sqrt(dx*dx + dy*dy + dz*dz));
    }
}
// v130: the camera basis (forward/right/up + focal scale) is computed once per frame, not once per point.
// Skeletons used to cost 8 projections x 5 trig calls = 40 transcendentals per player per frame.
static double g_cb_f[3], g_cb_r[3], g_cb_u[3], g_cb_k;
static int g_cb_w = 0, g_cb_h = 0, g_cb_ok = 0;
static void cam_basis(int w, int h) {
    const double R = 0.017453292519943295;
    double yaw = g_cam_rot[1] * R, pitch = g_cam_rot[0] * R;
    double cy = __builtin_cos(yaw), sy = __builtin_sin(yaw);
    double cp = __builtin_cos(pitch), sp = __builtin_sin(pitch);
    g_cb_f[0] = cp * cy;  g_cb_f[1] = cp * sy;  g_cb_f[2] = sp;
    g_cb_r[0] = -sy;      g_cb_r[1] = cy;       g_cb_r[2] = 0.0;
    g_cb_u[0] = -sp * cy; g_cb_u[1] = -sp * sy; g_cb_u[2] = cp;
    double t = __builtin_tan(g_cam_fov * 0.5 * R);
    if (t < 0.2) t = 0.2;
    g_cb_k = (w * 0.5) / t;
    g_cb_w = w; g_cb_h = h; g_cb_ok = 1;
}
static int esp_project(double px, double py, double pz, int w, int h, double *sx, double *sy) {
    if (!g_cam_ok || w < 64 || h < 64) return 0;
    if (!g_cb_ok || g_cb_w != w || g_cb_h != h) cam_basis(w, h);
    double dx = px - g_cam_loc[0], dy = py - g_cam_loc[1], dz = pz - g_cam_loc[2];
    double depth = dx*g_cb_f[0] + dy*g_cb_f[1] + dz*g_cb_f[2];
    if (depth < 50.0) return 0;                                   // behind the camera
    *sx = w * 0.5 + (dx*g_cb_r[0] + dy*g_cb_r[1] + dz*g_cb_r[2]) / depth * g_cb_k;
    *sy = h * 0.5 - (dx*g_cb_u[0] + dy*g_cb_u[1] + dz*g_cb_u[2]) / depth * g_cb_k;
    return 1;
}
static int cam_cache_read(void) {                                 // cheap: only memory reads, safe per frame
    if (!g_pc || !readable(g_pc + 0x320, 8)) return 0;
    uint64_t pcm = frd64(g_pc + 0x320);
    if (!plausible(pcm) || !readable(pcm + 0x1550, 0x60)) return 0;
    for (int off = 0; off <= 0x10; off += 8) {
        double l[3], r[3]; float f = 0;
        if (try_pov(pcm + 0x1550, off, l, r, &f)) {
            g_cam_loc[0] = l[0]; g_cam_loc[1] = l[1]; g_cam_loc[2] = l[2];
            g_cam_rot[0] = r[0]; g_cam_rot[1] = r[1]; g_cam_rot[2] = r[2];
            g_cam_fov = f; g_cam_ok = 1; g_pcm = pcm; g_pov_off = off;
            return 1;
        }
    }
    return 0;
}
static uint64_t g_chest_scan = 0, g_chest_pos = 0;   // chest ESP: last container walk / position refresh
static void hud_forget(void);
static void hud_unpatch_all(void);
static void world_guard(void);
static void esp_flush(const char *why, uint64_t pawn) {
    if (g_esp_world || g_nplr || g_nps_cache || g_ncann || g_nveh)
        logf_("esp: %s (world %lx, pawn %lx) - lists flushed", why, g_esp_world, pawn);
    g_nplr = 0; g_nps_cache = 0; g_ncann = 0; g_nveh = 0;
    g_scan_active = 0; g_cann_scan = 0; g_plr_logs = 0;
    g_chest_scan = 0;
    g_ncont = 0; g_nvis = 0;                         // container / aura lists are actors of the OLD world
    g_aura_n = 0; g_aura_scan = 0; g_aura_retry = 0; g_aura_log = 0;
    g_fn_isbot = 0; g_isbot_try = 0;                 // re-resolve in the new match
    g_fn_ps_pawn = 0; g_ps_pawn_try = 0;
    g_fn_camloc = g_fn_camrot = g_fn_fov = 0; g_cam_try = 0;
    // Health/skeleton UFunctions and bone FNames are objects of the class/name pool: they survive a match,
    // and re-resolving them costs a full GObjects walk (~1 s). Keep them; fn_matches re-verifies every call.
    // Only the per-match skeleton parameter layout is probed again.
    g_bone_lay = -1; g_probe_tries = 0; g_dbg_bone = 0; g_dbg_hp = 0; g_dbg_hp2 = 0; g_dbg_ps = 0;
    g_aim_logged = 0; g_silent_logged = 0; g_silent_bad = 0; g_suc_seen = 0;
    g_silent_ok = 0; g_silent_applied = 0; g_pov_off = -1; g_pcm = 0; g_pad_r2_until = 0;
    g_gpvp_hits = 0; g_hitnotify_hits = 0; g_aim_write_last = 0; g_hp_bar_logged = 0; g_aim_locks = 0;
    g_dmg_tries = 0; g_dmg_hits = 0; g_los_tries = 0; g_los_log = 0;
    g_lw_active = 0; g_lw_fail = 0; g_lw_log = 0; g_lw_ncont = 0; g_lw_li = g_lw_i = 0;
    g_lvl_off = 0; g_lvl_off_world = 0;
    g_silent_sw = 0; g_silent_returning = 0; g_aim_was = 0; g_aim_lock_ps = 0; g_skel_logged = 0;
    g_silent_prev[0] = g_silent_prev[1] = g_silent_prev[2] = 0.0;
    g_fov_orig = 0; g_fov_done = 0; g_fov_logged = 0;
}
// A dead player is put into a spectator pawn; the lobby has no local pawn at all. Both mean "not in a match".
static int pawn_is_spectator(uint64_t pawn) {
    if (!plausible(pawn)) return 0;
    char nm[96];
    uint64_t cls = frd64(pawn + OBJ_CLASS);
    if (!plausible(cls) || !class_name(cls, nm, sizeof nm)) return 0;
    return strstri(nm, "Spectator") || strstri(nm, "Observer");
}
// Restore the engine's own FOV BEFORE any cached camera pointer is dropped. Called on death / lobby return.
static void fov_suspend(void) {
    if (!g_fov_orig || g_fov_done || !g_pc) { g_fov_orig = 0; g_fov_done = 0; return; }
    uint64_t pcm = fast_ok(g_pc + 0x320, 8) ? frd64(g_pc + 0x320) : 0;
    float f = (float)g_fov_orig;
    if (plausible(pcm) && fast_ok(pcm + 0x27c, 4)) *(volatile float *)(pcm + 0x27c) = f;
    if (g_pcm && g_pov_off >= 0 && fast_ok(g_pcm + 0x1550 + (uint64_t)g_pov_off + 0x30, 0xc)) {
        *(volatile float *)(g_pcm + 0x1550 + (uint64_t)g_pov_off + 0x30) = f;
        *(volatile float *)(g_pcm + 0x1550 + (uint64_t)g_pov_off + 0x34) = f;
        *(volatile float *)(g_pcm + 0x1550 + (uint64_t)g_pov_off + 0x38) = f;
    }
    g_fov_orig = 0; g_fov_done = 0;
    logf_("fov: restored %d on suspend", (int)f);
}
// Cheap "can features run right now?" probe: valid local pawn, not spectating, and not dead.
// Health is checked in both places the title exposes it: the replicated PlayerState and the local pawn set.
static int local_alive_fast(void) {
    if (!g_pc || !plausible(g_pc)) return 0;
    uint64_t pawn = pawn_fast();
    if (!pawn || !actor_alive(pawn) || pawn_is_spectator(pawn)) return 0;
    uint64_t ps = frd64(g_pc + 0x270);
    if (plausible(ps) && fast_ok(ps + PS_CURHEALTH, 0x10)) {
        float h = f32_at(ps + PS_CURHEALTH), m = f32_at(ps + PS_MAXHEALTH);
        if (m >= 1.0f && m <= 20000.0f && h >= -1.0f && h <= m * 2.0f && h <= 0.0f) return 0;
    }
    uint64_t hs = frd64(pawn + PAWN_HEALTHSET);
    if (plausible(hs) && fast_ok(hs + HS_HEALTH, 0xc8)) {
        float h = f32_at(hs + HS_HEALTH + ATTR_CUR), m = f32_at(hs + HS_MAXHEALTH + ATTR_CUR);
        if (m >= 1.0f && m <= 20000.0f && h >= -1.0f && h <= m * 2.0f && h <= 0.0f) return 0;
    }
    return 1;
}
// Suspend EVERY gameplay feature. The feature toggles are kept so they come back automatically on the next
// life / next match; nothing is drawn, aimed, patched or written while this is in effect.
static void suspend_all(const char *why) {
    if (!g_ingame && !g_hud_hooked && !g_nplr && !g_nps_cache && !g_ncann && !g_nveh) return;
    if (g_ingame) logf_("match: %s - all features suspended", why);
    g_ingame = 0;
    g_hold_on = 0; g_silent_ok = 0; g_silent_sw = 0; g_silent_returning = 0;
    g_aim_was = 0; g_aim_lock_ps = 0; g_pad_r2_until = 0;
    fov_suspend();                                   // must run while the camera manager is still valid
    cannon_ghost_restore();
    uint64_t pawn = pawn_fast();
    if (g_grav_zeroed || g_nograv_n || g_noclip_n || g_tauto) tour_stop_gravity(pawn);
    g_tauto = 0; g_tphase = 0; g_hover_ok = 0; g_tmode = 0; g_tp_tries = 0;
    esp_flush(why, pawn);
    hud_forget();
    hud_unpatch_all();
    pad_inject_cancel();
}
// Runs on every ProcessEvent but only does real work every 20 ms. The moment the local player dies or the
// world is torn down, all features go dormant before the next native frame can touch stale pointers.
static void life_gate(void) {
    static uint64_t next = 0;
    uint64_t t = now_ms();
    if (t < next) return;
    next = t + 20;
    if (!g_ingame) return;                           // arming is done by world_guard / match_guard
    if (local_alive_fast()) return;
    suspend_all("local player dead or left");
}
// called every tick while in a match: the moment the pawn is gone (death, disconnect, map travel) the
// complete feature set is suspended BEFORE the game gets a chance to free PlayerStates/pawns
static void match_quick(void) {
    static uint64_t next = 0;
    if (!g_ingame) return;
    uint64_t t = now_ms();
    if (t < next) return;                                  // this runs on every ProcessEvent: 100 ms is plenty
    next = t + 100;
    ph(PH_MATCH);
    if (local_alive_fast()) return;
    suspend_all("out of match");
}
// re-arm once a live, non-spectator pawn exists again (throttled to 250 ms by world_guard)
static void match_guard(uint64_t world, uint64_t pawn) {
    if (g_ingame) return;                                  // match_quick owns the "leave" decision
    if (!world || !pawn || !actor_alive(pawn) || pawn_is_spectator(pawn)) return;
    if (!local_alive_fast()) return;                       // dead / spectating: stay suspended
    g_ingame = 1;
    logf_("match: player in game - features armed");
}
// Joining another match destroys the world and every actor in it. Cached PlayerState/cannon/vehicle pointers
// belong to the OLD match, so they are dropped BEFORE anything calls ProcessEvent on them - this is the crash
// on map travel. A running tour is stopped for the same reason.
static void world_guard(void) {
    static uint64_t last = 0;
    uint64_t t = now_ms();
    if (t - last < 250) return;
    last = t;
    uint64_t world = 0;
    if (plausible(g_pc)) {
        uint64_t lvl = frd64(g_pc + OBJ_OUTER);
        if (plausible(lvl)) world = frd64(lvl + 0xc8);
    }
    if (!plausible(world)) world = 0;                      // transient reads during travel are not a world
    uint64_t pawn = get_pawn();
    if (world != g_esp_world) {
        if (g_esp_world) logf_("world: changed %lx -> %lx (new match)", g_esp_world, world);
        g_esp_world = world;
        suspend_all(world ? "world changed" : "world gone");   // lists, HUD, FOV, flight, pad
    }
    match_guard(world, pawn);
}
// ---- v84: aimbot + silent aim -----------------------------------------------------------------------------
// The camera POV (g_cam_loc / g_cam_rot) is re-read from the camera manager every aim tick; targets are the
// players the ESP already tracks. FRotator in this build is three doubles (the camera reader proves it), so
// ControlRotation is written as doubles and the RPC rotation as the engine's 16-bit compressed axes.
//
//   AIMBOT   : while L2 (aim/ADS) is held the local controller's ControlRotation is driven onto the target,
//              so the camera locks and the shot follows it. No extra keybind.
//   SILENT   : the shot does not follow the screen. APlayerController::ServerUpdateCamera(CamLoc,
//              int32 CamPitchAndYaw) is the RPC that tells the server where the client is looking; its
//              packed pitch/yaw is rewritten on the way out, and the trace-time rotation is swapped in only
//              for the duration of a ProcessEvent call. The local camera is never moved, so the view stays
//              exactly where the player is aiming while the bullets still land on the target bone.
// aim point of one player: a cached skeleton bone when there is one, otherwise the usual body heights
static void aim_pt(const Plr *p, int part, double *o) {
    int k = (part == 0) ? 0 : (part == 1 ? 2 : 3);   // head / spine_03 / pelvis
    if (k >= 0 && k < 8 && (p->bones_ok & (1u << k))) {
        o[0] = p->bone[k][0]; o[1] = p->bone[k][1]; o[2] = p->bone[k][2];
        return;
    }
    o[0] = p->x; o[1] = p->y;
    o[2] = p->z + (part == 0 ? 170.0 : (part == 1 ? 105.0 : 55.0));
}
static void cam_forward(double *f) {
    double pr = (double)g_cam_rot[0] * 0.0174532925199433, yr = (double)g_cam_rot[1] * 0.0174532925199433;
    double cp = __builtin_cos(pr), sp = __builtin_sin(pr);
    f[0] = cp * __builtin_cos(yr); f[1] = cp * __builtin_sin(yr); f[2] = sp;
}
// nearest bone for the "NEAREST" part; falls back to the chest when nothing is cached
static int aim_nearest_bone(const Plr *p, const double *f, double *o) {
    double best = -2.0;
    for (int k = 0; k < 8; k++) {
        if (!(p->bones_ok & (1u << k))) continue;
        double dx = p->bone[k][0] - g_cam_loc[0], dy = p->bone[k][1] - g_cam_loc[1], dz = p->bone[k][2] - g_cam_loc[2];
        double len = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
        if (len < 40.0) continue;
        double dot = (dx*f[0] + dy*f[1] + dz*f[2]) / len;
        if (dot > best) { best = dot; o[0] = p->bone[k][0]; o[1] = p->bone[k][1]; o[2] = p->bone[k][2]; }
    }
    return best > -2.0;
}
// exact bone for the picked player (only called once per aim tick)
static int aim_bone_of(const Plr *p, int part, double *o) {
    int k = (part == 0) ? 0 : (part == 1 ? 2 : 3);
    if (g_bone_lay < 0 || k < 0 || k >= 8 || !g_bone_idx[k]) return 0;
    uint64_t mesh = frd64(p->pawn + ACTOR_MESH);
    if (!plausible(mesh)) return 0;
    return pe_bone_lay(mesh, (uint32_t)g_bone_idx[k], g_bone_lay, o);
}
// pick the player whose aim point is closest to the crosshair, but only inside fov_deg
// v126: aimbot visibility. The local controller's own LineOfSightTo trace decides whether an enemy can
// actually be seen; blocked players are ignored instead of being aimed at through walls. The result is
// cached for 200 ms per player. If the UFunction cannot be resolved (unknown build) the aimbot keeps
// working as before instead of refusing every target.
static int player_visible(Plr *p) {
    if (!p || !p->have || !p->alive || p->local) return 0;
    if (!g_fn_los) return 1;
    if (!plausible(g_pc) || !fast_ok(g_pc + CTRL_ROT, 0x18)) return 1;
    uint64_t t = now_ms();
    if (p->vis_ok && t - p->vis_t < 200) return p->vis;
    if (!fn_matches(g_fn_los, g_pc)) { g_fn_los = 0; return 1; }
    uint8_t b[0x28] __attribute__((aligned(16)));
    memset(b, 0, sizeof b);
    *(uint64_t *)b = p->pawn;                     // Other
    // ViewPoint stays zero: the controller uses its own view location
    ((pe_t)g_stub)((void *)g_pc, (void *)g_fn_los, b);
    p->vis_t = t; p->vis_ok = 1;
    p->vis = b[0x21] ? 1 : 0;
    if (g_los_log < 8) {
        g_los_log++;
        logf_("aim: LOS '%s' = %s", p->name[0] ? p->name : "player", p->vis ? "visible" : "blocked");
    }
    return p->vis;
}
// v125: the aim point of the player we are already locked onto. NEAREST re-picks the closest bone of that
// SAME player (never another player), so tracking a running target stays smooth.
static void aim_locked_pt(Plr *lp, int part, double *tp) {
    if (part == 3) {
        double f[3]; cam_forward(f);
        if (!aim_nearest_bone(lp, f, tp)) aim_pt(lp, 1, tp);
        return;
    }
    aim_pt(lp, part, tp);
    if (!g_esp_skel) { double b[3]; if (aim_bone_of(lp, part, b)) { tp[0]=b[0]; tp[1]=b[1]; tp[2]=b[2]; } }
}
static int aim_pick(int part, int fov_deg, double *out, Plr **who) {
    if (!g_ingame || !g_cam_ok || !g_pc) return 0;
    double f[3]; cam_forward(f);
    double lim = __builtin_cos((double)fov_deg * 0.0174532925199433);
    double best = lim, bp[3] = { 0, 0, 0 };
    Plr *bp_plr = 0;
    for (int i = 0; i < g_nplr; i++) {
        Plr *p = &g_plr[i];
        if (!p->have || !p->alive || p->local) continue;
        double mx = p->x - g_esp_me[0], my = p->y - g_esp_me[1], mz = p->z - g_esp_me[2];
        if (g_esp_me_ok && (mx*mx + my*my + mz*mz) > 50000.0 * 50000.0) continue;
        double cand[9][3]; int nc = 0;
        if (part == 3) {
            double nb[3];
            if (aim_nearest_bone(p, f, nb)) { cand[nc][0] = nb[0]; cand[nc][1] = nb[1]; cand[nc][2] = nb[2]; nc++; }
        }
        if (!nc) { aim_pt(p, part == 3 ? 1 : part, cand[0]); nc = 1; }
        for (int c = 0; c < nc; c++) {
            double dx = cand[c][0] - g_cam_loc[0], dy = cand[c][1] - g_cam_loc[1], dz = cand[c][2] - g_cam_loc[2];
            double len = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
            if (len < 60.0 || len > 50000.0) continue;
            double dot = (dx*f[0] + dy*f[1] + dz*f[2]) / len;
            if (dot <= best) continue;
            if (!player_visible(p)) continue;                 // v126: never aim at a player behind cover
            best = dot; bp[0] = cand[c][0]; bp[1] = cand[c][1]; bp[2] = cand[c][2]; bp_plr = p;
        }
    }
    if (!bp_plr) return 0;
    // the cached bones are only refreshed while the skeleton ESP runs; when it is off, ask the mesh once
    if (part != 3 && !g_esp_skel) {
        double b[3];
        if (aim_bone_of(bp_plr, part, b)) {
            double dx = b[0]-g_cam_loc[0], dy = b[1]-g_cam_loc[1], dz = b[2]-g_cam_loc[2];
            double len = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
            if (len > 60.0 && ((dx*f[0] + dy*f[1] + dz*f[2]) / len) >= lim) { bp[0]=b[0]; bp[1]=b[1]; bp[2]=b[2]; }
        }
    }
    out[0] = bp[0]; out[1] = bp[1]; out[2] = bp[2];
    if (who) *who = bp_plr;
    return 1;
}
static int aim_rot(const double *tp, double *pitch, double *yaw) {
    double dx = tp[0] - g_cam_loc[0], dy = tp[1] - g_cam_loc[1], dz = tp[2] - g_cam_loc[2];
    double hl = __builtin_sqrt(dx*dx + dy*dy);
    if (hl < 0.5 && __builtin_fabs(dz) < 0.5) return 0;
    *yaw = __builtin_atan2(dy, dx) * 57.29577951308232;
    *pitch = __builtin_atan2(dz, hl) * 57.29577951308232;
    return 1;
}
static void aim_probe_layout(void) {            // skeleton layout is shared with the ESP probe
    if (g_bone_lay >= 0 || g_probe_tries >= 4) return;
    if (!g_esp_me_ok) return;
    uint64_t pawn = pawn_fast();
    if (!plausible(pawn)) return;
    uint64_t mesh = frd64(pawn + ACTOR_MESH);
    if (!plausible(mesh)) return;
    g_probe_tries++;
    double pos[3] = { g_esp_me[0], g_esp_me[1], g_esp_me[2] };
    probe_bone_layout(mesh, pos);
}
// smooth approach: move a fraction of the remaining angle per tick (0 = snap)
// move current rotation toward (pitch,yaw) by at most max_step degrees (shortest way round)
static void rot_step(double *cr, double pitch, double yaw, double max_step) {
    double dp = pitch - cr[0], dy = yaw - cr[1];
    while (dy > 180.0) dy -= 360.0; while (dy < -180.0) dy += 360.0;
    double len = __builtin_sqrt(dp * dp + dy * dy);
    if (len < 0.0001 || (max_step > 0 && len <= max_step)) {
        cr[0] = pitch; cr[1] = yaw;
    } else if (max_step > 0) {
        double k = max_step / len;
        cr[0] += dp * k; cr[1] += dy * k;
    }
    if (cr[1] > 180.0) cr[1] -= 360.0; if (cr[1] < -180.0) cr[1] += 360.0;
    if (cr[0] > 89.0) cr[0] = 89.0; if (cr[0] < -89.0) cr[0] = -89.0;
    cr[2] = 0.0;
}
static double aim_dt(void) {
    uint64_t now = now_ms();
    double dt = (double)(now - g_aim_write_last) / 1000.0;
    g_aim_write_last = now;
    if (dt < 0.001) dt = 0.001; if (dt > 0.05) dt = 0.05;
    return dt;
}
// The rendered camera AND the shot trace both read the camera manager's cached POV. ControlRotation alone
// only reaches them after the camera's interpolation, which is exactly the "aimbot is delayed" behaviour.
// Writing the cached POV makes the aim take effect on the very next trace, and lets silent aim hit the bone.
static void cam_pov_set(double pitch, double yaw) {
    if (!g_pcm || g_pov_off < 0) return;
    uint64_t base = g_pcm + 0x1550 + (uint64_t)g_pov_off;
    if (fast_ok(base + 0x18, 0x18)) {
        double *r = (double *)(base + 0x18);
        r[0] = pitch; r[1] = yaw; r[2] = 0.0;
    }
}
static void aim_write(double pitch, double yaw) {
    double *cr = (double *)(g_pc + CTRL_ROT);
    if (g_aim_smooth <= 0) {
        cr[0] = pitch; cr[1] = yaw; cr[2] = 0.0;
    } else {
        double dt = aim_dt();
        double dy = yaw - cr[1];
        while (dy > 180.0) dy -= 360.0; while (dy < -180.0) dy += 360.0;
        double err = __builtin_sqrt((pitch - cr[0]) * (pitch - cr[0]) + dy * dy);
        double step = (err > 40.0) ? 1.0e9                       // big correction: snap, no visible delay
                                   : (160.0 + 110.0 * (double)g_aim_smooth) * dt;
        rot_step(cr, pitch, yaw, step);
    }
    g_hold_rot[0] = cr[0]; g_hold_rot[1] = cr[1]; g_hold_on = 1;
    cam_pov_set(cr[0], cr[1]);
}
// Called on every ProcessEvent (not just every 10 ms): the shot trace runs natively between our ticks, so
// the aim rotation has to be the last thing written to the controller/camera as often as possible.
static void aim_fast_hold(void) {
    if (!g_hold_on || !g_pc) return;
    if (!(g_aim && g_pad_l2)) { g_hold_on = 0; return; }   // silent aim never holds the view
    double *cr = (double *)(g_pc + CTRL_ROT);
    cr[0] = g_hold_rot[0]; cr[1] = g_hold_rot[1]; cr[2] = 0.0;
}
// silent aim: the target rotation is applied only for the duration of a game-thread ProcessEvent call,
// so the shot/ability trace sees it while the rendered camera (native tick) never does.
// It is refreshed by aim_tick and only used while R2 (fire) is held.
static void aim_silent_enter(double *scr, double *spov, int *hc, int *hp) {
    *hc = *hp = 0;
    if (!g_silent || !g_silent_ok || g_menu || !g_ingame || !g_pc) return;
    if (!g_pad_r2 && now_ms() >= g_pad_r2_until) return;
    if (!fast_ok(g_pc + CTRL_ROT, 0x18)) return;
    double *cr = (double *)(g_pc + CTRL_ROT);
    scr[0] = cr[0]; scr[1] = cr[1]; scr[2] = cr[2];
    cr[0] = g_silent_rot[0]; cr[1] = g_silent_rot[1]; cr[2] = 0.0;
    *hc = 1;
    if (!g_silent_applied) {
        g_silent_applied = 1;
        logf_("silent: rotation applied inside ProcessEvent (pitch %.1f yaw %.1f, fov %d)",
              g_silent_rot[0], g_silent_rot[1], g_silent_fov);
    }
    if (g_pcm && g_pov_off >= 0) {
        uint64_t ra = g_pcm + 0x1550 + (uint64_t)g_pov_off + 0x18;
        if (fast_ok(ra, 0x18)) {
            double *pr = (double *)ra;
            spov[0] = pr[0]; spov[1] = pr[1]; spov[2] = pr[2];
            pr[0] = g_silent_rot[0]; pr[1] = g_silent_rot[1]; pr[2] = 0.0;
            *hp = 1;
        }
    }
}
// hook the function the shot/trace path asks for its view. Rendering reads the camera manager, so the
// camera stays put. This is the documented UE silent-aim method (GetPlayerViewPoint).
static void viewpt_hook(void *self, double *loc, double *rot) {
    if (!g_viewpt_orig) return;
    g_viewpt_orig(self, loc, rot);
    g_viewpt_calls++;
    if (g_viewpt_calls == 1) logf_("silent: GetPlayerViewPoint detour active (self %lx pc %lx)", (uint64_t)self, g_pc);
    if (!g_silent || !g_silent_ok || g_menu || !g_ingame) return;
    if (!g_pad_r2 && now_ms() >= g_pad_r2_until) return;
    if (g_pc && (uint64_t)self != g_pc) return;
    if (!rot || !fast_ok((uint64_t)rot, 0x18)) return;
    rot[0] = g_silent_rot[0]; rot[1] = g_silent_rot[1];
    if (g_viewpt_hits++ == 0)
        logf_("silent: view point redirected (pitch %.1f yaw %.1f, part %s)", rot[0], rot[1], AIM_PART_NAME[g_silent_part]);
}
// native AFortPlayerController::GetPlayerViewPoint detour: the shot/ability trace reads its rotation from
// here. Only the returned rotation is replaced, and only while R2 is held with a target inside the silent
// FOV. Nothing on the camera side (ControlRotation, CameraCachePrivate, view targets) is touched, so the
// rendered view never moves.
// Sanity gate for the detoured virtual: the two out-parameters must look like a real view - a finite world
// location and a rotation inside rotator range. If the vtable slot is ever something else on another build,
// this check refuses to write, so the detour can only cost a missed shot, never corrupted engine state.
static int view_sane(const double *loc, const double *rot) {
    if (!loc || !rot || (const void *)loc == (const void *)rot) return 0;
    for (int i = 0; i < 3; i++) {
        double v = loc[i]; if (!(v == v)) return 0;
        if (v < -2000000.0 || v > 2000000.0) return 0;
    }
    for (int i = 0; i < 3; i++) { double v = rot[i]; if (!(v == v)) return 0; }
    if (rot[0] < -95.0 || rot[0] > 95.0) return 0;                 // pitch
    if (rot[1] < -400.0 || rot[1] > 400.0) return 0;                // yaw
    if (rot[2] < -400.0 || rot[2] > 400.0) return 0;                // roll
    return 1;
}
static void gvp_nat_hook(void *self, double *loc, double *rot) {
    if (!g_gvp_nat_orig) return;
    g_gvp_nat_orig(self, loc, rot);
    g_gvp_nat_calls++;
    if (g_gvp_nat_calls == 1)
        logf_("silent: native GetPlayerViewPoint hooked (fn %lx, self %lx, pc %lx)", g_gvp_nat_fn, (uint64_t)self, g_pc);
    if (!g_silent || !g_silent_ok || g_menu || !g_ingame || !g_pc) return;
    if ((uint64_t)self != g_pc) return;
    if (!fast_ok((uint64_t)loc, 0x18)) return;
    if (!writable((uint64_t)rot, 0x18)) {
        if (g_view_ro++ < 3) logf_("silent: view point rotation is read-only - not spoofed");
        return;
    }
    if (!g_view_ok) {
        if (!view_sane(loc, rot)) {
            if (g_view_bad++ < 3) logf_("silent: view point signature mismatch - slot not spoofed");
            return;
        }
        g_view_ok = 1;
        logf_("silent: view point verified (loc %.0f %.0f %.0f, rot %.1f %.1f %.1f, part %s)",
              loc[0], loc[1], loc[2], rot[0], rot[1], rot[2], AIM_PART_NAME[g_silent_part]);
    }
    if (!g_pad_r2 && now_ms() >= g_pad_r2_until) return;
    rot[0] = g_silent_rot[0]; rot[1] = g_silent_rot[1]; rot[2] = 0.0;
    if (g_gpvp_hits++ == 0)
        logf_("silent: shot rotation redirected natively (pitch %.1f yaw %.1f, part %s)",
              rot[0], rot[1], AIM_PART_NAME[g_silent_part]);
}
// Redirect one vtable slot with a single aligned 8-byte store. Readers see either the old or the new
// pointer, never a half-written one, and not one byte of game code is modified - no trampoline is needed
// because the original function is still there and is simply called through the saved pointer. v99 patched
// the function prologue while the game and the render thread were executing it, which can crash the title.
static int vtable_set(uint64_t slot, uint64_t value, uint64_t *out_old) {
    if (!readable(slot, 8)) return 0;
    uint64_t page = slot & ~0xfffULL;
    void *st = 0, *en = 0; int prot = 0;
    if (sceKernelQueryMemoryProtection((void *)page, &st, &en, &prot) != 0) return 0;
    uint64_t prev = *(volatile uint64_t *)slot;
    if (out_old) *out_old = prev;
    if (prev == value) return 1;
    if (sceKernelMprotect((void *)page, 0x1000, 0x7) != 0) return 0;
    *(volatile uint64_t *)slot = value;                    // one aligned store: atomic on x86-64
    sceKernelMprotect((void *)page, 0x1000, prot);
    return 1;
}
// the controller class changes between lobby and match, so the slot is re-checked on every in-game tick
static void install_gvp_native(void) {
    if (cfg_nogvp || !g_ingame || !plausible(g_pc)) return;
    uint64_t vt = frd64(g_pc);
    if (!plausible(vt)) return;
    uint64_t slot = vt + 0x7e8;
    if (!fast_ok(slot, 8)) return;
    uint64_t cur = frd64(slot);
    if (cur == (uint64_t)&gvp_nat_hook) return;             // this class is already redirected
    if (!plausible(cur) || !readable(cur, 16)) return;
    if (g_gvp_nat_orig && cur != (uint64_t)g_gvp_nat_orig) { // a second implementation: leave it alone
        static uint64_t seen = 0;
        if (seen != cur) { seen = cur; logf_("silent: view point impl %lx differs from %lx - left alone", cur, (uint64_t)g_gvp_nat_orig); }
        return;
    }
    if (!g_gvp_nat_orig) { g_gvp_nat_orig = (gvp_nat_t)cur; g_view_ok = 0; g_view_bad = 0; g_view_ro = 0; }
    g_gvp_nat_fn = cur;
    if (!vtable_set(slot, (uint64_t)&gvp_nat_hook, 0)) {
        logf_("silent: vtable 7e8 swap failed (vt %lx)", vt);
        return;
    }
    logf_("silent: GetPlayerViewPoint vtable 7e8 %lx swapped in (no code patch)", cur);
}
// camera FOV changer: APlayerCameraManager.DefaultFOV (+ the live camera cache so it applies at once)
static void fov_tick(void) {
    static uint64_t last = 0;
    uint64_t t = now_ms();
    if (t - last < 50) return;
    last = t;
    if (!g_ingame || !g_pc) return;
    if (!local_alive_fast()) return;
    uint64_t pcm = fast_ok(g_pc + 0x320, 8) ? frd64(g_pc + 0x320) : 0;
    if (!plausible(pcm) || !fast_ok(pcm + 0x27c, 4)) return;
    if (!g_fov_chg) {
        if (g_fov_orig && !g_fov_done) {
            *(volatile float *)(pcm + 0x27c) = (float)g_fov_orig;
            g_fov_done = 1;
        }
        return;
    }
    if (!g_fov_orig) {
        float cur = f32_at(pcm + 0x27c);
        if (cur >= 40.0f && cur <= 130.0f) g_fov_orig = (int)(cur + 0.5f);
        g_fov_done = 0;
    }
    float f = (float)g_fov_chg;
    if (g_pov_off < 0) cam_cache_read();
    int n = 0;
    if (fast_ok(pcm + 0x27c, 4)) { *(volatile float *)(pcm + 0x27c) = f; n++; }             // DefaultFOV
    // FTViewTarget.POV starts at +0x10 inside ViewTarget(+0x300) / PendingViewTarget(+0xc10)
    uint64_t vt[2] = { pcm + 0x300 + 0x10, pcm + 0xc10 + 0x10 };
    for (int i = 0; i < 2; i++) {
        if (!fast_ok(vt[i] + 0x30, 0xc)) continue;
        *(volatile float *)(vt[i] + 0x30) = f;                                              // FOV
        *(volatile float *)(vt[i] + 0x34) = f;                                              // DesiredFOV
        *(volatile float *)(vt[i] + 0x38) = f;                                              // FirstPersonFOV
        n += 3;
    }
    if (g_pov_off >= 0 && fast_ok(pcm + 0x1550 + (uint64_t)g_pov_off + 0x30, 0xc)) {
        *(volatile float *)(pcm + 0x1550 + (uint64_t)g_pov_off + 0x30) = f;
        *(volatile float *)(pcm + 0x1550 + (uint64_t)g_pov_off + 0x34) = f;
        *(volatile float *)(pcm + 0x1550 + (uint64_t)g_pov_off + 0x38) = f;
        n += 3;
    }
    if (!g_fov_logged) {
        g_fov_logged = 1;
        logf_("fov: pcm %lx wrote %d fields -> %.0f (cache read fov %.0f)", pcm, n, f, g_cam_fov);
    }
}
static void aim_silent_leave(const double *scr, const double *spov, int hc, int hp) {
    if (hc && fast_ok(g_pc + CTRL_ROT, 0x18)) {
        double *cr = (double *)(g_pc + CTRL_ROT);
        cr[0] = scr[0]; cr[1] = scr[1]; cr[2] = scr[2];
    }
    if (hp) {
        uint64_t ra = g_pcm + 0x1550 + (uint64_t)g_pov_off + 0x18;
        if (fast_ok(ra, 0x18)) { double *pr = (double *)ra; pr[0] = spov[0]; pr[1] = spov[1]; pr[2] = spov[2]; }
    }
}
// aimbot: L2 held -> drive ControlRotation onto the target
static void aim_tick(void) {
    static uint64_t last = 0;
    uint64_t t = now_ms();
    int refresh = (t - last >= 10);
    if (refresh) last = t;
    if (!g_aim || !g_ingame || g_menu || !g_pc) { g_hold_on = 0; return; }
    if (!local_alive_fast()) { g_hold_on = 0; return; }
    if (!fast_ok(g_pc + CTRL_ROT, 0x18)) return;
    if (!refresh) { aim_fast_hold(); return; }
    cam_cache_read();
    aim_probe_layout();
    if (!g_fn_gpvp || !g_fn_hitnotify) {
        const char *nm[2] = { "GetPlayerViewPoint", "OnDisplayHitNotify" };
        uint64_t out[2] = { g_fn_gpvp, g_fn_hitnotify };
        find_methods_on(frd64(g_pc + OBJ_CLASS), nm, out, 2);
        g_fn_gpvp = out[0]; g_fn_hitnotify = out[1];
    }
    if (!g_fn_dmg && g_dmg_tries < 3) {                       // retry on the game thread, once the objects exist
        g_dmg_tries++;
        const char *dn[1] = { "ClientAddDamageNumber" };
        find_methods_on(frd64(g_pc + OBJ_CLASS), dn, &g_fn_dmg, 1);
        logf_("esp: damage-number RPC %lx (%s)", g_fn_dmg, g_fn_dmg ? "resolved" : "retrying");
    }
    if (!g_fn_los && g_los_tries < 3) {                       // v126: visibility for the aimbot
        g_los_tries++;
        const char *ln[1] = { "LineOfSightTo" };
        find_methods_on(frd64(g_pc + OBJ_CLASS), ln, &g_fn_los, 1);
        logf_("aim: LineOfSightTo %lx (%s)", g_fn_los, g_fn_los ? "visible-only aim on" : "retrying");
    }
    g_hold_on = 0;
    if (!g_aim || !g_pad_l2) { g_aim_was = 0; g_aim_lock_ps = 0; g_aim_lock_until = 0; return; }
    // v125: once a target is acquired, L2 keeps it. The target is only replaced when it dies, its pawn
    // disappears, or it leaves a cone twice the acquisition FOV - not merely because somebody else is a
    // few pixels closer. That is what makes the aim "lock in" instead of flicking between players.
    double tp[3]; Plr *lp = 0;
    if (g_aim_was && g_aim_lock_ps) {
        for (int i = 0; i < g_nplr; i++) {
            Plr *q = &g_plr[i];
            if (q->ps != g_aim_lock_ps || !q->have || !q->alive || q->local) continue;
            double c[3], f[3];
            aim_locked_pt(q, g_aim_part, c);
            cam_forward(f);
            double dx = c[0] - g_cam_loc[0], dy = c[1] - g_cam_loc[1], dz = c[2] - g_cam_loc[2];
            double len = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
            double lim = __builtin_cos((double)(g_aim_fov * 2) * 0.0174532925199433);
            if (len > 60.0 && len < 50000.0 && (dx*f[0] + dy*f[1] + dz*f[2]) / len >= lim && player_visible(q)) lp = q;
            break;
        }
    }
    if (!lp) {
        double pick[3]; Plr *who = 0;
        if (!aim_pick(g_aim_part, g_aim_fov, pick, &who)) { g_aim_was = 0; g_aim_lock_ps = 0; return; }
        lp = who;
        if (lp && lp->ps != g_aim_lock_ps) {
            g_aim_lock_ps = lp->ps;
            if (g_aim_locks++ < 8)
                logf_("aim: locked on '%s' (kept until release/death, part %s)",
                      lp->name[0] ? lp->name : "player", AIM_PART_NAME[g_aim_part]);
        }
    }
    if (!lp) { g_hold_on = 0; return; }
    g_aim_was = 1;
    aim_locked_pt(lp, g_aim_part, tp);
    double pitch, yaw;
    if (!aim_rot(tp, &pitch, &yaw)) return;
    aim_write(pitch, yaw);
    if (!g_aim_logged) {
        g_aim_logged = 1;
        double dx = tp[0]-g_cam_loc[0], dy = tp[1]-g_cam_loc[1], dz = tp[2]-g_cam_loc[2];
        logf_("aim: locked on %s (part %s, fov %d, pitch %.1f yaw %.1f, %.0f m)",
              lp->name[0] ? lp->name : "player", AIM_PART_NAME[g_aim_part], g_aim_fov, pitch, yaw,
              __builtin_sqrt(dx*dx+dy*dy+dz*dz) / 100.0);
    }
}
// The UFunction is called by the weapon/targeting BP with its outputs in the params buffer:
//   Location FVector @0x00 (3 doubles), Rotation FRotator @0x18 (3 doubles).
// The detour on the native address is unavailable on this build (install logged "not hooked"), so the
// ProcessEvent hook rewrites the rotation on the way back out. The engine sees the target; the camera manager
// (which drives the rendered view) is never touched.
static void aim_gpvp_post(uint64_t fn, uint8_t *params) {
    if (!g_silent || !g_silent_ok || g_menu || !g_ingame) return;
    if (!g_fn_gpvp || fn != g_fn_gpvp || !params) return;
    if (!g_pad_r2 && now_ms() >= g_pad_r2_until) return;
    if (!fast_ok((uint64_t)params, 0x30)) return;
    double *rr = (double *)(params + 0x18);
    rr[0] = g_silent_rot[0]; rr[1] = g_silent_rot[1];
    if (!g_gpvp_hits++)
        logf_("silent: GetPlayerViewPoint params redirected (%.1f %.1f, part %s)", rr[0], rr[1], AIM_PART_NAME[g_silent_part]);
}
// ---- silent aim at the shot path -----------------------------------------------------------------------
// The BP weapon code asks these UFunctions for the fire direction. Layouts are straight from the CUSA07022
// SDK (FortniteGame_parameters.hpp), so nothing is guessed:
//   FortGameplayAbility::GetFireRotation                        FRotator ReturnValue      @0x08
//   FortGameplayAbility::GetProjectileInitialValues             FVector  outSpawnDirection @0x08
//   FortGameplayAbility::GetProjectileInitialValuesAndTargetLocation
//                                                               FVector  outTargetLocation @0x08
//                                                               FVector  outSpawnDirection @0x28
//   FortPawn::GetTargetingTransform                             FTransform ReturnValue    @0x10 (FQuat @+0x00)
//   FortPawn::GetDefaultTargetingRotation                       FRotator ReturnValue      @0x00
//   FortTargetingUtils::GetPlayerCameraTargetingTransform       FTransform ReturnValue    @0x30 (FortPawn @0x00)
// The engine runs first and produces the real value; only then is the direction replaced, and only for the
// local player's own shot while the trigger is held. The camera, ControlRotation and the weapon pose are
// never written, so the view does not move.
static void fire_quat(double *q) {                       // pitch/yaw -> UE FQuat (roll 0)
    const double D = 3.14159265358979323846 / 360.0;
    double sp = __builtin_sin(g_silent_rot[0] * D), cp = __builtin_cos(g_silent_rot[0] * D);
    double sy = __builtin_sin(g_silent_rot[1] * D), cy = __builtin_cos(g_silent_rot[1] * D);
    q[0] = sp * sy; q[1] = -sp * cy; q[2] = cp * sy; q[3] = cp * cy;
}
static void fire_dir(double *v) {                        // pitch/yaw -> unit direction (FRotator::Vector)
    const double D = 3.14159265358979323846 / 180.0;
    double p = g_silent_rot[0] * D, y = g_silent_rot[1] * D;
    double cp = __builtin_cos(p);
    v[0] = cp * __builtin_cos(y); v[1] = cp * __builtin_sin(y); v[2] = __builtin_sin(p);
}
static void aim_resolve_fire(void) {
    if (g_fire_stage >= 3) return;
    uint64_t t = now_ms();
    if (t < g_fire_try) return;
    g_fire_try = t + 600;                                // one GObjects walk per tick, never a long hitch
    if (g_fire_stage == 0) {
        uint64_t c = find_object_by_text("FortPawn", "Class");
        const char *nm[2] = { "GetTargetingTransform", "GetDefaultTargetingRotation" };
        uint64_t o[2] = { 0, 0 };
        if (c) find_methods_on(c, nm, o, 2);
        g_fn_pawn_tgt = o[0]; g_fn_pawn_defrot = o[1];
        g_fire_stage = 1;
        return;
    }
    if (g_fire_stage == 1) {
        uint64_t c = find_object_by_text("FortGameplayAbility", "Class");
        const char *nm[3] = { "GetFireRotation", "GetProjectileInitialValues", "GetProjectileInitialValuesAndTargetLocation" };
        uint64_t o[3] = { 0, 0, 0 };
        if (c) find_methods_on(c, nm, o, 3);
        g_fn_firerot = o[0]; g_fn_projvals = o[1]; g_fn_projvals_tgt = o[2];
        g_fire_stage = 2;
        return;
    }
    {
        uint64_t c = find_object_by_text("FortTargetingUtils", "Class");
        const char *nm[1] = { "GetPlayerCameraTargetingTransform" };
        uint64_t o[1] = { 0 };
        if (c) find_methods_on(c, nm, o, 1);
        g_fn_cam_tgt = o[0];
    }
    g_fire_stage = 3;
    int n = (g_fn_firerot != 0) + (g_fn_projvals != 0) + (g_fn_projvals_tgt != 0)
          + (g_fn_pawn_tgt != 0) + (g_fn_pawn_defrot != 0) + (g_fn_cam_tgt != 0);
    logf_("silent: shot fns %d/6 (firerot %lx proj %lx projt %lx ptgt %lx pdef %lx cam %lx)", n,
          g_fn_firerot, g_fn_projvals, g_fn_projvals_tgt, g_fn_pawn_tgt, g_fn_pawn_defrot, g_fn_cam_tgt);
}
static void aim_shot_post(uint64_t fn, uint8_t *p, uint64_t self) {
    if (!p || !fn || !g_silent || !g_silent_ok || g_menu || !g_ingame) return;
    int k = -1;
    if (fn == g_fn_firerot) k = 0;
    else if (fn == g_fn_projvals) k = 1;
    else if (fn == g_fn_projvals_tgt) k = 2;
    else if (fn == g_fn_pawn_tgt) k = 3;
    else if (fn == g_fn_pawn_defrot) k = 4;
    else if (fn == g_fn_cam_tgt) k = 5;
    if (k < 0) return;
    if (!g_pad_r2 && now_ms() >= g_pad_r2_until) return;
    uint64_t pawn = pawn_fast();
    switch (k) {
    case 0:                                             // FRotator& ReturnValue @0x08
        if (!writable((uint64_t)p + 8, 0x18)) return;
        { double *r = (double *)(p + 8); r[0] = g_silent_rot[0]; r[1] = g_silent_rot[1]; r[2] = 0.0; }
        break;
    case 1:                                             // FVector outSpawnDirection @0x08
        if (!writable((uint64_t)p + 8, 0x18)) return;
        fire_dir((double *)(p + 8));
        break;
    case 2:                                             // outTargetLocation @0x08 + outSpawnDirection @0x28
        if (!writable((uint64_t)p + 8, 0x18) || !writable((uint64_t)p + 0x28, 0x18)) return;
        { double *l = (double *)(p + 8); l[0] = g_silent_pt[0]; l[1] = g_silent_pt[1]; l[2] = g_silent_pt[2]; }
        fire_dir((double *)(p + 0x28));
        break;
    case 3:                                             // FTransform ReturnValue @0x10
        if (!pawn || self != pawn) return;
        if (!writable((uint64_t)p + 0x10, 0x20)) return;
        fire_quat((double *)(p + 0x10));
        break;
    case 4:                                             // FRotator ReturnValue @0x00
        if (!pawn || self != pawn) return;
        if (!writable((uint64_t)p, 0x18)) return;
        { double *r = (double *)p; r[0] = g_silent_rot[0]; r[1] = g_silent_rot[1]; r[2] = 0.0; }
        break;
    case 5:                                             // FortPawn @0x00, FTransform ReturnValue @0x30
        if (!pawn || frd64((uint64_t)p) != pawn) return;
        if (!writable((uint64_t)p + 0x30, 0x20)) return;
        fire_quat((double *)(p + 0x30));
        break;
    }
    if (g_shot_hits[k]++ == 0) {
        static const char *const KN[6] = { "GetFireRotation", "ProjectileInitialValues", "ProjectileInitialValuesAndTargetLocation",
                                           "GetTargetingTransform", "GetDefaultTargetingRotation", "GetPlayerCameraTargetingTransform" };
        logf_("silent: shot redirected via %s (pitch %.1f yaw %.1f)", KN[k], g_silent_rot[0], g_silent_rot[1]);
    }
}
// silent aim: rewrite the packed camera rotation of the outgoing ServerUpdateCamera RPC
static void aim_spoof_camera(uint8_t *params) {
    if (!g_silent || !g_ingame || !params) return;
    const double *cl = (const double *)params;
    if (++g_suc_seen == 1) logf_("silent: ServerUpdateCamera is being called - hooking its rotation");
    if (!(cl[0] == cl[0] && cl[1] == cl[1] && cl[2] == cl[2])) return;
    double ddx = cl[0] - g_cam_loc[0], ddy = cl[1] - g_cam_loc[1], ddz = cl[2] - g_cam_loc[2];
    // The buffer must start with THIS controller's camera location. If it does not, the parameter layout is
    // not what we expect and the RPC is left untouched instead of corrupting it.
    if (ddx*ddx + ddy*ddy + ddz*ddz > 2000.0 * 2000.0) {
        if (g_silent_bad < 3) {
            g_silent_bad++;
            logf_("silent: camera param does not match (%.0f %.0f %.0f vs %.0f %.0f %.0f) - layout check",
                  cl[0], cl[1], cl[2], g_cam_loc[0], g_cam_loc[1], g_cam_loc[2]);
        }
        return;
    }
    double tp[3];
    if (!aim_pick(g_silent_part, g_silent_fov, tp, 0)) return;
    double pitch, yaw;
    if (!aim_rot(tp, &pitch, &yaw)) return;
    uint32_t ps = (uint32_t)(int32_t)(pitch * 182.0444444444);   // FRotator::CompressAxisToShort
    uint32_t ys = (uint32_t)(int32_t)(yaw * 182.0444444444);
    *(int32_t *)(params + 0x18) = (int32_t)(((ps & 0xFFFF) << 16) | (ys & 0xFFFF));
    if (!g_silent_logged) {
        g_silent_logged = 1;
        logf_("silent: ServerUpdateCamera spoofed (part %s, fov %d, pitch %.1f yaw %.1f)",
              AIM_PART_NAME[g_silent_part], g_silent_fov, pitch, yaw);
    }
}

static void esp_tick(void) {
    uint64_t t = now_ms();
    // The walk must never run on every ProcessEvent: auto_tick is called once per ProcessEvent (hundreds
    // per frame), so the old code finished a whole scan inside a single frame and produced a ~20-30 ms
    // spike every time it restarted. One 2 ms slice per 12 ms spreads it over frames instead.
    static uint64_t slice_at = 0;
    if ((g_lw_active || g_scan_active) && !walk_wanted()) { g_lw_active = 0; g_scan_active = 0; }
    if ((g_lw_active || g_scan_active) && t - slice_at >= 12) {
        slice_at = t;
        uint64_t c = rcyc();
        if (g_lw_active) {
            if (level_walk_step()) level_walk_finish();
        } else {
            scan_world(0);
        }
        c = rcyc() - c; g_cy_scan += c; if (c > g_cy_scan_max) g_cy_scan_max = c; g_n_scan++;
    }
    if ((g_esp || g_esp_veh) && t - g_esp_pos >= 30) {                // cannons/vehicles move too (driven cars): 30 Hz
        g_esp_pos = t;
        refresh_esp();
    }
    if (t - g_esp_upd >= 250) {                                      // ~4 Hz background: lists, pawn, slow fallbacks
        g_esp_upd = t;
        uint64_t pawn = get_pawn();
        if (!pawn && (g_nplr || g_nps_cache || g_ncann || g_nveh)) esp_flush("no pawn (map travel)", pawn);
        if (pawn) {
            actor_pos(pawn, g_esp_me);                                 // memory read: no ProcessEvent on a travelling pawn
            g_esp_me_ok = in_world(g_esp_me[0], g_esp_me[1], g_esp_me[2]);
            g_esp_cur = teleport_target(pawn);
            if ((g_esp_plr || g_esp_skel || g_aim) && (!g_pfn[PFN_HP] || (!g_fn_bone && !g_fn_socket))) resolve_esp_fns(pawn);
        } else { g_esp_me_ok = 0; g_esp_cur = 0; }
        if (walk_wanted()) scan_world(0);                  // v130: never for player-only ESP
        if (!cam_cache_read()) camera_update(pawn);                   // ProcessEvent fallbacks only if the cache read failed
        if (!g_esp_logged && (g_ncann || g_nveh || g_nplr)) {
            g_esp_logged = 1;
            logf_("esp: %d cannons, %d vehicles, %d players, camera %s fov %.0f", g_ncann, g_nveh, g_nplr,
                  g_cam_ok ? "ok" : "none", g_cam_fov);
        }
    }
    // chest ESP: containers do not move, so the GObjects walk happens ONCE per match (retried every 20 s
    // while the list is still empty) and only positions are refreshed afterwards. The walk is ~25-40 ms, so
    // it must never sit on a timer that fires while the overlay is drawing.
    if (g_esp_chest) {
        // v130: containers come from the level actor list (about 1 ms per slice) instead of the old
        // synchronous 4-million-entry GObjects walk, which froze the game for seconds when chests were first
        // enabled. The list is staged, so the current boxes never blink out while a new walk runs.
        if (!g_lw_active && !g_scan_active && t - g_chest_scan >= (g_ncont ? 60000ULL : 5000ULL)) {
            g_chest_scan = t;
            if (!level_walk_start(1)) {
                g_chest_pos = t;
                scan_objects(); fill_positions();
                logf_("chest esp: %d containers (object walk fallback)", g_ncont);
            }
        }
        if (!g_lw_active && t - g_chest_pos >= 2000) {
            g_chest_pos = t; fill_positions();
        }
    }
    if (g_esp_chest && g_ncont) {                                     // opened/closed state, 4 chests per 250 ms
        static int cur = 0;
        for (int k = 0; k < 4 && k < g_ncont; k++) {
            int i = cur++ % g_ncont;
            Cont *c = &g_cont[i];
            if (t < c->open_next) continue;
            c->open = (uint8_t)(chest_open(c->obj) ? 1 : 0);
            c->open_next = t + 5000;
        }
    }
    if (g_esp_plr || g_aim) { players_refresh(); players_positions(); }   // list 4 Hz, positions every tick
    if (g_aim) aim_tick();                                       // L2 aim lock
}
static void esp_draw(uint64_t cv) {
    if (!g_ingame || (!g_esp && !g_esp_plr && !g_esp_veh && !g_esp_chest)) return;   // never project dangles from a dead match
    cam_cache_read();                                                // fresh camera EVERY frame -> boxes never step
    if (!g_cam_ok) return;
    g_cb_ok = 0;                                                     // v130: rebuild the basis for this frame
    int sw = *(volatile int *)(cv + 0x40), sh = *(volatile int *)(cv + 0x44);
    if (sw < 640 || sw > 7680) sw = 1920;
    if (sh < 360 || sh > 4320) sh = 1080;
    double S = sw / 1920.0; if (S < 0.5) S = 0.5; if (S > 2.0) S = 2.0;
    LC wtx = { 1, 1, 1, 1 }, shd = { 0, 0, 0, 1 };
    char label[80];
    int drawn = 0, labels = 0, plabels = 0;

    if (g_esp) {                                                     // cannons: box + distance
        LC near_c = { 1.00f, 0.90f, 0.10f, 1.0f }, far_c = { 1.00f, 0.55f, 0.10f, 1.0f };
        LC cur_c = { 0.30f, 1.00f, 0.40f, 1.0f };
        for (int i = 0; i < g_ncann && drawn < 24; i++) {
            if (g_esp_cur && g_cann[i].obj == g_esp_cur) continue;      // never draw the cannon we are riding
            double dx = g_cann[i].x - g_cam_loc[0], dy = g_cann[i].y - g_cam_loc[1], dz = g_cann[i].z - g_cam_loc[2];
            double d = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
            if (d > 80000.0) continue;
            double sx, sy;
            if (!esp_project(g_cann[i].x, g_cann[i].y, g_cann[i].z + 80.0, sw, sh, &sx, &sy)) continue;
            if (!(sx > -400.0 && sx < sw + 400.0 && sy > -400.0 && sy < sh + 400.0)) continue;
            double h = 60000.0 / (d > 200.0 ? d : 200.0);
            if (h < 16.0) h = 16.0; if (h > 110.0) h = 110.0;
            h *= S;
            double w = h * 0.62;
            double x = sx - w / 2, y = sy - h / 2;
            int riding = (g_cann[i].obj == g_esp_cur);
            c_box(x, y, w, h, 2.0 * S, riding ? cur_c : (d < 4000.0 ? near_c : far_c));
            if (labels < 10 && d < 6000.0) {
                labels++;
                snprintf(label, sizeof label, "Cannon %dm%s", (int)((d + 50.0) / 100.0), riding ? " IN" : "");
                c_text(label, x - 6 * S, y - 16 * S, 0.95 * S, wtx, shd);
            }
            drawn++;
        }
    }

    if (g_esp_veh) {                                                 // vehicles: box + "tower" beacon + distance
        LC vc = { 0.35f, 0.75f, 1.00f, 1.0f };
        for (int i = 0; i < g_nveh && drawn < 30; i++) {
            if (g_esp_cur && g_veh[i].obj == g_esp_cur) continue;       // never draw the vehicle we are riding
            double dx = g_veh[i].x - g_cam_loc[0], dy = g_veh[i].y - g_cam_loc[1], dz = g_veh[i].z - g_cam_loc[2];
            double d = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
            if (d > 100000.0) continue;
            double sx, sy;
            if (!esp_project(g_veh[i].x, g_veh[i].y, g_veh[i].z + 60.0, sw, sh, &sx, &sy)) continue;
            if (!(sx > -400.0 && sx < sw + 400.0 && sy > -400.0 && sy < sh + 400.0)) continue;
            double tx, ty; int tower = esp_project(g_veh[i].x, g_veh[i].y, g_veh[i].z + 1500.0, sw, sh, &tx, &ty);
            double h = 40000.0 / (d > 250.0 ? d : 250.0);
            if (h < 14.0) h = 14.0; if (h > 90.0) h = 90.0;
            h *= S;
            double w = h * 0.75;
            double x = sx - w / 2, y = sy - h / 2;
            if (tower && d < 8000.0 && ty > -200.0 && ty < sh + 200.0)  // the tower: a beam so it reads from far away
                c_line(sx, sy - h / 2, tx, ty, 2.0 * S, vc);
            c_box(x, y, w, h, 2.0 * S, vc);
            if (labels < 16 && d < 8000.0) {
                labels++;
                snprintf(label, sizeof label, "%s %dm", g_veh[i].nm[0] ? g_veh[i].nm : "Vehicle",
                         (int)((d + 50.0) / 100.0));
                c_text(label, x - 6 * S, y - 16 * S, 0.95 * S, wtx, shd);
            }
            drawn++;
        }
    }

    if (g_esp_plr) {                                                 // players: box + name + BOT/PLAYER + distance
        LC pc = { 0.95f, 0.95f, 1.00f, 1.0f }, botc = { 1.00f, 0.45f, 0.45f, 1.0f };
        LC skc = { 0.90f, 0.90f, 0.90f, 0.85f }, shc = { 0.25f, 0.65f, 1.00f, 1.0f };
        LC hpb = { 0.05f, 0.05f, 0.05f, 0.85f }, hpg = { 0.25f, 0.95f, 0.35f, 1.0f };
        LC hpy = { 1.00f, 0.85f, 0.20f, 1.0f }, hpr = { 1.00f, 0.25f, 0.25f, 1.0f };
        int skels = 0, hpbar = 0;
        // v130: the 24 nearest players only, nearest first. A full 100-player list used to draw every box on
        // the map, which was both unreadable and the most expensive thing on screen.
        int order[24], nord = 0;
        for (int i = 0; i < g_nplr; i++) {
            Plr *p2 = &g_plr[i];
            if (!p2->have || !p2->alive || p2->local) continue;
            if (!(p2->d2 > 0.0f) || p2->d2 > 60000.0f * 60000.0f) continue;
            int j = nord;
            while (j > 0 && g_plr[order[j-1]].d2 > p2->d2) { if (j < 24) order[j] = order[j-1]; j--; }
            if (j < 24) { order[j] = i; if (nord < 24) nord++; }
        }
        for (int oi = 0; oi < nord; oi++) {
            Plr *pl = &g_plr[order[oi]];                                // dead players disappear with their pawn
            double dx = pl->x - g_cam_loc[0], dy = pl->y - g_cam_loc[1], dz = pl->z - g_cam_loc[2];
            double d = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
            if (d > 60000.0) continue;
            double sx, sy;
            if (!esp_project(pl->x, pl->y, pl->z + 90.0, sw, sh, &sx, &sy)) continue;
            if (!(sx > -300.0 && sx < sw + 300.0 && sy > -300.0 && sy < sh + 300.0)) continue;
            double h = 110000.0 / (d > 250.0 ? d : 250.0);
            if (h < 26.0) h = 26.0; if (h > 160.0) h = 160.0;
            h *= S;
            double w = h * 0.45;
            if (w < 16.0 * S) w = 16.0 * S;
            double x = sx - w / 2, y = sy - h / 2;
            LC c = pl->bot ? botc : pc;
            if (g_esp_skel && pl->bones_ok && d < 9000.0 && skels < 8) {   // stick figure from cached bones
                skels++;
                if (!g_skel_logged) { g_skel_logged = 1; int nb = 0; for (int k = 0; k < 8; k++) if (pl->bones_ok & (1u << k)) nb++;
                                logf_("esp: drawing skeleton for %s (%d bones, layout %d, %.0f m)", pl->name, nb, g_bone_lay, d / 100.0); }
                double p[8][2]; unsigned ok = pl->bones_ok; int n = 0;
                for (int k = 0; k < 8; k++) {
                    if (!(ok & (1u << k))) continue;
                    if (esp_project(pl->bone[k][0], pl->bone[k][1], pl->bone[k][2], sw, sh, &p[k][0], &p[k][1])) n |= 1 << k;
                }
                if (n & 1) c_box(p[0][0] - 3.5 * S, p[0][1] - 3.5 * S, 7 * S, 7 * S, 1.6 * S, skc);   // head dot
                static const int pairs[7][2] = { {0,1},{1,2},{2,3},{2,4},{2,5},{3,6},{3,7} };
                for (int q = 0; q < 7; q++) {
                    int a = pairs[q][0], b = pairs[q][1];
                    if ((n & (1 << a)) && (n & (1 << b)))
                        c_line(p[a][0], p[a][1], p[b][0], p[b][1], 1.6 * S, skc);
                }
            }
            if (g_esp_snap && d < 15000.0) c_line(sw * 0.5, sh, sx, sy + h / 2, 1.0 * S, c);
            c_box(x, y, w, h, 2.0 * S, c);
            if (g_esp_hp && pl->hpk && hpbar < 18 && d < 20000.0) {     // vertical bars on the middle-left of the box
                hpbar++;
                if (!g_hp_bar_logged) {
                    g_hp_bar_logged = 1;
                    logf_("esp: health bars on (first %.0f/%.0f sh %.0f/%.0f for %s)", pl->hp, pl->hpmax, pl->sh, pl->shmax, pl->name);
                }
                double hp = pl->hpmax > 0.5f ? pl->hp / pl->hpmax : 0;
                if (hp < 0) hp = 0; if (hp > 1) hp = 1;
                double bh = h * 0.55;                                    // compact: never a full-box bar
                if (bh > 44.0 * S) bh = 44.0 * S;
                if (bh < 14.0 * S) bh = 14.0 * S;
                double bx = x + 3.0 * S;                                 // middle-left, just inside the box
                double by = y + (h - bh) * 0.5;
                LC hc2 = pl->hp_guess ? hpy : (hp > 0.5 ? hpg : (hp > 0.25 ? hpy : hpr));
                c_line(bx, by, bx, by + bh, 4.0 * S, hpb);               // dark track
                if (hp > 0.0) c_line(bx, by + bh * (1.0 - hp), bx, by + bh, 2.5 * S, hc2);
                if (pl->shmax > 0.5f) {
                    double sp = pl->sh / pl->shmax; if (sp < 0) sp = 0; if (sp > 1) sp = 1;
                    double sx2 = bx + 4.0 * S;
                    c_line(sx2, by, sx2, by + bh, 3.0 * S, hpb);
                    if (sp > 0.0) c_line(sx2, by + bh * (1.0 - sp), sx2, by + bh, 1.8 * S, shc);
                }
            }
            if (labels < 30 && d < 18000.0) {
                labels++; plabels++;
                const char *tag = !pl->bot_known ? "?" : (pl->bot ? "BOT" : "PLAYER");
                int dist = (int)((d + 50.0) / 100.0);
                if (pl->hpk)
                    snprintf(label, sizeof label, "%s [%s] %dm %s%d/%d", pl->named && pl->name[0] ? pl->name : "Player",
                             tag, dist, pl->hp_guess ? "~" : "", (int)pl->hp, (int)pl->hpmax);
                else
                    snprintf(label, sizeof label, "%s [%s] %dm", pl->named && pl->name[0] ? pl->name : "Player", tag, dist);
                c_text(label, x - 4 * S, y - 16 * S, 0.92 * S, c, shd);
            }
            drawn++;
        }
    }

    if (g_esp_chest) {                                              // chests: box + distance, grey when opened
        LC un = { 1.00f, 0.85f, 0.20f, 1.0f }, op = { 0.55f, 0.55f, 0.55f, 0.9f };
        int cl = 0;
        for (int i = 0; i < g_ncont && drawn < 70; i++) {
            double dx = g_cont[i].x - g_cam_loc[0], dy = g_cont[i].y - g_cam_loc[1], dz = g_cont[i].z - g_cam_loc[2];
            double d = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
            if (d > 12000.0) continue;
            double sx, sy;
            if (!esp_project(g_cont[i].x, g_cont[i].y, g_cont[i].z + 40.0, sw, sh, &sx, &sy)) continue;
            if (!(sx > -200.0 && sx < sw + 200.0 && sy > -200.0 && sy < sh + 200.0)) continue;
            double hh = 4500.0 / (d > 150.0 ? d : 150.0);
            if (hh < 10.0) hh = 10.0; if (hh > 60.0) hh = 60.0;
            hh *= S;
            double ww = hh * 1.1;
            int opened = g_cont[i].open || was_visited(g_cont[i].obj);
            c_box(sx - ww / 2, sy - hh / 2, ww, hh, 1.6 * S, opened ? op : un);
            if (labels < 40 && cl < 10 && d < 6000.0 && !opened) {
                cl++; labels++;
                snprintf(label, sizeof label, "Chest %dm", (int)((d + 50.0) / 100.0));
                c_text(label, sx - ww / 2, sy - hh / 2 - 14 * S, 0.85 * S, un, shd);
            }
            drawn++;
        }
    }
}

static void do_goto_cannon(void) {
    find_super_off();
    uint64_t pawn = get_pawn(); if (!pawn) { logf_("gotocannon: no pawn"); return; }
    double me[3]; get_vec(pawn, A_FN_GETLOC, me);
    scan_world(1);
    if (!g_ncann) { logf_("gotocannon: no cannon actor found");  return; }
    uint64_t myveh = get_vehicle(pawn);
    int b = -1; double bs = 1e300, bd = 0;
    // Nearest cannon ON THE SAME LEVEL. Straight 3D distance used to pick a prop 3000 uu below the island as
    // "nearest"; a vertical difference now counts 4x, the cannon we are riding is never a destination, and
    // anything under the floor is ignored. Every candidate is logged so a wrong pick can be diagnosed.
    logf_("gotocannon: %d cannons, riding %lx", g_ncann, myveh);
    for (int i = 0; i < g_ncann; i++) {
        if (!plausible(g_cann[i].obj) || (rd64(g_cann[i].obj + 8) & 0x18000)) continue;
        if (g_cann[i].z < -2500.0) continue;                         // under the island floor
        double dx = g_cann[i].x - me[0], dy = g_cann[i].y - me[1], dz = g_cann[i].z - me[2];
        double d = __builtin_sqrt(dx*dx + dy*dy + dz*dz);
        double score = dx*dx + dy*dy + dz*dz * 4.0;
        if (g_cann[i].obj == myveh) score += 1e15;                   // current ride is not a destination
        char cn[96] = "?"; class_name(rd64(g_cann[i].obj + OBJ_CLASS), cn, sizeof cn);
        logf_("  #%d %lx %s %.0f uu (dz %.0f)", i, g_cann[i].obj, cn, d, dz);
        if (score < bs) { bs = score; b = i; bd = d; }
    }
    if (b < 0) { logf_("gotocannon: no usable cannon");  return; }
    fly_gravity(0, 0); fly_nograv_all(1); fly_noclip_all();
    g_tn = 1; g_ti = 0; g_tphase = 0; g_topen = 0; g_tskip = 0; g_tret = 0; g_tmode = 1;
    g_tour[0][0] = g_cann[b].x; g_tour[0][1] = g_cann[b].y; g_tour[0][2] = g_cann[b].z;
    g_tobj[0] = 0; g_tcls[0] = 0;
    g_tauto = 1; g_tnext = now_ms(); g_flypos_ok = 0; g_flyn = 0; g_have_good = 0; g_tpawn = pawn; g_hover_ok = 0;
    rotation_forget(); g_reseat_n = 0; g_reseat_next = 0;
    logf_("gotocannon: flying to %lx at %.0f %.0f %.0f (%.0f uu)", g_cann[b].obj,
          g_cann[b].x, g_cann[b].y, g_cann[b].z, bd);
}

static int ui_resolve(void) {
    if (g_ps_state) return g_ps_state > 0;
    g_ps_state = -1;
    g_ps_name = find_fname_index("PrintString");
    if (!g_ps_name) { logf_("ui: no PrintString name"); return 0; }
    g_ps_fn = find_object_with_name(g_ps_name, "Function");
    if (!g_ps_fn) { logf_("ui: PrintString UFunction not found (name %u)", g_ps_name); return 0; }
    g_ps_key = g_pc ? (uint32_t)frd64(g_pc + OBJ_NAME) : 0;
    if (!g_ps_key) g_ps_key = g_ps_name;
    g_ps_state = 1;
    logf_("ui: PrintString %lx key %u", g_ps_fn, g_ps_key);
    ui_unlock_messages();
    return 1;
}

static void ui_msg(const char *s, int number, float dur, float r, float g, float b) {
    int n = 0;
    while (s[n] && n < 760) { g_ui_wbuf[n] = (uint16_t)(uint8_t)s[n]; n++; }
    g_ui_wbuf[n] = 0;
    if (!g_ps_key) g_ps_key = g_ps_name;
    uint8_t p[0x40] __attribute__((aligned(16))); memset(p, 0, sizeof p);
    *(uint64_t *)(p + 0x00) = g_pc;                                   // WorldContextObject
    *(uint64_t *)(p + 0x08) = (uint64_t)g_ui_wbuf;                    // InString (FString data)
    *(int32_t *)(p + 0x10) = n + 1;                                   // Num
    *(int32_t *)(p + 0x14) = 800;                                     // Max
    p[0x18] = 1;                                                      // bPrintToScreen
    p[0x19] = 0;                                                      // bPrintToLog
    *(float *)(p + 0x1c) = r; *(float *)(p + 0x20) = g; *(float *)(p + 0x24) = b; *(float *)(p + 0x28) = 1.0f;
    *(float *)(p + 0x2c) = dur;                                       // Duration
    *(int32_t *)(p + 0x30) = (int32_t)g_ps_key;                       // Key (FName index)
    *(int32_t *)(p + 0x34) = number;                                  // Key (FName number)
    call_pe2(g_pc, g_ps_fn, p);
    g_ps_calls++;
    g_ps_ok = 1;
}

static void ui_refresh(void) {
    if (!ui_resolve()) return;
    if (!g_pc && !resolve_pc()) { if (g_ps_calls == 0) logf_("ui: no PlayerController for PrintString"); return; }
    char txt[900]; int n = 0;
    n += snprintf(txt + n, sizeof txt - n, ">,< // funcult  %d/%d\n", g_msel + 1, MENU_N);
    for (int i = 0; i < MENU_N && n < (int)sizeof txt - 90; i++) {
        char line[80]; menu_item(i, line, sizeof line);
        n += snprintf(txt + n, sizeof txt - n, "%c %s\n", i == g_msel ? '>' : ' ', line);
    }
    n += snprintf(txt + n, sizeof txt - n, "UP/DOWN move  X select  O close");
    ui_msg(txt, 0, 0.9f, 1.0f, 1.0f, 1.0f);
    if (g_cannon_paused && g_need_cannon) ui_msg("PAUSED - get in a cannon", 1, 1.2f, 1.0f, 0.35f, 0.35f);
}

static uint64_t find_hud(void) {
    if (!g_pc && !resolve_pc()) return g_hud_obj;
    const uint64_t offs[3] = { 0x318, 0x328, 0x308 };
    for (int i = 0; i < 3; i++) {
        uint64_t h = frd64(g_pc + offs[i]);            // frd64: a stale controller must never fault here
        if (!plausible(h)) continue;
        uint64_t cls = frd64(h + OBJ_CLASS);
        if (g_hud_cls && is_sub_of(cls, g_hud_cls)) {
            if (h != g_hud_obj) {                       // lobby HUD is replaced when the match starts
                char nm[64] = "?";
                class_name(cls, nm, sizeof nm);
                logf_("ui: hud %lx (%s) at pc+%lx canvas %lx", h, nm, offs[i], frd64(h + g_canvas_off));
                g_hud_obj = h;
            }
            return h;
        }
    }
    return g_hud_obj;
}

static void ui_stats_log(void);
static int install_hud_hook(void);
static int g_hud_hooked;
static uint64_t g_hud_vt;
static void ui_tick(void) {
    static uint64_t last = 0;
    uint64_t t = now_ms();
    static int first = 0;
    if (!first) { first = 1; logf_("ui: first tick (pc %lx eng %lx)", g_pc, g_eng); }
    if (t - last < 5000) return;              // diagnostics + resource resolution on the game thread
    last = t;
    int have_pc = resolve_pc();               // the controller is replaced when a match loads
    if (have_pc) find_hud();                  // keep g_hud_obj in sync with pc->MyHUD
    if (!g_font) g_font = find_font();
    if (have_pc && (!g_hud_hooked || (g_hud_obj && frd64(g_hud_obj) != g_hud_vt)) && (ui_wanted() || ui_mark()))
        install_hud_hook();
    if (have_pc) {
        resolve_extras();                     // IsAlreadySearched + ExecuteConsoleCommand (name lookup, once)
        // (re)apply the graphics mode when a match is up (the world must exist for console commands)
        if (g_perf_pc != g_pc) {
            fly_forget();                     // the old world's pawn/cannon no longer exist
            if (get_pawn()) { g_perf_pc = g_pc; g_perf_on_done = 1; perf_apply(); }
        }
    }
    ui_stats_log();
}

static int ui_canvas_self(uint64_t self) {
    if (!self) return 0;
    if (!g_canvas_cls) {
        uint32_t idx = find_fname_index("canvas");
        if (idx) g_canvas_cls = find_object_with_name(idx, "Class");
        if (!g_canvas_cls) { g_canvas_cls = 1; return 0; }
    }
    if (g_canvas_cls == 1) return 0;
    return frd64(self + OBJ_CLASS) == g_canvas_cls;
}

// ---- v30: draw from AHUD::PostRender, the only live canvas in this title ---------------------------------
// Established from the live RAM dump (ps4-unreal-dumper, CUSA07022):
//   * GameEngine->GameViewport + 0x28 is the FViewportClient sub-object. Its vtable slot 5 (offset 0x28) is
//     an adjustor thunk (`add rdi, -0x28; call`) into UGameViewportClient::Draw(Viewport, FCanvas*), so the
//     Draw hook forwards (self, viewport, canvas) and `canvas` really is the frame's FCanvas*.
//   * Inside Draw the engine stores the frame UCanvas* into AHUD::Canvas (hud+0x2b8) and immediately calls
//     HUD vtable slot 0x788 = AHUD::PostRender; that function itself tests `[hud+0x2b8] != 0`.
//   * The engine binds the FCanvas* to the UCanvas at UCanvas+0x2e0 (`mov [ucanvas+0x2e0], rdx` at
//     0x2600010 and 0x2601fdc).
// PostRender is therefore the one place per frame where a live, engine-bound UCanvas exists. The shipped
// HUD is UMG (ReceiveDrawHUD is never dispatched: v27) and on-screen messages are compiled out (v28), so
// this is the render slot to use. We hook the live HUD's vtable entry, call the original (which queues the
// HUD's own canvas items), then draw into AHUD::Canvas; the engine flushes that canvas after Draw.
// The GVC::Draw hook stays installed as a diagnostic and as a fallback renderer if PostRender is missed.
#define GVC_DRAW_SLOT   0x28              // FViewportClient vtable slot 5 = Draw
#define HUD_RENDER_SLOT 0x788             // AHUD vtable slot = PostRender
#define UCANVAS_FCANVAS 0x2e0             // UCanvas::Canvas (FCanvas*)
static uint64_t g_gvc = 0, g_gvc_vt = 0, g_draw_slot = 0, g_draw_orig = 0;
static int g_draw_hooked = 0;
static uint64_t g_draw_frames = 0, g_draw_canvas = 0, g_draw_nodraw = 0, g_draw_t0 = 0;
static uint64_t g_hud_slot = 0, g_hud_orig = 0, g_hud_cv = 0, g_hud_frames = 0, g_post_ok = 0;
typedef struct { uint64_t vt, slot, orig; } HudPatch;
static HudPatch g_hpat[8]; static int g_nhpat = 0;
static uint64_t hud_orig_of(uint64_t vt) {
    for (int i = 0; i < g_nhpat && i < 8; i++) if (g_hpat[i].vt == vt) return g_hpat[i].orig;
    return 0;
}
static int patch_bytes(uint64_t at, const void *src, uint64_t n) {
    uint64_t first = at & ~0xfffULL, last = (at + n - 1) & ~0xfffULL;
    void *st = 0, *en = 0; int p1 = 0, p2 = 0;
    if (!n) return 1;
    if (sceKernelQueryMemoryProtection((void *)first, &st, &en, &p1) != 0) return 0;
    if (last != first && sceKernelQueryMemoryProtection((void *)last, &st, &en, &p2) != 0) return 0;
    if (sceKernelMprotect((void *)first, (size_t)(last - first + 0x1000), 0x7) != 0) return 0;
    memcpy((void *)at, src, (size_t)n);
    sceKernelMprotect((void *)first, 0x1000, p1);                    // exact original flags per page
    if (last != first) sceKernelMprotect((void *)last, 0x1000, p2);
    return 1;
}
static int patch_qword(uint64_t slot, uint64_t value, uint64_t *out_old) {
    uint64_t old = *(volatile uint64_t *)slot;
    if (!patch_bytes(slot, &value, 8)) return 0;
    if (out_old) *out_old = old;
    return 1;
}
static int g_hud_fail = 0;
static volatile int g_rendering = 0;

typedef void (*gvc_draw_t)(void *self, void *viewport, void *canvas);   // Draw(FViewport*, FCanvas*)
typedef void (*hud_render_t)(void *self);                               // AHUD::PostRender()

static int ui_wanted(void) { return g_menu || (g_ingame && (g_esp || g_esp_plr || g_esp_veh || g_esp_chest)) || (g_cannon_paused && g_need_cannon); }
static int ui_mark(void) { return g_draw_t0 && now_ms() - g_draw_t0 < 25000; }

static void draw_watermark(uint64_t cv) {
    int sw = *(volatile int *)(cv + 0x40);
    double S = (sw >= 1280 && sw <= 7680) ? sw / 1920.0 : 1.0;
    LC bg = { 0.05f, 0.32f, 0.10f, 0.88f }, br = { 0.25f, 1.0f, 0.35f, 1.0f };
    LC wtx = { 1, 1, 1, 1 }, shd = { 0, 0, 0, 1 };
    c_line(20 * S, 24 * S, 250 * S, 24 * S, 24 * S, bg);
    c_box(20 * S, 12 * S, 230 * S, 24 * S, 2 * S, br);
    c_text(">,< // funcult  v133", 28 * S, 18 * S, 1.0 * S, wtx, shd);
}

// Draw the menu/banner/watermark on a live UCanvas (its FCanvas* must already be bound by the engine).
static int ui_draw_on(uint64_t cv, uint64_t hud) {
    if (!g_font) g_font = find_font();
    if (!g_cfn_text || !g_cfn_line) resolve_canvas_fns(cv);   // ESP can be the first thing that draws
    if (!g_cfn_text || !g_cfn_line || !g_font) {
        g_draw_nodraw++;
        if (g_draw_nodraw < 6) logf_("ui: canvas %lx not ready text %lx line %lx font %lx", cv, g_cfn_text, g_cfn_line, g_font);
        return 0;
    }
    ph(PH_DRAW);
    g_draw_canvas = cv;
    g_canvas = cv;                                     // K2_Draw* always need THIS frame's canvas
    g_rendering = 1;                                   // our K2_Draw* calls must not run game logic in pe_hook
    uint64_t c0 = rcyc();
    if (g_menu) menu_render(cv);
    uint64_t c1 = rcyc();
    if (g_esp || g_esp_plr || g_esp_veh || g_esp_chest) esp_draw(cv);
    uint64_t c2 = rcyc();
    if (g_menu) { g_cy_menu += c1 - c0; if (c1 - c0 > g_cy_menu_max) g_cy_menu_max = c1 - c0; g_n_menu++; }
    if (g_esp || g_esp_plr || g_esp_veh || g_esp_chest) { g_cy_esp += c2 - c1; if (c2 - c1 > g_cy_esp_max) g_cy_esp_max = c2 - c1; g_n_esp++; }
    if (g_cannon_paused && g_need_cannon) banner_render(cv);
    if (ui_mark()) { g_canvas = cv; draw_watermark(cv); }
    g_rendering = 0;
    g_draw_ok++;
    return 1;
}

// primary renderer: AHUD::PostRender, called every frame from Draw right after AHUD::Canvas is assigned
static void hud_render_hook(void *self) {
    if (!plausible((uint64_t)self)) return;             // never touch a half-freed HUD object
    ph(PH_HUD);
    // Dispatch the ORIGINAL of the class this object actually belongs to. Several HUD classes
    // (lobby `FortUIBaseClass`, `FortUIPvP`, ...) all get patched over a session and each has its own
    // PostRender implementation; calling another class's one on this object crashes the title.
    uint64_t vt = frd64((uint64_t)self);
    uint64_t orig = vt ? hud_orig_of(vt) : 0;
    if (orig && !readable(orig, 8)) orig = 0;           // a stale/never-mapped original is never called
    if (orig) ((hud_render_t)orig)(self);
    if ((uint64_t)self != g_hud_obj) { ph(PH_IDLE); return; }  // class vtable is shared: only draw our HUD
    if (!ui_wanted() && !ui_mark()) {
        // Nothing has been drawn for ~10 s: take the hook back out so the engine's own transitions (map
        // load, lobby return) run completely untouched vtable entries. auto_tick re-arms it on demand.
        if (g_nhpat && ++g_hud_idle_frames > 120) { g_hud_idle_frames = 0; hud_unpatch_all(); }
        ph(PH_IDLE);
        return;
    }
    g_hud_idle_frames = 0;
    uint64_t f = ++g_hud_frames;
    uint64_t cv = frd64((uint64_t)self + g_canvas_off);
    if (!cv || !plausible(cv) || !readable(cv, 0x390)) {
        g_draw_nodraw++;
        if (f < 8) logf_("ui: postrender %lu: hud %lx canvas %lx", (unsigned long)f, (uint64_t)self, cv);
        ph(PH_IDLE);
        return;
    }
    if (g_canvas_cls > 1 && frd64(cv + OBJ_CLASS) != g_canvas_cls) {
        if (f < 4) logf_("ui: postrender canvas %lx class %lx (expected %lx) - skipped", cv, frd64(cv + OBJ_CLASS), g_canvas_cls);
        ph(PH_IDLE);
        return;                                            // a half-freed canvas is not worth a crash
    }
    g_hud_cv = cv;
    if (ui_draw_on(cv, (uint64_t)self)) {
        g_post_ok++;
        if (f == 1 || f == 120 || f == 600)
            logf_("ui: postrender drawing on canvas %lx (hud %lx, fcanvas %lx, frame %lu)", cv, (uint64_t)self, frd64(cv + UCANVAS_FCANVAS), (unsigned long)f);
    }
    ph(PH_IDLE);
}

// Drop every pointer that belongs to the current world's HUD/canvas. The class vtables stay patched
// (that is what keeps the hook alive) but nothing is drawn on until the new match's HUD is found again.
static void hud_forget(void) {
    g_hud_obj = 0; g_hud_hooked = 0; g_hud_vt = 0; g_hud_slot = 0; g_hud_orig = 0;
    g_hud_cv = 0; g_canvas = 0; g_draw_canvas = 0;
}

// Remove every vtable patch we own. Called when no feature has needed the HUD for a while: a pristine
// vtable is the safest possible state while the engine tears a level down and builds the next one.
static void hud_unpatch_all(void) {
    int n = 0;
    for (int i = 0; i < g_nhpat && i < 8; i++) {
        uint64_t slot = g_hpat[i].slot;
        if (!slot || !readable(slot, 8)) continue;
        if (frd64(slot) != (uint64_t)&hud_render_hook) continue;   // not ours any more - leave it alone
        if (patch_qword(slot, g_hpat[i].orig, 0)) n++;
    }
    if (g_nhpat) logf_("ui: %d hud hook(s) removed (idle/teardown)", n);
    g_nhpat = 0; g_hud_hooked = 0; g_hud_vt = 0; g_hud_slot = 0; g_hud_orig = 0;
    g_hud_idle_frames = 0;
}

static int install_hud_hook(void) {
    if (cfg_off || cfg_nohud) return 0;
    if (!g_hud_obj && !find_hud()) return 0;
    if (!plausible(g_hud_obj)) { g_hud_obj = 0; return 0; }
    if (g_hud_hooked && frd64(g_hud_obj) == g_hud_vt) return 1;  // same class table, hook still valid
    g_hud_hooked = 0;
    uint64_t vt = frd64(g_hud_obj);
    if (!plausible(vt) || !readable(vt + HUD_RENDER_SLOT, 8)) { if (!g_hud_fail++) logf_("ui: hud %lx vtable %lx unreadable", g_hud_obj, vt); return 0; }
    // never patch a class that is not an AHUD: slot 0x788 would be past the end of a smaller vtable and the
    // write would corrupt unrelated memory
    uint64_t hcls = frd64(g_hud_obj + OBJ_CLASS);
    if (!plausible(hcls) || !class_in_chain_named(hcls, "HUD")) {
        if (!g_hud_fail++) logf_("ui: hud %lx class %lx is not an AHUD - not hooking", g_hud_obj, hcls);
        return 0;
    }
    uint64_t e = frd64(vt + HUD_RENDER_SLOT);
    if (e == (uint64_t)&hud_render_hook) { g_hud_vt = vt; g_hud_hooked = 1; return 1; }   // already patched
    if (!plausible(e) || !readable(e, 8)) { if (!g_hud_fail++) logf_("ui: hud vtable %lx slot+%lx = %lx (not code)", vt, (uint64_t)HUD_RENDER_SLOT, e); return 0; }
    uint64_t slot = vt + HUD_RENDER_SLOT;
    uint64_t old = 0;
    if (!patch_qword(slot, (uint64_t)&hud_render_hook, &old)) {
        if (!g_hud_fail++) logf_("ui: hud hook cannot patch slot %lx", slot);
        return 0;
    }
    g_hud_orig = e;
    if (!hud_orig_of(vt)) {                                   // remember each class's own PostRender once
        if (g_nhpat < 8) { g_hpat[g_nhpat].vt = vt; g_hpat[g_nhpat].slot = slot; g_hpat[g_nhpat].orig = old; g_nhpat++; }
        else { static int full; if (!full) logf_("ui: hud patch table full (8 classes) - no orig recorded"); }
    }
    g_hud_vt = vt; g_hud_slot = slot; g_hud_hooked = 1;
    logf_("ui: postrender hooked: hud %lx vt %lx slot %lx orig %lx", g_hud_obj, vt, slot, e);
    return 1;
}

// fallback renderer: after UGameViewportClient::Draw the engine has bound AHUD::Canvas to this frame's FCanvas
static void draw_hook(void *self, void *viewport, void *canvas) {
    if (g_draw_orig) ((gvc_draw_t)g_draw_orig)(self, viewport, canvas);
    uint64_t n = ++g_draw_frames;
    if (n <= 3) logf_("ui: draw hook %lu: gvc %lx viewport %lx fcanvas %lx hud %lx hudcanvas %lx",
                      (unsigned long)n, (uint64_t)self, (uint64_t)viewport, (uint64_t)canvas,
                      g_hud_obj, g_hud_obj ? rd64(g_hud_obj + g_canvas_off) : 0);
    if (g_hud_hooked && g_post_ok) return;             // PostRender already draws
    if (!ui_wanted() && !ui_mark()) return;
    if (!g_hud_obj) return;
    uint64_t cv = rd64(g_hud_obj + g_canvas_off);
    if (!cv) { g_draw_nodraw++; if (n < 8) logf_("ui: draw %lu: hud %lx has no canvas", (unsigned long)n, g_hud_obj); return; }
    ui_draw_on(cv, g_hud_obj);
    if (n == 1 || n == 50) logf_("ui: draw fallback on canvas %lx (frame %lu)", cv, (unsigned long)n);
}

static int install_draw_hook(void) {
    if (g_draw_hooked) return 1;
    if (!resolve_engine()) return 0;
    uint64_t gvc = rd64(g_eng + 0xb70);
    if (!plausible(gvc) || !readable(gvc, 0x100)) { logf_("draw: GameViewport %lx not readable", gvc); return 0; }
    char cn[64] = "?";
    class_name(rd64(gvc + OBJ_CLASS), cn, sizeof cn);
    if (!strstr(cn, "ViewportClient")) logf_("draw: warning, GEngine+0xb70 is a %s", cn);
    uint64_t vt = 0, slot = 0;
    for (uint64_t off = 0x08; off < 0x100; off += 8) {          // FViewportClient sub-object vtable
        uint64_t q = rd64(gvc + off);
        if (!plausible(q) || q < 0xef00000ULL || (q & 7)) continue;
        uint64_t e = rd64(q + GVC_DRAW_SLOT);
        if (!plausible(e) || !readable(e, 16)) continue;
        const uint8_t *b = (const uint8_t *)e;
        // adjustor thunk: push rbp; mov rbp,rsp; add rdi, -off; call rel32  (tell-tale for a sub-object vtable)
        if (b[0] == 0x55 && b[1] == 0x48 && b[2] == 0x89 && b[3] == 0xe5 && b[4] == 0x48 && b[5] == 0x83 && b[6] == 0xc7 &&
            ((b[7] + off) & 0xff) == 0 && b[8] == 0xe8) {
            vt = q; slot = q + GVC_DRAW_SLOT; logf_("draw: sub-object vtable at gvc+%lx (adjust -0x%lx)", off, off);
            break;
        }
    }
    if (!slot) { logf_("draw: no Draw slot found on %s gvc %lx", cn, gvc); return 0; }
    uint64_t old_draw = 0;
    if (!patch_qword(slot, (uint64_t)&draw_hook, &old_draw)) { logf_("draw: cannot patch slot %lx", slot); return 0; }
    g_draw_orig = old_draw;
    g_gvc = gvc; g_gvc_vt = vt; g_draw_slot = slot; g_draw_hooked = 1; g_draw_t0 = now_ms();
    logf_("draw: hooked %s gvc %lx vt %lx slot %lx orig %lx", cn, gvc, vt, slot, g_draw_orig);
    return 1;
}

static void ui_stats_log(void) {
    logf_("ui: drawhook %d frames %lu | postrender %d/%lu frames %lu slot %lx | drawn %d nodraw %lu canvas %lx hud %lx font %lx",
          g_draw_hooked, (unsigned long)g_draw_frames, g_hud_hooked, (unsigned long)g_post_ok, (unsigned long)g_hud_frames,
          g_hud_slot, g_draw_ok, (unsigned long)g_draw_nodraw, g_hud_cv ? g_hud_cv : g_draw_canvas, g_hud_obj, g_font);
    // v80: what our own drawing costs per frame. done/ms gives the HUD frame rate, the rest is our share of it.
    uint64_t t = now_ms(), c = rcyc();
    uint64_t dm = t - g_stat_ms0, dc = c - g_stat_cyc0;
    if (g_stat_ms0 && dm) {
        uint64_t mhz = dc / (dm * 1000ULL);                 // cycles per microsecond
        uint64_t fr = g_draw_ok - g_stat_draw0;
        uint64_t us = mhz ? 1 : 0;
        #define CY2US(x) (mhz ? (uint64_t)((x) / mhz) : 0)
        logf_("perf: %lu draw/s | menu %lu us avg %lu max (%u) | esp %lu us avg %lu max (%u) | scan %lu us avg %lu max (%u) | cpu %lu MHz",
              (unsigned long)(fr * 1000 / dm),
              (unsigned long)CY2US(g_n_menu ? g_cy_menu / g_n_menu : 0), (unsigned long)CY2US(g_cy_menu_max), g_n_menu,
              (unsigned long)CY2US(g_n_esp ? g_cy_esp / g_n_esp : 0), (unsigned long)CY2US(g_cy_esp_max), g_n_esp,
              (unsigned long)CY2US(g_n_scan ? g_cy_scan / g_n_scan : 0), (unsigned long)CY2US(g_cy_scan_max), g_n_scan,
              (unsigned long)mhz);
        #undef CY2US
        (void)us;
    }
    g_stat_ms0 = t; g_stat_cyc0 = c; g_stat_draw0 = g_draw_ok;
    g_cy_menu = g_cy_menu_max = g_cy_esp = g_cy_esp_max = g_cy_scan = g_cy_scan_max = 0;
    g_n_menu = g_n_esp = g_n_scan = 0;
    (void)g_stat_frame0;
}


static void pad_install_tapout(void);
static void run_special(int sp) {
    if (sp == 1) { if (g_tauto) logf_("tpchest: tour running"); else do_tour_start(1, 1); }
    else if (sp == 2) do_chests_start(0);
    else if (sp == 7) do_chests_start(1);
    else if (sp == 8) do_near();
    else if (sp == 9) do_servercheat();
    else if (sp == 11) { double r = 0; for (const char *q = g_sc_text; *q >= '0' && *q <= '9'; q++) r = r * 10 + (*q - '0'); if (r >= 50 && r <= 100000) g_aura_r = r; logf_("aura radius = %.0f", g_aura_r);  }
    else if (sp == 10) { g_aura = !g_aura; g_aura_next = 0; g_aura_n = 0; g_aura_scan = 0; g_aura_retry = 0; g_aura_log = 0; logf_("aura %s", g_aura ? "ON" : "OFF"); }
    else if (sp == 3) do_probe();
    else if (sp == 14) {
        g_menu = !g_menu; g_msel = 0; g_menu_since = now_ms();
        if (g_menu) pad_install_tapout();           // patch libScePad from the game thread, on first open
        else { g_tapout_until = now_ms() + 350; logf_("tapout: %u neutralised reads", g_pad_neutralised); }
        logf_("menu: %s", g_menu ? "open" : "close");
    }
    else if (sp == 12) { if (g_tauto) { g_tauto = 0; g_hover_ok = 0; tour_stop_gravity(0); logf_("tour: stopped");  } else do_tour_start(0, 1); }   // 1 = fly back to the start when the tour is done
    else if (sp == 13) {                                    // "speed 2.5" = 2.5 s / chest, "speed 2500" = ms
        double r = 0, div = 1; int frac = 0, digits = 0;
        for (const char *q = g_sc_text; *q; q++) {
            if (*q >= '0' && *q <= '9') {
                digits++;
                if (frac) { div *= 10; r += (*q - '0') / div; } else r = r * 10 + (*q - '0');
            } else if (*q == '.' && !frac) frac = 1;
            else if (digits) break;
        }
        if (digits) {
            if (r < 10.0) g_tstep = (uint64_t)(r * 1000.0 + 0.5);
            else if (r <= 20000.0) g_tstep = (uint64_t)r;
        }
        if (g_tstep > 20000) g_tstep = 20000;
        logf_("stop time = %.1f s (%lu ms, applies to the chest tour and the player tower)", g_tstep / 1000.0, (unsigned long)g_tstep);
    }
    else if (sp == 4) { g_tauto = 0; g_hover_ok = 0; tour_stop_gravity(0); logf_("stopped: flight off, gravity/collision restored"); }
    else if (sp == 5) { g_nvis = 0; logf_("visited list cleared"); }
    else if (sp == 6) { g_trace = !g_trace; g_trace_n = 0; memset(g_tc, 0, sizeof g_tc); logf_("trace %s - open one chest by hand now", g_trace ? "ON" : "OFF");  }
    else if (sp == 15) { g_need_cannon = !g_need_cannon; logf_("cannon check %s", g_need_cannon ? "ON" : "OFF");  }
    else if (sp == 16) {
        g_perf = (g_perf + 1) % 3;                                  // OFF -> PERF -> LOW
        logf_("perf: mode %d (%s)", g_perf, g_perf == 0 ? "OFF" : (g_perf == 1 ? "PERF" : "LOW"));
        perf_apply(); g_perf_pc = g_pc; g_perf_on_done = 1;
    }
    else if (sp == 17) {
        g_esp = !g_esp; g_esp_logged = 0; g_cam_logged = 0;
        if (g_esp) g_cann_scan = 0;                 // full scan on the next game tick, not in the input path
        logf_("esp cannons: %s (%d cached)", g_esp ? "ON" : "OFF", g_ncann);
    }
    else if (sp == 19) {
        g_esp_plr = !g_esp_plr;
        if (g_esp_plr && !g_nps_cache) g_cann_scan = 0;               // a walk fills the player-state cache
        logf_("esp players: %s (%d tracked, %d cached)", g_esp_plr ? "ON" : "OFF", g_nplr, g_nps_cache);
    }
    else if (sp == 20) {
        g_esp_veh = !g_esp_veh; g_esp_logged = 0;
        if (g_esp_veh) g_cann_scan = 0;
        logf_("esp vehicles: %s (%d cached)", g_esp_veh ? "ON" : "OFF", g_nveh);
    }
    else if (sp == 21) {
        g_instant = !g_instant;
        logf_("tour mode: %s", g_instant ? "INSTANT (teleport straight onto each chest)" : "FLY (constant speed)");
    }
    else if (sp == 22) { g_esp_hp = !g_esp_hp; logf_("esp health bars: %s", g_esp_hp ? "ON" : "OFF"); }
    else if (sp == 23) {
        g_esp_skel = !g_esp_skel;
        if (g_esp_skel) { uint64_t p = get_pawn(); if (p) resolve_esp_fns(p); }
        logf_("esp skeleton: %s (socket %lx, bones %lu %lu %lu %lu %lu %lu %lu %lu)", g_esp_skel ? "ON" : "OFF", g_fn_socket,
              (unsigned long)g_bone_idx[0], (unsigned long)g_bone_idx[1], (unsigned long)g_bone_idx[2], (unsigned long)g_bone_idx[3],
              (unsigned long)g_bone_idx[4], (unsigned long)g_bone_idx[5], (unsigned long)g_bone_idx[6], (unsigned long)g_bone_idx[7]);
    }
    else if (sp == 24) {
        g_esp_chest = !g_esp_chest; g_chest_scan = 0;
        logf_("esp chests: %s (%d cached)", g_esp_chest ? "ON" : "OFF", g_ncont);
    }
    else if (sp == 25) { g_esp_snap = !g_esp_snap; logf_("esp snaplines: %s", g_esp_snap ? "ON" : "OFF"); }
    else if (sp == 26) {
        g_aim = !g_aim;
        logf_("aimbot: %s (hold L2, part %s, fov %d deg, smooth %d)", g_aim ? "ON" : "OFF",
              AIM_PART_NAME[g_aim_part], g_aim_fov, g_aim_smooth);
    }
    else if (sp == 27) { g_aim_part = (g_aim_part + 1) % AIM_PARTS; logf_("aimbot part: %s", AIM_PART_NAME[g_aim_part]); }
    else if (sp == 28) {
        static const int fv[] = { 3, 6, 12, 20, 35 };
        int k = 0; for (int i = 0; i < 5; i++) if (g_aim_fov == fv[i]) { k = (i + 1) % 5; break; }
        g_aim_fov = fv[k]; logf_("aimbot fov: %d deg", g_aim_fov);
    }
    else if (sp == 32) {
        g_aim_smooth = (g_aim_smooth + 1) % 11;
        logf_("aimbot smooth: %d (0 = instant)", g_aim_smooth);
    }
    else if (sp == 33) {
        static const int fv[] = { 0, 80, 90, 100, 110, 120 };
        int k = 0;
        for (int i = 0; i < 6; i++) if (g_fov_chg == fv[i]) { k = (i + 1) % 6; break; }
        if (!g_fov_chg && fv[k]) g_fov_orig = 0;          // capture the engine's own value on first use
        g_fov_chg = fv[k];
        logf_("camera fov: %s", g_fov_chg ? "set" : "OFF");
        if (g_fov_chg) fov_tick();
    }
    else if (sp == 36) {
        g_seatlock = !g_seatlock;
        g_sl_blocks = 0;
        if (!g_seatlock) { g_sl_veh_act = g_sl_pawn_act = g_sl_pc_act = 0; g_sl_stage = 0; }
        logf_("seat lock: %s", g_seatlock ? "ON" : "OFF");
    }
    else if (sp == 35) {
        g_waterguard = !g_waterguard;
        if (!g_waterguard) { g_wg_veh = 0; g_wg_lift = 0; }
        logf_("water guard: %s", g_waterguard ? "ON" : "OFF");
    }
    else if (sp == 34) {
        g_kickguard = !g_kickguard;
        if (!g_kickguard) kick_guard_forget();
        logf_("anti cannon kick: %s", g_kickguard ? "ON" : "OFF");
    }
    else if (sp == 37) {
        if (g_tauto && g_tmode == 2) do_tower_stop();
        else if (g_tauto) logf_("tower: a chest tour is running - stop it first");
        else do_tower_start();
    }
    else if (sp == 38) {
        int r = 0, digits = 0;
        for (const char *q = g_sc_text; *q; q++) {
            if (*q >= '0' && *q <= '9') { r = r * 10 + (*q - '0'); digits++; if (r > 100000) break; }
            else if (digits) break;
        }
        if (digits && r >= 200 && r <= 5000) g_tower_h = r;
        logf_("tower height = %d uu above the player", g_tower_h);
    }
    else if (sp == 39) {
        g_ghost_on = !g_ghost_on;
        if (!g_ghost_on) cannon_ghost_restore();
        logf_("cannon ghost: %s", g_ghost_on ? "ON (cannon hidden while riding)" : "OFF (cannon visible)");
    }
    else if (sp == 18) do_goto_cannon();
}

static void run_cmd(const Fn *f) {
    if (!g_stub) { logf_("%s: no ProcessEvent stub", f->name); return; }
    if (!resolve_pc()) { logf_("%s: controller chain not resolved", f->name); return; }
    if (f->sp) { run_special(f->sp); return; }
    uint64_t cm = find_cm();
    if (!f->on_controller && !cm) {
        logf_("%s: no CheatManager yet, calling EnableCheats first", f->name);
        call_pe(g_pc, FNS[0].addr);
        cm = find_cm();
        if (!cm) { dump_pc_words();
             logf_("%s: CheatManager still null at pc+%x (offset wrong or EnableCheats refused)", f->name, PC_CHEATMANAGER); return; }
    }
    if (!f->on_controller) logf_("%s: CheatManager %lx class %lx", f->name, cm, rd64(cm + OBJ_CLASS));
    uint64_t self = f->on_controller ? g_pc : cm;
    call_pe(self, f->addr);
    logf_("%s: called on %lx", f->name, self);
}

// queue (socket/pad thread -> game thread)
static const Fn *g_queue[QUEUE_SIZE];
static volatile int g_head = 0, g_tail = 0;
static int push_cmd(const Fn *f) {
    int t = ld(&g_tail);
    if (((t + 1) % QUEUE_SIZE) == ld(&g_head)) return 0;
    g_queue[t] = f;
    st(&g_tail, (t + 1) % QUEUE_SIZE);
    return 1;
}
static void request(const Fn *f) {
    if (ld(&g_hooked)) push_cmd(f);
    else run_cmd(f);                     // no hook: runs on the calling thread (can crash)
}

// hook: runs on whatever thread calls ProcessEvent; one drain at a time, everything else passes through

// ---- game-thread detection: the thread that makes the most ProcessEvent calls --------------------------
static uint64_t g_gt = 0; static uint64_t g_tid[8]; static uint32_t g_tcnt[8]; static uint32_t g_tcalls = 0;
static inline void thread_vote(uint64_t me) {
    int i = 0;
    for (; i < 8 && g_tid[i]; i++) if (g_tid[i] == me) break;
    if (i < 8) { g_tid[i] = me; g_tcnt[i]++; }
    if (++g_tcalls >= 20000) {
        int b = 0; for (int k = 1; k < 8; k++) if (g_tcnt[k] > g_tcnt[b]) b = k;
        if (g_tid[b] != g_gt) { g_gt = g_tid[b]; logf_("game thread = %lx (%u of 20000 calls)", g_gt, g_tcnt[b]); }
        memset(g_tid, 0, sizeof g_tid); memset(g_tcnt, 0, sizeof g_tcnt); g_tcalls = 0;
    }
}
static void pe_hook(void *self, void *fn, void *params) {
    if (g_rendering) { ph(PH_STUB); ((pe_t)g_stub)(self, fn, params); ph(PH_PE); return; }   // our own Draw-thread drawing
    ph(PH_PE);
    {
        uint32_t ri = g_pe_ri;
        g_pe_ring[ri & (PE_RING - 1)].fn = (uint64_t)fn;
        g_pe_ring[ri & (PE_RING - 1)].self = (uint64_t)self;
        g_pe_ri = ri + 1; g_pe_ctr = ri + 1;
    }
    // boot breadcrumb: tells us exactly how far the game got if the next report is a loading-screen crash
    static volatile int g_pe_n = 0;
    int pn = __atomic_add_fetch(&g_pe_n, 1, __ATOMIC_RELAXED);
    if (pn == 1 || pn == 5000 || pn == 10000 || pn == 15000 || pn == 19999)
        logf_("pe: %d calls (self %lx fn %lx gt %lx)", pn, (uint64_t)self, (uint64_t)fn, g_gt);
    uint64_t me = (uint64_t)scePthreadSelf(); thread_vote(me);
    if (!g_in_draw && ui_wanted() && plausible((uint64_t)self) && ui_canvas_self((uint64_t)self)) {
        uint64_t t = now_ms();
        if (t - g_canvas_last >= 8) {                      // any UCanvas draw in this title is a free render slot
            g_canvas_last = t;
            g_in_draw = 1;
            draw_ui_on_canvas((uint64_t)self);
            g_in_draw = 0;
            if (g_canvas_events++ < 4) logf_("ui: canvas %lx draws %d", (uint64_t)self, g_draw_ok);
        }
    }
    if (!g_in_draw && g_hud_obj && (uint64_t)self == g_hud_obj && ui_wanted()) {
        uint64_t c = frd64(g_hud_obj + g_canvas_off);      // HUD events fired while PostRender has the canvas set
        if (obj_is_class(c, "canvas")) {
            g_in_draw = 1;
            draw_ui_on_canvas(c);
            g_in_draw = 0;
        }
    }
    if (g_draw_idx && !g_in_draw && (uint32_t)frd64((uint64_t)fn + OBJ_NAME) == g_draw_idx) {
        ((pe_t)g_stub)(self, fn, params);          // let the HUD draw its own stuff first
        g_in_draw = 1;
        hud_draw((uint64_t)self);
        g_in_draw = 0;
        return;
    }
    if (!g_gt || me != g_gt) { ((pe_t)g_stub)(self, fn, params); return; }      // only the game thread may touch objects
    if (g_trace && !g_busy) trace_call((uint64_t)self, (uint64_t)fn, (uint8_t *)params);
    if ((uint64_t)fn == A_FN_INTERACT && !g_busy) learn_interact((uint64_t)self, (uint8_t *)params);
    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQ_REL) == 0) {
        while (ld(&g_head) != ld(&g_tail)) {
            int h = ld(&g_head);
            const Fn *f = g_queue[h];
            st(&g_head, (h + 1) % QUEUE_SIZE);
            run_cmd(f);
        }
        auto_tick();
        ui_tick();
        __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    }
    ph(PH_STUB);
    {
        if (g_waterguard && g_fn_waterdeep && (uint64_t)fn == g_fn_waterdeep && (uint64_t)self == g_wg_veh) {
            // the cannon we ride is already being held above the surface: its "water too deep" ejection is a no-op
            if (g_wg_logx++ < 8) logf_("water: swallowed OnWaterTooDeep for %lx", (uint64_t)self);
        }
        else if (seatlock_block((uint64_t)self, (uint64_t)fn)) { }   // seat lock: the ejection never reaches the game
        else ((pe_t)g_stub)(self, fn, params);         // the engine produces the real value first
        if (g_fn_hitnotify && (uint64_t)fn == g_fn_hitnotify && params) esp_on_hit((uint64_t)self, (uint8_t *)params);
        if (g_fn_dmg && (uint64_t)fn == g_fn_dmg && params) esp_on_damage((uint8_t *)params);
    }
    ph(PH_PE);
}

static int install_hook(void) {
    logf_("hook: installing (target %lx)", A_PROCESSEVENT);
    Detour_Construct(&g_det, DetourMode_x64);
    g_stub = Detour_DetourFunction(&g_det, A_PROCESSEVENT, (void *)&pe_hook);
    if (!g_stub) return -1;
    logf_("hook: detour ok (stub %p)", g_stub);
    if (readable(A_HITNOTIFY, 16)) {
        Detour_Construct(&g_det_hn, DetourMode_x64);
        g_hn_orig = (hn_t)Detour_DetourFunction(&g_det_hn, A_HITNOTIFY, (void *)&hn_hook);
    }
    logf_("esp: OnDisplayHitNotify %lx -> %p (%s)", A_HITNOTIFY, (void *)g_hn_orig, g_hn_orig ? "hooked" : "not hooked");
    g_fn_gpvp = find_object_by_text("GetPlayerViewPoint", "Function");
    g_fn_hitnotify = find_object_by_text("OnDisplayHitNotify", "Function");
    g_fn_dmg = find_object_by_text("ClientAddDamageNumber", "Function");
    logf_("esp: GetPlayerViewPoint fn %lx, OnDisplayHitNotify fn %lx, ClientAddDamageNumber fn %lx",
          g_fn_gpvp, g_fn_hitnotify, g_fn_dmg);
    resolve_engine();
    g_draw_idx = find_fname_index("ReceiveDrawHUD");
    if (!g_draw_idx) g_draw_idx = scan_function_fname("ReceiveDrawHUD");
    logf_("menu: ReceiveDrawHUD name index %u, engine %lx (name pool %lx)", g_draw_idx, g_eng, A_NAMEPOOL);
    // v28 UI resources resolved here (worker thread) so opening the menu never stalls the game thread
    g_ps_name = find_fname_index("PrintString");
    if (g_ps_name) g_ps_fn = find_object_with_name(g_ps_name, "Function");
    if (!g_ps_fn) g_ps_fn = find_object_by_text("PrintString", "Function");
    if (!g_ps_key) g_ps_key = g_ps_name;
    g_ps_state = g_ps_fn ? 1 : -1;
    { uint32_t ci = find_fname_index("canvas");
      g_canvas_cls = ci ? find_object_with_name(ci, "Class") : 0;
      if (!g_canvas_cls) g_canvas_cls = find_object_by_text("canvas", "Class");
      if (!g_canvas_cls) g_canvas_cls = 1; }
    { uint32_t hi = find_fname_index("HUD");
      g_hud_cls = hi ? find_object_with_name(hi, "Class") : 0;
      if (!g_hud_cls) g_hud_cls = find_object_by_text("HUD", "Class"); }
    ui_unlock_messages();
    logf_("ui: PrintString %lx (name %u) canvas class %lx HUD class %lx", g_ps_fn, g_ps_name, g_canvas_cls, g_hud_cls);
    // v29: everything the Draw hook needs is resolved here, on the worker thread
    if (!g_cfn_text) g_cfn_text = find_object_by_text("K2_DrawText", "Function");
    if (!g_cfn_box) g_cfn_box = find_object_by_text("K2_DrawBox", "Function");
    if (!g_cfn_line) g_cfn_line = find_object_by_text("K2_DrawLine", "Function");
    g_font = find_font();
    int dh = install_draw_hook();
    logf_("ui: canvas fns text %lx box %lx line %lx font %lx; draw hook %s",
          g_cfn_text, g_cfn_box, g_cfn_line, g_font, dh ? "armato" : "FALLITO");
    return 0;
}

// ---- pad (libScePad from the game's own modules, as in the BO2 plugin) ----------------------------------
typedef int32_t (*pad_gethandle_t)(int32_t, uint32_t, uint32_t);
typedef int32_t (*pad_readstate_t)(int32_t, OrbisPadData *);
typedef int32_t (*pad_read_t)(int32_t, OrbisPadData *, int32_t);
static pad_gethandle_t p_gethandle; static pad_readstate_t p_readstate;
static int g_pad = -1;

// ---- tapout: while the menu is open the game gets a neutral pad -------------------------------------------
// libScePad exports scePadReadState/scePadRead as 16-byte wrapper stubs, not pointer thunks:
//     8004c60a0: 31 d2              xor edx, edx            ; hidden arg (0 = ReadState, 1 = ReadStateExt)
//     8004c60a2: e9 49 ec ff ff     jmp 8004c4cf0           ; shared implementation(handle, data, ext)
//     8004c60b0: 31 c9              xor ecx, ecx            ; hidden arg for scePadRead
//     8004c60b2: e9 e9 ed ff ff     jmp 8004c4ea0           ; shared implementation(handle, data, count, ext)
// Copying such a stub elsewhere (what Detour does) breaks the relative jump, which is why v31 crashed.
// v33 overwrites the whole 16-byte stub with `jmp [rip+0]` + our hook and calls the shared implementation
// itself, reproducing the hidden argument. Installed from the game thread when the menu first opens (that
// is the thread polling the pad), and our own poll calls the implementation directly, so the menu keeps
// working while the game sees a neutral controller.
static int32_t (*volatile p_rs_impl)(int32_t, OrbisPadData *, int32_t);
static int32_t (*volatile p_rd_impl)(int32_t, OrbisPadData *, int32_t, int32_t);
static pad_read_t p_read_export;               // scePadRead export (set in pad_init)
static void *p_readstate_ext, *p_read_ext;     // scePadReadStateExt / scePadReadExt (optional)
static int g_pad_hooked = 0;

static void pad_neutral(OrbisPadData *d) {
    d->buttons = 0;
    d->leftStick.x = 128; d->leftStick.y = 128;
    d->rightStick.x = 128; d->rightStick.y = 128;
    d->analogButtons.l2 = 0; d->analogButtons.r2 = 0;
    d->touch.fingers = 0;
}
static int tapout_active(void) { return g_menu || (g_tapout_until && now_ms() < g_tapout_until); }

// virtual button injection: OR a button into the state the game reads for a few frames. Used to press Square
// so the GAME ITSELF re-enters the cannon after an ejection - no UFunction replay, no stale pointers.
static volatile uint64_t g_pad_inj_until = 0;
static volatile uint32_t g_pad_inj_btn = 0;
static void pad_inject(uint32_t btn, int ms) {
    g_pad_inj_btn = btn;
    g_pad_inj_until = now_ms() + (uint64_t)ms;
}
static void pad_inject_cancel(void) {                 // match ended: stop synthesising Square at once
    g_pad_inj_btn = 0;
    g_pad_inj_until = 0;
}
static void pad_inject_apply(OrbisPadData *d) {
    if (g_pad_inj_until && now_ms() < g_pad_inj_until) d->buttons |= g_pad_inj_btn;
}

// kind 0 = ReadState(handle, data, ext), kind 1 = Read(handle, data, count, ext)
static int32_t pad_rs_common(int32_t h, OrbisPadData *d, int32_t ext) {
    int32_t r = p_rs_impl ? p_rs_impl(h, d, ext) : -1;
    if (r == 0 && d) {
        if (tapout_active()) { pad_neutral(d); g_pad_neutralised++; }
        else pad_inject_apply(d);
    }
    return r;
}
static int32_t pad_rd_common(int32_t h, OrbisPadData *d, int32_t n, int32_t ext) {
    int32_t r = p_rd_impl ? p_rd_impl(h, d, n, ext) : -1;
    if (r >= 0 && d) {
        int32_t k = (n > 0 && n <= ORBIS_PAD_MAX_DATA_NUM) ? n : 1;
        for (int32_t i = 0; i < k; i++) {
            if (tapout_active()) pad_neutral(d + i);
            else pad_inject_apply(d + i);
        }
        if (tapout_active()) g_pad_neutralised++;
    }
    return r;
}
static int32_t pad_rs_hook(int32_t h, OrbisPadData *d) { return pad_rs_common(h, d, 0); }
static int32_t pad_rsx_hook(int32_t h, OrbisPadData *d) { return pad_rs_common(h, d, 1); }
static int32_t pad_rd_hook(int32_t h, OrbisPadData *d, int32_t n) { return pad_rd_common(h, d, n, 0); }
static int32_t pad_rdx_hook(int32_t h, OrbisPadData *d, int32_t n) { return pad_rd_common(h, d, n, 1); }

// Parse a libScePad 16-byte wrapper stub:
//   31 d2              xor edx, edx      ; kind 0 (ReadState), ext 0
//   31 c9              xor ecx, ecx      ; kind 1 (Read),      ext 0
//   ba 01 00 00 00     mov edx, 1        ; kind 0, ext 1 (ReadStateExt)
//   b9 01 00 00 00     mov ecx, 1        ; kind 1, ext 1 (ReadExt)
//   e9 rel32           jmp <shared implementation>
typedef struct { uint64_t impl; int kind, ext; } pad_stub_t;
static int pad_parse_stub(uint64_t addr, pad_stub_t *out) {
    if (!addr || !readable(addr, 16)) return 0;
    const uint8_t *p = (const uint8_t *)addr;
    int kind, ext; uint64_t at;
    if (p[0] == 0x31 && p[1] == 0xd2) { kind = 0; ext = 0; at = addr + 2; }
    else if (p[0] == 0x31 && p[1] == 0xc9) { kind = 1; ext = 0; at = addr + 2; }
    else if (p[0] == 0xba && p[1] == 1 && p[2] == 0 && p[3] == 0 && p[4] == 0) { kind = 0; ext = 1; at = addr + 5; }
    else if (p[0] == 0xb9 && p[1] == 1 && p[2] == 0 && p[3] == 0 && p[4] == 0) { kind = 1; ext = 1; at = addr + 5; }
    else return 0;
    if (!readable(at, 5) || *(const uint8_t *)at != 0xe9) return 0;
    int32_t rel; memcpy(&rel, (const void *)(at + 1), 4);
    uint64_t impl = at + 5 + (int64_t)rel;
    if (!readable(impl, 16)) return 0;
    out->impl = impl; out->kind = kind; out->ext = ext;
    return 1;
}

// Replace a 16-byte stub with `jmp qword ptr [rip+N]` + the hook address.
// When the stub's own jmp ends before offset 8 (the `31 xx` form), the swap is a single aligned
// 64-bit store, so a thread can never execute a half-written instruction.
static int pad_write_stub(uint64_t addr, void *hook) {
    if (addr & 7) { logf_("tapout: stub %lx not 8-byte aligned", addr); return 0; }
    const uint8_t *p = (const uint8_t *)addr;
    int jmp_end = (p[0] == 0x31) ? 7 : 10;             // end offset of the original jmp
    if (jmp_end <= 8) {
        if (!patch_qword(addr + 8, (uint64_t)hook, 0)) { logf_("tapout: patch payload %lx failed", addr + 8); return 0; }
        if (!patch_qword(addr, 0x90900000000225ffULL, 0)) { logf_("tapout: patch jmp %lx failed", addr); return 0; }
    } else {                                             // 5-byte prefix stub: old jmp covers offset 8
        uint8_t patch[16];
        memset(patch, 0x90, sizeof patch);
        patch[0] = 0xff; patch[1] = 0x25;                // jmp qword ptr [rip+0]
        memcpy(patch + 6, &hook, 8);
        if (!patch_bytes(addr, patch, sizeof patch)) { logf_("tapout: patch stub %lx failed", addr); return 0; }
    }
    return 1;
}

static void pad_install_tapout(void) {
    if (cfg_off || cfg_nopad) return;
    if (g_pad_hooked || !p_readstate) return;
    g_pad_hooked = 1;

    // candidates: the resolved exports and the stubs in their slots (Ext variants sit next to them)
    uint64_t cand[12]; int nc = 0;
    uint64_t rs0 = (uint64_t)p_readstate, rd0 = (uint64_t)p_read_export;
    for (int i = 0; i < 4; i++) cand[nc++] = rs0 + i * 0x10;
    if (rd0) for (int i = 0; i < 4; i++) cand[nc++] = rd0 + i * 0x10;
    if (p_readstate_ext) cand[nc++] = (uint64_t)p_readstate_ext;
    if (p_read_ext) cand[nc++] = (uint64_t)p_read_ext;

    // pass 1: find the two shared implementations
    uint64_t impl_state = 0, impl_count = 0;
    pad_stub_t s;
    if (pad_parse_stub(rs0, &s)) { if (s.kind == 0) impl_state = s.impl; else impl_count = s.impl; }
    if (rd0 && pad_parse_stub(rd0, &s)) { if (s.kind == 0) impl_state = s.impl; else impl_count = s.impl; }
    if (!impl_state && !impl_count) {
        logf_("tapout: no wrapper stub at readstate %p / read %p - tapout unavailable", p_readstate, p_read_export);
        return;
    }

    // pass 2: patch every stub that jumps into one of those implementations
    int n = 0;
    for (int i = 0; i < nc; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (cand[j] == cand[i]) dup = 1;
        if (dup || !pad_parse_stub(cand[i], &s)) continue;
        if (s.impl != (s.kind == 0 ? impl_state : impl_count)) continue;
        // publish the implementation before the stub starts calling into the hook
        if (s.kind == 0) p_rs_impl = (int32_t (*)(int32_t, OrbisPadData *, int32_t))s.impl;
        else             p_rd_impl = (int32_t (*)(int32_t, OrbisPadData *, int32_t, int32_t))s.impl;
        void *hook = (s.kind == 0) ? (s.ext ? (void *)&pad_rsx_hook : (void *)&pad_rs_hook)
                                   : (s.ext ? (void *)&pad_rdx_hook : (void *)&pad_rd_hook);
        if (pad_write_stub(cand[i], hook)) {
            n++;
            logf_("tapout: %s%s stub %lx -> %p (impl %lx)", s.kind ? "read" : "readstate", s.ext ? "ext" : "", cand[i], hook, s.impl);
        }
    }
    logf_("tapout: %d stubs patched; rs impl %lx rd impl %lx", n, (uint64_t)p_rs_impl, (uint64_t)p_rd_impl);
    if (!n) logf_("tapout: patch failed - the menu will not freeze the game");
}
static int32_t pad_read_own(int32_t h, OrbisPadData *d) {     // menu poll: always the untouched state
    if (p_rs_impl) return p_rs_impl(h, d, 0);
    return p_readstate ? p_readstate(h, d) : -1;
}
static int pad_init(void) {
    static int tried = 0, loaded = 0;
    if (g_pad >= 0) return 1;
    if (!p_readstate) {
        OrbisKernelModule mods[256]; size_t n = 0;
        int r = sceKernelGetModuleList(mods, 256, &n);
        if (r != 0) { if (!tried) logf_("pad: sceKernelGetModuleList failed %x", r); tried = 1; return 0; }
        for (size_t i = 0; i < n && !p_readstate; i++) {              // any module that exports the pad API
            void *rs = 0, *gh = 0, *rd = 0, *rse = 0, *rde = 0;
            sceKernelDlsym(mods[i], "scePadReadState", &rs); sceKernelDlsym(mods[i], "scePadGetHandle", &gh);
            sceKernelDlsym(mods[i], "scePadRead", &rd);
            sceKernelDlsym(mods[i], "scePadReadStateExt", &rse); sceKernelDlsym(mods[i], "scePadReadExt", &rde);
            if (rs && gh) {
                OrbisKernelModuleInfo mi; memset(&mi, 0, sizeof mi); mi.size = sizeof mi; sceKernelGetModuleInfo(mods[i], &mi);
                p_readstate = (pad_readstate_t)rs; p_gethandle = (pad_gethandle_t)gh;
                p_read_export = (pad_read_t)rd;
                p_readstate_ext = rse; p_read_ext = rde;
                logf_("pad module %s: gethandle %p readstate %p read %p (ext %p/%p)", mi.name, gh, rs, rd, rse, rde);
            }
        }
        if (!p_readstate && !loaded) {                                // not loaded by the game: load it ourselves
            loaded = 1;
            uint32_t h = sceKernelLoadStartModule("libScePad.sprx", 0, 0, 0, 0, 0);
            void *rs = 0, *gh = 0, *rd = 0, *rse = 0, *rde = 0;
            if ((int32_t)h > 0) {
                sceKernelDlsym(h, "scePadReadState", &rs); sceKernelDlsym(h, "scePadGetHandle", &gh);
                sceKernelDlsym(h, "scePadRead", &rd);
                sceKernelDlsym(h, "scePadReadStateExt", &rse); sceKernelDlsym(h, "scePadReadExt", &rde);
            }
            logf_("pad: %zu modules, none exports the pad API; LoadStartModule(libScePad) = %x rs %p gh %p rd %p", n, h, rs, gh, rd);
            if (rs && gh) {
                p_readstate = (pad_readstate_t)rs; p_gethandle = (pad_gethandle_t)gh;
                p_read_export = (pad_read_t)rd; p_readstate_ext = rse; p_read_ext = rde;
            }
        }
        if (!p_readstate || !p_gethandle) return 0;
    }
    int32_t user = -1;
    int ur = sceUserServiceGetInitialUser(&user);
    if (ur != 0 || user < 0) { if (!tried) logf_("pad: GetInitialUser failed %x user %d", ur, user); tried = 1; return 0; }
    int h = p_gethandle(user, 0, 0);
    if (h < 0) { if (!tried) logf_("pad: scePadGetHandle failed %x", h); tried = 1; return 0; }
    g_pad = h;
    logf_("pad handle %d (user %x)", h, user);
    return 1;
}

// ---- menu input: R1+Square opens/closes, UP/DOWN select, CROSS activates, CIRCLE closes ----
static void menu_key(uint32_t edge) {
    if (edge & ORBIS_PAD_BUTTON_UP) { g_msel = (g_msel + MENU_N - 1) % MENU_N; }
    else if (edge & ORBIS_PAD_BUTTON_DOWN) { g_msel = (g_msel + 1) % MENU_N; }
    else if (edge & ORBIS_PAD_BUTTON_CIRCLE) { g_menu = 0; g_tapout_until = now_ms() + 350; logf_("menu: closed (%u neutralised reads)", g_pad_neutralised); }
    else if (edge & ORBIS_PAD_BUTTON_CROSS) {
        switch (g_msel) {
        case 0: request(find_fn("aura")); break;
        case 1: request(find_fn("tour")); break;
        case 2: request(find_fn("instant")); break;
        case 3: {                                       // 0.1 -> 0.2 -> 0.5 -> 1 -> 2 -> 2.5 s at each chest
            static const uint64_t spd[] = { 100, 200, 500, 1000, 2000, 2500 };
            int k = 0;
            for (int i = 0; i < 6; i++) if (g_tstep == spd[i]) { k = (i + 1) % 6; break; }
            g_tstep = spd[k];
            logf_("chest stop = %.1f s", g_tstep / 1000.0);
        } break;
        case 4: request(find_fn("tpchest")); break;
        case 5: request(find_fn("perf")); break;
        case 6: request(find_fn("esp")); break;
        case 7: request(find_fn("pesp")); break;
        case 8: request(find_fn("vesp")); break;
        case 9: request(find_fn("gotocannon")); break;
        case 10: request(find_fn("god")); break;
        case 11: request(find_fn("ammo")); break;
        case 12: request(find_fn("stop")); break;
        case 13: request(find_fn("hpbar")); break;
        case 14: request(find_fn("skel")); break;
        case 15: request(find_fn("chestesp")); break;
        case 16: request(find_fn("snap")); break;
        case 17: request(find_fn("aim")); break;
        case 18: request(find_fn("aimpart")); break;
        case 19: request(find_fn("aimfov")); break;
        case 20: request(find_fn("aimsmooth")); break;
        case 21: request(find_fn("fov")); break;
        case 22: request(find_fn("kickguard")); break;
        case 23: request(find_fn("waterguard")); break;
        case 24: request(find_fn("seatlock")); break;
        case 25: request(find_fn("tower")); break;
        case 26: request(find_fn("ghost")); break;
        default: g_menu = 0; g_tapout_until = now_ms() + 350; logf_("menu: closed (%u neutralised reads)", g_pad_neutralised); return;
        }
    }
}
static void pad_tick(void) {
    static uint32_t prev;
    if (!pad_init()) return;
    OrbisPadData d; memset(&d, 0, sizeof d);
    if (pad_read_own(g_pad, &d) != 0) { g_pad = -1; return; }
    g_pad_l2 = ((d.buttons & ORBIS_PAD_BUTTON_L2) || d.analogButtons.l2 > 0x30) ? 1 : 0;
    g_pad_r2 = ((d.buttons & ORBIS_PAD_BUTTON_R2) || d.analogButtons.r2 > 0x30) ? 1 : 0;
    if (g_pad_r2) g_pad_r2_until = now_ms() + 120;
    static int pn = 0;
    if (++pn % 300 == 0) {
        int phe = g_phase;
        if (phe < 0 || phe > PH_STUB) phe = 0;
        logf_("pad: alive %d ticks (handle %d, buttons %x, phase %s)", pn, g_pad, d.buttons, PH_NAMES[phe]);
        // watchdog: the pad reader survives a game-thread stall, so it is the right place to report one.
        // When the game thread stops calling ProcessEvent for ~20 s, dump the last calls it made.
        static uint32_t last_ctr = 1; static int quiet = 0;
        uint32_t c = g_pe_ctr;
        if (c == last_ctr) { if (quiet < 99) quiet++; } else { quiet = 0; last_ctr = c; }
        if (quiet == 4) {
            logf_("stall: no ProcessEvent for ~20 s (phase %s, counter %u)", PH_NAMES[phe], c);
            for (int k = 0; k < PE_RING; k++) {
                uint32_t idx = (g_pe_ri - PE_RING + (uint32_t)k) & (PE_RING - 1);
                uint64_t f_ = g_pe_ring[idx].fn, s_ = g_pe_ring[idx].self;
                char fnn[64] = "?", cl[64] = "?";
                if (plausible(f_)) obj_name(f_, fnn, sizeof fnn);
                if (plausible(s_)) class_name(frd64(s_ + OBJ_CLASS), cl, sizeof cl);
                logf_("  last pe[%d] %s (%lx) on %s (%lx)", k, fnn, f_, cl, s_);
            }
        }
    }
    static uint32_t pfull;
    uint32_t edge = d.buttons & ~pfull; pfull = d.buttons;
    if (edge & ORBIS_PAD_BUTTON_SQUARE) g_pad_sq_ms = now_ms();
    if (g_menu) {
        if (edge) { static int en = 0; if (en < 30) logf_("menu: key edge %x (buttons %x)", edge, d.buttons); en++; }
        menu_key(edge);
    }
    uint32_t mask = 0; for (size_t i = 0; i < NFNS; i++) mask |= FNS[i].pad_combo;
    uint32_t b = d.buttons & mask;                         // exact match: L1+R1+R3 must not also fire L1+R1
    for (size_t i = 0; i < NFNS; i++) {
        uint32_t c = FNS[i].pad_combo;
        if (c && b == c && prev != c && (!g_menu || FNS[i].sp == 14)) request(&FNS[i]);
    }
    prev = b;
}

static void *worker_thread(void *arg) {
    (void)arg;
    uint64_t next = now_ms() + START_DELAY_US / 1000;
    int hooked_try = 0, checks = 0;
    for (;;) {
        sceKernelUsleep(PAD_POLL_MS * 1000);
        if (!hooked_try && now_ms() >= next) {
            // v128: the layout gate belongs here, when GObjects and the code are definitely mapped. Retry a
            // few times because a slow boot may still be initialising; refuse only if it never appears.
            uint64_t chunks = rd64(A_GOBJECTS);
            uint64_t c0 = plausible(chunks) ? rd64(chunks) : 0;
            int layout = plausible(c0) && readable(c0, 65536ULL * 24);
            int code = readable(A_PROCESSEVENT, 16);
            if ((!layout || !code) && !g_supported_title) {
                checks++;
                if (checks < 8) {
                    next = now_ms() + 5000;
                    logf_("build check %d: object table %s, code %s - waiting for the game to initialise",
                          checks, layout ? "ok" : "missing", code ? "ok" : "missing");
                    continue;
                }
                hooked_try = 1;
                logf_("build check failed: object table %s, code %s - not installing (this sprx is for the "
                      "CUSA07022/07669 v05.27 layout)", layout ? "ok" : "missing", code ? "ok" : "missing");
                continue;
            }
            hooked_try = 1;
            if (!layout || !code)
                logf_("build check: object table %s, code %s - known Fortnite title, installing anyway",
                      layout ? "ok" : "missing", code ? "ok" : "missing");
            else
                logf_("build check: object table ok, ProcessEvent target ok");
            int r = install_hook();
            if (r == 0) { st(&g_hooked, 1); logf_("ProcessEvent hooked at %lx (stub %p)", A_PROCESSEVENT, g_stub); }
            else logf_("hook failed, pad/socket commands would run on their own thread (can crash): disabled");
        }
        if (ld(&g_hooked)) pad_tick();
    }
    return 0;
}

// ---- command server -------------------------------------------------------------------------------------
static const Fn *find_fn(const char *n) {
    for (size_t i = 0; i < NFNS; i++) if (strcmp(FNS[i].name, n) == 0) return &FNS[i];
    return 0;
}
static void reply(int c, const char *fmt, ...) {
    char out[256]; va_list ap; va_start(ap, fmt); int n = vsnprintf(out, sizeof out, fmt, ap); va_end(ap);
    if (n > 0) sceNetSend(c, out, n, 0);
}
static void handle_line(int c, char *line) {
    char *e = line + strlen(line);
    while (e > line && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;
    if (!strcmp(line, "status")) {
        int ok = resolve_pc();
        reply(c, "hook=%d pc=%lx cm=%lx chain=%d\n", ld(&g_hooked), g_pc, g_pc ? find_cm() : 0, ok);
        return;
    }
    if (!strcmp(line, "list")) {
        for (size_t i = 0; i < NFNS; i++) reply(c, "%s\n", FNS[i].name);
        return;
    }
    if (!strncmp(line, "radius ", 7)) {
        if (!ld(&g_hooked)) { reply(c, "hook not installed yet\n"); return; }
        strncpy(g_sc_text, line + 7, sizeof g_sc_text - 1); g_sc_text[sizeof g_sc_text - 1] = 0;
        line = (char *)"radius";
    }
    if (!strncmp(line, "speed ", 6)) {
        if (!ld(&g_hooked)) { reply(c, "hook not installed yet\n"); return; }
        strncpy(g_sc_text, line + 6, sizeof g_sc_text - 1); g_sc_text[sizeof g_sc_text - 1] = 0;
        line = (char *)"speed";
    }
    if (!strncmp(line, "sc ", 3)) {
        if (!ld(&g_hooked)) { reply(c, "hook not installed yet\n"); return; }
        strncpy(g_sc_text, line + 3, sizeof g_sc_text - 1); g_sc_text[sizeof g_sc_text - 1] = 0;
        line = (char *)"sc";
    }
    const Fn *f = find_fn(line);
    if (!f) { reply(c, "unknown: %s (try: list)\n", line); return; }
    if (!ld(&g_hooked)) { reply(c, "hook not installed yet (wait %us after game start)\n", START_DELAY_US / 1000000u); return; }
    reply(c, push_cmd(f) ? "queued\n" : "queue full\n");
}
static void *server_thread(void *arg) {
    (void)arg;
    int s = sceNetSocket("fn_chests", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (s < 0) { logf_("sceNetSocket failed %x", s); return 0; }
    int one = 1; sceNetSetsockopt(s, 0xffff /* SOL_SOCKET */, 4 /* SO_REUSEADDR */, &one, sizeof one);
    OrbisNetSockaddr a; memset(&a, 0, sizeof a);
    a.len = 16; a.sa_family = ORBIS_NET_AF_INET;
    uint16_t port = sceNetHtons(CMD_PORT); memcpy(a.sa_data, &port, 2);   // sin_port; sin_addr = INADDR_ANY (zero)
    if (sceNetBind(s, &a, sizeof a) < 0 || sceNetListen(s, 2) < 0) { logf_("bind/listen %d failed", CMD_PORT); sceNetSocketClose(s); return 0; }
    logf_("command server on %d", CMD_PORT);
    for (;;) {
        int c = sceNetAccept(s, 0, 0);
        if (c < 0) { sceKernelUsleep(100000); continue; }
        char buf[128];
        int n = sceNetRecv(c, buf, sizeof buf - 1, 0);
        if (n > 0) { buf[n] = 0; handle_line(c, buf); }
        sceNetSocketClose(c);
    }
    return 0;
}

// ---- plugin entry ---------------------------------------------------------------------------------------
int32_t attr_public plugin_load(int32_t argc, const char *argv[]) {
    (void)argc; (void)argv;
    // Check the PREVIOUS start BEFORE writing this start's own marker: with 'A' written first, the check
    // always sees an unfinished file and keeps the plugin inert forever (that is exactly what v117-v122 did).
    {
        char last = 0;
        int same = boot_same_build();
        if (same && boot_last_stage(&last) && last != 'F') {
            if (last == 'X') {
                // a previous launch was already inert once: re-arm instead of locking the console out
                // for good (a manually copied sprx never gets the installer's marker cleanup).
                boot_reset();
            } else {
                boot_mark('X');
                return 0;                       // one inert launch after a crash: the game still boots
            }
        }
        if (!same) boot_reset();                // old build's marker (or none): start a fresh trace
    }
    boot_mark('A');
    // GoldHEN passes its own arguments to plugin_load, but they are NOT safe to read here: v123 died inside
    // the byte-checked argv read (boot trace A a b) before it could even open the log. Nothing on the load
    // path touches argv any more; the candidate folders below cover every known GoldHEN location.
    bootstrap_fs();
    boot_mark('B');
    struct proc_info pi;
    if (sys_sdk_proc_info(&pi) != 0) { logf_("fs: process info unavailable, off"); return 0; }
    boot_mark('C');
    logf_("==== >,< // funcult %s: %s v%s", BUILD_TAG, pi.titleid, pi.version);
    // v128: do NOT gate here. plugin_load runs before the game has created its object table, so a check at
    // this point reports "missing" even on a supported console (that is what disabled v127 everywhere).
    // The real check runs on the worker thread at hook time; here it is only a breadcrumb.
    g_supported_title = !strcmp(pi.titleid, "CUSA07022") || !strcmp(pi.titleid, "CUSA07669");
    {
        uint64_t chunks = rd64(A_GOBJECTS);
        uint64_t c0 = plausible(chunks) ? rd64(chunks) : 0;
        int layout = plausible(c0) && readable(c0, 65536ULL * 24);
        int code = readable(A_PROCESSEVENT, 16);
        logf_("build check at load: object table %s, code %s (%s v%s, %s title) - hook-time gate decides",
              layout ? "ok" : "not ready", code ? "ok" : "missing", pi.titleid, pi.version,
              g_supported_title ? "known" : "unknown");
    }
    logf_("hook in %u s", START_DELAY_US / 1000000u);
    boot_mark('D');
    cfg_load();
    if (cfg_off) { logf_("cfg: inert build, nothing will be installed"); boot_mark('E'); return 0; }
    Notify(TEX_ICON_SYSTEM, ">,< // funcult: R1+Square = menu");
    boot_mark('E');
    OrbisPthread t1, t2;
    scePthreadCreate(&t1, NULL, worker_thread, NULL, "fn_chests_w");
    scePthreadCreate(&t2, NULL, server_thread, NULL, "fn_chests_s");
    boot_mark('F');
    return 0;
}
int32_t attr_public plugin_unload(int32_t argc, const char *argv[]) { (void)argc; (void)argv; return 0; }
s32 attr_module_hidden module_start(s64 argc, const void *args) { (void)argc; (void)args; return 0; }
s32 attr_module_hidden module_stop(s64 argc, const void *args) { (void)argc; (void)args; return 0; }





