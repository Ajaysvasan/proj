const { format } = require('./date');

function formatUser(user) {
  return { id: user.id, email: user.email, createdAt: format(user.createdAt) };
}

module.exports = { formatUser };
