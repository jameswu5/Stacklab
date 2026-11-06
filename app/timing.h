// Places timestamped key events on the simulation's 60 Hz tick grid.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "core/game.h"

struct TickPos {
    uint64_t tick;
    double subframe;
};

// Integer maths, so tick boundaries never drift over a long run.
inline TickPos tickAt(uint64_t nsSinceStart) {
    uint64_t scaled = nsSinceStart * core::kTicksPerSecond;
    return {scaled / 1'000'000'000, static_cast<double>(scaled % 1'000'000'000) / 1e9};
}

// Inputs waiting for their tick. Capacity is reserved up front, so play doesn't allocate.
struct InputQueue {
    std::vector<core::InputEvent> events;
    std::vector<uint64_t> ticks;
    std::vector<uint64_t> times;  // event timestamps, for the latency readout
    size_t head = 0;
    size_t measured = 0;

    InputQueue() {
        events.reserve(256);
        ticks.reserve(256);
        times.reserve(256);
    }

    void clear() {
        events.clear();
        ticks.clear();
        times.clear();
        head = measured = 0;
    }

    // An event that reaches us after its tick ran goes at the start of the
    // current tick. Positions stay in order either way.
    void push(core::Action action, bool pressed, uint64_t timeNs, uint64_t startNs, uint64_t currentTick, bool hoisted = false) {
        TickPos pos = tickAt(timeNs > startNs ? timeNs - startNs : 0);
        if (pos.tick < currentTick) pos = {currentTick, 0};
        if (!events.empty() && pos.tick <= ticks.back()) {
            pos.tick = ticks.back();
            pos.subframe = std::max(pos.subframe, events.back().subframe);
        }
        events.push_back({action, pressed, pos.subframe, hoisted});
        ticks.push_back(pos.tick);
        times.push_back(timeNs);
    }

    // Inputs still waiting whose tick is at or before `frame`.
    std::span<const core::InputEvent> due(uint64_t frame) const {
        size_t end = head;
        while (end < events.size() && ticks[end] <= frame) end++;
        return std::span(events).subspan(head, end - head);
    }

    void consume(size_t count) {
        head += count;
        if (head == events.size()) clear();
    }
};
