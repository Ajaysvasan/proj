import tornado.ioloop
import tornado.web

from tornado_app.handlers import MainHandler, OrderHandler, UserHandler

handlers = [
    (r"/", MainHandler),
    (r"/users/([0-9]+)", UserHandler),
    tornado.web.url(r"/orders/(?P<order_id>\d+)/?", OrderHandler, name="order"),
]


def make_app():
    return tornado.web.Application(handlers)


if __name__ == "__main__":
    app = make_app()
    app.listen(8888)
    tornado.ioloop.IOLoop.current().start()
