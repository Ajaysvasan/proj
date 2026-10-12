function validateEmail(email) {
  return typeof email === 'string' && email.includes('@');
}

module.exports = { validateEmail };
