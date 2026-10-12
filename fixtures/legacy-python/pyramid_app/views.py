from pyramid.view import view_config


def home(request):
    return {"page": "home"}


@view_config(route_name="user", request_method="GET", renderer="json")
def user_view(request):
    return {"id": request.matchdict["id"]}
