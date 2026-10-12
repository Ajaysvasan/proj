const { UserRepository } = require('../repositories/userRepository');
const { NotFoundError, ValidationError } = require('../errors');
const { validateEmail } = require('../utils/validation');

class UserService {
  constructor() {
    this.repository = new UserRepository();
  }

  async findById(id) {
    const user = await this.repository.findById(id);
    if (!user) {
      throw new NotFoundError(`user ${id} not found`);
    }
    return user;
  }

  async create(data) {
    if (!validateEmail(data.email)) {
      throw new ValidationError('invalid email');
    }
    return this.repository.insert(data);
  }

  async remove(id) {
    await this.findById(id);
    return this.repository.delete(id);
  }
}

module.exports = new UserService();
