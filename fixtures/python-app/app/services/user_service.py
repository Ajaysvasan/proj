from app.errors import UserNotFound, ValidationError
from app.repositories.user_repository import UserRepository


class UserService:
    def __init__(self):
        self.repo = UserRepository()

    def get_user(self, user_id):
        user = self.repo.find(user_id)
        if user is None:
            raise UserNotFound(user_id)
        return user

    def create_user(self, payload):
        if "email" not in payload:
            raise ValidationError("email is required")
        return self.repo.insert(payload)
