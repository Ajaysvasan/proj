const db = require('../db');

class UserRepository {
  async findById(id) {
    return db.query('SELECT * FROM users WHERE id = $1', [id]);
  }

  async insert(data) {
    return db.query('INSERT INTO users (email) VALUES ($1)', [data.email]);
  }

  async delete(id) {
    return db.query('DELETE FROM users WHERE id = $1', [id]);
  }
}

module.exports = { UserRepository };
