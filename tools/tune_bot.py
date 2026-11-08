#!/usr/bin/env python3
"""Self-play tuning for ai::Bot's weights, by a regression ladder.

    python3 -I tools/tune_bot.py <bot_match> <start bot file> <output dir> <rounds> [seed]

Each round changes one weight of the champion, in turn: by a random factor, or by a
random amount when it's 0. The candidate plays the champion in versus, in pairs of games
on the same seeds with the sides swapped. A sequential probability ratio test (wins against losses, draws left out)
decides: H0 is a 50% win rate and H1 is 60%, each with 5% error. The candidate replaces
the champion only when the test accepts H1. Settings other than weights, such as width
and depth, are kept from the start file. The champion is written to <output dir>/champion.txt
after every round, with a log beside it.
"""
import math
import os
import random
import re
import subprocess
import sys

WEIGHTS = ["incoming", "surge", "combo", "holes", "height", "danger", "bumpiness", "well", "tSlot", "attack", "b2b", "wasted"]
# The first step for a weight at 0. `incoming` multiplies lines queued by column height, so it's small.
ZERO_STEP = {"incoming": 0.05}
P0, P1, ALPHA, BETA = 0.5, 0.6, 0.05, 0.05
UPPER = math.log((1 - BETA) / ALPHA)
LOWER = math.log(BETA / (1 - ALPHA))
BATCH = 24  # games per bot_match call, an even number so pairs stay together
MAX_GAMES = 480


def read(path):
    values = {}
    for line in open(path):
        parts = line.split("#")[0].split()
        if parts:
            values[parts[0]] = float(parts[1])
    return values


def write(path, values, note):
    with open(path, "w") as f:
        f.write(f"# {note}\n")
        for name, value in values.items():
            f.write(f"{name} {value:.4g}\n")


def match(bot_match, a, b, games, seed):
    out = subprocess.run([bot_match, "versus", str(games), str(seed), a, b], capture_output=True, text=True, check=True).stdout
    won, lost, drew = map(int, re.search(r"A won (\d+), lost (\d+), drew (\d+)", out).groups())
    return won, lost, drew


def sprt(bot_match, a, b, seed):
    """Plays a against b until the test decides or MAX_GAMES. Returns (accepted, won, lost, drew)."""
    won = lost = drew = 0
    while won + lost + drew < MAX_GAMES:
        w, l, d = match(bot_match, a, b, BATCH, seed)
        seed += BATCH
        won, lost, drew = won + w, lost + l, drew + d
        llr = won * math.log(P1 / P0) + lost * math.log((1 - P1) / (1 - P0))
        if llr >= UPPER:
            return True, won, lost, drew
        if llr <= LOWER:
            return False, won, lost, drew
    return False, won, lost, drew


def main():
    bot_match, start, out_dir, rounds = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    seed = int(sys.argv[5]) if len(sys.argv) > 5 else 1
    rng = random.Random(seed)
    os.makedirs(out_dir, exist_ok=True)
    champion_path = os.path.join(out_dir, "champion.txt")
    candidate_path = os.path.join(out_dir, "candidate.txt")
    champion = read(start)
    write(champion_path, champion, "start")
    log = open(os.path.join(out_dir, "log.txt"), "a")

    for round_ in range(rounds):
        candidate = dict(champion)
        name = WEIGHTS[round_ % len(WEIGHTS)]
        value = champion.get(name, 0)
        candidate[name] = value * math.exp(rng.gauss(0, 0.4)) if value else rng.gauss(0, ZERO_STEP.get(name, 0.5))
        write(candidate_path, candidate, f"candidate {round_}")
        # Seeds never repeat between rounds, so no candidate is fitted to one set of queues.
        accepted, won, lost, drew = sprt(bot_match, candidate_path, champion_path, seed * 1_000_000 + round_ * 1000)
        line = f"round {round_}, {name} {candidate[name]:.4g}: won {won}, lost {lost}, drew {drew}, {'accepted' if accepted else 'rejected'}"
        if accepted:
            champion = candidate
            write(champion_path, champion, line)
        print(line, flush=True)
        log.write(line + "  " + " ".join(f"{n}={candidate[n]:.4g}" for n in WEIGHTS) + "\n")
        log.flush()


if __name__ == "__main__":
    main()
