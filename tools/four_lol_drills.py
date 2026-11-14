#!/usr/bin/env python3
"""Turns scraped four.lol opener and DPC setups into drills for the Setups menu.

    python3 -I tools/four_lol_drills.py <scraped openers.json> data/four-lol/drills

Each entry's `boards` are its fumen pages, 20 rows top first. The drill is the last page
before anything clears (the setup, plus its T-spin if the next page shows one), cut to
the block of rows on the floor. Gray cells (X) are the starting board, and each letter's
cells are split into pieces, the targets. The queue is an order a player can build in
(see build_order), with the T-spin last. data/four-lol/ is gitignored: four.lol's content is for personal use only.
"""
import json
import os
import re
import sys

SHAPES = {"I": [(0, 0), (1, 0), (2, 0), (3, 0)], "O": [(0, 0), (1, 0), (0, 1), (1, 1)], "T": [(0, 0), (1, 0), (2, 0), (1, 1)],
          "L": [(0, 0), (1, 0), (2, 0), (2, 1)], "J": [(0, 0), (1, 0), (2, 0), (0, 1)], "S": [(0, 0), (1, 0), (1, 1), (2, 1)],
          "Z": [(1, 0), (2, 0), (0, 1), (1, 1)]}


def orientations(piece):
    """Each rotation's cells, sorted (y, x) and shifted so the first is (0, 0)."""
    out, cells = set(), SHAPES[piece]
    for _ in range(4):
        cells = [(y, -x) for x, y in cells]
        first = min((y, x) for x, y in cells)
        out.add(tuple(sorted((y - first[0], x - first[1]) for x, y in cells)))
    return [[(x, y) for y, x in shape] for shape in out]


def tile(piece, cells):
    """Splits one letter's cells into pieces, or None. Bottom-left cell first, as it starts any piece's sorted cells."""
    if not cells:
        return []
    x0, y0 = min(cells, key=lambda c: (c[1], c[0]))
    for shape in orientations(piece):
        placed = {(x0 + dx, y0 + dy) for dx, dy in shape}
        if placed <= cells:
            rest = tile(piece, cells - placed)
            if rest is not None:
                return [sorted(placed)] + rest
    return None


def setup_page(boards):
    """The last page reached by only adding pieces: the setup, with its T-spin before the clear."""
    filled = [{(x, 19 - i, c.upper()) for i, row in enumerate(b) for x, c in enumerate(row) if c != "."} for b in boards]
    last = 0
    while last + 1 < len(boards) and filled[last] <= filled[last + 1]:
        last += 1
    return boards[last]


def floor_block(board):
    """Rows from the floor up to the first empty one, bottom row first."""
    rows = []
    for row in reversed(board):
        if row == "." * 10:
            break
        rows.append(row.upper())
    return rows


def build_order(targets, gray, late):
    """Pieces that rest on something with no target left under them, then nothing placed over
    them, lowest first. Pieces in `late` (a T-spin added after page 1) go last."""
    placed, order, left = set(gray), [], [t for t in targets if t not in late]
    while left or late:
        if not left:
            left, late = late, []

        def rests(t):
            return any(y == 0 or (x, y - 1) in placed for x, y in t[1])

        def clear_below(t):
            return not any(x == x2 and y2 < y for o in left if o is not t for x2, y2 in o[1] for x, y in t[1])

        def open_above(t):
            return not any((x, y2) in placed for x, y in t[1] for y2 in range(y + 1, 20))

        ready = ([t for t in left if rests(t) and clear_below(t) and open_above(t)] or [t for t in left if rests(t) and clear_below(t)]
                 or [t for t in left if rests(t)] or left)
        best = min(ready, key=lambda t: min(y for _, y in t[1]))
        left.remove(best)
        order.append(best)
        placed |= set(best[1])
    return order


def drill(entry):
    rows = floor_block(setup_page(entry["boards"]))
    cells = {}
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c != ".":
                cells.setdefault(c, set()).add((x, y))
    gray = cells.pop("X", set())
    targets = []
    for piece, own in sorted(cells.items()):
        pieces = tile(piece, own)
        if pieces is None:
            raise ValueError(f"{entry['name']}: {piece} cells don't split into pieces")
        targets += [(piece, p) for p in pieces]
    first = floor_block(entry["boards"][0])
    on_first = {(x, y) for y, row in enumerate(first) for x, c in enumerate(row) if c != "."}
    order = build_order(targets, gray, [t for t in targets if not on_first & set(t[1])])
    name = re.sub(r"\s*\(~?[\d.]+%\)$", "", entry["name"]).replace(" Extra ", " ").replace(" / ", ": ").replace(" and ", ", ")
    chance = entry.get("chance")
    return {
        "name": name,
        "blurb": f"{len(order)} pieces" + (f", then a {chance} PC" if chance else ""),
        "board": ["".join("X" if (x, y) in gray else "." for x in range(10)) for y in reversed(range(len(rows)))] if gray else [],
        "queue": {"type": "fixed", "pieces": "".join(piece for piece, _ in order)},
        "targets": [{"piece": piece, "cells": [list(c) for c in p]} for piece, p in order],
        "source": entry["page"],
    }


def main(source, target):
    os.makedirs(target, exist_ok=True)
    entries = json.load(open(source))
    for i, entry in enumerate(entries):
        with open(os.path.join(target, f"{i:02d}.json"), "w") as file:
            json.dump(drill(entry), file, indent=1, ensure_ascii=False)
    print(f"{len(entries)} drills written to {target}")


if __name__ == "__main__":
    # A self-check: an S and a Z beside each other split back into their own pieces.
    assert tile("S", {(0, 0), (1, 0), (1, 1), (2, 1)}) == [[(0, 0), (1, 0), (1, 1), (2, 1)]]
    assert len(tile("O", {(0, 0), (1, 0), (0, 1), (1, 1), (0, 2), (1, 2), (0, 3), (1, 3)})) == 2
    main(*sys.argv[1:])
