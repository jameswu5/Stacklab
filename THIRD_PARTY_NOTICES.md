# Third-party notices

## Triangle.js

`core/game.cpp` ports the engine rules, kick tables and piece data of
[Triangle.js](https://github.com/halp1/triangle). `tools/gen_fixtures.mjs` adapts its replay
test's options mapping, and `tests/fixtures/` holds output from its engine.

Copyright (c) 2025 halp

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## tetris-fumen

`core/fumen.cpp` follows the format as implemented by
[tetris-fumen](https://github.com/knewjade/tetris-fumen), including its piece shape and
position tables, and `tests/fumen_test.cpp` uses strings and expected values from its tests
and README.

MIT License

Copyright (c) 2019

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## msgpackr

`tests/ttrmx.h` is written from the unpacker of
[msgpackr](https://github.com/kriszyp/msgpackr), as bundled in Triangle.js, to read `.ttrmx` replays.

MIT License

Copyright (c) 2020 Kris Zyp

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## four.lol

PC practice's setup hint shows perfect-clear setups from [four.lol](https://four.lol/perfect-clears/1st)
by davdav1233 and cosin307, with the setups' chances from knewjade's solution-finder, as
four.lol credits them. © 2018-2022 davdav1233 and cosin307. All Rights Reserved. They're used
privately, with credit, and aren't in this repository: `tools/four_lol_setups.py` trims a local
copy into the gitignored `data/four-lol/`, and the app says "No setups file" without it.

Setup drills use four.lol's opener pages ([Black Tea Cannon](https://four.lol/openers/black-tea-cannon),
[TKI](https://four.lol/openers/tki), [New DT Cannon](https://four.lol/openers/new-dt-cannon),
[MKO](https://four.lol/openers/mko), [Albatross](https://four.lol/openers/albatross)) and its
[DPC](https://four.lol/perfect-clears/dpc) setups, likewise used privately with credit:
`tools/four_lol_drills.py` turns a local copy into the gitignored `data/four-lol/drills/`.
