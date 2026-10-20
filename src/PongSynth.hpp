// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PongSynth.hpp
 * @brief Pong's sound: a few square-wave voices on the audio module, each
 *        playing one short tune (a blip, an arpeggio) per effect.
 *
 * play() is called from one thread (the LVGL thread, where the game steps);
 * it only writes an effect id into a lock-free ring that mix() drains on the
 * audio thread. mix() allocates nothing, locks nothing, logs nothing.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include <crosspad/audio/IAudioNode.hpp>

namespace pong {

enum class Sfx : uint8_t { Paddle, Wall, Serve, ScoreP1, ScoreP2, LevelUp, Win, Lose, MenuMove, MenuSelect, Count };

class Synth : public crosspad::IAudioNode {
public:
    const char* name() const override { return "Pong"; }
    bool isGenerator() const override { return true; }
    void onPrepare(uint32_t, uint32_t sampleRate) override { rate_.store(sampleRate, std::memory_order_relaxed); }

    /// Producer side (one thread). A full ring drops the effect: no audio thread is draining.
    void play(Sfx s) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= kRing) return;
        ring_[head % kRing] = static_cast<uint8_t>(s);
        head_.store(head + 1, std::memory_order_release);
    }

    void mix(float* out, uint32_t frames) override {
        const uint32_t rate = rate_.load(std::memory_order_relaxed);
        if (rate == 0) return;
        drain(rate);
        for (Voice& v : voices_) {
            if (!v.tune) continue;
            for (uint32_t i = 0; i < frames; ++i) {
                if (v.left == 0 && !nextNote(v, rate)) break;
                const float env = envelope(v, rate);
                const float s = (v.phase < 0.5f ? 1.0f : -1.0f) * v.tune->level * env;
                out[i * 2 + 0] += s;
                out[i * 2 + 1] += s;
                v.phase += v.inc;
                if (v.phase >= 1.0f) v.phase -= 1.0f;
                --v.left;
            }
        }
    }

private:
    struct Note {
        float hz;   ///< 0 = a rest
        uint16_t ms;
    };
    struct Tune {
        const Note* notes;
        uint8_t count;
        float level;
    };
    struct Voice {
        const Tune* tune = nullptr;
        uint8_t next = 0;      ///< the note after the one sounding
        uint32_t left = 0;     ///< samples left of the sounding note
        uint32_t len = 1;
        float phase = 0.0f;
        float inc = 0.0f;
        bool rest = false;
    };

    static constexpr uint32_t kRing = 16;
    static constexpr unsigned kVoices = 3;
    static constexpr float kAttackS = 0.002f;

    static const Tune& tune(Sfx s) {
        static constexpr Note kPaddle[] = {{880.0f, 60}};
        static constexpr Note kWall[] = {{440.0f, 45}};
        static constexpr Note kServe[] = {{660.0f, 35}};
        static constexpr Note kScoreP1[] = {{523.3f, 80}, {659.3f, 80}, {784.0f, 140}};
        static constexpr Note kScoreP2[] = {{392.0f, 110}, {293.7f, 110}, {196.0f, 220}};
        static constexpr Note kLevelUp[] = {{523.3f, 90}, {659.3f, 90}, {784.0f, 90}, {1046.5f, 260}};
        static constexpr Note kWin[] = {{784.0f, 110}, {0.0f, 30}, {784.0f, 110}, {1046.5f, 400}};
        static constexpr Note kLose[] = {{392.0f, 170}, {329.6f, 170}, {261.6f, 170}, {196.0f, 420}};
        static constexpr Note kMenuMove[] = {{990.0f, 25}};
        static constexpr Note kMenuSelect[] = {{1320.0f, 60}};
        static const Tune kTunes[] = {
            {kPaddle, 1, 0.30f},   {kWall, 1, 0.22f},       {kServe, 1, 0.18f},
            {kScoreP1, 3, 0.25f},  {kScoreP2, 3, 0.25f},    {kLevelUp, 4, 0.25f},
            {kWin, 4, 0.25f},      {kLose, 4, 0.25f},       {kMenuMove, 1, 0.15f},
            {kMenuSelect, 1, 0.20f},
        };
        static_assert(sizeof(kTunes) / sizeof(kTunes[0]) == static_cast<size_t>(Sfx::Count), "a tune per Sfx");
        return kTunes[static_cast<uint8_t>(s)];
    }

    void drain(uint32_t rate) {
        uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        for (; tail != head; ++tail) {
            const uint8_t id = ring_[tail % kRing];
            if (id >= static_cast<uint8_t>(Sfx::Count)) continue;
            Voice& v = pickVoice();
            v.tune = &tune(static_cast<Sfx>(id));
            v.next = 0;
            v.left = 0;
            v.phase = 0.0f;
            nextNote(v, rate);
        }
        tail_.store(tail, std::memory_order_release);
    }

    Voice& pickVoice() {
        for (Voice& v : voices_)
            if (!v.tune) return v;
        Voice& v = voices_[steal_];   // all busy: take them in turn
        steal_ = (steal_ + 1) % kVoices;
        return v;
    }

    static bool nextNote(Voice& v, uint32_t rate) {
        if (v.next >= v.tune->count) {
            v.tune = nullptr;
            return false;
        }
        const Note& n = v.tune->notes[v.next++];
        v.len = v.left = n.ms * rate / 1000u + 1u;
        v.rest = n.hz <= 0.0f;
        v.inc = v.rest ? 0.0f : n.hz / static_cast<float>(rate);
        return true;
    }

    static float envelope(const Voice& v, uint32_t rate) {
        if (v.rest) return 0.0f;
        const uint32_t done = v.len - v.left;
        const float attack = kAttackS * static_cast<float>(rate);
        if (static_cast<float>(done) < attack) return static_cast<float>(done) / attack;
        return static_cast<float>(v.left) / static_cast<float>(v.len);   // linear decay to the note's end
    }

    uint8_t ring_[kRing]{};
    std::atomic<uint32_t> head_{0}, tail_{0};
    std::atomic<uint32_t> rate_{0};
    Voice voices_[kVoices];
    unsigned steal_ = 0;
};

} // namespace pong
