const express = require('express');
const userController = require('../controllers/userController');
const { authenticate } = require('../middleware/auth');
const { asyncHandler } = require('../utils/asyncHandler');

const router = express.Router();

router.get('/:id', authenticate, userController.getUser);
router.post('/', authenticate, asyncHandler(userController.createUser));
router.delete('/:id', authenticate, async (req, res) => {
  await userController.removeUser(req, res);
});

module.exports = router;
