const { format } = require('../utils/text');

function requestLogger(req, res, next) {
  console.log(format(`${req.method} ${req.url}`));
  next();
}

module.exports = { requestLogger };
