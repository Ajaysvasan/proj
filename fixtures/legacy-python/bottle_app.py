from bottle import Bottle, route, run


@route("/hello/<name>")
def hello(name):
    return "hello " + name


app = Bottle()


@app.route("/submit", method="POST")
def submit():
    return "ok"


if __name__ == "__main__":
    run(host="localhost", port=8080)
