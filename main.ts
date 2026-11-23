require('node:v8').setFlagsFromString('--no-incremental-marking --no-turbofan --always-sparkplug');
const { readFileSync, writeFileSync, openSync, closeSync, writeSync } = require('node:fs');
const { run } = require('./solve.js');
const input = (process.env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const output = (process.env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';

let source = readFileSync(input, 'latin1');
const latin1 = !source.includes('\\u') || !/\\u(?!00[0-7][0-9a-f])[0-9a-f]{4}/i.test(source);
if (!latin1) source = Buffer.from(source, 'latin1').toString('utf8');
const cases = JSON.parse(source);
const options = { json: true, latin1 };
const fd = openSync(output, 'w');
try {
  let separator = '[';
  for (const { input: site } of cases) {
    const answer = run(site, options);
    writeSync(fd, separator);
    if (latin1) {
      const written = writeSync(fd, answer, null, 'latin1');
      if (written < answer.length) writeFileSync(fd, Buffer.from(answer, 'latin1').subarray(written));
    } else {
      writeFileSync(fd, answer, 'utf8');
    }
    separator = ',';
  }
  writeSync(fd, cases.length ? ']' : '[]');
} finally {
  closeSync(fd);
}
