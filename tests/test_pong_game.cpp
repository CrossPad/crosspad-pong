// SPDX-License-Identifier: GPL-3.0-or-later
// Host test of Pong's rules (no LVGL, no platform):
//   c++ -std=c++17 -Wall -Wextra -I src tests/test_pong_game.cpp src/PongGame.cpp -o /tmp/pong_test && /tmp/pong_test
#include "PongGame.hpp"

#include <cstdio>
#include <cstdlib>

using pong::Game;

namespace {

int s_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++s_failures;
}

constexpr float kDt = 0.016f;
constexpr float kW = 320.0f, kH = 236.0f;

/// Player 1 follows the ball as an encoder would (nudges of whole detents).
void autopilot(Game& g) {
    const float want = g.ballY() + Game::kBall * 0.5f - Game::kPaddleH * 0.5f;
    const float d = want - g.paddle(0).target;
    if (d > 8.0f) g.nudge(0, 16.0f);
    else if (d < -8.0f) g.nudge(0, -16.0f);
    if (g.serveWaitsForPlayer()) g.serve();
}

} // namespace

int main() {
    {   // An idle player loses level 1: the CPU wins 5 points, game over.
        Game g(kW, kH, 1234);
        g.newMatch(pong::Mode::OnePlayer);
        uint32_t all = 0;
        for (int i = 0; i < 60 * 300 && g.phase() != pong::Phase::GameOver; ++i) {
            if (g.serveWaitsForPlayer()) g.serve();
            all |= g.step(kDt);
        }
        check(g.phase() == pong::Phase::GameOver, "idle player: game over");
        check(g.winner() == 1 && g.score(1) == Game::kWinScore, "idle player: CPU wins 5");
        check((all & pong::kEvWall) && (all & pong::kEvScoreP2), "idle player: walls and CPU points seen");
    }
    {   // A player tracking the ball wins level 1 and reaches level 2 (banner, then serve).
        Game g(kW, kH, 99);
        g.newMatch(pong::Mode::OnePlayer);
        uint32_t all = 0;
        int levelUps = 0;
        for (int i = 0; i < 60 * 600 && g.phase() != pong::Phase::GameOver && g.level() < 3; ++i) {
            autopilot(g);
            const uint32_t ev = g.step(kDt);
            if (ev & pong::kEvLevelUp) {
                ++levelUps;
                check(g.phase() == pong::Phase::Banner && g.score(0) == 0 && g.score(1) == 0,
                      "level up: banner, scores reset");
            }
            all |= ev;
        }
        std::printf("     reached level %d, levelUps %d\n", g.level(), levelUps);
        check(levelUps >= 1 && g.level() >= 2, "tracking player reaches level 2");
        check(all & pong::kEvPaddle, "paddle hits seen");
    }
    {   // Levels get harder.
        const auto a = Game::levelParams(1), b = Game::levelParams(5), c = Game::levelParams(9);
        check(a.ballSpeed < b.ballSpeed && b.ballSpeed < c.ballSpeed, "ball faster with level");
        check(a.cpuSpeed < b.cpuSpeed && b.cpuSpeed < c.cpuSpeed, "CPU faster with level");
        check(a.cpuError > c.cpuError && !a.cpuPredicts && c.cpuPredicts, "CPU smarter with level");
    }
    {   // Two players: no CPU, the right paddle answers its own input; first to 5 wins.
        Game g(kW, kH, 7);
        g.newMatch(pong::Mode::TwoPlayers);
        const float before = g.paddle(1).y;
        g.setHeld(1, -1);
        for (int i = 0; i < 20; ++i) g.step(kDt);
        g.setHeld(1, 0);
        check(g.paddle(1).y < before, "player 2's paddle moves up when held");
        for (int i = 0; i < 60 * 300 && g.phase() != pong::Phase::GameOver; ++i) {
            if (g.serveWaitsForPlayer()) g.serve();
            g.step(kDt);
        }
        check(g.phase() == pong::Phase::GameOver && g.level() == 1, "two players: a match, no levels");
    }
    std::printf("%s\n", s_failures ? "FAILED" : "all passed");
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
