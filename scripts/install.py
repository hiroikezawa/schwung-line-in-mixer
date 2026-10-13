#!/usr/bin/env python3
"""Install Line In Mixer on a Move through Schwung Manager's custom-module form.

    scripts/install.py [URL] [--check]

URL defaults to http://move.local:7700. Uploads dist/line-in-mixer-module.tar.gz
(build with scripts/build.sh arm64) to POST /modules/install-custom, exactly
like the Manager's own "Custom install" upload, which unpacks it into
modules/audio_fx/line-in-mixer/.

SPDX-License-Identifier: MIT
Copyright (c) 2026 Hiro Ikezawa
"""
import argparse
import http.cookiejar
import io
import json
import pathlib
import tarfile
import urllib.parse
import urllib.request
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent


def session(base):
    parsed = urllib.parse.urlparse(base)
    if parsed.scheme not in ("http", "https") or not parsed.netloc or parsed.path not in ("", "/"):
        raise ValueError("Use a Manager base URL, e.g. http://move.local:7700")
    jar = http.cookiejar.CookieJar()
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                        urllib.request.HTTPCookieProcessor(jar))
    with opener.open(base.rstrip("/") + "/modules", timeout=10) as response:
        response.read()
    token = next((c.value for c in jar if c.name == "csrf_token"), None)
    if not token:
        raise RuntimeError("Manager did not supply its csrf_token cookie")
    return opener, token


def check_package(module_id, payload):
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
        for member in archive.getmembers():
            path = pathlib.PurePosixPath(member.name)
            if not path.parts or path.parts[0] != module_id or ".." in path.parts or member.issym() or member.islnk():
                raise ValueError(f"package must contain only {module_id}/")
        dsp = archive.extractfile(f"{module_id}/{module_id}.so").read()
        if dsp[:6] != b"\x7fELF\x02\x01" or int.from_bytes(dsp[18:20], "little") != 183:
            raise ValueError("refusing a non-ARM64 .so (that is the desktop test build)")
        manifest = json.load(archive.extractfile(f"{module_id}/module.json"))
        if manifest.get("id") != module_id or manifest.get("component_type") != "audio_fx":
            raise ValueError("module.json does not describe this audio FX")


def install(opener, token, base, module_id, payload):
    boundary = "line-in-mixer-" + uuid.uuid4().hex
    parts = []
    for name, value in [("csrf_token", token), ("source", "tarball")]:
        parts.append((f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n").encode())
    parts.append((f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
                  f"filename=\"{module_id}-module.tar.gz\"\r\nContent-Type: application/gzip\r\n\r\n").encode())
    parts.extend([payload, f"\r\n--{boundary}--\r\n".encode()])
    request = urllib.request.Request(base.rstrip("/") + "/modules/install-custom", data=b"".join(parts),
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary, "X-CSRF-Token": token})
    with opener.open(request, timeout=60) as response:
        response.read()
        flash = urllib.parse.parse_qs(urllib.parse.urlparse(response.url).query).get("flash", [""])[0]
    # Newer Managers append a hint ("- restart Move to run the new version ...").
    if not flash.startswith(f"Installed {module_id} from tarball"):
        raise RuntimeError(f"Manager did not confirm installing {module_id}: {flash!r}")
    print(flash)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("url", nargs="?", default="http://move.local:7700")
    parser.add_argument("--check", action="store_true", help="check the connection only; upload nothing")
    args = parser.parse_args()
    try:
        module_id = json.loads((ROOT / "module.json").read_text())["id"]
        package = ROOT / "dist" / f"{module_id}-module.tar.gz"
        if not package.exists():
            raise FileNotFoundError(f"{package} missing: run scripts/build.sh arm64 first")
        payload = package.read_bytes()
        check_package(module_id, payload)
        opener, token = session(args.url)
        if args.check:
            print("Connected to Schwung Manager:", args.url, "- package OK:", module_id)
            return
        install(opener, token, args.url, module_id, payload)
    except Exception as exc:  # report, never a traceback
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
