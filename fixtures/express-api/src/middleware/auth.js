const { verifyToken } = require('../utils/token');

function authenticate(req, res, next) {
  const header = req.headers.authorization;
  if (!header) {
    return res.status(401).json({ error: 'missing token' });
  }
  req.user = verifyToken(header.replace('Bearer ', ''));
  next();
}

module.exports = { authenticate };
