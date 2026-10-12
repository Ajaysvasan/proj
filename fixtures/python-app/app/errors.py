class AppError(Exception):
    status = 500


class UserNotFound(AppError):
    status = 404


class ValidationError(AppError):
    status = 400
