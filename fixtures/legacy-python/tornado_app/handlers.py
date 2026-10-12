import tornado.web

from tornado_app.store import UserStore


class BaseHandler(tornado.web.RequestHandler):
    def post(self, *args):
        self.set_status(405)


class MainHandler(tornado.web.RequestHandler):
    def get(self):
        self.write("hello")


class UserHandler(BaseHandler):
    def get(self, user_id):
        self.write(UserStore().load(user_id))

    def put(self, user_id):
        UserStore().save(user_id, self.request.body)


class OrderHandler(tornado.web.RequestHandler):
    def get(self, order_id):
        self.write(order_id)

    def delete(self, order_id):
        self.set_status(204)
