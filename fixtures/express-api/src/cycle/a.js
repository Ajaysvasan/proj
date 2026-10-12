const b = require('./b');

function ping(n) {
  return n <= 0 ? 0 : b.pong(n - 1);
}

module.exports = { ping };
