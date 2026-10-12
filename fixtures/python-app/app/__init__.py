from flask import Flask

from .routes.users import bp as users_bp


def create_app():
    app = Flask(__name__)
    app.register_blueprint(users_bp, url_prefix="/api/users")
    return app
