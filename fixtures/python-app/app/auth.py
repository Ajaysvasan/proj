import functools


def login_required(view):
    @functools.wraps(view)
    def wrapper(*args, **kwargs):
        return view(*args, **kwargs)

    return wrapper
