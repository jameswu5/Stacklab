// Writes the golden fixtures in tests/fixtures/ by running Triangle.js's engine.
// Passing them proves we match Triangle, not TETR.IO; real replays test that.
//
//   npm install --ignore-scripts --prefix <dir> @haelp/teto@4.2.8
//   node tools/gen_fixtures.mjs <dir>
//   node tools/gen_fixtures.mjs <dir> --replay <replay file or folder> <out.json>
//
// --replay writes lock-by-lock data for real replays' garbage-free rounds, to
// compare per-lock attack and boards with replay_parity. Keep the output out of
// the repo: it carries players' inputs.
//
// The options mapping in engineParams is adapted from Triangle.js's
// test/engine/replay.ts (MIT, see THIRD_PARTY_NOTICES.md), with its swapped
// boardwidth and boardheight corrected.
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const VERSION = "4.2.8";
const SOURCE = `Triangle.js ${VERSION} engine, not TETR.IO`;
const tetoDir = path.resolve(process.argv[2]);
const installed = JSON.parse(fs.readFileSync(path.join(tetoDir, "node_modules/@haelp/teto/package.json")));
if (installed.version !== VERSION) throw new Error(`need @haelp/teto ${VERSION}, found ${installed.version}`);
const require = createRequire(tetoDir + "/");
const { Engine, Bag7, kickData, tetrominoes } = require("@haelp/teto/engine");

const outDir = path.join(path.dirname(new URL(import.meta.url).pathname), "../tests/fixtures");

const engineParams = (seed, o = {}) => ({
  board: { width: o.boardwidth ?? 10, height: o.boardheight ?? 20, buffer: 20 },
  kickTable: o.kickset ?? "SRS+",
  options: {
    comboTable: o.combotable ?? "multiplier",
    garbageBlocking: o.garbageblocking ?? "combo blocking",
    clutch: o.clutch ?? true,
    garbageTargetBonus: o.garbagetargetbonus ?? "none",
    spinBonuses: o.spinbonuses ?? "all-mini+",
    stock: 0,
  },
  queue: { minLength: 10, seed, type: o.bagtype ?? "7-bag" },
  garbage: {
    bombs: false,
    cap: { absolute: 0, increase: 0, max: 40, value: 8, marginTime: 0 },
    boardWidth: o.boardwidth ?? 10,
    garbage: { speed: 20, holeSize: 1 },
    messiness: { change: 1, nosame: false, timeout: 0, within: 0, center: false },
    multiplier: {
      value: o.garbagemultiplier ?? 1,
      increase: o.garbageincrease ?? 0.008,
      marginTime: o.garbagemargin ?? 10800,
    },
    specialBonus: o.garbagespecialbonus ?? false,
    openerPhase: o.openerphase ?? 0,
    seed,
    rounding: o.roundmode ?? "down",
  },
  gravity: { value: o.g ?? 0.02, increase: o.gincrease ?? 0, marginTime: o.gmargin ?? 0 },
  handling: { arr: 0, das: 6, dcd: 0, sdf: 41, safelock: false, cancel: false, may20g: true, irs: "tap", ihs: "tap", ...o.handling },
  b2b: {
    chaining: o.b2bchaining ?? !o.b2bcharging,
    charging: o.b2bcharging ? { at: o.b2bcharge_at ?? 4, base: o.b2bcharge_base ?? 3 } : false,
  },
  pc: { b2b: o.allclear_b2b ?? 0, garbage: o.allclear_garbage ?? 0 },
  misc: {
    allowed: { hardDrop: true, spin180: o.allow180 ?? true, hold: o.display_hold ?? true, retry: false, undo: false },
    infiniteHold: false,
    movement: { infinite: false, lockResets: o.lockresets ?? 15, lockTime: o.locktime ?? 30, may20G: o.gravitymay20g ?? true },
    stride: false,
    date: new Date("2026-01-01"),
  },
});

const rowsOf = (state) => {
  const rows = state.map((r) => r.map((c) => (c ? (c.mino === "gb" ? "G" : c.mino.toUpperCase()) : ".")).join(""));
  while (rows.length && !/[^.]/.test(rows.at(-1))) rows.pop();
  return rows;
};

const cellsOf = (e) =>
  e.falling.absoluteBlocks.map(([x, y]) => [x, y]).sort((a, b) => a[0] - b[0] || a[1] - b[1]).flat();

// Runs one game and fills in everything replay_parity compares against. `board`
// and `queue` are injected after the first piece has spawned from the seed.
const play = (game) => {
  // Written out because a missing value means T-spins in replay_parity but all-mini+ here.
  const options = { spinbonuses: "all-mini+", ...game.options };
  const e = new Engine(engineParams(game.seed, options));
  if (game.board) {
    game.board.forEach((row, y) => [...row].forEach((c, x) => {
      if (c !== ".") e.board.state[y][x] = { mino: c === "G" ? "gb" : c === "X" ? "z" : c.toLowerCase(), connections: 0 };
    }));
  }
  if (game.queue) e.queue.splice(0, e.queue.length, ...game.queue);
  if (game.b2b !== undefined) e.stats.b2b = game.b2b;
  if (game.combo !== undefined) e.stats.combo = game.combo;

  const locks = [];
  let tick = 0;
  e.events.on("falling.lock", (r) => {
    const lock = { frame: tick, piece: r.mino.toUpperCase(), lines: r.lines, b2b: r.stats.b2b, combo: r.stats.combo };
    if (r.spin !== "none") lock.spin = r.spin;
    if (r.rawGarbage.length) lock.sends = r.rawGarbage;
    if (r.lines > 0 && e.board.perfectClear) lock.pc = true;
    lock.board = rowsOf(e.board.state);
    locks.push(lock);
  });

  const byFrame = [];
  for (const [frame, key, pressed, subframe, hoisted] of game.inputs) {
    (byFrame[frame] ??= []).push({ frame, type: pressed ? "keydown" : "keyup", data: { key, subframe, hoisted: !!hoisted } });
  }

  const trace = [];
  let lastCells = "";
  let toppedOut = false;
  for (; tick < game.frames && !toppedOut; tick++) {
    e.tick(byFrame[tick] ?? []);
    toppedOut = e.toppedOut;
    if (game.trace) {
      const cells = cellsOf(e);
      if (String(cells) !== lastCells) trace.push([tick, ...cells]);
      lastCells = String(cells);
    }
  }

  const out = { ...game, options, frames: tick, locks };
  if (game.trace) out.trace = trace;
  if (!locks.length && game.board === undefined) delete out.locks;
  out.end = {
    toppedOut,
    board: rowsOf(e.board.state),
    piece: e.falling.symbol.toUpperCase(),
    rot: e.falling.rotation,
    cells: cellsOf(e),
    hold: e.held ? e.held.toUpperCase() : "",
    next: [...e.queue].slice(0, 5).join("").toUpperCase(),
    pieces: e.stats.pieces,
    lines: e.stats.lines,
    attack: e.stats.garbage.attack,
    b2b: e.stats.b2b,
    combo: e.stats.combo,
  };
  return out;
};

// Input script: L R CW CCW 180 H HD tap for one frame; SD taps soft drop for two;
// +KEY and -KEY press and release without using a frame; wN waits N frames;
// @.3 sets the subframe and ! marks a hoisted press, as in +L@.3!
const KEYS = { L: "moveLeft", R: "moveRight", CW: "rotateCW", CCW: "rotateCCW", 180: "rotate180", H: "hold", HD: "hardDrop", SD: "softDrop" };

const compile = (script) => {
  const inputs = [];
  let t = 0;
  for (const raw of script.trim().split(/\s+/).filter(Boolean)) {
    const [, sign, name, sub, hoist] = raw.match(/^([+-]?)([A-Za-z0-9]+?)(?:@([.\d]+))?(!?)$/);
    const subframe = sub ? Number(sub) : 0;
    if (/^w\d+$/.test(name)) t += Number(name.slice(1));
    else if (sign === "+") inputs.push(hoist ? [t, KEYS[name], 1, subframe, 1] : [t, KEYS[name], 1, subframe]);
    else if (sign === "-") inputs.push([t, KEYS[name], 0, subframe]);
    else if (name === "SD") {
      inputs.push([t, KEYS.SD, 1, 0], [t + 1, KEYS.SD, 0, 0]);
      t += 2;
    } else {
      inputs.push([t, KEYS[name], 1, subframe], [t, KEYS[name], 0, 0.5]);
      t += 1;
    }
  }
  return { inputs, frames: t + 1 };
};

const seedStarting = (piece) => {
  for (let seed = 1; ; seed++) if (new Bag7(seed).next()[0] === piece) return seed;
};

// `pieces` is the first piece followed by the injected queue. Padded so a game
// never reaches the bag, whose state differs after injection.
const scripted = (name, pieces, script, extra = {}) => {
  const [first, ...rest] = pieces.toLowerCase();
  return play({
    name,
    seed: seedStarting(first),
    queue: [...rest, ..."iotszjliotszjl"],
    ...compile(script),
    ...extra,
  });
};

// Breadth-first search for inputs whose hard drop satisfies `goal`, so a directed
// game is checked to hit the case it claims rather than trusted to.
const solve = (pieces, board, goal, options) => {
  const seed = seedStarting(pieces[0]);
  const queue = [...pieces.slice(1), ..."iotszjliotszjl"];
  const run = (script) => play({ name: "", seed, queue, board, options, ...compile(script) });
  const key = (script) => {
    const { cells } = run(script).end;
    return String(cells);
  };
  const seen = new Set([key("")]);
  let frontier = [""];
  for (let depth = 0; depth < 9; depth++) {
    const next = [];
    for (const script of frontier) {
      const lock = run(script + " HD").locks[0];
      if (lock && goal(lock)) return (script + " HD").trim();
      for (const move of ["L", "R", "CW", "CCW", "180", "SD"]) {
        const longer = script + " " + move;
        const k = key(longer);
        if (seen.has(k)) continue;
        seen.add(k);
        next.push(longer);
      }
    }
    frontier = next;
  }
  throw new Error(`no solution for ${pieces} on ${board.join("/")}`);
};

const directed = (name, pieces, board, goal, options, extra = {}) =>
  scripted(name, pieces, solve(pieces, board, goal, options), { board, options, ...extra });

const blocksOf = (piece, rot, x, y) => tetrominoes[piece].matrix.data[rot].map(([bx, by]) => [x + bx, y - by]);
const has = (cells, [x, y]) => cells.some(([a, b]) => a === x && b === y);

const spawnOf = (piece) => [Math.floor(5 - tetrominoes[piece].matrix.w / 2), 22];

const rotateTap = (from, to, frame, subframe) => {
  const key = (to - from + 4) % 4 === 1 ? "rotateCW" : (to - from + 4) % 4 === 3 ? "rotateCCW" : "rotate180";
  return [[frame, key, 1, subframe], [frame, key, 0, subframe + 0.05]];
};

const toRows = (cells) => {
  const rows = [];
  for (const [x, y] of cells) {
    while (rows.length <= y) rows.push(Array(10).fill("."));
    rows[y][x] = "X";
  }
  return rows.map((r) => r.join(""));
};

// Every SRS+ kick, including 180s: floating blockers fail each earlier test so
// rotation has to take kick k. k == kicks.length blocks all of them.
const kickGames = () => {
  const games = [];
  const table = kickData["SRS+"];
  let unreachable = 0;
  for (const piece of "jlstzi") {
    for (const from of [0, 1, 2, 3]) {
      for (const to of [0, 1, 2, 3]) {
        if (from === to) continue;
        const id = `${from}${to}`;
        const kicks = table[`${piece}_kicks`]?.[id] ?? table.kicks[id];
        const [sx, sy] = spawnOf(piece);
        const tests = [[0, 0], ...kicks].map(([dx, dy]) => blocksOf(piece, to, sx + dx, sy - dy));
        const source = [...blocksOf(piece, 0, sx, sy), ...blocksOf(piece, from, sx, sy)];
        for (let k = 0; k <= kicks.length; k++) {
          const target = k < kicks.length ? tests[k + 1] : [];
          const keep = [...source, ...target];
          const blockers = [];
          let reachable = true;
          for (let j = 0; j <= k && j < tests.length; j++) {
            if (tests[j].some((c) => has(blockers, c) || c[0] < 0 || c[0] > 9)) continue;
            const pick = tests[j].find((c) => !has(keep, c));
            if (!pick) reachable = false;
            else blockers.push(pick);
          }
          if (!reachable) {
            unreachable++;
            continue;
          }
          const name = `${piece} ${id} kick ${k === kicks.length ? "none fit" : k}`;
          const inputs = [...(from ? rotateTap(0, from, 0, 0) : []), ...rotateTap(from, to, 0, 0.2)];
          const game = play({ name, seed: seedStarting(piece), board: toRows(blockers), inputs, frames: 1 });
          const wanted = k === kicks.length ? blocksOf(piece, from, sx, sy) : target;
          const got = game.end.cells;
          if (!wanted.every(([x, y]) => [0, 2, 4, 6].some((i) => got[i] === x && got[i + 1] === y))) {
            throw new Error(`${name} landed on ${got}`);
          }
          games.push(game);

          // Same rotation resting on a blocker, then locked: spin state survives
          // only without a fall, so this reaches detection on every kick.
          if (k < kicks.length) {
            const floor = target
              .map(([x, y]) => [x, y - 1])
              .filter((c) => !has(keep, c) && c[1] >= 0);
            if (!floor.length) continue;
            const resting = [...blockers, floor[0]];
            const dropped = play({
              name: name + " locked",
              seed: seedStarting(piece),
              board: toRows(resting),
              inputs: [...inputs, [0, "hardDrop", 1, 0.4], [0, "hardDrop", 0, 0.5]],
              frames: 1,
            });
            games.push(dropped);
          }
        }
      }
    }
  }
  games.push(play({ name: "o rotates in place", seed: seedStarting("o"), ...compile("CW CW 180 CCW") }));
  console.log(`kicks: ${games.length} games, ${unreachable} kick positions impossible to isolate from spawn`);
  return games;
};

// Fin and TST kicks count as full T-spins however the corners fall.
const finGames = () => {
  const games = [];
  const table = kickData["SRS+"].kicks;
  for (const id of ["23", "03", "21", "01"]) {
    const from = Number(id[0]);
    const to = Number(id[1]);
    const k = table[id].findIndex(([dx, dy]) => Math.abs(dx) === 1 && dy === 2);
    const [sx, sy] = spawnOf("t");
    const tests = [[0, 0], ...table[id]].map(([dx, dy]) => blocksOf("t", to, sx + dx, sy - dy));
    const target = tests[k + 1];
    const keep = [...blocksOf("t", 0, sx, sy), ...blocksOf("t", from, sx, sy), ...target];
    const blockers = [];
    let reachable = true;
    for (let j = 0; j <= k; j++) {
      if (tests[j].some((c) => has(blockers, c))) continue;
      const pick = tests[j].find((c) => !has(keep, c));
      if (pick) blockers.push(pick);
      else reachable = false;
    }
    if (!reachable) continue;
    const [dx, dy] = table[id][k];
    const cx = sx + dx + 1;
    const cy = sy - dy - 1;
    const corners = [[cx - 1, cy - 1], [cx + 1, cy - 1], [cx - 1, cy + 1], [cx + 1, cy + 1]];
    corners.forEach((skip, i) => {
      const fill = corners.filter((c, n) => n !== i && !has(keep, c));
      const floor = target.map(([x, y]) => [x, y - 1]).find((c) => !has(keep, c));
      games.push(play({
        name: `t ${id} fin corners missing ${i}`,
        seed: seedStarting("t"),
        board: toRows([...blockers, ...fill, floor]),
        inputs: [...(from ? rotateTap(0, from, 0, 0) : []), ...rotateTap(from, to, 0, 0.2), [0, "hardDrop", 1, 0.4], [0, "hardDrop", 0, 0.5]],
        frames: 1,
      }));
    });
  }
  return games;
};

const is = (spin, lines) => (l) => (l.spin ?? "none") === spin && l.lines === lines;

const TSD = ["XXXX.XXXXX", "XXX...XXXX", "XXXX..XXXX"];
const TSS = ["XXXX.XXXXX", "XX....XXXX", "XXXX..XXXX"];
const TST = ["XXXX.XXXXX", "XXX..XXXXX", "XXXX.XXXXX", "X....XXXXX", "X...XXXXXX"];
const MINI = ["XXXX.XXXXX", "XXX...XXXX", "XXXX......"];

// Tries slots open only where the piece ends and straight above where it rotates
// from, until the engine itself reports `goal`.
const allSpinBoard = (piece, goal) => {
  for (const x of [3, 4, 5]) {
    for (const from of [0, 1, 2, 3]) {
      for (const to of [0, 1, 2, 3]) {
        if (from === to) continue;
        const floor = Math.min(...blocksOf(piece, to, x, 0).map(([, y]) => y));
        const final = blocksOf(piece, to, x, -floor);
        const approach = blocksOf(piece, from, x, -floor);
        const open = [...final, ...approach];
        for (const [cx, cy] of approach) for (let y = cy; y < 7; y++) open.push([cx, y]);
        if (open.some(([cx, cy]) => cx < 0 || cx > 9 || cy < 0)) continue;
        const board = Array.from({ length: 7 }, (_, y) =>
          Array.from({ length: 10 }, (_, cx) => (has(open, [cx, y]) ? "." : "X")).join(""));
        try {
          return { board, script: solve(piece, board, goal) };
        } catch {}
      }
    }
  }
  throw new Error(`no all-spin board for ${piece}`);
};

const spinGames = () => {
  const games = [
    directed("tspin single", "t", TSS, is("normal", 1)),
    directed("tspin double", "t", TSD, is("normal", 2)),
    directed("tspin triple", "t", TST, is("normal", 3)),
    directed("tspin mini single", "t", MINI, is("mini", 1)),
    directed("tspin no lines", "t", ["XXXX.XXXX.", "XXX...XXX.", "XXXX..XXXX"], is("normal", 0)),
    directed("tspin double as t-spins rule", "t", TSD, is("normal", 2), { spinbonuses: "T-spins" }),
    directed("tspin double as no-spin rule", "t", TSD, is("none", 2), { spinbonuses: "none" }),
    directed("spin then move loses it", "t", ["XXXXXXX...", "XXXXXXX..."], is("none", 0)),
  ];
  const anySpin = (l) => l.spin === "mini" && l.lines > 0;
  for (const piece of "szjl") {
    const { board, script } = allSpinBoard(piece, anySpin);
    games.push(scripted(`${piece} all-spin clears`, piece, script, { board }));
    games.push(scripted(`${piece} same under t-spins rule`, piece, script, { board, options: { spinbonuses: "T-spins" } }));
  }
  for (const piece of "ostzjli") {
    const pit = piece === "i" ? ["XXXXXXXXXX".slice(0, 6) + "...."] : ["XXXXXX....", "XXXXXX...X"];
    games.push(scripted(`${piece} no spin on open ground`, piece, "R R R HD", { board: pit }));
  }
  return games;
};

// Vertical I into a one-wide well clears `lines` and keeps attack simple.
const wellRows = (lines) => Array(lines).fill("XXXXXXXXX.");
const WELL_SCRIPT = solve("i", wellRows(4), (l) => l.lines === 4);

const CHARGING = { b2bcharging: true, b2bchaining: false, b2bcharge_at: 4, b2bcharge_base: 3 };

const attackGames = () => {
  const games = [];
  for (const lines of [1, 2, 3, 4]) {
    for (const b2b of [-1, 0, 1, 2, 3, 5, 8, 24, 67]) {
      games.push(scripted(`${lines} lines at b2b ${b2b}`, "i", WELL_SCRIPT, { board: wellRows(lines), b2b }));
    }
    for (const combo of [-1, 0, 1, 2, 3, 4, 7, 12, 30]) {
      games.push(scripted(`${lines} lines at combo ${combo}`, "i", WELL_SCRIPT, { board: wellRows(lines), combo }));
    }
    for (const [b2b, combo] of [[3, 2], [8, 5], [1, 1]]) {
      games.push(scripted(`${lines} lines at b2b ${b2b} combo ${combo}`, "i", WELL_SCRIPT, { board: wellRows(lines), b2b, combo }));
    }
    for (const options of [{ garbagemultiplier: 1.37 }, { garbagemultiplier: 0.5 }, { roundmode: "rng", garbagemultiplier: 1.37 }, { b2bchaining: false }]) {
      games.push(scripted(`${lines} lines with ${JSON.stringify(options)}`, "i", WELL_SCRIPT, { board: wellRows(lines), options, b2b: 2, combo: 3 }));
    }
  }
  const tsd = solve("t", TSD, is("normal", 2));
  const tst = solve("t", TST, is("normal", 3));
  const mini = solve("t", MINI, is("mini", 1));
  for (const [b2b, combo] of [[-1, -1], [0, 0], [1, 1], [3, 2], [8, 5], [24, 9]]) {
    games.push(scripted(`tsd b2b ${b2b} combo ${combo}`, "t", tsd, { board: TSD, b2b, combo }));
    games.push(scripted(`tst b2b ${b2b} combo ${combo}`, "t", tst, { board: TST, b2b, combo }));
    games.push(scripted(`mini b2b ${b2b} combo ${combo}`, "t", mini, { board: MINI, b2b, combo }));
  }

  // Rounding: one fractional send after another, so the draws have to line up.
  const well20 = wellRows(20);
  const dropAll = solve("i", well20, (l) => l.lines === 4) + " " + "CW R R R R HD ".repeat(4);
  games.push(scripted("rng rounding across quads", "iiiii", dropAll, { board: well20, options: { roundmode: "rng", garbagemultiplier: 1.37 }, combo: 1, b2b: 2 }));
  games.push(scripted("rng rounding, another seed", "iiiii", dropAll, { board: well20, options: { roundmode: "rng", garbagemultiplier: 0.77 }, b2b: 5 }));

  // Multiplier growth over time.
  for (const [wait, options] of [[60, { garbagemargin: 0, garbageincrease: 0.1 }], [120, { garbagemargin: 60, garbageincrease: 0.05 }], [30, { garbagemargin: 60, garbageincrease: 0.05 }]]) {
    games.push(scripted(`multiplier after ${wait} frames ${JSON.stringify(options)}`, "i", `+CW w1 -CW +R w20 -R w${wait} HD`, { board: wellRows(4), options: { ...options, locktime: 1000 }, combo: 1 }));
  }

  // Natural chains, no injected counters: five quads then a single breaks b2b.
  const chain = solve("i", wellRows(21), (l) => l.lines === 4) + " " + "CW R R R R HD ".repeat(5);
  games.push(scripted("b2b chain breaks, chaining", "iiiiii", chain, { board: wellRows(21) }));
  games.push(scripted("b2b chain breaks, surge", "iiiiii", chain, { board: wellRows(21), options: CHARGING }));
  for (const [at, base] of [[1, 0], [2, 3], [4, 5]]) {
    const options = { ...CHARGING, b2bcharge_at: at, b2bcharge_base: base };
    games.push(scripted(`surge at ${at} base ${base}`, "iiiiii", chain, { board: wellRows(21), options }));
    games.push(scripted(`surge at ${at} base ${base} multiplied`, "iiiiii", chain, { board: wellRows(21), options: { ...options, garbagemultiplier: 1.37 } }));
  }
  const single = solve("i", wellRows(1), (l) => l.lines === 1);
  for (const b2b of [2, 3, 4, 5, 6, 10, 30]) {
    games.push(scripted(`surge from b2b ${b2b}`, "i", single, { board: wellRows(1), b2b, options: CHARGING }));
  }
  games.push(scripted("no surge when no clear", "i", "HD", { b2b: 8, options: CHARGING }));

  // Natural combos: O doubles in a two-wide well, then flat I singles.
  const oWell = Array(10).fill("XXXXXXXX..");
  games.push(scripted("combo of doubles", "ooooo", "R R R R HD ".repeat(5), { board: oWell }));
  const iRows = Array(6).fill("XXXXXX....");
  games.push(scripted("combo of singles", "iiiiiii", "R R R HD ".repeat(6), { board: iRows }));
  games.push(scripted("combo breaks on a miss", "oojoo", "R R R R HD R R R R HD L L L HD R R R R HD R R R R HD", { board: oWell }));

  // Perfect clears: pc garbage, pc b2b, and both combined with an existing chain.
  const pcQuad = wellRows(4);
  const pcSingle = ["XXXXXX...."];
  const iFlat = "R R R HD";
  for (const options of [{}, { allclear_garbage: 10 }, { allclear_garbage: 5, allclear_b2b: 1 }, { allclear_garbage: 3, allclear_b2b: 2 }, { allclear_garbage: 5, garbagemultiplier: 1.37 }, { ...CHARGING, allclear_garbage: 5, allclear_b2b: 1 }]) {
    for (const b2b of [-1, 0, 6]) {
      const label = `${JSON.stringify(options)} b2b ${b2b}`;
      games.push(scripted(`pc quad ${label}`, "i", WELL_SCRIPT, { board: pcQuad, options, b2b }));
      games.push(scripted(`pc single ${label}`, "i", iFlat, { board: pcSingle, options, b2b }));
    }
  }

  // Special bonus: cleared garbage plus a spin or quad.
  const garbageWell = Array(4).fill("GGGGGGGGG.");
  const garbageTsd = TSD.map((r) => r.replaceAll("X", "G"));
  for (const garbagespecialbonus of [true, false]) {
    const options = { garbagespecialbonus };
    games.push(scripted(`garbage quad bonus ${garbagespecialbonus}`, "i", WELL_SCRIPT, { board: garbageWell, options }));
    games.push(scripted(`garbage tsd bonus ${garbagespecialbonus}`, "t", tsd, { board: garbageTsd, options }));
    games.push(scripted(`garbage single, no bonus ${garbagespecialbonus}`, "i", single, { board: ["GGGGGGGGG."], options }));
  }
  return games;
};

const handlingGames = () => {
  const games = [];
  const held = (name, pieces, script, handling, options = {}, extra = {}) =>
    games.push(scripted(name, pieces, script, { options: { ...options, handling }, trace: true, ...extra }));

  // ARR 0 and fractional DAS included; every tick's position is compared.
  for (const das of [0, 1, 6, 7.2, 10, 20]) {
    for (const arr of [0, 1, 2, 2.5, 5]) {
      held(`hold left das ${das} arr ${arr}`, "t", "+L w40 -L w4", { das, arr });
      held(`hold right das ${das} arr ${arr} off-frame`, "i", "+R@.3 w30 -R@.7 w4", { das, arr });
    }
  }
  for (const dcd of [1, 3, 6]) {
    for (const arr of [0, 2]) {
      const handling = { das: 7.2, arr, dcd };
      held(`dcd ${dcd} arr ${arr} rotate at wall`, "t", "+L w25 CW w2 -L w8", handling);
      held(`dcd ${dcd} arr ${arr} held through spawn`, "ttt", "+L w15 HD w20 HD w20 -L", handling);
      held(`dcd ${dcd} arr ${arr} hold at wall`, "tj", "+R w25 H w20 -R", handling);
    }
  }
  for (const g of [0.02, 0.5, 1, 5]) {
    for (const sdf of [1, 5, 6, 20, 40, 41]) {
      held(`soft drop g ${g} sdf ${sdf}`, "t", "w3 +SD w30 -SD w40", { sdf }, { g, locktime: 20 });
    }
    held(`gravity g ${g}`, "tj", "w120", {}, { g });
  }
  for (const [g, may20] of [[21, true], [21, false], [20, true], [40, true]]) {
    for (const may20g of [true, false]) {
      const options = { g, gravitymay20g: may20 };
      held(`20g ${g} room ${may20} player ${may20g}`, "tjl", "L L R CW w40 H R R w40", { may20g, sdf: 41 }, options);
      held(`20g soft drop ${g} room ${may20} player ${may20g}`, "tj", "+SD w5 -SD R R w20", { may20g, sdf: 10 }, options);
    }
  }
  for (const cancel of [false, true]) {
    held(`both directions cancel ${cancel}`, "t", "+L w10 +R w10 -R w10 -L w3", { das: 6, arr: 2, cancel });
    held(`both directions other order cancel ${cancel}`, "t", "+R w10 +L w10 -R w10 -L w3", { das: 6, arr: 2, cancel });
    held(`release first key cancel ${cancel}`, "t", "+L w3 +R w3 -L w12 -R w3", { das: 6, arr: 2, cancel });
  }
  for (const safelock of [false, true]) {
    for (const wait of [1, 3, 6, 7, 8, 10]) {
      held(`safelock ${safelock} hard drop ${wait} after natural lock`, "tjl", `+SD w32 -SD w${wait} HD w3`, { safelock }, { locktime: 5 });
    }
  }
  for (const irs of ["tap", "hold", "off"]) {
    held(`irs ${irs} cw held`, "tt", "+CW HD w3 -CW w3", { irs });
    held(`irs ${irs} ccw held`, "tt", "+CCW HD w3 -CCW w3", { irs });
    held(`irs ${irs} 180 held`, "tt", "+180 HD w3 -180 w3", { irs });
    held(`irs ${irs} cw and 180 held`, "tt", "+CW +180 HD w3 -CW -180 w3", { irs });
  }
  for (const ihs of ["tap", "hold", "off"]) {
    held(`ihs ${ihs} hold key held`, "tjl", "+H HD w3 -H HD w3", { ihs });
  }
  for (const [das, dcd] of [[6, 0], [7.2, 2], [10, 10]]) {
    held(`hoisted left das ${das} dcd ${dcd}`, "t", "+L! w20 -L w3", { das, arr: 0, dcd });
    held(`hoisted right das ${das} dcd ${dcd} arr 2`, "t", "+R! w20 -R w3", { das, arr: 2, dcd });
  }
  held("kick leaves the falling fraction", "t", "w3 CW w30 CCW w30", {});
  return games;
};

const lockGames = () => {
  const games = [];
  const traced = (name, pieces, script, options = {}, extra = {}) =>
    games.push(scripted(name, pieces, script, { options, trace: true, ...extra }));

  for (const locktime of [1, 15, 30, 60]) {
    traced(`lock delay ${locktime}`, "tj", "+SD w2 -SD w80", { locktime });
  }
  for (const lockresets of [0, 1, 5, 15, 30]) {
    traced(`move resets ${lockresets}`, "tj", "SD " + "w10 L w10 R ".repeat(12), { lockresets });
    traced(`rotate resets ${lockresets}`, "tj", "SD " + "w10 CW ".repeat(24), { lockresets });
    traced(`fast move resets ${lockresets}`, "tj", "SD " + "L R ".repeat(20) + "w40", { lockresets });
  }
  traced("rotations past the fall-faster limit in the air", "t", "CW ".repeat(50) + "w60");
  traced("rotations past the kick movement limit", "t", "SD " + "w2 CW ".repeat(40) + "w40", { lockresets: 15, locktime: 60 });
  traced("rotations past both limits, kicked", "j", "SD " + "CW CCW 180 ".repeat(15) + "w40", { locktime: 60 }, { board: ["XXXXXXXXXX".slice(0, 2) + "......XX", "XX......XX"] });
  // Falling off a ledge to a new lowest row refunds the resets.
  traced("ledge refunds resets", "o", "SD " + "w3 L w3 R ".repeat(6) + "R R R R w50", { lockresets: 15 }, { board: ["XXXXXX....", "XXXXXX....", "XXXXXX...."] });
  traced("lock reset does not refund on the same row", "o", "SD " + "w3 L w3 R ".repeat(10) + "w50", { lockresets: 15 });

  // Tucks under an overhang, reached by soft drop and sideways moves.
  const tuckBoards = [
    ["t", "tuck under left overhang", ["XX........", "XX........", "XXXXX.....", ".........."].slice(0, 3)],
    ["l", "tuck l under ledge", ["XXX.......", "XXX.......", "XXXXXX....", "XXXXXX...."]],
    ["s", "tuck s under ledge", ["XX....XXXX", "XX....XXXX", "XXXX..XXXX"]],
    ["j", "tuck j under right ledge", [".......XXX", ".......XXX", "....XXXXXX", "....XXXXXX"]],
  ];
  for (const [piece, name, board] of tuckBoards) {
    const covered = (l) => {
      const top = l.board.length;
      return top > 0 && l.lines === 0 && hasCellUnderOverhang(l.board, board, piece);
    };
    const script = solve(piece, board, covered);
    games.push(scripted(name, piece, script, { board, trace: true }));
    const sdf = script.replaceAll("SD", "+SD w8 -SD");
    games.push(scripted(`${name} slow soft drop`, piece, sdf, { board, trace: true, options: { handling: { sdf: 20 }, locktime: 60 } }));
  }
  return games;
};

// A locked cell with an original block somewhere above it in the same column.
const hasCellUnderOverhang = (after, before, piece) => {
  const mark = piece.toUpperCase();
  for (let y = 0; y < after.length; y++) {
    for (let x = 0; x < 10; x++) {
      if (after[y][x] !== mark) continue;
      if (before[y]?.[x] !== ".") continue;
      for (let up = y + 1; up < after.length; up++) if (after[up][x] === "Z") return true;
    }
  }
  return false;
};

const holdGames = () => {
  const games = [];
  const seedGame = (name, seed, script, options, extra = {}) =>
    games.push(play({ name, seed, options, trace: true, ...compile(script), ...extra }));
  seedGame("first hold takes the next piece", 1, "H w2 HD w2");
  seedGame("second hold in a turn is ignored", 2, "H H H w2 HD");
  seedGame("hold swaps after a lock", 3, "H HD H HD H HD");
  seedGame("hold resets the piece", 4, "R R CW w3 H w3 H w3 HD");
  seedGame("hold while falling keeps gravity going", 5, "w70 H w70 HD", { g: 0.1 });
  seedGame("hold disabled", 6, "H w2 HD H HD", { display_hold: false });
  seedGame("180 disabled", 7, "180 w2 HD", { allow180: false });
  seedGame("hold after soft drop", 8, "SD H w3 SD H w3 HD");

  // Top out, with no injected state: stack the middle until the spawn is blocked.
  for (const seed of [11, 12, 13]) seedGame(`stack to top out ${seed}`, seed, "HD ".repeat(30));
  seedGame("hold blocked at the top", 14, "HD ".repeat(9) + "H H HD ".repeat(10));

  // Clutch: a clear lifts a blocked spawn out of the way. The tower starts above
  // the flat I's row, so the first piece spawns clear of it.
  const towerRow = "...XXXX...";
  const tower = [...["XXXXXXXXX."], ...Array(21).fill(".........."), ...Array(9).fill(towerRow)];
  const toWell = "R R R CW R HD";
  for (const clutch of [true, false]) {
    games.push(scripted(`spawn blocked after a clear, clutch ${clutch}`, "itt", toWell + " w5 HD w5", { board: tower, options: { clutch }, trace: true }));
    games.push(scripted(`spawn blocked with no clear, clutch ${clutch}`, "itt", "R R R HD w5", { board: tower, options: { clutch } }));
  }
  return games;
};

// Reproducible random input: mostly plain games, hitting what scripts don't.
const fuzzGames = () => {
  let state = 0x57ac1ab;
  const rand = () => {
    state = (state + 0x6d2b79f5) | 0;
    let t = Math.imul(state ^ (state >>> 15), 1 | state);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
  const pick = (...items) => items[Math.floor(rand() * items.length)];
  const keys = ["moveLeft", "moveRight", "softDrop", "hardDrop", "rotateCW", "rotateCCW", "rotate180", "hold"];
  const games = [];
  const seeds = [0, 1, 2147483646, 2147483647, 2147483648, 4294967295, 123456789, 741234845];
  for (let i = 0; i < 24; i++) {
    const seed = i < seeds.length ? seeds[i] : Math.floor(rand() * 2147483647) + 1;
    const options = {
      g: pick(0.02, 0.02, 0.1, 1, 25),
      locktime: pick(30, 30, 8),
      lockresets: pick(15, 15, 4),
      spinbonuses: pick("all-mini+", "all-mini+", "T-spins", "none"),
      allclear_garbage: pick(0, 5),
      allclear_b2b: pick(0, 1),
      garbagemultiplier: pick(1, 1, 1.37),
      roundmode: pick("down", "rng"),
      ...pick({}, CHARGING),
      handling: {
        das: pick(6, 7.2, 10, 0),
        arr: pick(0, 2, 1.5),
        dcd: pick(0, 2),
        sdf: pick(41, 6, 20),
        safelock: pick(true, false),
        cancel: pick(true, false),
        may20g: pick(true, false),
        irs: pick("tap", "hold"),
        ihs: pick("tap", "hold"),
      },
    };
    const inputs = [];
    const down = new Set();
    const frames = 900;
    for (let f = 0; f < frames; f++) {
      if (rand() > 0.35) continue;
      const key = keys[Math.floor(rand() * keys.length)];
      const subframe = Math.floor(rand() * 10) / 10;
      const last = inputs.at(-1);
      const at = last && last[0] === f ? Math.max(subframe, last[3]) : subframe;
      inputs.push([f, key, down.has(key) ? 0 : 1, at]);
      if (down.has(key)) down.delete(key);
      else down.add(key);
    }
    games.push(play({ name: `fuzz ${i}`, seed, options, inputs, frames }));
  }
  return games;
};

const write = (file, games) => {
  fs.mkdirSync(outDir, { recursive: true });
  const body = games.map((g) => "  " + JSON.stringify(g)).join(",\n");
  const text = `{"source":"${SOURCE}","games":[\n${body}\n]}\n`;
  fs.writeFileSync(path.join(outDir, file), text);
  console.log(`${file}: ${games.length} games, ${(text.length / 1024).toFixed(0)} KB`);
};

const replayToFixture = (source, out) => {
  const files = fs.statSync(source).isDirectory()
    ? fs.readdirSync(source).filter((f) => /\.ttrm?$/.test(f)).map((f) => path.join(source, f))
    : [source];
  const games = [];
  for (const file of files) {
    const replay = JSON.parse(fs.readFileSync(file));
    const rounds = replay.replay.rounds ? replay.replay.rounds.flat().map((p) => p.replay) : [replay.replay];
    rounds.forEach((r, i) => {
      if (r.events.some((e) => e.type === "ige" && e.data.type === "interaction")) return;
      const end = r.events.find((e) => e.type === "end");
      if (end?.frame === 0) return;
      const { username, ...options } = end?.data.options ?? r.options;
      const inputs = r.events
        .filter((e) => (e.type === "keydown" || e.type === "keyup") && KEY_NAMES.has(e.data.key))
        .map((e) => [e.frame, e.data.key, e.type === "keydown" ? 1 : 0, e.data.subframe, e.data.hoisted ? 1 : 0]);
      games.push(play({ name: `${path.basename(file)} player round ${i}`, seed: options.seed, options, inputs, frames: Math.max(r.frames ?? 0, end?.frame ?? 0) + 1 }));
    });
  }
  fs.writeFileSync(out, JSON.stringify({ source: SOURCE, games }));
  console.log(`${out}: ${games.length} games`);
};
const KEY_NAMES = new Set(Object.values(KEYS));

if (process.argv[3] === "--replay") {
  replayToFixture(process.argv[4], process.argv[5]);
} else {
  write("kicks.json", [...kickGames(), ...finGames()]);
  write("spins.json", spinGames());
  write("attack.json", attackGames());
  write("handling.json", handlingGames());
  write("lock.json", lockGames());
  write("hold_topout.json", holdGames());
  write("fuzz.json", fuzzGames());
}
