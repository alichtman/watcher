const assert = require('assert');
const {execFileSync, spawnSync} = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const moduleRoot = path.resolve(__dirname, '..');

const describeLinux =
  process.platform === 'linux' && !process.env.TEST_WASM
    ? describe
    : describe.skip;

// Bind mounts are the only way to build a directory cycle without symlinks,
// and they need a private mount namespace to stay contained.
function isolatedMountFailure() {
  const result = spawnSync('unshare', ['-Urm', 'true'], {
    encoding: 'utf8',
    timeout: 5000,
  });
  if (result.status === 0) return null;

  if (result.error) return result.error.message;
  return (result.stderr || `exited with status ${result.status}`).trim();
}

function runFixture(name, command, args) {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'watcher-deep-'));
  const snapshotPath = `${tmpDir}.txt`;

  try {
    const result = spawnSync(
      command,
      [
        ...args,
        path.join(__dirname, 'fixtures', name),
        tmpDir,
        snapshotPath,
        moduleRoot,
      ],
      {encoding: 'utf8', timeout: 60000},
    );

    const output = `${result.stdout || ''}${result.stderr || ''}`;
    assert.ifError(result.error);
    assert.equal(result.signal, null, output);
    assert.equal(result.status, 0, output);
    assert(result.stdout.includes('ok'), output);
  } finally {
    execFileSync('rm', ['-rf', tmpDir, snapshotPath]);
  }
}

describeLinux('deep directory traversal', () => {
  it('skips paths beyond PATH_MAX instead of matching ignore globs against them', function () {
    this.timeout(60000);

    runFixture('deep-directory-child.js', process.execPath, []);
  });

  it('stops at directory cycles without descending forever', function () {
    const unavailableReason = isolatedMountFailure();
    if (unavailableReason) {
      const message = `Directory cycle test requires an isolated mount fixture: ${unavailableReason}`;
      if (process.env.REQUIRE_DIRECTORY_CYCLE_TEST === '1') {
        assert.fail(message);
      }

      console.warn(message);
      this.skip();
    }

    this.timeout(60000);

    runFixture('directory-cycle-child.js', 'unshare', [
      '-Urm',
      process.execPath,
    ]);
  });
});
