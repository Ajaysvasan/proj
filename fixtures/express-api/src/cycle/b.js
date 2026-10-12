const a = require('./a');

function pong(n) {
  return n <= 0 ? 0 : a.ping(n - 1);
}

module.exports = { pong };
