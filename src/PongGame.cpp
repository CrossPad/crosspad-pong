// SPDX-License-Identifier: GPL-3.0-or-later
#include "PongGame.hpp"

#include <cmath>

namespace pong {

namespace {

constexpr int kTwoPlayerParamsLevel = 3;   // two people get a livelier ball than level 1

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

} // namespace

LevelParams Game::levelParams(int level) {
    const float n = static_cast<float>(level < 1 ? 0 : level > kMaxLevel ? kMaxLevel - 1 : level - 1);
    LevelParams p;
    p.ballSpeed = 140.0f + 20.0f * n;
    p.ballMaxSpeed = p.ballSpeed * 1.6f;
    p.cpuSpeed = 60.0f + 30.0f * n;
    p.cpuReactX = 0.6f - 0.05f * n;
    p.cpuError = 50.0f - 3.0f * n;
    p.cpuPredicts = level >= 7;
    return p;
}

Game::Game(float w, float h, uint32_t seed) : w_(w), h_(h), rng_(seed ? seed : 0x9E3779B9u) {
    startLevel(1);
}

uint32_t Game::rand() {
    // xorshift32
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

float Game::randUnit() { return static_cast<float>(rand() % 2001u) / 1000.0f - 1.0f; }

void Game::newMatch(Mode mode) {
    mode_ = mode;
    champion_ = false;
    winner_ = 0;
    pending_ = 0;
    startLevel(1);
}

void Game::startLevel(int level) {
    level_ = level;
    params_ = levelParams(mode_ == Mode::TwoPlayers ? kTwoPlayerParamsLevel : level);
    score_[0] = score_[1] = 0;
    server_ = 0;
    for (Paddle& p : pad_) p.y = p.target = (h_ - kPaddleH) * 0.5f;
    phase_ = Phase::Serve;
    timer_ = kCpuServeDelay;
    placeBallOnServer();
}

void Game::placeBallOnServer() {
    const Paddle& p = pad_[server_];
    bx_ = server_ == 0 ? paddleX(0) + kPaddleW + 2.0f : paddleX(1) - kBall - 2.0f;
    by_ = p.y + (kPaddleH - kBall) * 0.5f;
    vx_ = vy_ = 0.0f;
}

bool Game::serveWaitsForPlayer() const { return phase_ == Phase::Serve && !isCpu(server_); }

void Game::nudge(int player, float px) {
    if (player < 0 || player > 1 || isCpu(player)) return;
    pad_[player].target = clampf(pad_[player].target + px, 0.0f, h_ - kPaddleH);
}

void Game::setHeld(int player, int dir) {
    if (player < 0 || player > 1) return;
    held_[player] = dir < 0 ? -1 : dir > 0 ? 1 : 0;
}

void Game::serve() {
    if (serveWaitsForPlayer()) pending_ |= launch();
}

uint32_t Game::launch() {
    speed_ = params_.ballSpeed;
    const float a = randUnit() * kMaxServeRad;
    const float dir = server_ == 0 ? 1.0f : -1.0f;
    vx_ = dir * speed_ * std::cos(a);
    vy_ = speed_ * std::sin(a);
    cpuAimOffset_ = randUnit() * params_.cpuError;
    phase_ = Phase::Play;
    return kEvServe;
}

uint32_t Game::step(float dt) {
    uint32_t ev = pending_;
    pending_ = 0;
    if (phase_ == Phase::GameOver || dt <= 0.0f) return ev;

    movePaddle(0, dt);
    if (isCpu(1)) moveCpu(dt);
    else movePaddle(1, dt);

    switch (phase_) {
        case Phase::Banner:
            timer_ -= dt;
            if (timer_ <= 0.0f) {
                phase_ = Phase::Serve;
                timer_ = kCpuServeDelay;
            }
            placeBallOnServer();
            break;
        case Phase::Serve:
            placeBallOnServer();
            if (isCpu(server_)) {
                timer_ -= dt;
                if (timer_ <= 0.0f) ev |= launch();
            }
            break;
        case Phase::Play:
            ev |= moveBall(dt);
            break;
        case Phase::GameOver:
            break;
    }
    return ev;
}

void Game::movePaddle(int player, float dt) {
    Paddle& p = pad_[player];
    if (held_[player]) p.target = clampf(p.target + held_[player] * kHeldSpeed * dt, 0.0f, h_ - kPaddleH);
    const float d = p.target - p.y;
    const float max = kPaddleSlew * dt;
    p.y += d > max ? max : d < -max ? -max : d;
}

float Game::predictArrivalY() const {
    if (vx_ <= 0.0f) return by_ + kBall * 0.5f;
    const float t = (paddleX(1) - (bx_ + kBall)) / vx_;
    const float range = h_ - kBall;
    float y = by_ + vy_ * (t > 0.0f ? t : 0.0f);
    y = std::fmod(y, 2.0f * range);
    if (y < 0.0f) y += 2.0f * range;
    if (y > range) y = 2.0f * range - y;
    return y + kBall * 0.5f;
}

void Game::moveCpu(float dt) {
    Paddle& p = pad_[1];
    float aim = h_ * 0.5f;
    if (phase_ == Phase::Play && vx_ > 0.0f && bx_ > params_.cpuReactX * w_) {
        aim = (params_.cpuPredicts ? predictArrivalY() : by_ + kBall * 0.5f) + cpuAimOffset_;
    }
    const float centre = p.y + kPaddleH * 0.5f;
    const float d = aim - centre;
    const float max = params_.cpuSpeed * dt;
    if (std::fabs(d) > 1.0f) p.y += d > max ? max : d < -max ? -max : d;
    p.y = clampf(p.y, 0.0f, h_ - kPaddleH);
    p.target = p.y;
}

void Game::bounce(int player) {
    const Paddle& p = pad_[player];
    const float offset = clampf(((by_ + kBall * 0.5f) - (p.y + kPaddleH * 0.5f)) / ((kPaddleH + kBall) * 0.5f), -1.0f, 1.0f);
    const float a = offset * kMaxBounceRad;
    speed_ = speed_ * kSpeedUpPerHit;
    if (speed_ > params_.ballMaxSpeed) speed_ = params_.ballMaxSpeed;
    const float dir = player == 0 ? 1.0f : -1.0f;
    vx_ = dir * speed_ * std::cos(a);
    vy_ = speed_ * std::sin(a);
    bx_ = player == 0 ? paddleX(0) + kPaddleW : paddleX(1) - kBall;
    if (player == 0) cpuAimOffset_ = randUnit() * params_.cpuError;
}

uint32_t Game::moveBall(float dt) {
    uint32_t ev = 0;
    const int n = static_cast<int>(std::ceil(dt / kSubStep));
    const float sdt = dt / static_cast<float>(n);
    const float bottom = h_ - kBall;
    for (int i = 0; i < n; ++i) {
        bx_ += vx_ * sdt;
        by_ += vy_ * sdt;
        if (by_ < 0.0f) {
            by_ = -by_;
            vy_ = -vy_;
            ev |= kEvWall;
        } else if (by_ > bottom) {
            by_ = 2.0f * bottom - by_;
            vy_ = -vy_;
            ev |= kEvWall;
        }
        for (int pl = 0; pl < 2; ++pl) {
            if ((pl == 0) != (vx_ < 0.0f)) continue;   // only the paddle the ball heads for
            const float px = paddleX(pl);
            const Paddle& p = pad_[pl];
            const float centre = bx_ + kBall * 0.5f;
            const bool notPast = pl == 0 ? centre >= px : centre <= px + kPaddleW;   // no save from behind
            if (notPast && bx_ + kBall >= px && bx_ <= px + kPaddleW && by_ + kBall >= p.y && by_ <= p.y + kPaddleH) {
                bounce(pl);
                ev |= kEvPaddle;
            }
        }
        if (bx_ + kBall < 0.0f) return ev | pointTo(1);
        if (bx_ > w_) return ev | pointTo(0);
    }
    return ev;
}

uint32_t Game::pointTo(int player) {
    uint32_t ev = player == 0 ? kEvScoreP1 : kEvScoreP2;
    ++score_[player];
    server_ = 1 - player;   // the side that lost the point serves
    if (score_[player] < kWinScore) {
        phase_ = Phase::Serve;
        timer_ = kCpuServeDelay;
        placeBallOnServer();
        return ev;
    }
    if (mode_ == Mode::OnePlayer && player == 0 && level_ < kMaxLevel) {
        startLevel(level_ + 1);
        phase_ = Phase::Banner;
        timer_ = kBannerSeconds;
        return ev | kEvLevelUp;
    }
    champion_ = mode_ == Mode::OnePlayer && player == 0;
    winner_ = player;
    phase_ = Phase::GameOver;
    vx_ = vy_ = 0.0f;
    return ev | kEvGameOver;
}

} // namespace pong
