# [houdini] The only hython entry point (plan §3.1, §3.3).
#
# generate.mjs launches `hython run_generator.py` inside a kill-on-close Job
# Object with the working directory set to the stage, and sends one JSON job
# on stdin. Nothing here trusts that job: the registry file must hash to the
# value Node saw, every field is checked again, and the builder must sit
# inside generators/, hash to its pinned value, and run from the same bytes
# that were hashed (no window between check and use).
#
# The checks import nothing from Houdini, so a plain Python runs them in the
# unit tests (job-rules.test.mjs); `hou` is imported in main() only. Keep this
# file Python 3.9-compatible: that is hython 19.5's interpreter.
import hashlib
import json
import math
import os
import re
import sys

SCHEMA_VERSION = 1
ID = re.compile(r"^[a-z0-9-]{1,64}$")
SEED = re.compile(r"^[0-9]{1,10}$")
TIERS = ("preview", "validate", "final")
MAX_SEED = 2 ** 31 - 1
MAX_JOB_BYTES = 65536
HERE = os.path.dirname(os.path.abspath(__file__))
HOUDINI_DIR = os.path.dirname(HERE)
GENERATORS_DIR = os.path.join(HOUDINI_DIR, "generators")
REGISTRY_PATH = os.path.join(HOUDINI_DIR, "registry.json")
FRAME_NAME = re.compile(r"^frames/frame_[0-9]{4}\.exr$")


class Rejected(Exception):
    pass


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise Rejected(message)


def exact_keys(value, keys, what):
    require(isinstance(value, dict), "%s is not an object" % what)
    extra = sorted(set(value) - set(keys))
    require(not extra, "%s has unknown fields: %s" % (what, ", ".join(extra)))
    missing = sorted(set(keys) - set(value))
    require(not missing, "%s is missing fields: %s" % (what, ", ".join(missing)))


def check_params(params, schema):
    require(isinstance(params, dict), "params is not an object")
    extra = sorted(set(params) - set(schema))
    require(not extra, "unknown parameters: %s" % ", ".join(extra))
    checked = {}
    for name in sorted(schema):
        rule = schema[name]
        require(name in params, "parameter %s is missing" % name)
        value = params[name]
        kind = rule["type"]
        if kind in ("float", "int"):
            require(not isinstance(value, bool) and isinstance(value, (int, float)), "parameter %s is not a number" % name)
            require(math.isfinite(value), "parameter %s is not finite" % name)
            require(rule["min"] <= value <= rule["max"], "parameter %s is out of range" % name)
            if kind == "int":
                require(float(value).is_integer(), "parameter %s is not an integer" % name)
                value = int(value)
            else:
                value = float(value)
        elif kind == "enum":
            require(isinstance(value, str) and value in rule["values"], "parameter %s is not one of its values" % name)
        else:
            raise Rejected("parameter %s has an unknown type" % name)
        checked[name] = value
    return checked


def inside(base, path):
    base = os.path.normcase(os.path.realpath(base))
    path = os.path.normcase(os.path.realpath(path))
    return os.path.commonpath([base, path]) == base and path != base


def check_job(job, registry_bytes):
    """Validate a job against the registry bytes; return (entry, checked job)."""
    exact_keys(job, ("schemaVersion", "id", "params", "seed", "tier", "threads", "registrySha256"), "job")
    require(job["schemaVersion"] == SCHEMA_VERSION, "unsupported job schema")
    require(isinstance(job["registrySha256"], str) and job["registrySha256"] == sha256(registry_bytes),
            "the registry changed since the job was checked")
    registry = json.loads(registry_bytes.decode("utf-8"))
    generators = registry.get("generators")
    require(isinstance(generators, dict), "registry has no generators")
    require(isinstance(job["id"], str) and ID.match(job["id"]) is not None, "generator id is malformed")
    require(job["id"] in generators, "unknown generator")
    entry = generators[job["id"]]
    seed = job["seed"]
    require(isinstance(seed, int) and not isinstance(seed, bool) and 0 <= seed <= MAX_SEED, "seed is out of range")
    require(job["tier"] in TIERS and job["tier"] in entry["tiers"], "tier is not one of preview, validate, final")
    threads = job["threads"]
    require(isinstance(threads, int) and not isinstance(threads, bool) and 1 <= threads <= 64, "threads is out of range")
    params = check_params(job["params"], entry["params"])
    return entry, {"id": job["id"], "params": params, "seed": seed, "tier": job["tier"], "threads": threads}


def load_builder(entry, generators_dir=GENERATORS_DIR):
    """Read the builder once, confine and hash it, and return its namespace."""
    relative = entry["builder"]
    require(isinstance(relative, str) and re.match(r"^generators/[a-z0-9_]{1,64}\.py$", relative) is not None,
            "builder path is malformed")
    path = os.path.join(generators_dir, relative.split("/", 1)[1])
    require(inside(generators_dir, path), "builder escapes generators/")
    with open(path, "rb") as handle:
        data = handle.read()
    require(sha256(data) == entry["builderSha256"], "builder does not match its pinned hash")
    namespace = {"__name__": "pe_builder_" + entry["builder"].split("/")[1][:-3]}
    exec(compile(data, relative, "exec"), namespace)
    require(callable(namespace.get("generate")), "builder has no generate()")
    return namespace


def frame_manifest(names, count, workdir):
    """Check the builder's frame list and hash each frame."""
    require(isinstance(names, list) and len(names) == count, "builder returned %d frames, not %d" % (len(names or []), count))
    frames = []
    for index, name in enumerate(names):
        require(isinstance(name, str) and FRAME_NAME.match(name) is not None, "frame name is malformed: %r" % (name,))
        path = os.path.join(workdir, *name.split("/"))
        require(inside(workdir, path) and os.path.isfile(path), "frame is missing: %s" % name)
        with open(path, "rb") as handle:
            frames.append({"index": index, "name": name, "sha256": sha256(handle.read())})
    return {"schemaVersion": 1, "frames": frames}


def reject(error):
    print("[houdini-run] " + json.dumps({"ok": False, "rejected": str(error)}))
    sys.stdout.flush()
    sys.exit(2)


def main():
    raw = sys.stdin.buffer.read(MAX_JOB_BYTES + 1)
    try:
        require(len(raw) <= MAX_JOB_BYTES, "job is too large")
        with open(REGISTRY_PATH, "rb") as handle:
            registry_bytes = handle.read()
        entry, job = check_job(json.loads(raw.decode("utf-8")), registry_bytes)
    except (Rejected, ValueError) as error:
        reject(error)
    # Only the pin checks are rejections; an error the builder raises while
    # loading is a generator failure (exit 1).
    try:
        builder = load_builder(entry)
    except Rejected as error:
        reject(error)
    import hou  # noqa: E402 -- only inside hython

    grid = entry["grid"]
    ctx = {"params": job["params"], "seed": job["seed"], "tier": dict(entry["tiers"][job["tier"]]),
           "tierName": job["tier"], "grid": grid, "threads": job["threads"]}
    workdir = os.getcwd()
    names = builder["generate"](hou, ctx)
    manifest = frame_manifest(names, grid * grid, workdir)
    with open(os.path.join(workdir, "frames.manifest.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print("[houdini-run] " + json.dumps({"ok": True, "frames": len(manifest["frames"])}))
    sys.stdout.flush()


if __name__ == "__main__":
    main()
