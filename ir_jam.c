#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_infrared.h>
#include <furi_hal_light.h>
#include <furi_hal_vibro.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t FREQS[] = {30000, 33000, 36000, 38000, 40000, 43000, 46000, 50000, 53000, 56000, 60000};
#define NFREQ (sizeof(FREQS) / sizeof(FREQS[0]))
static const uint8_t DUTYS[] = {10, 20, 25, 33, 40, 50, 60, 70, 80, 90};
#define NDUTY (sizeof(DUTYS) / sizeof(DUTYS[0]))
static const uint16_t MINPS[] = {200, 300, 400, 600, 800, 1000, 1500, 2000};
#define NMINP (sizeof(MINPS) / sizeof(MINPS[0]))
static const uint16_t MAXPS[] = {400, 600, 800, 1200, 1600, 2000, 3000, 4000};
#define NMAXP (sizeof(MAXPS) / sizeof(MAXPS[0]))
static const char* MODES[] = {"Normal", "Custom"};

static const char* INFO[] = {
    "IR Jammer",
    "By planeguy",
    "",
    "github.com/Planeguy3",
    "/IR-Jammer-Flipperzero",
    "",
    "Tip: point the Flipper",
    "right at the remote or",
    "at the receiver you're",
    "jamming for best range",
    "and effect.",
    "",
    "Normal mode runs a",
    "fixed 38kHz noise",
    "signal - covers most",
    "remotes, most stable.",
    "",
    "Custom lets you pick",
    "freq/duty/pulse width.",
    "",
    "Range = IR LED range,",
    "a couple meters.",
    "",
    "Only test your stuff",
};
#define NINFO (sizeof(INFO) / sizeof(INFO[0]))
#define IVIS 5

typedef enum { ScrMenu, ScrSettings, ScrInfo, ScrRun } Scr;

typedef struct {
    ViewPort* vp;
    Gui* gui;
    FuriMessageQueue* inq;
    FuriTimer* timer;
    Scr scr;
    uint8_t msel, ssel, ioff;      // Dont know if anybody will read this but i kinda want to add comments 
    uint8_t mode, fi, di, mni, mxi; // settings stored as presets
    // these three get used by IR callback
    // more setup
    volatile bool run, led, lvl, stopr;
    volatile uint32_t seed;
} App;

// this keeps it fast
static FuriHalInfraredTxGetDataState jcb(void* ctx, uint32_t* dur, bool* lvl) {
    App* a = ctx;

    // controls stopping the ir jam
    if(a->stopr) {
        *dur = 100;
        *lvl = false;
        return FuriHalInfraredTxGetDataStateLastDone;
    }

    // already getting lazy with this
    uint32_t s = a->seed;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    a->seed = s;

    uint16_t mn = MINPS[a->mni], mx = MAXPS[a->mxi];
    if(mx <= mn) mx = mn + 100; // extra protection
    uint32_t span = mx - mn;
    if(!span) span = 1;

    *dur = mn + (s % span);
    *lvl = a->lvl;
    a->lvl = !a->lvl;
    return FuriHalInfraredTxGetDataStateOk;
}

static void buzz(void) {
    furi_hal_vibro_on(true);
    furi_delay_ms(80);
    furi_hal_vibro_on(false);
}

// protection against button mashing children
static void drain_queue(App* a) {
    InputEvent e;
    while(furi_message_queue_get(a->inq, &e, 0) == FuriStatusOk) {}
}

static void jstop(App* a) {
    if(a->run) {
        a->stopr = true;
        // without this the TX can keep firing for a bit after it is supposed to have stopped
        furi_hal_infrared_async_tx_wait_termination();
        a->run = false;
    }
    a->led = false;
    furi_hal_light_set(LightRed, 0);
}

static void jstart(App* a) {
    if(a->run) return;
    a->stopr = false;
    a->seed = furi_get_tick() | 1U;
    a->lvl = true;

    uint32_t freq = a->mode == 0 ? 38000 : FREQS[a->fi];
    float duty = a->mode == 0 ? 0.33f : DUTYS[a->di] / 100.0f;
    // this is my favorate part of the code 
    if(duty <= 0.0f) duty = 0.05f;
    if(duty >= 1.0f) duty = 0.95f;

    furi_hal_infrared_set_tx_output(FuriHalInfraredTxPinInternal);
    furi_hal_infrared_async_tx_set_data_isr_callback(jcb, a);
    furi_hal_infrared_async_tx_start(freq, duty);
    a->run = true;
    drain_queue(a);
}

// handles LED blink
static void on_tick(void* ctx) {
    App* a = ctx;
    if(a->run) {
        a->led = !a->led;
        furi_hal_light_set(LightRed, a->led ? 255 : 0);
    }
    view_port_update(a->vp);
}

static void draw_header(Canvas* c, const char* t) {
    canvas_draw_box(c, 0, 0, 128, 12);
    canvas_set_color(c, ColorWhite);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 4, 9, t);
    canvas_draw_circle(c, 119, 5, 2);
    canvas_draw_circle(c, 119, 5, 5);
    canvas_set_color(c, ColorBlack);
}

static void draw(Canvas* c, void* ctx) {
    App* a = ctx;
    char b[32];
    canvas_clear(c);

    if(a->scr == ScrMenu) {
        draw_header(c, "IR Jammer");
        canvas_set_font(c, FontSecondary);
        const char* items[] = {"Start", "Settings", "Info"};
        for(uint8_t i = 0; i < 3; i++) {
            uint8_t y = 24 + i * 14;
            if(a->msel == i) {
                canvas_draw_box(c, 2, y - 9, 124, 12);
                canvas_set_color(c, ColorWhite);
            }
            canvas_draw_str(c, 6, y, items[i]);
            canvas_set_color(c, ColorBlack);
        }
        canvas_draw_str(c, 2, 62, "Up/Dn Ok  Back:exit");

    } else if(a->scr == ScrSettings) {
        draw_header(c, "Settings");
        canvas_set_font(c, FontSecondary);
        const char* lbl[] = {"Mode", "Freq", "Duty", "MinPulse", "MaxPulse"};
        for(uint8_t i = 0; i < 5; i++) {
            uint8_t y = 21 + i * 8;
            canvas_draw_str(c, 2, y, a->ssel == i ? ">" : " ");
            canvas_draw_str(c, 8, y, lbl[i]);
            switch(i) {
            case 0: snprintf(b, sizeof(b), "%s", MODES[a->mode]); break;
            case 1:
                if(a->mode == 0) snprintf(b, sizeof(b), "auto");
                else snprintf(b, sizeof(b), "%lu Hz", (unsigned long)FREQS[a->fi]);
                break;
            case 2: snprintf(b, sizeof(b), "%u %%", (unsigned)DUTYS[a->di]); break;
            case 3: snprintf(b, sizeof(b), "%u us", (unsigned)MINPS[a->mni]); break;
            default: snprintf(b, sizeof(b), "%u us", (unsigned)MAXPS[a->mxi]); break;
            }
            canvas_draw_str(c, 78, y, b);
        }
        canvas_draw_str(c, 2, 63, "L/R:chg  Back:menu");

    } else if(a->scr == ScrInfo) {
        draw_header(c, "Info");
        canvas_set_font(c, FontSecondary);
        for(uint8_t i = 0; i < IVIS; i++) {
            uint8_t idx = a->ioff + i;
            if(idx >= NINFO) break;
            canvas_draw_str(c, 2, 22 + i * 9, INFO[idx]);
        }

    } else {
        draw_header(c, "IR Jammer");
        canvas_set_font(c, FontSecondary);
        snprintf(b, sizeof(b), "Mode: %s", MODES[a->mode]);
        canvas_draw_str(c, 4, 22, b);
        if(a->mode == 0) {
            canvas_draw_str(c, 4, 31, "Freq 38000Hz  Duty 33%");
        } else {
            snprintf(b, sizeof(b), "Freq %luHz  Duty %u%%", (unsigned long)FREQS[a->fi], (unsigned)DUTYS[a->di]);
            canvas_draw_str(c, 4, 31, b);
        }
        snprintf(b, sizeof(b), "Pulse %u-%u us", (unsigned)MINPS[a->mni], (unsigned)MAXPS[a->mxi]);
        canvas_draw_str(c, 4, 40, b);

        canvas_set_font(c, FontPrimary);
        canvas_draw_str(c, 4, 54, a->run ? "RUNNING" : "STOPPED");
        if(a->run) {
            if(a->led) canvas_draw_disc(c, 108, 50, 4);
            else canvas_draw_circle(c, 108, 50, 4);
        }
        canvas_set_font(c, FontSecondary);
        canvas_draw_str(c, 2, 63, a->run ? "Ok:stop  Back:menu" : "Ok:start  Back:menu");
    }
}

static void on_input(InputEvent* e, void* ctx) {
    App* a = ctx;
    furi_message_queue_put(a->inq, e, 0);
}

int32_t ir_jam_app(void* p) {
    UNUSED(p);
    App* a = malloc(sizeof(App));
    if(!a) return 1;
    memset(a, 0, sizeof(App));

    // i hate this part
    a->mode = 0; a->fi = 3; a->di = 3; a->mni = 1; a->mxi = 1;
    a->scr = ScrMenu;

    a->inq = furi_message_queue_alloc(16, sizeof(InputEvent));
    a->vp = view_port_alloc();
    if(!a->inq || !a->vp) {
        if(a->vp) view_port_free(a->vp);
        if(a->inq) furi_message_queue_free(a->inq);
        free(a);
        return 1;
    }
    view_port_draw_callback_set(a->vp, draw, a);
    view_port_input_callback_set(a->vp, on_input, a);
    a->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(a->gui, a->vp, GuiLayerFullscreen);

    a->timer = furi_timer_alloc(on_tick, FuriTimerTypePeriodic, a);
    if(a->timer) furi_timer_start(a->timer, furi_ms_to_ticks(80));

    bool running = true;
    InputEvent e;

    // main loop - very good
    while(running) {
        if(furi_message_queue_get(a->inq, &e, 100) == FuriStatusOk) {
            if(a->scr == ScrRun) {
                if(e.type == InputTypeShort) {
                    if(e.key == InputKeyOk) {
                        if(a->run) jstop(a); else jstart(a);
                        buzz();
                        drain_queue(a);
                    } else if(e.key == InputKeyBack) {
                        if(a->run) { jstop(a); buzz(); }
                        a->scr = ScrMenu;
                        drain_queue(a);
                    }
                }
                view_port_update(a->vp);
                continue;
            }

            if(e.type != InputTypeShort && e.type != InputTypeRepeat) {
                view_port_update(a->vp);
                continue;
            }

            if(a->scr == ScrMenu) {
                if(e.key == InputKeyUp) a->msel = a->msel == 0 ? 2 : a->msel - 1;
                else if(e.key == InputKeyDown) a->msel = (a->msel + 1) % 3;
                else if(e.key == InputKeyOk) a->scr = a->msel == 0 ? ScrRun : a->msel == 1 ? ScrSettings : ScrInfo;
                else if(e.key == InputKeyBack) running = false;

            } else if(a->scr == ScrSettings) {
                if(e.key == InputKeyUp) a->ssel = a->ssel == 0 ? 4 : a->ssel - 1;
                else if(e.key == InputKeyDown) a->ssel = (a->ssel + 1) % 5;
                else if(e.key == InputKeyLeft || e.key == InputKeyRight) {
                    int8_t d = e.key == InputKeyRight ? 1 : -1;
                    int32_t v;
                    switch(a->ssel) {
                    case 0: a->mode = a->mode ? 0 : 1; break;
                    case 1:
                        v = (int32_t)a->fi + d;
                        if(v < 0) v = 0;
                        if(v >= (int32_t)NFREQ) v = NFREQ - 1;
                        a->fi = (uint8_t)v;
                        break;
                    case 2:
                        v = (int32_t)a->di + d;
                        if(v < 0) v = 0;
                        if(v >= (int32_t)NDUTY) v = NDUTY - 1;
                        a->di = (uint8_t)v;
                        break;
                    case 3:
                        v = (int32_t)a->mni + d;
                        if(v < 0) v = 0;
                        if(v >= (int32_t)NMINP) v = NMINP - 1;
                        a->mni = (uint8_t)v;
                        break;
                    case 4:
                        v = (int32_t)a->mxi + d;
                        if(v < 0) v = 0;
                        if(v >= (int32_t)NMAXP) v = NMAXP - 1;
                        a->mxi = (uint8_t)v;
                        break;
                    }
                } else if(e.key == InputKeyBack) a->scr = ScrMenu;

            } else if(a->scr == ScrInfo) {
                if(e.key == InputKeyUp) { if(a->ioff > 0) a->ioff--; }
                else if(e.key == InputKeyDown) { if((uint32_t)a->ioff + IVIS < NINFO) a->ioff++; }
                else if(e.key == InputKeyBack) a->scr = ScrMenu;
            }
        }
        view_port_update(a->vp);
    }

    jstop(a);
    if(a->timer) { furi_timer_stop(a->timer); furi_timer_free(a->timer); }
    gui_remove_view_port(a->gui, a->vp);
    furi_record_close(RECORD_GUI);
    view_port_free(a->vp);
    furi_message_queue_free(a->inq);
    free(a);
    return 0;
}

// This took longer to make then it should have