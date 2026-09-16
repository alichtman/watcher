const fs = require('fs');
const path = require('path');

const watchDir = process.argv[2];
const snapshotPath = process.argv[3];
const moduleRoot = process.argv[4];
const watcher = require(moduleRoot);

const SEGMENT = 'directory-component-0123456789';

// Build a tree far deeper than PATH_MAX. The path is never materialized as a
// string here either, so the depth is limited only by how many descriptors the
// loop walks through.
function createDeepTree() {
  const openFlags = fs.constants.O_RDONLY | fs.constants.O_DIRECTORY;
  let parent = fs.openSync(watchDir, openFlags);

  try {
    for (let i = 0; i < 2000; i++) {
      const child = `/proc/self/fd/${parent}/${SEGMENT}`;
      fs.mkdirSync(child);
      const next = fs.openSync(child, openFlags);
      fs.closeSync(parent);
      parent = next;
    }
  } finally {
    fs.closeSync(parent);
  }
}

async function main() {
  createDeepTree();
  fs.writeFileSync(path.join(watchDir, 'shallow.txt'), 'shallow');

  // Previously this recursed into std::regex with a multi-kilobyte path and
  // died with SIGSEGV.
  const subscription = await watcher.subscribe(watchDir, () => {}, {
    backend: 'inotify',
    ignore: ['**/ignored/**'],
  });
  await subscription.unsubscribe();

  // The brute force backend has no PATH_MAX constraint of its own, so an
  // overlong path must not fail the whole snapshot.
  await watcher.writeSnapshot(watchDir, snapshotPath, {backend: 'brute-force'});

  const snapshot = fs.readFileSync(snapshotPath, 'utf8');
  if (!snapshot.includes(path.join(watchDir, 'shallow.txt'))) {
    throw new Error('Expected entries within PATH_MAX to be recorded');
  }

  // Every recorded path must be short enough for the OS to reopen.
  for (const entry of snapshot.split('\n')) {
    const offset = entry.indexOf(watchDir);
    if (offset === -1) continue;

    const size = Number(entry.slice(0, offset));
    if (size >= 4096) {
      throw new Error(
        `Expected paths beyond PATH_MAX to be skipped, got ${size}`,
      );
    }
  }

  process.stdout.write('ok\n');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
