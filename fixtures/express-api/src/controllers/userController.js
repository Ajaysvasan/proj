const userService = require('../services/userService');
const { formatUser } = require('../utils/format');

async function getUser(req, res, next) {
  try {
    const user = await userService.findById(req.params.id);
    res.json(formatUser(user));
  } catch (err) {
    next(err);
  }
}

async function createUser(req, res) {
  const user = await userService.create(req.body);
  res.status(201).json(formatUser(user));
}

async function removeUser(req, res) {
  await userService.remove(req.params.id);
  res.status(204).end();
}

module.exports = { getUser, createUser, removeUser };
