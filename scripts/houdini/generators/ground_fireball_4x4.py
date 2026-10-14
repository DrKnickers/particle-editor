# [houdini] ground-fireball-4x4: a 16-frame additive flipbook of a ground burst.
#
# A burning sphere sitting on the ground is the source for the first three
# frames: volume-scattered points carrying burn, temperature and density,
# broken up by noise, pushed outward (mostly sideways, a ground burst spreads
# before it rises). The Pyro solver, closed at the ground, carries it. Frames
# 1, 3, ..., 31 become the 16 cells, so the flash, the billow and the cooling
# fade each get a share.
#
# Mantra renders emission only, as white light from the temperature field. The
# smoke is unlit, so it renders black and adds nothing; it only hides fire
# behind it. Mantra draws nothing where a volume has no density, so density is
# sourced and kept from dissipating. A compositing pass then takes each frame
# through the palette's tone curve: dim emission takes the tint, hot emission
# runs to white. An edge mask fades the frame's border to black, so even the
# most turbulent plume never meets the frame's edge. Additive: RGB on black;
# the profile writes alpha 255.
#
# The seed moves the source noise and the solver's turbulence. `temperature`
# scales the blast speed, buoyancy, cooling and brightness; `turbulence` the
# solver's turbulence and disturbance. Expression and snippet text is built
# here from the checked parameters, formatted as numbers; nothing from the job
# reaches it as text.
#
# Contract (run_generator.py): generate(hou, ctx) writes frames/ under the
# working directory and returns the frame names in index order.
import os
import random

# Per tier: (pixel samples per axis, volume step rate, voxel size).
QUALITY = {"preview": (2, 0.25, 0.06), "validate": (3, 0.5, 0.04), "final": (4, 1.0, 0.025)}
PALETTES = {"orange": (1.0, 0.42, 0.1), "blue": (0.25, 0.5, 1.0), "green": (0.35, 1.0, 0.3)}
FIRST, STEP = 1, 2   # simulation frames FIRST, FIRST + STEP, ... are the cells
CENTRE = 0.45        # the burst's height above the ground
GAIN = 1.7
EDGE = 0.05          # the edge mask's width, as a fraction of the frame


def _parm(node, name):
    parm = node.parm(name)
    if parm is None:
        raise RuntimeError("missing parameter %s.%s" % (node.type().name(), name))
    return parm


def _set(node, values):
    for name, value in values.items():
        _parm(node, name).set(value)


def generate(hou, ctx):
    size, params, seed, count = ctx["frame"], ctx["params"], ctx["seed"], ctx["grid"] * ctx["grid"]
    rng = random.Random(seed)
    samples, steprate, voxel = QUALITY[ctx["tierName"]]
    heat, turb = float(params["temperature"]), float(params["turbulence"])

    obj = hou.node("/obj")
    geo = obj.createNode("geo", "fireball")
    shape = geo.createNode("sphere", "core")
    _set(shape, {"type": 2, "rows": 20, "cols": 20, "scale": 0.45})
    shape.parmTuple("t").set((0.0, CENTRE, 0.0))
    source = geo.createNode("pyrosource", "source")
    source.setInput(0, shape)
    _set(source, {"initialize": "sourceburn", "mode": 2, "particlesep": voxel * 0.5})  # mode 2: volume scatter
    _parm(source, "attributes").set(3)
    _set(source, {"attribute3": 0, "name3": "density", "fval3": 1.0})
    noise = geo.createNode("attribnoise::2.0", "breakup")
    noise.setInput(0, source)
    _set(noise, {"attribs": "burn temperature density", "operation": 3, "elementsize": 0.25,
                 "offsetvx": rng.uniform(0, 50), "offsetvy": rng.uniform(0, 50), "offsetvz": rng.uniform(0, 50)})
    blast = geo.createNode("attribwrangle", "blast")
    blast.setInput(0, noise)
    _parm(blast, "snippet").set(
        "vector d = normalize(@P - {0, %.4f, 0}); v@v = set(d.x * 1.4, abs(d.y) * 0.8, d.z * 1.4) * %.4f;"
        % (CENTRE, 1.2 + 0.6 * heat))
    raster = geo.createNode("volumerasterizeattributes", "raster")
    raster.setInput(0, blast)
    _set(raster, {"attributes": "burn temperature density v", "voxelsize": voxel})
    solver = geo.createNode("pyrosolver", "solver")
    solver.setInput(0, raster)
    # assignmaterial off: the solver would bind its own shader over ours.
    _set(solver, {"assignmaterial": 0, "divsize": voxel, "dissipation": 0.02,
                  "srclimitframerange": 1, "srcframerangemin": 1, "srcframerangemax": 3,
                  "closeystatus": 1, "closeyat": 0.0, "flames_lifespan": 0.5 + 0.4 * heat,
                  "tempcooling": 0.25 - 0.08 * heat, "buoyancylift": 0.12 + 0.1 * heat,
                  "enable_turbulence": 1, "turbulence": 0.3 + 1.2 * turb, "turbulence_swirlsize": 0.4,
                  "turbulence_seed": rng.uniform(0, 100),
                  "enable_disturbance": 1, "disturbance": 0.2 + 0.8 * turb, "disturbance_refscale": voxel * 4})
    solver.setDisplayFlag(True)
    solver.setRenderFlag(True)

    shader = hou.node("/mat").createNode("pyroshader", "fire")
    _set(shader, {"s_densityscale": 0.15, "fi_volumename": "temperature", "fi_int": 1.2 + 0.8 * heat,
                  "fc_colormode": 0, "fc_constantcolorr": 1.0, "fc_constantcolorg": 1.0, "fc_constantcolorb": 1.0})
    _parm(geo, "shop_materialpath").set(shader.path())

    cam = obj.createNode("cam", "cam")
    _set(cam, {"resx": size, "resy": size, "projection": 1, "orthowidth": 3.9})
    cam.parmTuple("t").set((0.0, 1.56, 8.0))
    rop = hou.node("/out").createNode("ifd", "mantra")
    _set(rop, {"camera": cam.path(), "vobject": "", "forceobject": geo.path(), "alights": "", "soho_autoheadlight": 0,
               "vm_picture": "render/frame_$F4.exr", "vm_samplesx": samples, "vm_samplesy": samples,
               "vm_volumesteprate": steprate, "vm_usemaxthreads": 0, "vm_threadcount": int(ctx["threads"])})
    os.makedirs("render", exist_ok=True)
    last = FIRST + STEP * (count - 1)
    rop.render(frame_range=(FIRST, last, STEP), verbose=False)

    # The palette: intensity (red; the emission is white, so all three channels
    # agree) through its tone curve, per frame, under an edge
    # mask that fades the outer twentieth of the frame to black, so a plume
    # that drifts that far fades out rather than meeting the frame's edge.
    net = hou.node("/img").createNode("img", "grade")
    read = net.createNode("file", "render")
    _parm(read, "filename1").set("render/frame_$F4.exr")
    tone = net.createNode("pixel", "tone")
    tone.setInput(0, read)
    mask = "clamp(min(min($X,%d-$X),min($Y,%d-$Y))/%.4f,0,1)" % (size - 1, size - 1, size * EDGE)
    for channel, tint in zip("rgb", PALETTES[params["palette"]]):
        _parm(tone, channel + "expr").setExpression(
            "clamp(($R*%.4f)*%.4f+pow($R*%.4f,3)*0.8,0,1)*%s" % (GAIN, tint, GAIN, mask), hou.exprLanguage.Hscript)
    _parm(tone, "aexpr").setExpression("1", hou.exprLanguage.Hscript)
    comp = hou.node("/out").createNode("comp", "graded")
    _parm(comp, "coppath").set(tone.path())
    _parm(comp, "convertcolorspace").set(0)
    _parm(comp, "mkpath").set(1)
    _parm(comp, "copoutput").set("graded/frame_$F4.exr")
    comp.render(frame_range=(FIRST, last, STEP), verbose=False)

    # Cells are numbered 1..count in time order.
    os.makedirs("frames", exist_ok=True)
    names = []
    for index in range(count):
        name = "frames/frame_%04d.exr" % (index + 1)
        os.replace("graded/frame_%04d.exr" % (FIRST + STEP * index), name)
        names.append(name)
    return names
