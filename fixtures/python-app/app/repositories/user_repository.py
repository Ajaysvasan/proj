from app import db


class UserRepository:
    def find(self, user_id):
        return db.query("SELECT * FROM users WHERE id = %s", user_id)

    def insert(self, payload):
        return db.query("INSERT INTO users (email) VALUES (%s)", payload["email"])
