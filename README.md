# crosspad-pong

Pong, written from scratch for CrossPad: the one app of the `pong` firmware
kit, started from the Firmware app's library like any other firmware and left
through its pause menu's **Quit**, which switches back to the other firmware
slot.

Everything goes through crosspad-core and crosspad-gui (2.x), nothing through
a platform's own API: the pads are a pad logic (`IPadLogicHandler`), the
encoder an LVGL group, the power click a back handler, the sound a node on
the audio module (`IAudioNode`), leaving the firmware the Firmware views'
service (`IFwViewService::flowSwitchAndRestart`).

## The game

- **1 PLAYER**: you (left, blue) against the CPU (right, red). The first to
  5 points wins the level; win it and the next level starts (a "LEVEL n"
  banner, scores back to 0). Each level the ball serves faster and gets
  faster with every hit, and the CPU moves faster, reacts earlier and aims
  better; from level 7 it reads where the ball will arrive. Lose a level and
  it is game over; win all 9 and you are the champion.
- **2 PLAYERS**: one match to 5, player 1 on the encoder and the left pads,
  player 2 on the right pads.
- The side that lost the point serves. Where the ball hits the paddle sets
  its angle: the edges send it steep.

## Controls

Pads, by their printed labels (1 = bottom left, 16 = top right):

| Pad | | Pad | | Pad | | Pad | |
|---|---|---|---|---|---|---|---|
| 13 | P1 up | 14 | serve / choose | 15 | serve / choose | 16 | P2 up |
| 9 | P1 up | 10 | serve / choose | 11 | serve / choose | 12 | P2 up |
| 5 | P1 down | 6 | serve / choose | 7 | serve / choose | 8 | P2 down |
| 1 | P1 down | 2 | serve / choose | 3 | serve / choose | 4 | P2 down |

- **Encoder turn**: player 1's paddle, 16 px a detent (up = up); in a menu,
  the highlight.
- **Encoder click**: serve; in a menu, choose; on a result, skip to the menu.
- **Side pads held**: the paddle moves while held. In a menu either side
  column moves the highlight (top two up, bottom two down).
- **Power click**: pause menu (RESUME / RESTART / QUIT) and back to the game;
  in the start menu it puts the highlight on QUIT.
- **QUIT**: back to the firmware in the other slot. With none there the menu
  says so and the game goes on.

The pads light the court in 4x4: player 1's paddle in the left column (blue),
the CPU's or player 2's in the right one (red), the ball white wherever it is,
the middle pads blinking green while a serve waits for you; a point flashes
all of them in the scorer's colour, a level won flashes them green.

## Sound

`src/PongSynth.hpp`: three square-wave voices on the audio module, a short
tune per effect (paddle, wall, serve, point won, point lost, level up, win,
lose, menu). The game thread writes effect ids into a lock-free ring; the
audio thread allocates and locks nothing.

## Layout

| Path | |
|---|---|
| `src/PongGame.{hpp,cpp}` | the rules in plain C++: court, paddles, ball, CPU, scores, levels |
| `src/PongSynth.hpp` | the sound node |
| `src/PongApp.cpp` | the app: court on the panel, menus, encoder, pads and their LEDs, power, Quit |
| `tests/test_pong_game.cpp` | host test of the rules (command in the file) |
| `CMakeLists.txt` | the ESP-IDF component: built into the `pong` kit only (`crosspad-app.json` `kits`), empty in every other image |
| `assets/pong.png` | launcher icon |

The game steps on a 16 ms `lv_timer` on the LVGL thread; it needs no task of
its own, no frame buffer (the court is a handful of LVGL objects) and no data
on the card.

## Not done

- No settings (speed, points to win) and no saved high score.
- Not built or played on the board yet: only in the crosspad-pc simulator.

## Licence

GPL-3.0-or-later (`LICENSE`). No third-party code or data.
