/* main.c -- FF4: The After Years Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * The Cuore engine (libff4a.so) is a GLSurfaceView-style renderer: on Android
 * the Java MainActivity owned the EGL context and the GameThread pushed
 * lifecycle + per-frame render calls into six native entry points. We recreate
 * that here -- a GLES1 context, a fake JNI MainActivity, and a libnx main loop
 * driving resume()/setViewport()/render()/touch()/pause()/release().
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "obb.h"
#include "gfx.h"
#include "opensles.h"
#include "movie_player.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module game_mod; // libff4a.so

#define INTRO_MOVIE "opening.mkv"

// provide a replacement heap init so the newlib heap is separate from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = umin(size, MEMORY_MB * 1024 * 1024);
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  const char *files[] = { SO_NAME, OBB_NAME };
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++)
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s.\nCheck your data files.", files[i]);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

// ---------------------------------------------------------------------------
// EGL / GLES1 context on the default NWindow
// ---------------------------------------------------------------------------

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) { debugPrintf("egl: no display\n"); return 0; }
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) { debugPrintf("egl: bindAPI failed\n"); return 0; }

  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, // GLES1
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) {
    debugPrintf("egl: no config\n");
    return 0;
  }

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) { debugPrintf("egl: no surface\n"); return 0; }

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) { debugPrintf("egl: no context\n"); return 0; }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY)
    return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// engine entry points (signatures recovered from libff4a + classes.dex)
// ---------------------------------------------------------------------------

static void (*e_setViewport)(void *env, void *thiz, int x, int y, int w, int h);
static void (*e_touch)(void *env, void *thiz, int action, int count, float x0, float y0, float x1, float y1);
static void (*e_render)(void *env, void *thiz, int frame);
static void (*e_resume)(void *env, void *thiz);
static void (*e_pause)(void *env, void *thiz);
static void (*e_release)(void *env, void *thiz);

static void resolve_entry_points(void) {
  e_setViewport = (void *)so_find_addr_rx(&game_mod, "setViewport");
  e_touch       = (void *)so_find_addr_rx(&game_mod, "touch");
  e_render      = (void *)so_find_addr_rx(&game_mod, "render");
  e_resume      = (void *)so_find_addr_rx(&game_mod, "resume");
  e_pause       = (void *)so_find_addr_rx(&game_mod, "pause");
  e_release     = (void *)so_find_addr_rx(&game_mod, "release");
}

// ---------------------------------------------------------------------------
// input: touch panel, normalised into the view
// (the engine compares coords against a 16/width threshold, i.e. they are in
//  [0,1] relative to the letterboxed game view)
// ---------------------------------------------------------------------------

// The engine's touch(env, thiz, e, f, x0,y0,x1,y1) takes e = current touch-point
// count and f = max count (NOT an action code); it derives press/move/release
// from the count transitions and compares f==1 for the single-touch path.
// Coordinates are normalised [0,1] within the game view.

static void *thiz;

static void norm_in_view(float px, float py, float *nx, float *ny) {
  *nx = (px - jni_view_x) / (float)jni_view_w;
  *ny = (py - jni_view_y) / (float)jni_view_h;
}

static int touch_down = 0;
static float touch_lx = 0, touch_ly = 0;

// touch() also clears the engine's `cont` button accumulator (render() does
// `cont |= getKeyEvent()` each frame and never clears it). It must be called
// every frame (count=0 when idle), or held directions latch and the cursor
// flies through menus -- so this runs whether or not the panel is touched.
static void update_touch(void) {
  int count = 0;
  float nx = 0, ny = 0, nx1 = 0, ny1 = 0;

  HidTouchScreenState st = {0};
  if (hidGetTouchScreenStates(&st, 1) && st.count > 0) {
    // libnx reports panel coordinates in 1280x720 space; scale to our screen
    const float sx = (float)screen_width / 1280.0f;
    const float sy = (float)screen_height / 720.0f;
    norm_in_view(st.touches[0].x * sx, st.touches[0].y * sy, &nx, &ny);
    nx1 = nx; ny1 = ny;
    if (st.count > 1)
      norm_in_view(st.touches[1].x * sx, st.touches[1].y * sy, &nx1, &ny1);
    count = st.count;
    touch_lx = nx;
    touch_ly = ny;
  }

  if (count > 0 && !touch_down) {
    touch_down = 1;
    debugPrintf("touch DOWN n=%d (%.3f,%.3f)\n", count, nx, ny);
  } else if (count == 0 && touch_down) {
    touch_down = 0;
    debugPrintf("touch UP\n");
    nx = nx1 = touch_lx;  // report the release at the last press position
    ny = ny1 = touch_ly;
  }

  // Always call: count>0 feeds the panel; count==0 clears `cont` for this frame.
  e_touch(fake_env, thiz, count, count, nx, ny, nx1, ny1);
}

// engine button bitmask bits (from MainActivity's keycode->bit table)
#define K_A     0x00001 // confirm
#define K_B     0x00002 // cancel
#define K_RIGHT 0x00010
#define K_LEFT  0x00020
#define K_UP    0x00040
#define K_DOWN  0x00080
#define K_R1    0x00100 // map
#define K_L1    0x00200
#define K_Y     0x00400 // pause menu
#define K_X     0x00800

static PadState pad;

static void update_keys(void) {
  padUpdate(&pad);
  const u64 d = padGetButtons(&pad);
  int m = 0;
  // Nintendo-native: A (right) confirms, B (bottom) cancels
  if (d & HidNpadButton_A) m |= K_A;
  if (d & HidNpadButton_B) m |= K_B;
  if (d & HidNpadButton_X) m |= K_X;
  if (d & HidNpadButton_Y) m |= K_Y;
  if (d & HidNpadButton_L) m |= K_L1;
  if (d & HidNpadButton_R) m |= K_R1;
  if (d & HidNpadButton_Plus)  m |= K_Y;
  if (d & HidNpadButton_Minus) m |= K_R1;
  // d-pad and left stick both drive directions
  if (d & (HidNpadButton_Up    | HidNpadButton_StickLUp))    m |= K_UP;
  if (d & (HidNpadButton_Down  | HidNpadButton_StickLDown))  m |= K_DOWN;
  if (d & (HidNpadButton_Left  | HidNpadButton_StickLLeft))  m |= K_LEFT;
  if (d & (HidNpadButton_Right | HidNpadButton_StickLRight)) m |= K_RIGHT;

  // Send the raw held-button state, exactly like Android's `g` bitmask. The
  // engine has its own menu auto-repeat keyed off this held state; with the
  // frame rate now capped, that repeat runs at the intended speed. (An earlier
  // wrapper-side auto-repeat fought the engine's tracking and made the cursor
  // latch into continuous scroll.)
  static int log_m = -1;
  if (m != log_m) { debugPrintf("keys: 0x%x\n", m); log_m = m; }
  jni_set_keys(m);
}

static void play_intro_movie(void) {
  struct stat st;
  if (stat(INTRO_MOVIE, &st) < 0 || st.st_size <= 0)
    return;

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  eglSwapBuffers(s_display, s_surface);

  debugPrintf("intro: playing %s\n", INTRO_MOVIE);
  if (!movie_play(INTRO_MOVIE, 1))
    return;

  while (appletMainLoop() && movie_is_playing()) {
    padUpdate(&pad);
    const u64 down = padGetButtonsDown(&pad);
    if (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X |
                HidNpadButton_Y | HidNpadButton_Plus | HidNpadButton_Minus))
      movie_skip();
    movie_main_loop_tick();
  }
  movie_stop();

  glClear(GL_COLOR_BUFFER_BIT);
  eglSwapBuffers(s_display, s_surface);
}

// watchdog: if the render loop stalls (engine blocked on a worker thread),
// force a crash after a grace period so Atmosphere dumps every thread's stack
static volatile int g_render_frames = 0;
#if DEBUG_LOG
static void watchdog_thread(void *arg) {
  (void)arg;
  debugPrintf("watchdog: armed\n");
  svcSleepThread(10000000000ULL); // 10 s
  debugPrintf("watchdog: frames=%d after 10s\n", g_render_frames);
  if (g_render_frames < 30) {
    debugPrintf("watchdog: render stalled -- forcing crash for a dump\n");
    svcSleepThread(200000000ULL); // let the log flush
    __builtin_trap(); // brk -> Atmosphere all-thread crash report
  }
}
#endif

int main(void) {
  cpu_boost(1);

  const int config_rc = read_config(CONFIG_NAME);
  if (config_rc != 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

  // shared system font for drawFont
  plInitialize(PlServiceType_User);
  gfx_init();

  // SDL is used as a library (audio only); tell it main is already running
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO) < 0)
    debugPrintf("SDL_Init(audio) failed: %s\n", SDL_GetError());

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES context.");

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();
  play_intro_movie();

  debugPrintf("heap: newlib %u MB, .so zone %u KB at %p\n",
      MEMORY_MB, (unsigned)(heap_so_limit / 1024), heap_so_base);

  if (so_load(&game_mod, SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  update_imports();
  so_relocate(&game_mod);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  // resolve the engine entry points now, while the symbol table (which lives in
  // load_base) is still accessible -- so_finalize maps the code via
  // svcMapProcessCodeMemory, which locks load_base out
  resolve_entry_points();
  if (!e_render || !e_resume || !e_setViewport)
    fatal_error("Could not resolve engine entry points.");

  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  // libff4a reads its stack-protector canary from tpidr_el0+0x28; set it up on
  // this (main) thread before any engine code runs
  tls_setup_guard();

  so_execute_init_array(&game_mod); // engine C++ constructors / static init
  so_free_temp(&game_mod);

  jni_init();
  thiz = jni_make_thiz();

  if (obb_open(OBB_NAME) < 0)
    fatal_error("Could not open\n%s.\nCheck your data files.", OBB_NAME);

  jni_compute_viewport();

  debugPrintf("resume()\n");
  e_resume(fake_env, thiz);

  debugPrintf("setViewport(%d,%d %dx%d)\n", jni_view_x, jni_view_y, jni_view_w, jni_view_h);
  e_setViewport(fake_env, thiz, jni_view_x, jni_view_y, jni_view_w, jni_view_h);

  int frame = 0;
  int boot_frames = 0;
#if DEBUG_LOG
  Thread wd;
  Result wrc = threadCreate(&wd, watchdog_thread, NULL, NULL, 0x8000, 0x2C, -2);
  debugPrintf("watchdog: threadCreate rc=0x%x\n", wrc);
  if (R_SUCCEEDED(wrc))
    threadStart(&wd);
#endif
  // Fixed-timestep engine: loop rate = game speed, so it must match the engine's
  // setFPS target. Pace via vsync on the 60 Hz panel -- present every (60/target)
  // vblanks (interval 2 = 30 fps), which throttles the loop and yields the CPU.
  int last_interval = 1; // matches egl_init's eglSwapInterval(...,1)
  while (appletMainLoop() && !jni_quit_requested) {
    update_keys();
    update_touch();

    const int target = jni_get_target_fps();
    int interval = 60 / (target > 0 ? target : 60);
    if (interval < 1) interval = 1;
    if (interval != last_interval) {
      eglSwapInterval(s_display, interval);
      last_interval = interval;
    }

    e_render(fake_env, thiz, frame);
    eglSwapBuffers(s_display, s_surface);

    if (frame < 5 || (frame % 120) == 0)
      debugPrintf("render: frame %d done\n", frame);
    frame++;
    g_render_frames = frame;

    if (boot_frames < 10 && ++boot_frames == 10)
      cpu_boost(0);
  }

  debugPrintf("shutting down\n");
  if (e_pause)   e_pause(fake_env, thiz);
  if (e_release) e_release(fake_env, thiz);

  opensles_shutdown();
  egl_deinit();
  obb_close();
  plExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
