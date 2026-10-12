import web

urls = (
    "/", "Index",
    "/items/(\\d+)", "Item",
)


class Index:
    def GET(self):
        return "index"


class Item:
    def GET(self, item_id):
        return item_id

    def POST(self, item_id):
        return "saved"


app = web.application(urls, globals())
