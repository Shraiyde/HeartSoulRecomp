#ifdef PLATFORM_SDL2
// For dladdr in glibc.
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#endif
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

#ifdef __ANDROID__
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#include "global.h"
#include "coop.h"
#include "platform.h"
#include "main.h"
#include "battle_setup.h"
#include "script_pokemon_util.h"
#include "overworld.h"
#include "field_screen_effect.h"
#include "constants/species.h"
#include "constants/items.h"
#include "constants/flags.h"
#include "event_data.h"
#include "rtc.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "m4a.h"
#include "cgb_audio.h"
#include "gba/flash_internal.h"
#include "platform/dma.h"
#include "platform/framedraw.h"
#include "platform/system.h"
#include "platform/dualscreen.h"


SDL_Thread *mainLoopThread;
SDL_Window *sdlWindow;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
SDL_sem *vBlankSemaphore;
SDL_atomic_t isFrameAvailable;
bool speedUp = false;
unsigned int videoScale = 1;
bool videoScaleChanged = false;
bool isRunning = true;
bool paused = false;
double simTime = 0;
double lastGameTime = 0;
double curGameTime = 0;
double fixedTimestep = 1.0 / 60.0; // 16.666667ms
double timeScale = 1.0;
// HNS_FAST_FORWARD=toggle: a pull of the right trigger turns fast forward on,
// the next pull turns it off. Otherwise (hold) it runs while the trigger is in.
// smart: as toggle, but menus run at normal speed (DualScreen_IsSlowScreen),
// so a cursor does not race; the field and battles stay fast.
static bool sFastForwardToggle;
static bool sFastForwardSmart;
struct SiiRtcInfo internalClock;

static FILE *sSaveFile = NULL;

extern void AgbMain(void);
extern void MainLoop(void);
extern void DoSoftReset(void);

int DoMain(void *param);
void ProcessEvents(void);
void VDraw(SDL_Texture *texture);
typedef struct TouchLayout {
    int w, h;
    SDL_Rect game;
    SDL_Rect l, r, a, b, selectBtn, startBtn;
    int dpadCx, dpadCy, dpadReach;
} TouchLayout;
#ifdef __ANDROID__
static void BuildTouchLayout(TouchLayout *layout);
static void DrawTouchControls(SDL_Renderer *renderer, const TouchLayout *layout);
#endif

static void ReadSaveFile(char *path);
static void StoreSaveFile(void);
static void CloseSaveFile(void);

static void UpdateInternalClock(void);

static u16 keys;
// Sized for the largest frame. Rows are packed at gRenderWidth, and there
// are gRenderHeight of them.
static uint16_t sFrameImage[MAX_RENDER_WIDTH * MAX_RENDER_HEIGHT];
static bool sHeadless = false;
// Frames run so far in headless mode, for HNS_CLOCK.
static unsigned long sHeadlessFrame;

// How the frame fills the screen, from HNS_SCALE:
//   fit      (default) as large as fits, keeping its shape
//   integer  as large as fits at a whole multiple, so every GBA pixel is the
//            same size; the picture can be smaller than with fit
//   stretch  fills the whole screen, changing its shape
// Fit and integer leave black bars where the shapes differ.
enum
{
    SCALE_FIT,
    SCALE_INTEGER,
    SCALE_STRETCH,
};
static int sScaleMode = SCALE_FIT;

static void ApplyScaleMode(void)
{
    if (sdlRenderer == NULL)
        return;
#ifdef __ANDROID__
    // The phone layout places the picture itself. A logical size would
    // letterbox it in the middle and draw the controls on top of it.
    SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
    SDL_RenderSetIntegerScale(sdlRenderer, SDL_FALSE);
    return;
#endif
    if (sScaleMode == SCALE_STRETCH)
    {
        // No logical size: the frame is copied over the whole output.
        SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
        SDL_RenderSetIntegerScale(sdlRenderer, SDL_FALSE);
        return;
    }
    SDL_RenderSetLogicalSize(sdlRenderer, gRenderWidth, gRenderHeight);
    SDL_RenderSetIntegerScale(sdlRenderer, sScaleMode == SCALE_INTEGER ? SDL_TRUE : SDL_FALSE);
}

static void SetScaleMode(const char *name)
{
    if (name != NULL && strcmp(name, "integer") == 0)
        sScaleMode = SCALE_INTEGER;
    else if (name != NULL && strcmp(name, "stretch") == 0)
        sScaleMode = SCALE_STRETCH;
    else
        sScaleMode = SCALE_FIT;
    ApplyScaleMode();
}

// Widescreen: HNS_WIDESCREEN=1 renders 288x160 instead of 240x160. See
// include/platform.h. Off by default. Screenshots are saved at the size
// that was rendered.
void Platform_SetWidescreen(bool32 enabled)
{
#ifdef RENDERER_EASY_DRAW
    gRenderMargin = enabled ? WIDESCREEN_MARGIN : 0;
#else
    (void)enabled;
    gRenderMargin = 0;
#endif
    gRenderWidth = DISPLAY_WIDTH + 2 * gRenderMargin;
    ApplyScaleMode();
#ifndef __ANDROID__
    if (sdlWindow != NULL)
        SDL_SetWindowSize(sdlWindow, gRenderWidth * videoScale, gRenderHeight * videoScale);
#endif
}

// Tall screen: HNS_TALL=1 adds 28 lines above and below, so with widescreen
// the frame is 288x216 (4:3). See include/platform.h. Meant to be used with
// widescreen on; without it the extra lines stay black. Off by default.
void Platform_SetTallScreen(bool32 enabled)
{
#ifdef RENDERER_EASY_DRAW
    gRenderMarginY = enabled ? TALL_MARGIN : 0;
#else
    (void)enabled;
    gRenderMarginY = 0;
#endif
    gRenderHeight = DISPLAY_HEIGHT + 2 * gRenderMarginY;
    ApplyScaleMode();
#ifndef __ANDROID__
    if (sdlWindow != NULL)
        SDL_SetWindowSize(sdlWindow, gRenderWidth * videoScale, gRenderHeight * videoScale);
#endif
}

// Headless test mode, for running the game without a display or a person.
//   HNS_HEADLESS_FRAMES=N   run N frames as fast as possible, then exit
//   HNS_SHOTS=60,300        save shot_00060.bmp and shot_00300.bmp
//   HNS_SHOT_EVERY=N        also save a shot every N frames
//   HNS_INPUT=120:A,200+30:DOWN   press A on frame 120, hold DOWN for 30 frames from 200
//   HNS_STATE_DUMP=N        print the bottom-screen state JSON to stderr every N frames
//   HNS_DOC_DUMP=1:0,2:0    with HNS_STATE_DUMP, print these bottom-screen documents (kind:param)
//   HNS_TAP=2700:MOVE1,2900:RUN   bottom-screen taps: MOVE1-4, FIGHT, BAG, POKEMON, RUN
//   HNS_WAV=path            write everything the game queues as audio to a WAV file
//                           (32-bit float, stereo, 42048 Hz)
//   HNS_AUDIO_LOG=1         log song starts, unhandled sound commands and voice
//                           types to stderr, and print note counts at the end
//   HNS_GBS=FRAME           set FLAG_SYS_GBS_ENABLED on that frame, so music started
//                           afterwards uses the Game Boy sound engine
//   HNS_SONG=300:5,400:21   start song 5 on frame 300 and song 21 on frame 400, to hear
//                           a sound effect or a piece of music on its own
//   HNS_TEST_BATTLE=N       give a Cyndaquil and start a wild battle on frame N
//   HNS_WARP=N:G:M:X:Y      on frame N, warp to map group G, map M, position X,Y
//   HNS_WIDESCREEN=1        render 288x160 (also applies with a display)
//   HNS_TALL=1              add 28 lines above and below: 288x216 with widescreen. Meant
//                           with HNS_WIDESCREEN=1; the overworld map fills the extra lines
//   HNS_SCALE=MODE          fit (default), integer or stretch: how the frame fills
//                           the window. On Android the start screen sets both.
//   HNS_CLOCK=EPOCH         start the clock at that Unix time and advance it one second
//                           every 60 frames, so two runs see the same time of day and
//                           RNG seed and their shots can be compared byte for byte
//   HNS_LAYER_DEBUG=1       tint each pixel by the layer that drew it (BG0 red, BG1
//                           green, BG2 blue, BG3 yellow, sprites magenta, backdrop grey)
//   HNS_LAYER_HIDE=MASK     hide layers: bits 0-3 are BG0-BG3, bit 4 is sprites (0x17
//                           leaves BG3 alone). Both are in src/platform/gba_easy_draw.c
// Use with SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy.
static u16 HeadlessKeyFromName(const char *name, size_t len)
{
    static const struct { const char *name; u16 key; } names[] = {
        {"A", A_BUTTON}, {"B", B_BUTTON}, {"START", START_BUTTON}, {"SELECT", SELECT_BUTTON},
        {"L", L_BUTTON}, {"R", R_BUTTON}, {"UP", DPAD_UP}, {"DOWN", DPAD_DOWN},
        {"LEFT", DPAD_LEFT}, {"RIGHT", DPAD_RIGHT},
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        if (strlen(names[i].name) == len && strncmp(names[i].name, name, len) == 0)
            return names[i].key;
    }
    return 0;
}

static u16 HeadlessKeysForFrame(const char *script, unsigned long frame)
{
    u16 result = 0;

    while (script != NULL && *script != '\0')
    {
        char *end;
        unsigned long start = strtoul(script, &end, 10);
        unsigned long length = 2;
        const char *name;
        size_t nameLen;

        if (*end == '+')
            length = strtoul(end + 1, &end, 10);
        if (*end != ':')
            break;
        name = end + 1;
        nameLen = strcspn(name, ",");
        if (frame >= start && frame < start + length)
            result |= HeadlessKeyFromName(name, nameLen);
        script = name + nameLen;
        if (*script == ',')
            script++;
    }
    return result;
}

static bool HeadlessWantsShot(const char *list, unsigned long every, unsigned long frame)
{
    if (every != 0 && frame % every == 0)
        return true;
    while (list != NULL && *list != '\0')
    {
        char *end;

        if (strtoul(list, &end, 10) == frame)
            return true;
        if (*end != ',')
            break;
        list = end + 1;
    }
    return false;
}

// Starts the songs HNS_SONG lists for this frame.
static void HeadlessStartSongs(const char *list, unsigned long frame, bool32 gbsEnabled)
{
    while (list != NULL && *list != '\0')
    {
        char *end;
        unsigned long start = strtoul(list, &end, 10);
        unsigned long songNum;

        if (*end != ':')
            break;
        songNum = strtoul(end + 1, &end, 10);
        if (start == frame)
            m4aSongNumStart(songNum, gbsEnabled);
        if (*end != ',')
            break;
        list = end + 1;
    }
}

static void HeadlessSaveShot(unsigned long frame)
{
    char path[64];
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(sFrameImage, gRenderWidth, gRenderHeight,
                                                              16, gRenderWidth * sizeof(uint16_t), SDL_PIXELFORMAT_ABGR1555);

    if (surface == NULL)
        return;
    snprintf(path, sizeof(path), "shot_%05lu.bmp", frame);
    // 24-bit, since few image tools read 16-bit bitmaps.
    {
        SDL_Surface *rgb = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_BGR24, 0);

        if (rgb != NULL)
        {
            SDL_SaveBMP(rgb, path);
            SDL_FreeSurface(rgb);
        }
    }
    SDL_FreeSurface(surface);
}

#define WAV_SAMPLE_RATE 42048
#define WAV_HEADER_SIZE 44

static FILE *sWavFile = NULL;
static u32 sWavDataBytes = 0;
static u32 sWavWrites = 0;

static void WavPut32(u8 *dest, u32 value)
{
    dest[0] = value;
    dest[1] = value >> 8;
    dest[2] = value >> 16;
    dest[3] = value >> 24;
}

// Rewritten as the file grows, so a run that is killed still leaves a valid file.
static void WavWriteHeader(void)
{
    u8 header[WAV_HEADER_SIZE];

    memcpy(header, "RIFF", 4);
    WavPut32(header + 4, WAV_HEADER_SIZE - 8 + sWavDataBytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    WavPut32(header + 16, 16);
    header[20] = 3; // IEEE float
    header[21] = 0;
    header[22] = 2; // channels
    header[23] = 0;
    WavPut32(header + 24, WAV_SAMPLE_RATE);
    WavPut32(header + 28, WAV_SAMPLE_RATE * 2 * sizeof(float));
    header[32] = 2 * sizeof(float);
    header[33] = 0;
    header[34] = 8 * sizeof(float);
    header[35] = 0;
    memcpy(header + 36, "data", 4);
    WavPut32(header + 40, sWavDataBytes);
    fseek(sWavFile, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), sWavFile);
    fseek(sWavFile, 0, SEEK_END);
    fflush(sWavFile);
}

static void WavOpen(const char *path)
{
    sWavFile = fopen(path, "wb");
    if (sWavFile == NULL)
    {
        fprintf(stderr, "headless: cannot write %s\n", path);
        return;
    }
    sWavDataBytes = 0;
    sWavWrites = 0;
    WavWriteHeader();
}

static void WavWrite(const float *samples, u32 size)
{
    if (sWavFile == NULL)
        return;
    fwrite(samples, 1, size, sWavFile);
    sWavDataBytes += size;
    // About once a second.
    if (++sWavWrites % 60 == 0)
        WavWriteHeader();
}

static void WavClose(void)
{
    if (sWavFile == NULL)
        return;
    WavWriteHeader();
    fclose(sWavFile);
    sWavFile = NULL;
}

#ifndef _WIN32
// qemu's debugger stub only reports faults, so the watchdog turns a stuck
// frame into one. "thread apply all bt" then shows where the game was.
static void HeadlessWatchdog(int signum, siginfo_t *info, void *context)
{
    (void)signum;
    (void)info;
    fprintf(stderr, "headless: frame did not finish, callback2=%p state=%d\n", (void *)gMain.callback2, gMain.state);
#if defined(__arm__) && defined(__linux__)
    {
        ucontext_t *uc = context;

        fprintf(stderr, "headless: stuck at pc=%#lx lr=%#lx\n", uc->uc_mcontext.arm_pc, uc->uc_mcontext.arm_lr);
    }
#elif defined(__aarch64__) && defined(__linux__)
    {
        ucontext_t *uc = context;

        fprintf(stderr, "headless: stuck at pc=%#llx lr=%#llx\n",
                (unsigned long long)uc->uc_mcontext.pc, (unsigned long long)uc->uc_mcontext.regs[30]);
    }
#else
    (void)context;
#endif
    *(volatile int *)0 = 0;
}
#endif

static int RunHeadless(unsigned long frameCount)
{
    const char *script = getenv("HNS_INPUT");
    const char *shots = getenv("HNS_SHOTS");
    const char *everyText = getenv("HNS_SHOT_EVERY");
    unsigned long every = everyText != NULL ? strtoul(everyText, NULL, 10) : 0;
    const char *testBattleText = getenv("HNS_TEST_BATTLE");
    unsigned long testBattleFrame = testBattleText != NULL ? strtoul(testBattleText, NULL, 10) : 0;
    const char *giveMonText = getenv("HNS_GIVE_MON");
    unsigned long giveMonFrame = giveMonText != NULL ? strtoul(giveMonText, NULL, 10) : 0;
    const char *gbsText = getenv("HNS_GBS");
    unsigned long gbsFrame = gbsText != NULL ? strtoul(gbsText, NULL, 10) : 0;
    const char *wavPath = getenv("HNS_WAV");
    const char *songs = getenv("HNS_SONG");
    const char *warpText = getenv("HNS_WARP");
    unsigned long warpFrame = 0;
    int warpGroup = 0, warpMap = 0, warpX = 0, warpY = 0;
    unsigned long frame;

    if (wavPath != NULL && wavPath[0] != '\0')
        WavOpen(wavPath);

#ifndef _WIN32
    {
        struct sigaction action;

        memset(&action, 0, sizeof(action));
        action.sa_sigaction = HeadlessWatchdog;
        action.sa_flags = SA_SIGINFO;
        sigaction(SIGALRM, &action, NULL);
    }
#endif
    if (warpText != NULL && sscanf(warpText, "%lu:%d:%d:%d:%d", &warpFrame, &warpGroup, &warpMap, &warpX, &warpY) != 5)
        warpFrame = 0;
    for (frame = 1; frame <= frameCount; frame++)
    {
        sHeadlessFrame = frame;
        keys = HeadlessKeysForFrame(script, frame);
        // HNS_WARP gets a test to a map that scripted input cannot reach.
        // The player must be standing in the overworld.
        if (warpFrame != 0 && frame == warpFrame)
        {
            SetWarpDestination(warpGroup, warpMap, WARP_ID_NONE, warpX, warpY);
            DoWarp();
            ResetInitialPlayerAvatarState();
        }
        // HNS_TEST_BATTLE=FRAME gives the player a Cyndaquil and starts a wild
        // battle on that frame. The player must be standing in the overworld.
        // HNS_GIVE_MON=FRAME gives the Cyndaquil on its own, so a later warp
        // reloads the map with it in the party (and following the player).
        if (giveMonFrame != 0 && frame == giveMonFrame)
            ScriptGiveMon(SPECIES_CYNDAQUIL, 5, ITEM_NONE);
        if (testBattleFrame != 0 && frame == testBattleFrame)
        {
            if (giveMonFrame == 0)
                ScriptGiveMon(SPECIES_CYNDAQUIL, 10, ITEM_NONE);
            CreateScriptedWildMon(SPECIES_SENTRET, 3, ITEM_NONE);
            // HNS_TEST_BATTLE_KIND=wild uses the path of a grass encounter
            // instead of a scripted one.
            if (getenv("HNS_TEST_BATTLE_KIND") != NULL && strcmp(getenv("HNS_TEST_BATTLE_KIND"), "wild") == 0)
                BattleSetup_StartWildBattle();
            else
                BattleSetup_StartScriptedWildBattle();
        }
        if (gbsFrame != 0 && frame == gbsFrame)
            FlagSet(FLAG_SYS_GBS_ENABLED);
        gAudioLogFrame = frame;
        HeadlessStartSongs(songs, frame, gbsFrame != 0 && frame >= gbsFrame);
#ifndef _WIN32
        // A frame that never finishes ends the run with SIGALRM, which a
        // debugger reports with the place it was stuck.
        alarm(20);
#endif
        ENTER_VBLANK();
        MainLoop();
        DualScreen_HeadlessFrame(frame);
        VDraw(sdlTexture);
        RunDMAsAndVBlank();
        AudioUpdate();
        if (HeadlessWantsShot(shots, every, frame))
            HeadlessSaveShot(frame);
    }
    WavClose();
    if (gAudioLog)
        AudioLogSummary();
    printf("headless: ran %lu frames\n", frameCount);
    return 0;
}

// About 100 ms at 42048 Hz.
#define AUDIO_QUEUE_TARGET 4200

static unsigned long sAudioUnderruns;

// Logs frame pacing and audio underruns every 5 seconds, to logcat on Android
// (tag SDL/APP). Underruns are passes that found the audio queue empty.
static void ReportPerformance(Uint64 now)
{
    static Uint64 sWindowStart;
    static unsigned long sPasses;
    static unsigned long sUnderrunsAtStart;
    double seconds;

    if (sWindowStart == 0)
    {
        sWindowStart = now;
        sUnderrunsAtStart = sAudioUnderruns;
        return;
    }
    sPasses++;
    seconds = (double)(now - sWindowStart) / (double)SDL_GetPerformanceFrequency();
    if (seconds < 5.0)
        return;
    SDL_Log("perf: %.1f loop passes/s, %.2f ms each, audio underruns %lu, queued %u samples",
            sPasses / seconds, seconds * 1000.0 / sPasses, sAudioUnderruns - sUnderrunsAtStart,
            SDL_GetQueuedAudioSize(1) / 8);
    sWindowStart = now;
    sPasses = 0;
    sUnderrunsAtStart = sAudioUnderruns;
}

static void RefillAudioQueue(void)
{
    Uint32 queued = SDL_GetQueuedAudioSize(1) / 8;
    int updates = 0;

    if (queued == 0)
        sAudioUnderruns++;
    while (queued < AUDIO_QUEUE_TARGET && updates < 8)
    {
        AudioUpdate();
        queued = SDL_GetQueuedAudioSize(1) / 8;
        updates++;
    }
}

int main(int argc, char **argv)
{
    const char *headlessFrames = getenv("HNS_HEADLESS_FRAMES");
    const char *widescreen = getenv("HNS_WIDESCREEN");
    const char *tall = getenv("HNS_TALL");

    // Open an output console on Windows
#ifdef _WIN32
    AllocConsole() ;
    AttachConsole( GetCurrentProcessId() ) ;
    freopen( "CON", "w", stdout ) ;
#endif

#ifdef __ANDROID__
    // The save lives in the app's own external files folder
    // (Android/data/<package>/files), which needs no permission and which a
    // file manager can reach to back the save up.
    {
        const char *dir = SDL_AndroidGetExternalStoragePath();

        if (dir != NULL)
            chdir(dir);
    }
#endif

    ReadSaveFile("pokeemerald.sav");

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    {
        DBGPRINTF("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    // Before the window exists, so it is created at the right size.
#ifdef __ANDROID__
    // The app's start screen sets HNS_WIDESCREEN from the player's choice
    // (HeartSoulActivity). Without one, use widescreen only on a screen at
    // least 16:10, so 4:3 handhelds (Anbernic RG DS) get the full GBA
    // picture instead of a letterboxed wide one.
    {
        SDL_DisplayMode mode;
        bool wide = true;

        if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0)
        {
            int longSide = mode.w > mode.h ? mode.w : mode.h;
            int shortSide = mode.w > mode.h ? mode.h : mode.w;

            wide = longSide * 10 >= shortSide * 16;
        }
        if (widescreen != NULL && widescreen[0] != '\0')
            wide = strtoul(widescreen, NULL, 10) != 0;
        SDL_Log("display %dx%d, widescreen %s, tall %s, scale %s", mode.w, mode.h, wide ? "on" : "off",
                tall != NULL && strtoul(tall, NULL, 10) != 0 ? "on" : "off",
                getenv("HNS_SCALE") != NULL ? getenv("HNS_SCALE") : "fit");
        Platform_SetWidescreen(wide);
        // The start screen sets HNS_TALL too (4:3 screens with widescreen on).
        if (tall != NULL && strtoul(tall, NULL, 10) != 0)
            Platform_SetTallScreen(TRUE);
    }
#else
    Platform_SetWidescreen(widescreen != NULL && strtoul(widescreen, NULL, 10) != 0);
    Platform_SetTallScreen(tall != NULL && strtoul(tall, NULL, 10) != 0);
#endif

#ifdef __ANDROID__
    {
        SDL_DisplayMode display;
        int windowW = gRenderWidth;
        int windowH = gRenderHeight;

        // The size passed here is the SurfaceView size. A game-sized window
        // is a short band in the middle of the phone, and the controls then
        // have nowhere to go except on top of the picture.
        if (SDL_GetDesktopDisplayMode(0, &display) == 0 && display.w > 0 && display.h > 0)
        {
            windowW = display.w;
            windowH = display.h;
        }
        sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     windowW, windowH,
                                     SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    }
#else
    sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, gRenderWidth * videoScale, gRenderHeight * videoScale,
                                 SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
    if (sdlRenderer == NULL)
    {
        DBGPRINTF("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    // Black, and cleared every frame: the bars around a picture that does not
    // fill the screen were white, and kept whatever the last frame left there.
    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SetScaleMode(getenv("HNS_SCALE"));
    {
        const char *mode = getenv("HNS_FAST_FORWARD");

        sFastForwardSmart = mode != NULL && strcmp(mode, "smart") == 0;
        sFastForwardToggle = sFastForwardSmart || (mode != NULL && strcmp(mode, "toggle") == 0);
    }

    // Created at the largest geometry. Only the top-left gRenderWidth x
    // gRenderHeight is uploaded and drawn, so widescreen and the tall screen
    // can change without a new texture.
    sdlTexture = SDL_CreateTexture(sdlRenderer,
                                   SDL_PIXELFORMAT_ABGR1555,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   MAX_RENDER_WIDTH, MAX_RENDER_HEIGHT);
    if (sdlTexture == NULL)
    {
        DBGPRINTF("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    simTime = curGameTime = lastGameTime = SDL_GetPerformanceCounter();

    isFrameAvailable.value = 0;
    vBlankSemaphore = SDL_CreateSemaphore(0);

    SDL_AudioSpec want;

    SDL_memset(&want, 0, sizeof(want)); /* or SDL_zero(want) */
    want.freq = 42048;
    want.format = AUDIO_F32;
    want.channels = 2;
    // ~97 ms. 1024 frames is only ~24 ms, and the callback underruns when the
    // game thread is busy drawing the touch controls before vsync.
    want.samples = 4096;
    cgb_audio_init(want.freq);


    if (SDL_OpenAudio(&want, 0) < 0)
        SDL_Log("Failed to open audio: %s", SDL_GetError());
    else
    {
        if (want.format != AUDIO_F32) /* we let this one thing change. */
            SDL_Log("We didn't get Float32 audio format.");
        SDL_PauseAudio(0);
    }

    memset(&internalClock, 0, sizeof(internalClock));
    internalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();

    sHeadless = headlessFrames != NULL;
    gAudioLog = getenv("HNS_AUDIO_LOG") != NULL;

    AgbMain();

    if (sHeadless)
    {
        int result = RunHeadless(strtoul(headlessFrames, NULL, 10));

        CloseSaveFile();
        SDL_Quit();
        return result;
    }

    double accumulator = 0.0;

    bool isGameStepDrawn = false;
    while (isRunning)
    {
        double deltaTime;

        ProcessEvents();

        curGameTime = SDL_GetPerformanceCounter();
        deltaTime = (double)((curGameTime - lastGameTime) / (double)SDL_GetPerformanceFrequency());
        //apply speedup
        if (!(sFastForwardSmart && timeScale > 1.0 && DualScreen_IsSlowScreen()))
            deltaTime *= timeScale;

        if (!paused)
        {
            accumulator += deltaTime;

            isGameStepDrawn = false;

            while (accumulator >= fixedTimestep)
            {
                //run game logic, draw frame and process DMAs and vblank
                ENTER_VBLANK(); //you must be in VBlank before running a game tick
                // Only the step that is drawn may ask for live tall margins,
                // not one before it that the overworld ran and nothing drew.
                gRenderMarginYLive = FALSE;
                MainLoop();
                DualScreen_FrameHook();
                if (!isGameStepDrawn)
                {
                    VDraw(sdlTexture);
                    //SDL_RenderClear(sdlRenderer);
                    isGameStepDrawn = true;
                }
                RunDMAsAndVBlank();

                accumulator -= fixedTimestep;
            }

            // Each AudioUpdate mixes one GBA frame of sound (about 701 stereo
            // float samples, 8 bytes each). Keep about 100 ms queued and refill
            // it completely each pass, so a slow frame or a late vsync on a
            // handheld does not empty the device buffer and leave a gap.
            RefillAudioQueue();
            ReportPerformance(SDL_GetPerformanceCounter());

            if (videoScaleChanged)
            {
#ifndef __ANDROID__
                // On Android this shrinks the SurfaceView to the game aspect,
                // which is why the picture sat in a band with the buttons on it.
                SDL_SetWindowSize(sdlWindow, gRenderWidth * videoScale, gRenderHeight * videoScale);
#endif
                videoScaleChanged = false;
            }
        }

        lastGameTime = curGameTime;

        {
#ifdef __ANDROID__
            TouchLayout layout;
            SDL_Rect frameRect = {0, 0, gRenderWidth, gRenderHeight};

            BuildTouchLayout(&layout);
            SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
            SDL_RenderSetViewport(sdlRenderer, NULL);
            SDL_RenderSetScale(sdlRenderer, 1.0f, 1.0f);
            SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
            SDL_RenderClear(sdlRenderer);
            SDL_RenderCopy(sdlRenderer, sdlTexture, &frameRect, &layout.game);
            DrawTouchControls(sdlRenderer, &layout);
#else
            SDL_Rect frameRect = {0, 0, gRenderWidth, gRenderHeight};

            SDL_RenderClear(sdlRenderer);
            SDL_RenderCopy(sdlRenderer, sdlTexture, &frameRect, NULL);
#endif
        }
        if (!paused)
            RefillAudioQueue();
        SDL_RenderPresent(sdlRenderer);
    }

    CloseSaveFile();

    SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    return 0;
}

static void ReadSaveFile(char *path)
{
    // Check whether the saveFile exists, and create it if not
    sSaveFile = fopen(path, "r+b");
    if (sSaveFile == NULL)
    {
        sSaveFile = fopen(path, "w+b");
    }
    if (sSaveFile == NULL)
    {
        // Neither readable and writable nor creatable (a save copied in by
        // another user, say). Load what can be read and do not save.
        FILE *readOnly = fopen(path, "rb");
        int bytesRead = 0;
        fprintf(stderr, "save file %s cannot be opened for writing; saving is off\n", path);
        if (readOnly != NULL)
        {
            bytesRead = fread(FLASH_BASE, 1, sizeof(FLASH_BASE), readOnly);
            fclose(readOnly);
        }
        for (int i = bytesRead; i < sizeof(FLASH_BASE); i++)
            FLASH_BASE[i] = 0xFF;
        return;
    }

    fseek(sSaveFile, 0, SEEK_END);
    int fileSize = ftell(sSaveFile);
    fseek(sSaveFile, 0, SEEK_SET);

    // Only read as many bytes as fit inside the buffer
    // or as many bytes as are in the file
    int bytesToRead = (fileSize < sizeof(FLASH_BASE)) ? fileSize : sizeof(FLASH_BASE);

    int bytesRead = fread(FLASH_BASE, 1, bytesToRead, sSaveFile);

    // Fill the buffer if the savefile was just created or smaller than the buffer itself
    for (int i = bytesRead; i < sizeof(FLASH_BASE); i++)
    {
        FLASH_BASE[i] = 0xFF;
    }
}

static void StoreSaveFile()
{
    if (sSaveFile != NULL)
    {
        fseek(sSaveFile, 0, SEEK_SET);
        fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), sSaveFile);
        // Android kills apps rather than letting them exit, so anything left in
        // the stdio buffer would be lost. Write it to the disk now.
        fflush(sSaveFile);
#ifndef _WIN32
        fsync(fileno(sSaveFile));
#endif
    }
}

// Goes to logcat on Android (tag SDL/APP) and to standard error elsewhere.
void Platform_Log(const char *message)
{
    SDL_Log("%s", message);
}

void Platform_ReportNullTask(u8 taskId, void *creator, const s16 *data)
{
    const char *name = "?";
    void *base = NULL;
#ifndef _WIN32
    Dl_info info;

    if (creator != NULL && dladdr(creator, &info) != 0)
    {
        if (info.dli_sname != NULL)
            name = info.dli_sname;
        base = info.dli_fbase;
    }
#endif
    SDL_Log("null task %u, created at %p (library offset %#lx, in %s), data %d %d %d %d %d %d %d %d",
            taskId, creator, (unsigned long)((char *)creator - (char *)base), name,
            data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
}

void Platform_StoreSaveFile(void)
{
    StoreSaveFile();
}

void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    DBGPRINTF("ReadFlash(sectorNum=0x%04X,offset=0x%08X,size=0x%02X)\n",sectorNum,offset,size);
    FILE * savefile = fopen("pokeemerald.sav", "r+b");
    if (savefile == NULL)
    {
        puts("Error opening save file.");
        return;
    }
    if (fseek(savefile, (sectorNum << gFlash->sector.shift) + offset, SEEK_SET))
    {
        fclose(savefile);
        return;
    }
    if (fread(dest, 1, size, savefile) != size)
    {
        fclose(savefile);
        return;
    }
    fclose(savefile);
}

void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame)
{
    // samplesPerFrame is a size in bytes.
    if (sHeadless)
    {
        WavWrite(audioBuffer, samplesPerFrame);
        return;
    }
    SDL_QueueAudio(1, audioBuffer, samplesPerFrame);
}


static void CloseSaveFile()
{
    if (sSaveFile != NULL)
    {
        fclose(sSaveFile);
    }
}

// Key mappings
#define KEY_A_BUTTON      SDLK_z
#define KEY_B_BUTTON      SDLK_x
#define KEY_START_BUTTON  SDLK_RETURN
#define KEY_SELECT_BUTTON SDLK_BACKSLASH
#define KEY_L_BUTTON      SDLK_a
#define KEY_R_BUTTON      SDLK_s
#define KEY_DPAD_UP       SDLK_UP
#define KEY_DPAD_DOWN     SDLK_DOWN
#define KEY_DPAD_LEFT     SDLK_LEFT
#define KEY_DPAD_RIGHT    SDLK_RIGHT

#define HANDLE_KEYUP(key) \
case KEY_##key:  keys &= ~key; break;

#define HANDLE_KEYDOWN(key) \
case KEY_##key:  keys |= key; break;

// Game controllers. Buttons follow the labels SDL reports: the button the pad
// calls A is GBA A. On the AYN Thor that is the right face button, as on a GBA.
static u16 sPadKeys;
static bool sPadFastForward;
// Whether the right trigger is in. It counts as released only below a lower
// threshold, so a trigger resting near the line does not toggle twice.
static bool sPadTriggerIn;

// Android touchscreen controls. Positions are output pixels, the same space
// the buttons are drawn in. A finger stays held until it lifts.
typedef struct { SDL_FingerID id; int x, y; bool active; } TouchPoint;
static TouchPoint sTouchPoints[10];
static u16 sTouchKeys;

static void TouchOutputSize(int *w, int *h)
{
    *w = 0;
    *h = 0;
    if (sdlRenderer != NULL)
        SDL_GetRendererOutputSize(sdlRenderer, w, h);
    if ((*w <= 0 || *h <= 0) && sdlWindow != NULL)
        SDL_GetWindowSize(sdlWindow, w, h);
    if (*w <= 0)
        *w = gRenderWidth;
    if (*h <= 0)
        *h = gRenderHeight;
}

static void PlaceTouchRows(TouchLayout *layout, int left, int top, int right, int bottom)
{
    int w = right - left;
    int bandH = bottom - top;
    int rowH, btnH, btnW, face, reach, gap;

    if (w < 8)
        w = 8;
    if (bandH < 12)
        bandH = 12;
    gap = bandH / 30;
    if (gap < 2)
        gap = 2;
    top += gap;
    bandH = bottom - top;
    if (bandH < 12)
        bandH = 12;
    rowH = bandH / 3;
    btnH = rowH * 72 / 100;
    if (btnH < 8)
        btnH = 8;
    btnW = w * 22 / 100;
    if (btnW < btnH)
        btnW = btnH;
    layout->l = (SDL_Rect){ left + w * 4 / 100, top + (rowH - btnH) / 2, btnW, btnH };
    layout->r = (SDL_Rect){ right - w * 4 / 100 - btnW, top + (rowH - btnH) / 2, btnW, btnH };

    reach = rowH * 40 / 100;
    if (reach < 8)
        reach = 8;
    layout->dpadReach = reach;
    layout->dpadCx = left + w * 24 / 100;
    if (layout->dpadCx - reach < left)
        layout->dpadCx = left + reach;
    layout->dpadCy = top + rowH + rowH / 2;
    if (layout->dpadCy - reach < top + rowH)
        layout->dpadCy = top + rowH + reach;
    if (layout->dpadCy + reach > top + rowH * 2)
        layout->dpadCy = top + rowH * 2 - reach;

    face = btnH;
    if (face > w * 18 / 100)
        face = w * 18 / 100;
    if (face < 8)
        face = 8;
    layout->a = (SDL_Rect){ left + w * 82 / 100 - face / 2, layout->dpadCy - face / 2, face, face };
    layout->b = (SDL_Rect){ left + w * 62 / 100 - face / 2, layout->dpadCy - face / 2, face, face };
    if (layout->a.x + layout->a.w > right)
        layout->a.x = right - layout->a.w;
    if (layout->b.x < left)
        layout->b.x = left;

    {
        int sw = w * 28 / 100;
        int sh = btnH * 70 / 100;
        int sy = top + rowH * 2 + (rowH - sh) / 2;

        if (sh < 8)
            sh = 8;
        if (sy < top + rowH * 2)
            sy = top + rowH * 2;
        if (sy + sh > bottom)
            sy = bottom - sh;
        layout->selectBtn = (SDL_Rect){ left + w / 2 - sw - w / 40, sy, sw, sh };
        layout->startBtn = (SDL_Rect){ left + w / 2 + w / 40, sy, sw, sh };
    }
}

static void BuildTouchLayout(TouchLayout *layout)
{
    int w, h, gw, gh;
    int topInset, controls, gameMaxW, gameMaxH;

    TouchOutputSize(&w, &h);
    memset(layout, 0, sizeof(*layout));
    layout->w = w;
    layout->h = h;
    gw = gRenderWidth > 0 ? gRenderWidth : 240;
    gh = gRenderHeight > 0 ? gRenderHeight : 160;

    if (h >= w)
    {
        // Portrait. The top strip is the existing BOTTOM SCREEN button.
        // The picture keeps its shape under that, and every control is
        // below the picture.
        topInset = h / 10;
        controls = h * 46 / 100;
        if (topInset + controls >= h)
            controls = h - topInset - gh;
        if (controls < h / 5)
            controls = h / 5;
        gameMaxH = h - topInset - controls;
        gameMaxW = w;
        if (gameMaxH < 1)
            gameMaxH = 1;
        if ((long)gameMaxW * gh <= (long)gameMaxH * gw)
        {
            layout->game.w = gameMaxW;
            layout->game.h = (int)((long)gameMaxW * gh / gw);
        }
        else
        {
            layout->game.h = gameMaxH;
            layout->game.w = (int)((long)gameMaxH * gw / gh);
        }
        if (layout->game.w < 1)
            layout->game.w = 1;
        if (layout->game.h < 1)
            layout->game.h = 1;
        layout->game.x = (w - layout->game.w) / 2;
        layout->game.y = topInset + (gameMaxH - layout->game.h) / 2;
        PlaceTouchRows(layout, 0, layout->game.y + layout->game.h, w, h);
    }
    else
    {
        controls = w * 42 / 100;
        gameMaxW = w - controls;
        gameMaxH = h;
        if (gameMaxW < 1)
            gameMaxW = 1;
        if ((long)gameMaxW * gh <= (long)gameMaxH * gw)
        {
            layout->game.w = gameMaxW;
            layout->game.h = (int)((long)gameMaxW * gh / gw);
        }
        else
        {
            layout->game.h = gameMaxH;
            layout->game.w = (int)((long)gameMaxH * gw / gh);
        }
        if (layout->game.w < 1)
            layout->game.w = 1;
        if (layout->game.h < 1)
            layout->game.h = 1;
        layout->game.x = 0;
        layout->game.y = (h - layout->game.h) / 2;
        PlaceTouchRows(layout, layout->game.x + layout->game.w, 0, w, h);
    }
}

static int TouchRectContains(const SDL_Rect *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static u16 TouchKeysAt(int x, int y)
{
    TouchLayout layout;
    int dx, dy, ax, ay;
    u16 result = 0;

    BuildTouchLayout(&layout);
    if (TouchRectContains(&layout.l, x, y))
        result |= L_BUTTON;
    if (TouchRectContains(&layout.r, x, y))
        result |= R_BUTTON;
    if (TouchRectContains(&layout.a, x, y))
        result |= A_BUTTON;
    if (TouchRectContains(&layout.b, x, y))
        result |= B_BUTTON;
    if (TouchRectContains(&layout.selectBtn, x, y))
        result |= SELECT_BUTTON;
    if (TouchRectContains(&layout.startBtn, x, y))
        result |= START_BUTTON;
    dx = x - layout.dpadCx;
    dy = y - layout.dpadCy;
    ax = dx < 0 ? -dx : dx;
    ay = dy < 0 ? -dy : dy;
    if (ax <= layout.dpadReach && ay <= layout.dpadReach)
    {
        int slop = layout.dpadReach / 5;

        if (dx < -slop)
            result |= DPAD_LEFT;
        if (dx > slop)
            result |= DPAD_RIGHT;
        if (dy < -slop)
            result |= DPAD_UP;
        if (dy > slop)
            result |= DPAD_DOWN;
    }
    return result;
}

static void RebuildTouchKeys(void)
{
    int i;
    sTouchKeys = 0;
    for (i = 0; i < (int)(sizeof(sTouchPoints) / sizeof(sTouchPoints[0])); i++)
        if (sTouchPoints[i].active)
            sTouchKeys |= TouchKeysAt(sTouchPoints[i].x, sTouchPoints[i].y);
}

static void HandleTouchEvent(const SDL_TouchFingerEvent *finger, bool down)
{
    int i, freeSlot = -1;
    int ow = 0, oh = 0;
    int x, y;

    // SDL_TouchFingerEvent x/y are 0..1 across the window. Rects are renderer
    // output pixels. Multiply by the output size, not the letterboxed game.
    TouchOutputSize(&ow, &oh);
    if (finger->x > 1.0f || finger->y > 1.0f)
    {
        x = (int)finger->x;
        y = (int)finger->y;
    }
    else
    {
        x = (int)(finger->x * ow);
        y = (int)(finger->y * oh);
    }

    for (i = 0; i < (int)(sizeof(sTouchPoints) / sizeof(sTouchPoints[0])); i++) {
        if (sTouchPoints[i].active && sTouchPoints[i].id == finger->fingerId) {
            if (down) {
                sTouchPoints[i].x = x;
                sTouchPoints[i].y = y;
            } else {
                sTouchPoints[i].active = false;
            }
            RebuildTouchKeys();
            return;
        }
        if (!sTouchPoints[i].active && freeSlot < 0) freeSlot = i;
    }
    if (down && freeSlot >= 0) {
        sTouchPoints[freeSlot].id = finger->fingerId;
        sTouchPoints[freeSlot].x = x;
        sTouchPoints[freeSlot].y = y;
        sTouchPoints[freeSlot].active = true;
        RebuildTouchKeys();
    }
}

#ifdef __ANDROID__
static Uint8 TouchGlyphRow(char ch, int row)
{
    static const Uint8 a[5] = { 0x2, 0x5, 0x7, 0x5, 0x5 };
    static const Uint8 b[5] = { 0x6, 0x5, 0x6, 0x5, 0x6 };
    static const Uint8 e[5] = { 0x7, 0x4, 0x6, 0x4, 0x7 };
    static const Uint8 l[5] = { 0x4, 0x4, 0x4, 0x4, 0x7 };
    static const Uint8 r[5] = { 0x6, 0x5, 0x6, 0x5, 0x5 };
    static const Uint8 s[5] = { 0x3, 0x4, 0x2, 0x1, 0x6 };
    static const Uint8 t[5] = { 0x7, 0x2, 0x2, 0x2, 0x2 };
    switch (ch)
    {
    case 'A': return a[row];
    case 'B': return b[row];
    case 'E': return e[row];
    case 'L': return l[row];
    case 'R': return r[row];
    case 'S': return s[row];
    case 'T': return t[row];
    default: return 0;
    }
}

static void DrawTouchText(SDL_Renderer *renderer, const char *text, int centerX, int centerY, int pixel)
{
    SDL_Rect dot;
    int length = (int)strlen(text);
    int width, x, y, i, row, col;
    if (length <= 0) return;
    if (pixel < 1) pixel = 1;
    width = (length * 4 - 1) * pixel;
    x = centerX - width / 2;
    y = centerY - (5 * pixel) / 2;
    dot.w = pixel;
    dot.h = pixel;
    for (i = 0; i < length; i++)
    {
        for (row = 0; row < 5; row++)
        {
            Uint8 bits = TouchGlyphRow(text[i], row);
            for (col = 0; col < 3; col++)
            {
                if (bits & (1 << (2 - col)))
                {
                    dot.x = x + i * 4 * pixel + col * pixel;
                    dot.y = y + row * pixel;
                    SDL_RenderFillRect(renderer, &dot);
                }
            }
        }
    }
}

static void DrawTouchControls(SDL_Renderer *renderer, const TouchLayout *layout)
{
    SDL_Rect bar;
    int thick = layout->dpadReach / 2;

    if (thick < 4)
        thick = 4;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 18, 24, 36, 210);
    bar = (SDL_Rect){ layout->dpadCx - layout->dpadReach, layout->dpadCy - thick / 2,
                      layout->dpadReach * 2, thick };
    SDL_RenderFillRect(renderer, &bar);
    bar = (SDL_Rect){ layout->dpadCx - thick / 2, layout->dpadCy - layout->dpadReach,
                      thick, layout->dpadReach * 2 };
    SDL_RenderFillRect(renderer, &bar);

    SDL_SetRenderDrawColor(renderer, 48, 56, 72, 230);
    SDL_RenderFillRect(renderer, &layout->l);
    SDL_RenderFillRect(renderer, &layout->r);
    SDL_SetRenderDrawColor(renderer, 35, 112, 74, 230);
    SDL_RenderFillRect(renderer, &layout->a);
    SDL_SetRenderDrawColor(renderer, 142, 58, 52, 230);
    SDL_RenderFillRect(renderer, &layout->b);
    SDL_SetRenderDrawColor(renderer, 30, 34, 44, 230);
    SDL_RenderFillRect(renderer, &layout->selectBtn);
    SDL_RenderFillRect(renderer, &layout->startBtn);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    DrawTouchText(renderer, "L", layout->l.x + layout->l.w / 2, layout->l.y + layout->l.h / 2, 2);
    DrawTouchText(renderer, "R", layout->r.x + layout->r.w / 2, layout->r.y + layout->r.h / 2, 2);
    DrawTouchText(renderer, "A", layout->a.x + layout->a.w / 2, layout->a.y + layout->a.h / 2, 2);
    DrawTouchText(renderer, "B", layout->b.x + layout->b.w / 2, layout->b.y + layout->b.h / 2, 2);
    DrawTouchText(renderer, "SEL", layout->selectBtn.x + layout->selectBtn.w / 2,
                  layout->selectBtn.y + layout->selectBtn.h / 2, 1);
    DrawTouchText(renderer, "START", layout->startBtn.x + layout->startBtn.w / 2,
                  layout->startBtn.y + layout->startBtn.h / 2, 1);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}
#endif


static u16 PadKeyFromButton(Uint8 button)
{
    switch (button)
    {
    case SDL_CONTROLLER_BUTTON_A:             return A_BUTTON;
    case SDL_CONTROLLER_BUTTON_B:             return B_BUTTON;
    case SDL_CONTROLLER_BUTTON_START:         return START_BUTTON;
    case SDL_CONTROLLER_BUTTON_BACK:          return SELECT_BUTTON;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return L_BUTTON;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return R_BUTTON;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return DPAD_RIGHT;
    default:                                  return 0;
    }
}

#define PAD_STICK_THRESHOLD 16000
#define PAD_TRIGGER_RELEASE 8000

static u16 PadStickKeys(void)
{
    u16 result = 0;
    int i;

    for (i = 0; i < SDL_NumJoysticks(); i++)
    {
        SDL_GameController *pad = SDL_IsGameController(i) ? SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i)) : NULL;
        int x, y;

        if (pad == NULL)
            continue;
        x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (x < -PAD_STICK_THRESHOLD) result |= DPAD_LEFT;
        if (x >  PAD_STICK_THRESHOLD) result |= DPAD_RIGHT;
        if (y < -PAD_STICK_THRESHOLD) result |= DPAD_UP;
        if (y >  PAD_STICK_THRESHOLD) result |= DPAD_DOWN;
    }
    return result;
}

void ProcessEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            isRunning = false;
            break;
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameControllerOpen(event.cdevice.which);
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            sPadKeys |= PadKeyFromButton(event.cbutton.button);
            break;
        case SDL_CONTROLLERBUTTONUP:
            sPadKeys &= ~PadKeyFromButton(event.cbutton.button);
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            {
                // The right trigger fast forwards: while held, or on and
                // off with each pull (HNS_FAST_FORWARD=toggle).
                bool in = sPadTriggerIn ? event.caxis.value > PAD_TRIGGER_RELEASE
                                        : event.caxis.value > PAD_STICK_THRESHOLD;

                if (in == sPadTriggerIn)
                    break;
                sPadTriggerIn = in;
                if (sFastForwardToggle)
                {
                    if (in)
                        sPadFastForward = !sPadFastForward;
                }
                else
                {
                    sPadFastForward = in;
                }
                if (!speedUp)
                    timeScale = sPadFastForward ? 5.0 : 1.0;
            }
            break;
        case SDL_KEYUP:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYUP(A_BUTTON)
            HANDLE_KEYUP(B_BUTTON)
            HANDLE_KEYUP(START_BUTTON)
            HANDLE_KEYUP(SELECT_BUTTON)
            HANDLE_KEYUP(L_BUTTON)
            HANDLE_KEYUP(R_BUTTON)
            HANDLE_KEYUP(DPAD_UP)
            HANDLE_KEYUP(DPAD_DOWN)
            HANDLE_KEYUP(DPAD_LEFT)
            HANDLE_KEYUP(DPAD_RIGHT)
            case SDLK_SPACE:
                if (speedUp)
                {
                    speedUp = false;
                    timeScale = 1.0;
                    //SDL_ClearQueuedAudio(1);
                    //SDL_PauseAudio(0);
                }
                break;
            }
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYDOWN(A_BUTTON)
            HANDLE_KEYDOWN(B_BUTTON)
            HANDLE_KEYDOWN(START_BUTTON)
            HANDLE_KEYDOWN(SELECT_BUTTON)
            HANDLE_KEYDOWN(L_BUTTON)
            HANDLE_KEYDOWN(R_BUTTON)
            HANDLE_KEYDOWN(DPAD_UP)
            HANDLE_KEYDOWN(DPAD_DOWN)
            HANDLE_KEYDOWN(DPAD_LEFT)
            HANDLE_KEYDOWN(DPAD_RIGHT)
            case SDLK_r:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    DoSoftReset();
                }
                break;
            case SDLK_p:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    paused = !paused;
                }
                break;
            case SDLK_SPACE:
                if (!speedUp)
                {
                    speedUp = true;
                    timeScale = 5.0;
                    //SDL_PauseAudio(1);
                }
                break;
            }
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
            HandleTouchEvent(&event.tfinger, true);
            break;
        case SDL_FINGERUP:
            HandleTouchEvent(&event.tfinger, false);
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            {
                unsigned int w = event.window.data1;
                unsigned int h = event.window.data2;
                
                videoScale = 0;
                if (w / gRenderWidth > videoScale)
                    videoScale = w / gRenderWidth;
                if (h / gRenderHeight > videoScale)
                    videoScale = h / gRenderHeight;
                if (videoScale < 1)
                    videoScale = 1;

                videoScaleChanged = true;
            }
            break;
        }
    }
}

#ifdef _WIN32
#define STICK_THRESHOLD 0.5f
u16 GetXInputKeys()
{
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));

    DWORD dwResult = XInputGetState(0, &state);
    u16 xinputKeys = 0;

    if (dwResult == ERROR_SUCCESS)
    {
        /* A */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) >> 12;
        /* B */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) >> 13;
        /* Start */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) >> 1;
        /* Select */ xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) >> 3;
        /* L */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) << 1;
        /* R */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) >> 1;
        /* Up */     xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) << 6;
        /* Down */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) << 6;
        /* Left */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) << 3;
        /* Right */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) << 1;


        /* Control Stick */
        float xAxis = (float)state.Gamepad.sThumbLX / (float)SHRT_MAX;
        float yAxis = (float)state.Gamepad.sThumbLY / (float)SHRT_MAX;

        if (xAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_LEFT;
        if (xAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_RIGHT;
        if (yAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_DOWN;
        if (yAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_UP;


        /* Speedup */
        // Note: 'speedup' variable is only (un)set on keyboard input
        double oldTimeScale = timeScale;
        timeScale = (state.Gamepad.bRightTrigger > 0x80 || speedUp) ? 5.0 : 1.0;

        if (oldTimeScale != timeScale)
        {
            if (timeScale > 1.0)
            {
                SDL_PauseAudio(1);
            }
            else
            {
                SDL_ClearQueuedAudio(1);
                SDL_PauseAudio(0);
            }
        }
    }

    return xinputKeys;
}
#endif // _WIN32

u16 Platform_GetKeyInput(void)
{
#ifdef _WIN32
    u16 gamepadKeys = GetXInputKeys();
    return (gamepadKeys != 0) ? gamepadKeys : keys;
#endif

    return keys | sPadKeys | sTouchKeys | PadStickKeys() | DualScreen_ConsumeInjectedKeys();
}

void VDraw(SDL_Texture *texture)
{
    // DrawFrame packs gRenderHeight rows at gRenderWidth, so upload that part
    // of the (larger) texture.
    SDL_Rect frameRect = {0, 0, gRenderWidth, gRenderHeight};

    memset(sFrameImage, 0, gRenderWidth * gRenderHeight * sizeof(sFrameImage[0]));
    DrawFrame(sFrameImage);
    SDL_UpdateTexture(texture, &frameRect, sFrameImage, gRenderWidth * sizeof (Uint16));
    REG_VCOUNT = 161; // prep for being in VBlank period
}

int DoMain(void *data)
{
    AgbMain();
}

void VBlankIntrWait(void)
{
    return;
}

u8 BinToBcd(u8 bin)
{
    int placeCounter = 1;
    u8 out = 0;
    do
    {
        out |= (bin % 10) * placeCounter;
        placeCounter *= 16;
    }
    while ((bin /= 10) > 0);

    return out;
}

void Platform_GetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = internalClock.status;
}

void Platform_SetStatus(struct SiiRtcInfo *rtc)
{
    internalClock.status = rtc->status;
}

static void UpdateInternalClock(void)
{
    const char *fixedClock = getenv("HNS_CLOCK");
    time_t rawTime = time(NULL);
    struct tm *time;

    if (fixedClock != NULL)
    {
        rawTime = (time_t)strtoll(fixedClock, NULL, 10) + sHeadlessFrame / 60;
        time = gmtime(&rawTime);
    }
    else
    {
        time = localtime(&rawTime);
    }

    internalClock.year = BinToBcd(time->tm_year - 100);
    internalClock.month = BinToBcd(time->tm_mon + 1);
    internalClock.day = BinToBcd(time->tm_mday);
    internalClock.dayOfWeek = BinToBcd(time->tm_wday);
    internalClock.hour = BinToBcd(time->tm_hour);
    internalClock.minute = BinToBcd(time->tm_min);
    internalClock.second = BinToBcd(time->tm_sec);
}

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->year = internalClock.year;
    rtc->month = internalClock.month;
    rtc->day = internalClock.day;
    rtc->dayOfWeek = internalClock.dayOfWeek;
    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetDateTime: %d-%02d-%02d %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->year),
                                                         ConvertBcdToBinary(rtc->month),
                                                         ConvertBcdToBinary(rtc->day),
                                                         ConvertBcdToBinary(rtc->hour),
                                                         ConvertBcdToBinary(rtc->minute),
                                                         ConvertBcdToBinary(rtc->second));
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    internalClock.month = rtc->month;
    internalClock.day = rtc->day;
    internalClock.dayOfWeek = rtc->dayOfWeek;
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetTime: %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->hour),
                                        ConvertBcdToBinary(rtc->minute),
                                        ConvertBcdToBinary(rtc->second));
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_SetAlarm(u8 *alarmData)
{
    // TODO
}

void SoftReset(u32 resetFlags)
{
    puts("Soft Reset called. Exiting.");
    exit(0);
}

#endif