const { Pool } = require('pg');

const pool = new Pool();

async function query(sql, params) {
  return pool.query(sql, params);
}

module.exports = { query };
