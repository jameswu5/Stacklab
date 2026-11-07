#!/usr/bin/env python3
"""Runs replay_parity over the .ttrmx archive without unpacking all of it to disk.

    archive_parity.py <replay_parity> <replays.tar.gz> <temp folder> <output file>

Streams the tarball and runs a batch of files at a time, deleting each batch after.
The output holds replay ids, so keep it out of the repository.
"""
import re
import shutil
import subprocess
import sys
import tarfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from threading import BoundedSemaphore, Lock

BATCH_FILES = 60
WORKERS = 6

runner, archive, temp, output = sys.argv[1:5]
totals = [0, 0, 0, 0, 0]
lock = Lock()
slots = BoundedSemaphore(WORKERS)
out = open(output, "w", errors="replace")


def run(folder):
    result = subprocess.run([runner, str(folder)], capture_output=True, text=True, errors="replace")
    lines = result.stdout.splitlines()
    summary = re.search(r": (\d+) passed, (\d+) failed, (\d+) skipped.*?, (\d+) skipped.*?, (\d+) sends", lines[-1]) if lines else None
    with lock:
        out.write("\n".join(line for line in lines if not line.startswith("PASS")) + "\n")
        if result.returncode not in (0, 1) or not summary:
            out.write(f"CRASH {folder.name}: exit {result.returncode}\n{result.stderr[-500:]}\n")
            totals[1] += 1
        else:
            for i in range(5):
                totals[i] += int(summary.group(i + 1))
        out.flush()
    shutil.rmtree(folder)
    slots.release()


Path(temp).mkdir(parents=True, exist_ok=True)
with ThreadPoolExecutor(WORKERS) as pool, tarfile.open(archive, "r|gz") as tar:
    batch, count, batches = None, 0, 0
    for member in tar:
        if not member.name.endswith(".ttrmx") or not member.isfile():
            continue
        if batch is None:
            slots.acquire()
            batch = Path(temp) / f"batch{batches}"
            batch.mkdir()
            batches += 1
        (batch / Path(member.name).name).write_bytes(tar.extractfile(member).read())
        count += 1
        if count % BATCH_FILES == 0:
            pool.submit(run, batch)
            batch = None
    if batch is not None:
        pool.submit(run, batch)

print(f"{count} files: {totals[0]} passed, {totals[1]} failed, {totals[2]} skipped with garbage stripped, {totals[3]} skipped as never started, {totals[4]} sends compared")
