const jwt = require('jsonwebtoken');

function verifyToken(token) {
  try {
    return jwt.verify(token, process.env.SECRET);
  } catch (e) {
    throw new Error('invalid token');
  }
}

module.exports = { verifyToken };
