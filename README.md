# Stacklab

A native macOS block-stacking trainer in C++, built so practice transfers to TETR.IO: sprints, cheese races, perfect-clear and setup coaching, and versus against a bot.

## Setup

```sh
brew install cmake sdl3 sdl3_ttf
cmake -S . -B build
cmake --build build -j3
ctest --test-dir build
./build/stacklab
```

Optional: PC practice's setup hint reads a local, gitignored copy of four.lol's setups (see `THIRD_PARTY_NOTICES.md`):

```sh
python3 -I tools/four_lol_setups.py <scraped pc-setups.json> data/four-lol/pc-setups.json
```
