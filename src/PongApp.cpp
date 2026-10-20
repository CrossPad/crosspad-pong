// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PongApp.cpp
 * @brief Pong as a CrossPad app: the court on the panel, the encoder and the
 *        pads as the paddles, the pad LEDs as a 4x4 picture of the court, the
 *        effects as a node on the audio module, and the pause menu's Quit as
 *        the way back to the other firmware slot.
 *
 * Meant as the one app of a firmware of its own (the "pong" kit). The game is
 * small: it steps on an lv_timer on the LVGL thread, where the encoder, the
 * power click and the menus live too. Pads arrive on another thread and cross
 * through two atomic masks.
 */

#if USE_LVGL

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include <crosspad/app/AppRegistrar.hpp>
#include <crosspad/audio/IAudioModule.hpp>
#include <crosspad/pad/IPadLogicHandler.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/platform/IClock.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include "crosspad-gui/components/app_icon.h"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/fw/IFwViewService.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "crosspad-gui/widgets/gui_helpers.h"

#include "lvgl.h"

#include "PongGame.hpp"
#include "PongSynth.hpp"

class App;

namespace {

constexpr const char* kPadLogicName = "Pong";
constexpr uint32_t kTickMs = 16;
constexpr float kMaxDt = 0.05f;          // a stalled frame does not throw the ball through a paddle
constexpr float kDetentPx = 16.0f;       // one encoder detent moves the paddle this far
constexpr uint32_t kGameOverMs = 2500;   // the result stays up this long before the menu
constexpr int kCols = 4;

/* ── Pads (index 0 = bottom left, 15 = top right) ──────────────────────────
 *   12 13 14 15     left column: player 1 up (top two) / down (bottom two)
 *    8  9 10 11     right column: player 2 the same (two players only)
 *    4  5  6  7     middle four columns 1-2: serve, and choose in a menu
 *    0  1  2  3     in a menu either side column moves the highlight        */
constexpr uint16_t kLeftUp = (1u << 8) | (1u << 12);
constexpr uint16_t kLeftDown = (1u << 0) | (1u << 4);
constexpr uint16_t kRightUp = (1u << 11) | (1u << 15);
constexpr uint16_t kRightDown = (1u << 3) | (1u << 7);
constexpr uint16_t kMiddle = 0x6666;

constexpr uint32_t kColP1 = 0x3FA9FF;
constexpr uint32_t kColP2 = 0xFF5A3C;
constexpr uint32_t kColBall = 0xFFFFFF;
constexpr uint32_t kColDim = 0x505050;

/// Pad colours as 0xRRGGBB.
constexpr uint32_t kLedP1 = 0x0040FF;
constexpr uint32_t kLedP2 = 0xFF2000;
constexpr uint32_t kLedBall = 0xFFFFFF;
constexpr uint32_t kLedServe = 0x00A020;
constexpr uint32_t kLedMenuP1 = 0x000830;
constexpr uint32_t kLedMenuP2 = 0x300800;
constexpr uint32_t kLedMenuSelect = 0x003010;
constexpr uint32_t kLedLevelUp = 0x00FF40;
constexpr uint32_t kFlashMs = 250;
constexpr uint32_t kLevelFlashMs = 600;

enum class Screen : uint8_t { Menu, Playing, Paused };

struct MenuDef {
    const char* items[3];
};
constexpr MenuDef kStartMenu{{"1 PLAYER", "2 PLAYERS", "QUIT"}};
constexpr MenuDef kPauseMenu{{"RESUME", "RESTART", "QUIT"}};
constexpr int kMenuItems = 3;
constexpr int kQuitItem = 2;

/* ── Pads → LVGL thread ──────────────────────────────────────────────── */

std::atomic<uint16_t> s_held{0};
std::atomic<uint16_t> s_pressed{0};
std::atomic<bool> s_ledsStale{true};

class PongPadLogic : public crosspad::IPadLogicHandler {
public:
    void onActivate(crosspad::PadManager&) override { s_ledsStale.store(true); }
    void onDeactivate(crosspad::PadManager& pm) override {
        s_held.store(0);
        for (uint8_t i = 0; i < 16; ++i) pm.setPadColor(i, crosspad::RgbColor(0, 0, 0));
        s_ledsStale.store(true);
    }
    void onPadPress(crosspad::PadManager&, uint8_t pad, uint8_t) override {
        if (pad >= 16) return;
        s_held.fetch_or(static_cast<uint16_t>(1u << pad));
        s_pressed.fetch_or(static_cast<uint16_t>(1u << pad));
    }
    void onPadRelease(crosspad::PadManager&, uint8_t pad) override {
        if (pad >= 16) return;
        s_held.fetch_and(static_cast<uint16_t>(~(1u << pad)));
    }
    void onPadPressure(crosspad::PadManager&, uint8_t, uint8_t) override {}
};

std::shared_ptr<PongPadLogic> s_padLogic;
pong::Synth s_synth;
bool s_synthAttached = false;

/* ── State (LVGL thread) ─────────────────────────────────────────────── */

std::unique_ptr<pong::Game> s_game;
Screen s_screen = Screen::Menu;
const MenuDef* s_menu = &kStartMenu;
int s_menuSel = 0;
uint64_t s_lastUs = 0;
uint32_t s_gameOverAt = 0;
uint32_t s_flashUntil = 0;
uint32_t s_flashColor = 0;
uint32_t s_leds[16];
int s_shownScore[2] = {-1, -1};
int s_shownLevel = -1;

lv_obj_t* s_root = nullptr;
lv_obj_t* s_input = nullptr;
lv_obj_t* s_paddle[2] = {};
lv_obj_t* s_ball = nullptr;
lv_obj_t* s_score[2] = {};
lv_obj_t* s_levelLabel = nullptr;
lv_obj_t* s_hint = nullptr;
lv_obj_t* s_banner = nullptr;
lv_obj_t* s_menuBox = nullptr;
lv_obj_t* s_menuTitle = nullptr;
lv_obj_t* s_menuNote = nullptr;
lv_obj_t* s_menuItem[kMenuItems] = {};
lv_group_t* s_group = nullptr;
lv_group_t* s_prevGroup = nullptr;
lv_timer_t* s_timer = nullptr;
uint32_t s_backToken = 0;

uint32_t nowMs() { return static_cast<uint32_t>(crosspad::getClock().getTimeUs() / 1000); }

lv_indev_t* navEncoder() { return crosspad_gui::getGuiPlatform().getNavigationEncoder(); }

lv_obj_t* makeRect(lv_obj_t* parent, int32_t w, int32_t h, uint32_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

/* ── Menus ───────────────────────────────────────────────────────────── */

void paintMenu() {
    for (int i = 0; i < kMenuItems; ++i) {
        lv_label_set_text(s_menuItem[i], s_menu->items[i]);
        const bool sel = i == s_menuSel;
        lv_obj_set_style_bg_opa(s_menuItem[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_menuItem[i], lv_color_hex(sel ? 0x000000 : 0xC0C0C0), 0);
    }
}

void openMenu(const MenuDef& menu, const char* title, const char* note) {
    s_menu = &menu;
    s_menuSel = 0;
    lv_label_set_text(s_menuTitle, title);
    lv_label_set_text(s_menuNote, note);
    paintMenu();
    lv_obj_remove_flag(s_menuBox, LV_OBJ_FLAG_HIDDEN);
    s_ledsStale = true;
}

void closeMenu() { lv_obj_add_flag(s_menuBox, LV_OBJ_FLAG_HIDDEN); }

void showStartMenu(const char* note) {
    s_screen = Screen::Menu;
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    openMenu(kStartMenu, "PONG", note);
}

void startMatch(pong::Mode mode) {
    s_game->newMatch(mode);
    s_screen = Screen::Playing;
    s_lastUs = crosspad::getClock().getTimeUs();
    s_shownScore[0] = s_shownScore[1] = s_shownLevel = -1;
    closeMenu();
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    std::printf("[Pong] new match, %s\n", mode == pong::Mode::OnePlayer ? "1 player" : "2 players");
}

void pause() {
    s_screen = Screen::Paused;
    s_game->setHeld(0, 0);
    s_game->setHeld(1, 0);
    openMenu(kPauseMenu, "PAUSED", "");
}

void resume() {
    s_screen = Screen::Playing;
    s_lastUs = crosspad::getClock().getTimeUs();
    closeMenu();
}

/// Back to the other firmware slot; false (and a note in the menu) when the platform refused.
bool leaveFirmware() {
    std::printf("[Pong] Quit: back to the other firmware\n");
    if (crosspad_gui::fwViewService().flowSwitchAndRestart(false)) return true;
    lv_label_set_text(s_menuNote, "There is no other firmware to go back to.");
    return false;
}

void menuMove(int d) {
    s_menuSel = (s_menuSel + d + kMenuItems) % kMenuItems;
    s_synth.play(pong::Sfx::MenuMove);
    paintMenu();
}

void menuSelect() {
    s_synth.play(pong::Sfx::MenuSelect);
    if (s_menu == &kStartMenu) {
        if (s_menuSel == 0) startMatch(pong::Mode::OnePlayer);
        else if (s_menuSel == 1) startMatch(pong::Mode::TwoPlayers);
        else leaveFirmware();
    } else {
        if (s_menuSel == 0) resume();
        else if (s_menuSel == 1) startMatch(s_game->mode());
        else leaveFirmware();
    }
}

/* ── Input ───────────────────────────────────────────────────────────── */

/* Power: pause and resume in a match; in the start menu it puts the
 * highlight on QUIT (a click then leaves). Pushed before the encoder's edit
 * mode, which is always on here, would take the click. */
bool onPowerBack(void*) {
    switch (s_screen) {
        case Screen::Playing: pause(); break;
        case Screen::Paused: resume(); break;
        case Screen::Menu:
            s_menuSel = kQuitItem;
            paintMenu();
            break;
    }
    return true;
}

void onEncoderKey(lv_event_t* e) {
    const uint32_t k = lv_event_get_key(e);
    if (k != LV_KEY_LEFT && k != LV_KEY_RIGHT && k != LV_KEY_ENTER) return;
    lv_event_stop_processing(e);
    const int dir = k == LV_KEY_LEFT ? -1 : k == LV_KEY_RIGHT ? 1 : 0;   // wheel up = LEFT = up
    if (s_screen != Screen::Playing) {
        if (dir) menuMove(dir);
        else menuSelect();
        return;
    }
    if (dir) {
        s_game->nudge(0, dir * kDetentPx);
    } else if (s_game->phase() == pong::Phase::GameOver) {
        s_gameOverAt = 0;   // the click skips the result
    } else {
        s_game->serve();
    }
}

int heldDir(uint16_t held, uint16_t up, uint16_t down) {
    return ((held & down) ? 1 : 0) - ((held & up) ? 1 : 0);
}

void handlePads() {
    const uint16_t pressed = s_pressed.exchange(0);
    const uint16_t held = s_held.load();
    if (s_screen != Screen::Playing) {
        if (pressed & (kLeftUp | kRightUp)) menuMove(-1);
        else if (pressed & (kLeftDown | kRightDown)) menuMove(1);
        else if (pressed & kMiddle) menuSelect();
        return;
    }
    s_game->setHeld(0, heldDir(held, kLeftUp, kLeftDown));
    s_game->setHeld(1, s_game->mode() == pong::Mode::TwoPlayers ? heldDir(held, kRightUp, kRightDown) : 0);
    if (pressed & kMiddle) s_game->serve();
}

/* ── Drawing ─────────────────────────────────────────────────────────── */

void flash(uint32_t color, uint32_t ms) {
    s_flashColor = color;
    s_flashUntil = nowMs() + ms;
}

void onEvents(uint32_t ev) {
    const pong::Game& g = *s_game;
    if (ev & pong::kEvGameOver) {
        const bool won = g.winner() == 0 || g.mode() == pong::Mode::TwoPlayers;
        s_synth.play(won ? pong::Sfx::Win : pong::Sfx::Lose);
        flash(g.winner() == 0 ? kLedP1 : kLedP2, kLevelFlashMs);
        s_gameOverAt = nowMs() + kGameOverMs;
        std::printf("[Pong] game over: winner %d, level %d, %d-%d\n", g.winner(), g.level(), g.score(0), g.score(1));
    } else if (ev & pong::kEvLevelUp) {
        s_synth.play(pong::Sfx::LevelUp);
        flash(kLedLevelUp, kLevelFlashMs);
        std::printf("[Pong] level %d\n", g.level());
    } else if (ev & (pong::kEvScoreP1 | pong::kEvScoreP2)) {
        const bool p1 = ev & pong::kEvScoreP1;
        s_synth.play(p1 ? pong::Sfx::ScoreP1 : pong::Sfx::ScoreP2);
        flash(p1 ? kLedP1 : kLedP2, kFlashMs);
        std::printf("[Pong] point %s: %d-%d, level %d\n", p1 ? "P1" : "P2", g.score(0), g.score(1), g.level());
    } else if (ev & pong::kEvPaddle) {
        s_synth.play(pong::Sfx::Paddle);
    } else if (ev & pong::kEvWall) {
        s_synth.play(pong::Sfx::Wall);
    } else if (ev & pong::kEvServe) {
        s_synth.play(pong::Sfx::Serve);
    }
}

void drawCourt() {
    const pong::Game& g = *s_game;
    for (int p = 0; p < 2; ++p) {
        lv_obj_set_pos(s_paddle[p], static_cast<int32_t>(g.paddleX(p)), static_cast<int32_t>(g.paddle(p).y));
        if (g.score(p) != s_shownScore[p]) {
            s_shownScore[p] = g.score(p);
            lv_label_set_text_fmt(s_score[p], "%d", g.score(p));
        }
    }
    lv_obj_set_pos(s_ball, static_cast<int32_t>(g.ballX()), static_cast<int32_t>(g.ballY()));
    if (g.level() != s_shownLevel || g.phase() == pong::Phase::GameOver) {
        s_shownLevel = g.level();
        if (g.mode() == pong::Mode::TwoPlayers) lv_label_set_text(s_levelLabel, "2 PLAYERS  -  FIRST TO 5");
        else lv_label_set_text_fmt(s_levelLabel, "LEVEL %d  -  FIRST TO %d", g.level(), pong::Game::kWinScore);
    }

    const char* banner = nullptr;
    const char* hint = "";
    if (g.phase() == pong::Phase::Banner) {
        static char text[16];
        std::snprintf(text, sizeof text, "LEVEL %d", g.level());
        banner = text;
    } else if (g.phase() == pong::Phase::GameOver) {
        banner = g.mode() == pong::Mode::TwoPlayers ? (g.winner() == 0 ? "PLAYER 1 WINS" : "PLAYER 2 WINS")
                 : g.champion()                     ? "CHAMPION!"
                                                    : "GAME OVER";
    } else if (g.serveWaitsForPlayer() && s_screen == Screen::Playing) {
        hint = g.server() == 0 ? "CLICK OR A MIDDLE PAD TO SERVE" : "PLAYER 2: A MIDDLE PAD TO SERVE";
    }
    if (banner) {
        lv_label_set_text(s_banner, banner);
        lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }
    if (std::strcmp(lv_label_get_text(s_hint), hint) != 0) lv_label_set_text(s_hint, hint);
}

/// The pads as the court in 4x4: the paddles in the side columns, the ball
/// where it is, the serve pads lit while a serve waits.
void drawLeds() {
    uint32_t want[16] = {};
    const uint32_t now = nowMs();
    const pong::Game& g = *s_game;
    if (static_cast<int32_t>(s_flashUntil - now) > 0) {
        for (uint32_t& c : want) c = s_flashColor;
    } else if (s_screen != Screen::Playing) {
        for (int i = 0; i < 16; ++i) {
            const int col = i % kCols;
            want[i] = col == 0 ? kLedMenuP1 : col == 3 ? kLedMenuP2 : kLedMenuSelect;
        }
    } else {
        const float rowH = g.height() / 4.0f;
        auto rowOf = [&](float y) {
            const int r = 3 - static_cast<int>(y / rowH);
            return r < 0 ? 0 : r > 3 ? 3 : r;
        };
        for (int p = 0; p < 2; ++p) {
            const int top = rowOf(g.paddle(p).y);
            const int bottom = rowOf(g.paddle(p).y + pong::Game::kPaddleH - 1.0f);
            for (int r = bottom; r <= top; ++r) want[r * kCols + (p == 0 ? 0 : 3)] = p == 0 ? kLedP1 : kLedP2;
        }
        if (g.serveWaitsForPlayer() && ((now / 400) & 1)) {
            for (int i = 0; i < 16; ++i)
                if (kMiddle & (1u << i)) want[i] = kLedServe;
        }
        if (g.phase() == pong::Phase::Play || g.phase() == pong::Phase::Serve) {
            int col = static_cast<int>((g.ballX() + pong::Game::kBall * 0.5f) / (g.width() / kCols));
            col = col < 0 ? 0 : col > 3 ? 3 : col;
            want[rowOf(g.ballY() + pong::Game::kBall * 0.5f) * kCols + col] = kLedBall;
        }
    }
    const bool stale = s_ledsStale.exchange(false);
    crosspad::PadManager& pm = crosspad::getPadManager();
    for (uint8_t i = 0; i < 16; ++i) {
        if (!stale && want[i] == s_leds[i]) continue;
        s_leds[i] = want[i];
        pm.setPadColor(i, crosspad::RgbColor((want[i] >> 16) & 0xFF, (want[i] >> 8) & 0xFF, want[i] & 0xFF));
    }
}

void tick(lv_timer_t*) {
    // A long press of the encoder toggles edit mode off; the game wants the turns.
    if (s_group && !lv_group_get_editing(s_group)) lv_group_set_editing(s_group, true);
    handlePads();
    const uint64_t us = crosspad::getClock().getTimeUs();
    float dt = static_cast<float>(us - s_lastUs) / 1e6f;
    s_lastUs = us;
    if (dt > kMaxDt) dt = kMaxDt;
    if (s_screen == Screen::Playing) {
        onEvents(s_game->step(dt));
        if (s_game->phase() == pong::Phase::GameOver && static_cast<int32_t>(nowMs() - s_gameOverAt) >= 0) {
            static char note[48];
            const pong::Game& g = *s_game;
            if (g.mode() == pong::Mode::TwoPlayers)
                std::snprintf(note, sizeof note, "Player %d won %d-%d", g.winner() + 1, g.score(0), g.score(1));
            else if (g.champion())
                std::snprintf(note, sizeof note, "Champion: all %d levels won", pong::Game::kMaxLevel);
            else
                std::snprintf(note, sizeof note, "Game over at level %d (%d-%d)", g.level(), g.score(0), g.score(1));
            showStartMenu(note);
        }
    }
    drawCourt();
    drawLeds();
}

/* ── Build and teardown ──────────────────────────────────────────────── */

void buildCourt(int32_t w, int32_t h) {
    constexpr int32_t kDash = 8;
    for (int32_t y = 4; y < h; y += 2 * kDash) {
        lv_obj_t* d = makeRect(s_root, 2, kDash, kColDim);
        lv_obj_set_pos(d, w / 2 - 1, y);
    }
    for (int p = 0; p < 2; ++p) {
        s_score[p] = makeLabel(s_root, &lv_font_montserrat_24, p == 0 ? kColP1 : kColP2);
        lv_label_set_text(s_score[p], "0");
        lv_obj_align(s_score[p], LV_ALIGN_TOP_MID, p == 0 ? -w / 4 : w / 4, 4);
        s_paddle[p] = makeRect(s_root, static_cast<int32_t>(pong::Game::kPaddleW),
                               static_cast<int32_t>(pong::Game::kPaddleH), p == 0 ? kColP1 : kColP2);
    }
    s_ball = makeRect(s_root, static_cast<int32_t>(pong::Game::kBall), static_cast<int32_t>(pong::Game::kBall),
                      kColBall);
    s_levelLabel = makeLabel(s_root, &lv_font_montserrat_14, 0x909090);
    lv_label_set_text(s_levelLabel, "");
    lv_obj_align(s_levelLabel, LV_ALIGN_BOTTOM_MID, 0, -4);
    s_hint = makeLabel(s_root, &lv_font_montserrat_14, 0x60D060);
    lv_label_set_text(s_hint, "");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -24);
    s_banner = makeLabel(s_root, &lv_font_montserrat_24, 0xFFE040);
    lv_obj_set_style_bg_color(s_banner, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_banner, 10, 0);
    lv_obj_align(s_banner, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
}

void buildMenu() {
    s_menuBox = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_menuBox);
    lv_obj_set_size(s_menuBox, 220, 170);
    lv_obj_center(s_menuBox);
    lv_obj_set_style_bg_color(s_menuBox, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_menuBox, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_menuBox, lv_color_hex(kColP1), 0);
    lv_obj_set_style_border_width(s_menuBox, 2, 0);
    lv_obj_set_style_radius(s_menuBox, 6, 0);
    lv_obj_remove_flag(s_menuBox, LV_OBJ_FLAG_SCROLLABLE);

    s_menuTitle = makeLabel(s_menuBox, &lv_font_montserrat_24, 0xFFFFFF);
    lv_obj_align(s_menuTitle, LV_ALIGN_TOP_MID, 0, 8);
    s_menuNote = makeLabel(s_menuBox, &lv_font_montserrat_14, 0xFFC060);
    lv_obj_set_width(s_menuNote, 204);
    lv_label_set_long_mode(s_menuNote, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_menuNote, LV_ALIGN_TOP_MID, 0, 38);
    for (int i = 0; i < kMenuItems; ++i) {
        s_menuItem[i] = makeLabel(s_menuBox, &lv_font_montserrat_16, 0xC0C0C0);
        lv_obj_set_width(s_menuItem[i], 150);
        lv_obj_set_style_bg_color(s_menuItem[i], lv_color_hex(kColP1), 0);
        lv_obj_set_style_radius(s_menuItem[i], 4, 0);
        lv_obj_set_style_pad_ver(s_menuItem[i], 3, 0);
        lv_obj_align(s_menuItem[i], LV_ALIGN_BOTTOM_MID, 0, -8 - (kMenuItems - 1 - i) * 30);
    }
}

lv_obj_t* lv_CreatePong(lv_obj_t* parent, App* app) {
    (void)app;
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(parent);
    const int32_t w = lv_obj_get_content_width(parent);
    const int32_t h = lv_obj_get_content_height(parent);

    s_game = std::make_unique<pong::Game>(static_cast<float>(w), static_cast<float>(h),
                                          static_cast<uint32_t>(crosspad::getClock().getTimeUs()));
    buildCourt(w, h);
    buildMenu();

    /* The encoder's group: one object always in edit mode, so a turn is a
     * key here rather than a move of focus. Scrollable, so LVGL counts it
     * editable; it has nothing to scroll. */
    s_input = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_input);
    lv_obj_set_size(s_input, 1, 1);
    s_group = lv_group_create();
    lv_group_add_obj(s_group, s_input);
    lv_obj_add_event_cb(s_input, onEncoderKey, static_cast<lv_event_code_t>(LV_EVENT_KEY | LV_EVENT_PREPROCESS),
                        nullptr);
    if (lv_indev_t* nav = navEncoder()) {
        s_prevGroup = lv_indev_get_group(nav);
        lv_indev_set_group(nav, s_group);
    }
    lv_group_focus_obj(s_input);
    lv_group_set_editing(s_group, true);

    s_backToken = crosspad_gui::ui_back_push(onPowerBack, nullptr);

    if (!s_padLogic) {
        s_padLogic = std::make_shared<PongPadLogic>();
        crosspad::getPadManager().registerPadLogic(kPadLogicName, s_padLogic);
    }
    if (!s_synthAttached) {
        crosspad::getAudioModule().addNode(&s_synth);
        s_synthAttached = true;
    }
    s_ledsStale = true;
    s_lastUs = crosspad::getClock().getTimeUs();
    showStartMenu("Encoder: paddle and menus\nPower: pause");
    drawCourt();
    s_timer = lv_timer_create(tick, kTickMs, nullptr);
    return s_root;
}

void lv_DestroyPong(lv_obj_t* root) {
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    crosspad_gui::ui_back_remove(s_backToken);
    s_backToken = 0;
    if (lv_indev_t* nav = navEncoder()) lv_indev_restore_group(nav, s_prevGroup);
    if (s_group) {
        lv_group_delete(s_group);
        s_group = nullptr;
    }
    s_root = s_input = s_ball = s_levelLabel = s_hint = s_banner = nullptr;
    s_menuBox = s_menuTitle = s_menuNote = nullptr;
    for (auto& o : s_paddle) o = nullptr;
    for (auto& o : s_score) o = nullptr;
    for (auto& o : s_menuItem) o = nullptr;
    s_game.reset();
    if (root) lv_obj_delete_async(root);
}

} // namespace

REGISTER_APP_PL(Pong, nullptr, crosspad_gui::resolveAppIcon("pong.png"), lv_CreatePong, lv_DestroyPong, nullptr,
                nullptr, nullptr, 0, kPadLogicName)

#endif // USE_LVGL
