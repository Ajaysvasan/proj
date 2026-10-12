import falcon


class ThingsResource:
    def on_get(self, req, resp):
        resp.media = []

    def on_post(self, req, resp):
        resp.status = falcon.HTTP_201


class HealthResource:
    def on_get(self, req, resp):
        resp.media = {"ok": True}


app = falcon.App()
things = ThingsResource()
app.add_route("/things", things)
app.add_route("/health", HealthResource())
