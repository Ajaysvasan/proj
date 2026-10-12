const express = require('express');
const userRoutes = require('./routes/users');
const orderRoutes = require('./routes/orders');
const { requestLogger } = require('./middleware/logging');
const { errorHandler } = require('./middleware/errors');

const app = express();

app.use(express.json());
app.use(requestLogger);
app.use('/api/users', userRoutes);
app.use('/api/orders', orderRoutes);

app.get('/health', (req, res) => {
  res.json({ status: 'ok' });
});

app.use(errorHandler);

module.exports = app;
