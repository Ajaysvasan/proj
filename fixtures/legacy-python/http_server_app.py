from http.server import BaseHTTPRequestHandler, HTTPServer

from api.pets import get_pet, list_pets


class PetHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/pets":
            self.respond(list_pets())
        elif self.path.startswith("/pets/"):
            self.respond(get_pet(self.path.rsplit("/", 1)[-1]))
        else:
            self.send_error(404)

    def do_POST(self):
        match self.path:
            case "/pets":
                self.create_pet()
            case "/pets/import":
                self.import_pets()

    def respond(self, body):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(str(body).encode())

    def create_pet(self):
        self.respond("created")

    def import_pets(self):
        self.respond("imported")


def app(environ, start_response):
    start_response("200 OK", [])
    return [b"ok"]


if __name__ == "__main__":
    HTTPServer(("", 8000), PetHandler).serve_forever()
