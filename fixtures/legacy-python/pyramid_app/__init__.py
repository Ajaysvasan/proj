from pyramid.config import Configurator


def main(global_config, **settings):
    config = Configurator(settings=settings)
    config.add_route("home", "/")
    config.add_route("user", "/users/{id}")
    config.add_view("pyramid_app.views.home", route_name="home")
    config.scan()
    return config.make_wsgi_app()
