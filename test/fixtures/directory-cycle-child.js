// Runs inside `unshare -Urm`, so the bind mounts below are private to this
// process tree and disappear when it exits.
const {execFileSync} = require('child_process');
const fs = require('fs');
const path = require('path');

const tmpDir = process.argv[2];
const snapshotPath = process.argv[3];
const moduleRoot = process.argv[4];
const watcher = require(moduleRoot);

const bind = (source, target) =>
  execFileSync('mount', ['--bind', source, target]);

// cycle/self resolves to cycle itself, so cycle/self/self/... never bottoms
// out. Both directories report the same (dev, ino), which is the only signal
// available: a bind mount is not a symlink, so O_NOFOLLOW does not help.
function createCycle() {
  const root = path.join(tmpDir, 'cycle');
  fs.mkdirSync(path.join(root, 'self'), {recursive: true});
  fs.writeFileSync(path.join(root, 'marker.txt'), 'cycle');
  bind(root, path.join(root, 'self'));
}

// Two sibling bind mounts of one directory share a (dev, ino) without forming
// a cycle, so both must still be traversed.
function createAliases() {
  const shared = path.join(tmpDir, 'shared');
  fs.mkdirSync(shared, {recursive: true});
  fs.writeFileSync(path.join(shared, 'marker.txt'), 'shared');

  for (const name of ['a', 'b']) {
    const target = path.join(tmpDir, 'aliases', name);
    fs.mkdirSync(target, {recursive: true});
    bind(shared, target);
  }
}

async function main() {
  createCycle();
  createAliases();

  await watcher.writeSnapshot(tmpDir, snapshotPath, {backend: 'brute-force'});
  const snapshot = fs.readFileSync(snapshotPath, 'utf8');

  // The aliases are siblings rather than ancestors of each other, so sharing
  // an identity must not stop either from being traversed.
  const expected = [
    path.join(tmpDir, 'cycle', 'marker.txt'),
    path.join(tmpDir, 'shared', 'marker.txt'),
    path.join(tmpDir, 'aliases', 'a', 'marker.txt'),
    path.join(tmpDir, 'aliases', 'b', 'marker.txt'),
  ];

  for (const entry of expected) {
    if (!snapshot.includes(entry)) {
      throw new Error(`Expected ${entry} in the snapshot`);
    }
  }

  const cycled = path.join(tmpDir, 'cycle', 'self') + path.sep;
  if (snapshot.includes(cycled)) {
    throw new Error(`Expected traversal to stop at the cycle, found ${cycled}`);
  }

  process.stdout.write('ok\n');
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
