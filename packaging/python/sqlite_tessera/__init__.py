import os

__version__ = "1.2.1"


def loadable_path():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "tessera")


def load(conn):
    conn.load_extension(loadable_path())
