/* Minimal KAGE backend -- exists only so linking succeeds and we can watch how
 * far startup gets.
 *
 * KAGE is the game engine's platform/rendering layer. A real Android version
 * needs EGL + GLES2 wired in here; for now everything returns "success but does
 * nothing" so the game keeps running and we can see what it asks for next.
 */
#include <stdint.h>
#include <string.h>
#include "kage_pc_backend.h"
#include "guest.h"

static int g_mode = 0;
static int g_ready = 0;
static double g_t0 = 0;

int  kage_pc_backend_set_mode(int m) { g_mode = m; return 1; }
int  kage_pc_backend_mode(void) { return g_mode; }

int  kage_pc_backend_initialize(uint32_t w, uint32_t h) {
    (void)w; (void)h; g_ready = 1; return 1;
}
void kage_pc_backend_shutdown(void) { g_ready = 0; }
int  kage_pc_backend_present(void) { return 1; }

void kage_pc_backend_note_loop_head(void) {}
void kage_pc_backend_note_service_entry(void) {}
void kage_pc_backend_note_update_entry(void) {}
void kage_pc_backend_audio_cooperative_poll(struct CPU *c) { (void)c; }
void kage_pc_backend_note_render_entry(void) {}
void kage_pc_backend_note_render_return(void) {}
void kage_pc_backend_note_limiter_entry(void) {}
void kage_pc_backend_note_limiter_exit(void) {}
void kage_pc_backend_note_menu_init(uint32_t s) { (void)s; }
void kage_pc_backend_note_menu_render(uint32_t s) { (void)s; }
void kage_pc_backend_note_menu_character(uint32_t s) { (void)s; }
void kage_pc_backend_note_menu_character_wheel_snapshot(
    uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }

void kage_pc_backend_keyboard_snapshot(uint8_t keys[KAGE_PC_KEY_COUNT]) {
    memset(keys, 0, KAGE_PC_KEY_COUNT);
}

int  kage_pc_backend_console_trace_enabled(void) { return 0; }
int  kage_pc_backend_note_console_command(uint32_t st) { (void)st; return 0; }

int  kage_pc_backend_virtual_clipboard_open(uintptr_t o, uint32_t *r) {
    (void)o; if (r) *r = 0; return 1;
}
int  kage_pc_backend_virtual_clipboard_get_data(uint32_t f, uintptr_t *r) {
    (void)f; if (r) *r = 0; return 1;
}
int  kage_pc_backend_virtual_global_lock(uintptr_t h, uintptr_t *r) {
    (void)h; if (r) *r = 0; return 1;
}
int  kage_pc_backend_virtual_global_unlock(uintptr_t h, uint32_t *r) {
    (void)h; if (r) *r = 0; return 1;
}
int  kage_pc_backend_virtual_clipboard_close(uint32_t *r) {
    if (r) *r = 1; return 1;
}
void kage_pc_backend_note_guest_gl_call(const char *n) { (void)n; }

int  kage_pc_backend_time_seconds(double *s) { if (s) *s = g_t0; return 0; }

int  kage_pc_backend_set_vsync(int e) { (void)e; return 1; }
int  kage_pc_backend_vsync_enabled(void) { return 0; }

/* query interface */
int         kage_pc_backend_ready(void) { return g_ready; }
const char *kage_pc_backend_last_error(void) { return ""; }
const char *kage_pc_backend_gl_version(void) { return "OpenGL ES 3.2 (stub)"; }
uintptr_t   kage_pc_backend_gl_proc(const char *n) { (void)n; return 0; }
int         kage_pc_backend_accelerated(void) { return 0; }
unsigned    kage_pc_backend_present_count(void) { return 0; }
unsigned    kage_pc_backend_clear_count(void) { return 0; }
unsigned    kage_pc_backend_vsync_change_count(void) { return 0; }
uint32_t    kage_pc_backend_refresh_rate(void) { return 60; }
void        kage_pc_backend_probe_rgba(uint8_t rgba[4]) { (void)rgba; }
uint32_t    kage_pc_backend_width(void) { return 1280; }
uint32_t    kage_pc_backend_height(void) { return 720; }

int guest_kage_pc_handoff_ready(void) { return g_ready; }
