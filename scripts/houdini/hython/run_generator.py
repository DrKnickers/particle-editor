# [houdini] The only hython entry point (plan §3.1, §3.3).
#
# generate.mjs launches `hython run_generator.py` inside a kill-on-close Job
# Object with the working directory set to the stage, and sends one JSON job
# on stdin. Nothing here trusts that job: the registry file must hash to the
# value Node saw, every field is checked again, and the builder must sit
# inside generators/, hash to its pinned value, and run from the same bytes
# that were hashed (no window between check and use). So must each shared
# module the entry pins; the builder gets them as ctx["modules"].
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
# Entries kept off the public mirror, beside the main registry (rules.mjs readRegistry).
PRIVATE_REGISTRY = "registry-private.json"
FRAME_NAME = re.compile(r"^frames/frame_[0-9]{4}\.exr$")
MODULE = re.compile(r"^[a-z0-9_]{1,64}$")
# The guard band profiles.mjs leaves round every frame (its PAD).
PAD = 2


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


def read_registry_files(houdini_dir):
    """The main registry's bytes, and the private one's or None when it is absent."""
    with open(os.path.join(houdini_dir, "registry.json"), "rb") as handle:
        main = handle.read()
    try:
        with open(os.path.join(houdini_dir, PRIVATE_REGISTRY), "rb") as handle:
            extra = handle.read()
    except FileNotFoundError:
        extra = None
    return main, extra


def registry_digest(main_bytes, private_bytes):
    """The main file's hash alone, or the hash of both files' hashes (as readRegistry)."""
    if private_bytes is None:
        return sha256(main_bytes)
    return sha256((sha256(main_bytes) + sha256(private_bytes)).encode("ascii"))


def merge_registry(main_bytes, private_bytes):
    """Both files' generators in one dict; an id may live in one file only."""
    generators = {}
    for data in (main_bytes, private_bytes):
        if data is None:
            continue
        more = json.loads(data.decode("utf-8")).get("generators")
        require(isinstance(more, dict), "registry has no generators")
        twice = sorted(set(generators) & set(more))
        require(not twice, "generators in both registry files: %s" % ", ".join(twice))
        generators.update(more)
    return generators


def check_job(job, registry_bytes, private_bytes=None):
    """Validate a job against the registry bytes; return (entry, checked job)."""
    exact_keys(job, ("schemaVersion", "id", "params", "seed", "tier", "threads", "registrySha256"), "job")
    require(job["schemaVersion"] == SCHEMA_VERSION, "unsupported job schema")
    require(isinstance(job["registrySha256"], str) and job["registrySha256"] == registry_digest(registry_bytes, private_bytes),
            "the registry changed since the job was checked")
    generators = merge_registry(registry_bytes, private_bytes)
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


def load_modules(entry, generators_dir=GENERATORS_DIR):
    """Read each shared module once, confine and hash it; return its namespace by name."""
    modules = entry.get("modules", {})
    require(isinstance(modules, dict), "modules is not an object")
    loaded = {}
    for name in sorted(modules):
        require(isinstance(name, str) and MODULE.match(name) is not None, "module name is malformed")
        path = os.path.join(generators_dir, name + ".py")
        require(inside(generators_dir, path) and os.path.isfile(path), "module %s is not in generators/" % name)
        with open(path, "rb") as handle:
            data = handle.read()
        require(sha256(data) == modules[name], "module %s does not match its pinned hash" % name)
        namespace = {"__name__": "pe_module_" + name}
        exec(compile(data, "generators/%s.py" % name, "exec"), namespace)
        loaded[name] = namespace
    return loaded


def frame_manifest(result, count, workdir):
    """Check what the builder returned and hash each frame.

    A builder returns its frame names in index order, or {"frames": names,
    "cellSpan": spans}: the span, in sim units, each frame was framed to.
    """
    names, spans = result, None
    if isinstance(result, dict):
        require(set(result) <= {"frames", "cellSpan"} and "frames" in result,
                "builder returned fields other than frames and cellSpan: %s" % ", ".join(sorted(map(str, result))))
        names, spans = result["frames"], result.get("cellSpan")
    require(isinstance(names, list) and len(names) == count, "builder returned %d frames, not %d" % (len(names or []), count))
    if spans is not None:
        require(isinstance(spans, list) and len(spans) == count, "builder returned a cellSpan without one span per frame")
        require(all(not isinstance(v, bool) and isinstance(v, (int, float)) and math.isfinite(v) and v > 0 for v in spans),
                "builder returned a cellSpan that is not all positive finite numbers")
    frames = []
    for index, name in enumerate(names):
        require(isinstance(name, str) and FRAME_NAME.match(name) is not None, "frame name is malformed: %r" % (name,))
        path = os.path.join(workdir, *name.split("/"))
        require(inside(workdir, path) and os.path.isfile(path), "frame is missing: %s" % name)
        with open(path, "rb") as handle:
            frames.append({"index": index, "name": name, "sha256": sha256(handle.read())})
    manifest = {"schemaVersion": 1, "frames": frames}
    if spans is not None:
        manifest["cellSpan"] = [float(v) for v in spans]
    return manifest


def reject(error):
    print("[houdini-run] " + json.dumps({"ok": False, "rejected": str(error)}))
    sys.stdout.flush()
    sys.exit(2)


def main():
    raw = sys.stdin.buffer.read(MAX_JOB_BYTES + 1)
    try:
        require(len(raw) <= MAX_JOB_BYTES, "job is too large")
        registry_bytes, private_bytes = read_registry_files(HOUDINI_DIR)
        entry, job = check_job(json.loads(raw.decode("utf-8")), registry_bytes, private_bytes)
    except (Rejected, ValueError) as error:
        reject(error)
    # Only the pin checks are rejections; an error the builder raises while
    # loading is a generator failure (exit 1).
    try:
        modules = load_modules(entry)
        builder = load_builder(entry)
    except Rejected as error:
        reject(error)
    import hou  # noqa: E402 -- only inside hython

    grid = entry["grid"]
    tier = dict(entry["tiers"][job["tier"]])
    # Frames are rendered at `frame` texels a side; the packer insets each one
    # by `pad` inside its `cell`. `cellSeconds`, when the entry has it, is the
    # simulation time each cell shows.
    ctx = {"params": job["params"], "seed": job["seed"], "tier": tier, "tierName": job["tier"],
           "grid": grid, "pad": PAD, "frame": tier["cell"] - 2 * PAD, "threads": job["threads"],
           "modules": modules, "cellSeconds": entry.get("cellSeconds")}
    workdir = os.getcwd()
    manifest = frame_manifest(builder["generate"](hou, ctx), grid * grid, workdir)
    with open(os.path.join(workdir, "frames.manifest.json"), "w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print("[houdini-run] " + json.dumps({"ok": True, "frames": len(manifest["frames"])}))
    sys.stdout.flush()


if __name__ == "__main__":
    main()
