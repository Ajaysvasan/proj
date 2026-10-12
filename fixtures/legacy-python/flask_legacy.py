from flask import Flask
from flask.views import MethodView

app = Flask(__name__)


class UserAPI(MethodView):
    def get(self, user_id):
        return {"id": user_id}

    def post(self):
        return {"created": True}


def ping():
    return "pong"


app.add_url_rule("/users/<int:user_id>", view_func=UserAPI.as_view("users"))
app.add_url_rule("/ping", "ping", ping)
