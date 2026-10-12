const app = require('./app');
const { loadConfig } = require('./config');

const config = loadConfig();

app.listen(config.port, () => {
  console.log(`listening on ${config.port}`);
});
