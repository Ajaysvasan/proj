import sqlite3

_connection = None


def query(sql, *params):
    return _cursor().execute(sql, params).fetchall()


def _cursor():
    global _connection
    if _connection is None:
        _connection = sqlite3.connect(":memory:")
    return _connection.cursor()
