class UserStore:
    def load(self, user_id):
        return {"id": user_id}

    def save(self, user_id, body):
        return True
