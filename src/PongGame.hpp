// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PongGame.hpp
 * @brief Pong's rules in plain C++: the court, two paddles, the ball, the
 *        CPU, the score and the levels. No LVGL, no pads, no sound -- the app
 *        feeds it input and turns what step() reports into all of those, and a
 *        host test drives it the same way (tests/test_pong_game.cpp).
 */
#pragma once

#include <cstdint>

namespace pong {

enum class Mode : uint8_t { OnePlayer, TwoPlayers };

enum class Phase : uint8_t {
    Serve,     ///< the ball rests on the server's paddle
    Play,      ///< the ball is in play
    Banner,    ///< a level was won: "LEVEL n" for a moment, then Serve
    GameOver,  ///< the match is decided; the app offers the menu
};

/// What one step() did, as bits, for the app's sound and lights.
enum Event : uint32_t {
    kEvPaddle = 1u << 0,    ///< the ball came off a paddle
    kEvWall = 1u << 1,      ///< the ball came off the top or the bottom
    kEvScoreP1 = 1u << 2,   ///< the left player won the point
    kEvScoreP2 = 1u << 3,   ///< the right player (CPU or player 2) won it
    kEvLevelUp = 1u << 4,   ///< player 1 won the level: the banner is up
    kEvGameOver = 1u << 5,  ///< the match is over
    kEvServe = 1u << 6,     ///< the ball was served
};

/// What one level asks of the player, in px/s of a 320-wide court.
struct LevelParams {
    float ballSpeed;     ///< at the serve
    float ballMaxSpeed;  ///< each paddle hit adds kSpeedUpPerHit up to this
    float cpuSpeed;      ///< the CPU paddle's top speed
    float cpuReactX;     ///< the CPU follows the ball only past this share of the court
    float cpuError;      ///< the CPU aims up to this many px off the ball's centre
    bool cpuPredicts;    ///< the CPU aims where the ball will arrive, walls included
};

struct Paddle {
    float y = 0.0f;       ///< top edge
    float target = 0.0f;  ///< where the player asked it to go (top edge)
};

class Game {
public:
    static constexpr int kWinScore = 5;
    static constexpr int kMaxLevel = 9;
    static constexpr float kPaddleW = 6.0f;
    static constexpr float kPaddleH = 40.0f;
    static constexpr float kPaddleMargin = 10.0f;
    static constexpr float kBall = 6.0f;

    static LevelParams levelParams(int level);

    /// A court of w x h px; seed feeds the serve angles and the CPU's aim.
    Game(float w, float h, uint32_t seed);

    void newMatch(Mode mode);

    /// Move player 1's (2's) paddle by px (negative = up), as an encoder detent does.
    void nudge(int player, float px);
    /// Held direction of a player's paddle: -1 up, 0 still, +1 down.
    void setHeld(int player, int dir);
    /// The player whose serve it is serves; a CPU serve is not the player's.
    void serve();

    /// Advance by dt seconds; returns the Event bits of what happened.
    uint32_t step(float dt);

    Mode mode() const { return mode_; }
    Phase phase() const { return phase_; }
    int level() const { return level_; }
    int score(int player) const { return score_[player]; }
    /// Who serves now: 0 = player 1, 1 = the right side.
    int server() const { return server_; }
    /// Whether the serve waits for a person (not the CPU).
    bool serveWaitsForPlayer() const;
    /// 0 = player 1 won the match, 1 = the right side did.
    int winner() const { return winner_; }
    /// The last level was beaten too: player 1 is the champion.
    bool champion() const { return champion_; }

    float width() const { return w_; }
    float height() const { return h_; }
    float ballX() const { return bx_; }
    float ballY() const { return by_; }
    const Paddle& paddle(int player) const { return pad_[player]; }
    float paddleX(int player) const { return player == 0 ? kPaddleMargin : w_ - kPaddleMargin - kPaddleW; }

private:
    static constexpr float kPaddleSlew = 700.0f;    ///< px/s a paddle follows its target
    static constexpr float kHeldSpeed = 230.0f;     ///< px/s of a held pad
    static constexpr float kSpeedUpPerHit = 1.05f;
    static constexpr float kMaxBounceRad = 0.95f;   ///< ~55 degrees off the horizontal
    static constexpr float kMaxServeRad = 0.45f;
    static constexpr float kCpuServeDelay = 0.8f;
    static constexpr float kBannerSeconds = 2.0f;
    static constexpr float kSubStep = 0.004f;       ///< no tunnelling through a paddle

    uint32_t rand();
    float randUnit();   ///< -1..1
    void startLevel(int level);
    void placeBallOnServer();
    void movePaddle(int player, float dt);
    void moveCpu(float dt);
    uint32_t moveBall(float dt);
    uint32_t pointTo(int player);
    uint32_t launch();
    void bounce(int player);
    float predictArrivalY() const;
    bool isCpu(int player) const { return player == 1 && mode_ == Mode::OnePlayer; }

    float w_, h_;
    uint32_t rng_;
    Mode mode_ = Mode::OnePlayer;
    Phase phase_ = Phase::Serve;
    int level_ = 1;
    int score_[2] = {0, 0};
    int server_ = 0;
    int winner_ = 0;
    bool champion_ = false;
    int held_[2] = {0, 0};
    LevelParams params_{};
    uint32_t pending_ = 0;     ///< events of serve(), reported by the next step()
    Paddle pad_[2];
    float bx_ = 0.0f, by_ = 0.0f, vx_ = 0.0f, vy_ = 0.0f, speed_ = 0.0f;
    float timer_ = 0.0f;       ///< CPU serve delay or banner time left
    float cpuAimOffset_ = 0.0f;
};

} // namespace pong
