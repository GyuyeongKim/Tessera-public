import argparse
import base64
import hashlib
import os
import re
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def version():
    src = open(os.path.join(ROOT, "core", "tessera.hpp")).read()
    v = re.search(r'kVersion = "([^"]+)"', src).group(1)
    init = open(os.path.join(HERE, "sqlite_tessera", "__init__.py")).read()
    if '__version__ = "%s"' % v not in init:
        raise SystemExit("sqlite_tessera/__init__.py does not say version %s" % v)
    return v


def metadata(v):
    body = open(os.path.join(HERE, "README.md")).read()
    head = [
        "Metadata-Version: 2.1",
        "Name: sqlite-tessera",
        "Version: " + v,
        "Summary: Fast join-aggregates over skewed sparse relations as a SQLite extension",
        "Author: Gyuyeong Kim",
        "License: Apache-2.0",
        "Project-URL: Homepage, https://github.com/GyuyeongKim/Tessera-public",
        "Project-URL: Source, https://github.com/GyuyeongKim/Tessera-public",
        "Keywords: sqlite,sqlite-extension,join,aggregation,similarity",
        "Classifier: License :: OSI Approved :: Apache Software License",
        "Classifier: Programming Language :: Python :: 3",
        "Classifier: Topic :: Database",
        "Requires-Python: >=3.8",
        "Description-Content-Type: text/markdown",
    ]
    return "\n".join(head) + "\n\n" + body


def digest(data):
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True)
    ap.add_argument("--plat", required=True)
    ap.add_argument("--out", default="dist")
    a = ap.parse_args()
    v = version()
    ext = os.path.splitext(a.lib)[1]
    info = "sqlite_tessera-%s.dist-info" % v
    files = [
        ("sqlite_tessera/__init__.py", open(os.path.join(HERE, "sqlite_tessera", "__init__.py"), "rb").read()),
        ("sqlite_tessera/tessera" + ext, open(a.lib, "rb").read()),
        (info + "/METADATA", metadata(v).encode()),
        (info + "/WHEEL", ("Wheel-Version: 1.0\nGenerator: sqlite-tessera build_wheel.py\nRoot-Is-Purelib: false\n"
                           "Tag: py3-none-%s\n" % a.plat).encode()),
        (info + "/LICENSE", open(os.path.join(ROOT, "LICENSE"), "rb").read()),
        (info + "/NOTICE", open(os.path.join(ROOT, "NOTICE"), "rb").read()),
    ]
    record = "".join("%s,%s,%d\n" % (n, digest(d), len(d)) for n, d in files) + info + "/RECORD,,\n"
    files.append((info + "/RECORD", record.encode()))
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, "sqlite_tessera-%s-py3-none-%s.whl" % (v, a.plat))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for n, d in files:
            zi = zipfile.ZipInfo(n, date_time=(2026, 1, 1, 0, 0, 0))
            zi.external_attr = (0o755 if n.endswith(ext) else 0o644) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(zi, d)
    print(path)


if __name__ == "__main__":
    main()
