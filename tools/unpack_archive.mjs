// Turns Triangle.js's .ttrmx League archive into .ttrm JSON that replay_parity reads.
//
//   npm install --ignore-scripts --prefix <dir> @haelp/teto@4.2.8
//   node tools/unpack_archive.mjs <dir> <folder of .ttrmx> <out folder>
//   replay_parity <out folder>
//
// Keeps round and player positions, but rounds with incoming garbage shrink to the
// one event that makes the runner skip them, because full JSON would take many GB.
// The output has no usernames but still holds players' inputs, so it stays in the
// gitignored replay-archive/.
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const [tetoDir, inDir, outDir] = process.argv.slice(2).map((p) => path.resolve(p));
const { pack } = createRequire(tetoDir + "/")("@haelp/teto/utils");

const GARBAGE_STUB = { events: [{ type: "ige", frame: 0, data: { type: "interaction" } }] };
const isGarbage = (e) => e.type === "ige" && e.data.type === "interaction";

// Keeps only what the runner reads, since other events and fields can name players.
const clean = (replay) => {
  const events = replay.events
    .filter((e) => e.type === "keydown" || e.type === "keyup" || e.type === "end")
    .map((e) => {
      if (e.type !== "end") return e;
      const { username, ...options } = e.data.options ?? {};
      return { type: e.type, frame: e.frame, data: { options, stats: e.data.stats, game: e.data.game } };
    });
  return { frames: replay.frames, events };
};

fs.mkdirSync(outDir, { recursive: true });
let files = 0, unreadable = 0, rounds = 0, kept = 0;
for (const name of fs.readdirSync(inDir).filter((f) => f.endsWith(".ttrmx")).sort()) {
  let top;
  try {
    top = pack.unpack(fs.readFileSync(path.join(inDir, name))).replay;
  } catch {
    unreadable++;
    continue;
  }
  const out = top.replay.rounds.map((players) =>
    players.map((p) => {
      rounds++;
      if (p.replay.events.some(isGarbage)) return { replay: GARBAGE_STUB };
      kept++;
      return { replay: clean(p.replay) };
    }),
  );
  fs.writeFileSync(path.join(outDir, name.replace(/x$/, "")), JSON.stringify({ ts: top.ts, version: top.version, replay: { rounds: out } }));
  files++;
}
console.log(`${files} files, ${unreadable} unreadable, ${rounds} player rounds, ${kept} without incoming garbage`);
