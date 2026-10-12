from flask import Blueprint, abort, jsonify, request

from app.auth import login_required
from app.errors import UserNotFound
from app.services.user_service import UserService

bp = Blueprint("users", __name__)
service = UserService()


@bp.route("/<int:user_id>", methods=["GET"])
@login_required
def get_user(user_id):
    try:
        user = service.get_user(user_id)
    except UserNotFound:
        abort(404)
    return jsonify(user)


@bp.route("/", methods=["POST"])
def create_user():
    payload = request.get_json()
    return jsonify(service.create_user(payload)), 201
