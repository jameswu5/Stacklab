// Plays the move search's moves as real key presses in core::Game.
#pragma once

#include <cmath>
#include <initializer_list>
#include <span>

#include "ai/finesse.h"

inline int rowOf(const core::Game& game) { return static_cast<int>(std::floor(game.falling.y)); }

// Runs ticks in core until the piece locks or the ticks run out.
struct Driver {
    core::Game& game;
    ai::Finesse* finesse = nullptr;
    bool locked = false;
    core::Spin spin = core::Spin::None;
    bool perfectClear = false;

    void tick(std::initializer_list<core::InputEvent> inputs) {
        if (locked) return;
        std::span span(inputs.begin(), inputs.size());
        const auto& locks = finesse ? finesse->tick(game, span) : game.tick(span);
        if (locks.empty()) return;
        locked = true;
        spin = locks[0].spin;
        perfectClear = locks[0].perfectClear;
    }

    void tap(core::Action action) { tick({{action, true, 0}, {action, false, 0}}); }

    void move(ai::Move move) {
        switch (move) {
            case ai::Move::Left: tap(core::Action::Left); break;
            case ai::Move::Right: tap(core::Action::Right); break;
            case ai::Move::RotateCW: tap(core::Action::RotateCW); break;
            case ai::Move::RotateCCW: tap(core::Action::RotateCCW); break;
            case ai::Move::Rotate180: tap(core::Action::Rotate180); break;
            case ai::Move::DasLeft:
            case ai::Move::DasRight: {
                core::Action action = move == ai::Move::DasLeft ? core::Action::Left : core::Action::Right;
                tick({{action, true, 0}});
                for (int i = 0; i < game.handling.das + 1; i++) tick({});
                tick({{action, false, 0}});
                break;
            }
            case ai::Move::SoftDrop: {
                tick({{core::Action::SoftDrop, true, 0}});
                for (int last = -1; !locked && last != rowOf(game);) {
                    last = rowOf(game);
                    tick({});
                }
                tick({{core::Action::SoftDrop, false, 0}});
                break;
            }
            case ai::Move::Down:
                tick({{core::Action::SoftDrop, true, 0}});
                tick({{core::Action::SoftDrop, false, 0}});
                break;
        }
    }

    void hardDrop() { tick({{core::Action::HardDrop, true, 0}, {core::Action::HardDrop, false, 0}}); }
};
