const { AppError } = require('../errors');

function errorHandler(err, req, res, next) {
  const status = err instanceof AppError ? err.status : 500;
  res.status(status).json({ error: err.message });
}

module.exports = { errorHandler };
