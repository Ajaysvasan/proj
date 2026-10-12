const express = require('express');
const handlers = require('./controllers/userController');

const router = express.Router();
const base = '/dynamic';

// Path computed at runtime: recorded as an unresolved route pattern.
router.get(base + '/items', handlers.getUser);

// Method chosen at runtime: not recognized as a route at all.
for (const method of ['get', 'post']) {
  router[method]('/loop', handlers.getUser);
}

module.exports = router;
