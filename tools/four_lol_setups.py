#!/usr/bin/env python3
"""Trims a scraped four.lol pc-setups.json into the setups PC practice shows.

    python3 -I tools/four_lol_setups.py <scraped pc-setups.json> data/four-lol/pc-setups.json

Keeps entries with role "setup", a board, a residue and a PC number (so not DPC). A board
that holds several setups, as blocks between empty rows or side by side, becomes one
setup per block. Each is 4 rows of 10, top row first: piece letters, X for gray, '.' empty.
`need` lists pieces the condition says must be on hand besides the board's ("Hold J",
"I on hold", the 5th PC's carried pair), and `hold` the pieces the attempt may start with
in hold (3rd PC's "(held) piece: S/Z"). Both are empty when the text doesn't say.
data/four-lol/ is gitignored: four.lol's content is for personal use only.
"""
import json
import re
import sys

PAGES = {"opener": "PCO", "grace-system": "Grace System", "nilgiris-1st": "Nilgiri's 1st", "ILSZ": "ILSZ",
         "2nd": "2nd PC", "3rd": "3rd PC", "4th": "4th PC", "5th": "5th PC", "6th": "6th PC", "7th": "7th PC"}
PIECE = "[IJLOSTZ]"


def blocks(board):
    rows = [row.upper() for row in board]
    block = []
    for row in rows + [""]:
        if row.strip(".") and len(block) < 4:
            block.append(row)
            continue
        if block:
            yield block
        block = [row] if row.strip(".") else []


def split_columns(block):
    """Side by side setups, split at columns that are empty in every row."""
    used = [any(row[x] != "." for row in block) for x in range(10)]
    groups, start = [], None
    for x in range(11):
        if x < 10 and used[x]:
            start = x if start is None else start
        elif start is not None:
            groups.append((start, x))
            start = None
    return [["." * a + row[a:b] + "." * (10 - b) for row in block] for a, b in groups]


def pieces(block):
    letters = [c for row in block for c in row if c not in ".X"]
    counts = {c: letters.count(c) for c in set(letters)}
    return len(letters) // 4 if all(n % 4 == 0 for n in counts.values()) else None


def one(pattern, text):
    found = set(re.findall(pattern, text))
    return found.pop() if len(found) == 1 else ""


def main(source, target):
    out = []
    for entry in json.load(open(source)):
        if entry["role"] != "setup" or not entry["board"] or entry["residue"] is None or entry["pc"] is None:
            continue
        condition = entry["condition"] or ""
        need = one(rf"\bHold ({PIECE})\b", condition) or one(rf"\b({PIECE}) on hold\b", condition)
        need = need or one(rf"carried-over pieces \(hold \+ leftover\) are ({PIECE}{{2}})(?:;|$)", condition)
        hold = one(rf"\(held\) piece: ({PIECE}(?:/{PIECE})?)(?:;|$)", condition).replace("/", "")
        # Solution-finder patterns (-p ...) are for the finder, not for reading.
        shown = "; ".join(part for part in condition.split("; ") if not part.startswith("fumen comment: -p"))
        page = entry["page"].split("/")[-1].split("#")[0]
        name = re.sub(r"\s*\(?~?[\d.]+%\)?$", "", entry["name"].replace("(intro)", "")).strip()
        name = f"{PAGES.get(page, page)} {name}".strip()
        for block in blocks(entry["board"]):
            for part in split_columns(block) if (pieces(block) or 0) > 7 else [block]:
                if not pieces(part) or pieces(part) > 7:
                    continue
                board = ["." * 10] * (4 - len(part)) + part
                out.append({"residue": entry["residue"], "name": name, "chance": entry["chance"] or "",
                            "condition": shown, "board": board, "need": need, "hold": hold})
    with open(target, "w") as file:
        json.dump(out, file, indent=1, ensure_ascii=False)
    print(f"{len(out)} setups written to {target}")


if __name__ == "__main__":
    main(*sys.argv[1:])
