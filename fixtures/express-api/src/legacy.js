const missing = require('./does-not-exist');

// Same name as the controller's getUser, in a different module.
function getUser(id) {
  return missing.lookup(id);
}

class LegacySync {
  constructor(store) {
    this.store = store;
  }

  // The field's type is unknown, so this call cannot be resolved exactly:
  // two project methods are named findById.
  sync() {
    return this.store.findById(1);
  }
}

module.exports = { getUser, LegacySync };
