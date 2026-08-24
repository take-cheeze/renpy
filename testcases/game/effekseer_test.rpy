################################################################################
# Effekseer
#
# A manual test for the Effekseer integration (see renpy/gl2/EFFEKSEER.md).
#
# This test requires:
#
# 1. Ren'Py built with Effekseer support (EFFEKSEER=/path/to/effekseer, see
#    run.sh / setup.py). Without it, the test reports that the module is
#    missing instead of failing with a traceback.
#
# 2. An effect file at testcases/game/effekseer/test.efkefc. The Effekseer
#    editor ships sample effects in its Sample/ directory; export one with its
#    resources embedded so it is self-contained (external resources are not
#    routed through Ren'Py's loader yet).
#
# The controls exist because the first run is expected to look wrong before it
# looks right: the projection/camera defaults are placeholders, and the
# Effekseer output is not premultiplied yet. Adjust the camera to find the
# effect, and use the backgrounds to tell an alpha problem from a
# nothing-is-drawing problem.
################################################################################

init python:

    EFFEKSEER_TEST_FILE = "effekseer/test.efkefc"

    def effekseer_available():
        """
        Returns (available, message). Never raises, so the test can report a
        missing build or a missing effect file as text.
        """

        try:
            import renpy.gl2.effekseer as effekseer
        except ImportError as e:
            return False, "Could not import the Effekseer module: {}".format(e)

        if effekseer.effekseermodel is None:
            return False, "Ren'Py was built without Effekseer support. Rebuild with EFFEKSEER set."

        if not renpy.loadable(EFFEKSEER_TEST_FILE):
            return False, "No effect file at game/{}.".format(EFFEKSEER_TEST_FILE)

        return True, "Ready."


default effekseer_distance = 10.0
default effekseer_fov = 45.0
default effekseer_bg = "#000"
default effekseer_status = ""
default effekseer_ok = False


screen effekseer_test():

    # The background makes alpha problems legible: an effect that looks correct
    # on black but has dark fringes on white is a premultiplied-alpha problem,
    # not a geometry problem.
    add effekseer_bg

    # Recreated whenever the camera changes, since the camera is set when the
    # displayable is constructed.
    add Effekseer(
        EFFEKSEER_TEST_FILE,
        loop=True,
        camera_distance=effekseer_distance,
        fov=effekseer_fov,
        ):
        align (0.5, 0.5)

    frame:
        align (0.0, 0.0)
        has vbox

        text "Effekseer test" size 22
        text "[effekseer_status]" size 14

        null height 8

        text "camera_distance: [effekseer_distance:.1f]" size 14
        hbox:
            textbutton "-" action SetVariable("effekseer_distance", max(effekseer_distance - 2.0, 1.0))
            textbutton "+" action SetVariable("effekseer_distance", effekseer_distance + 2.0)

        text "fov: [effekseer_fov:.0f]" size 14
        hbox:
            textbutton "-" action SetVariable("effekseer_fov", max(effekseer_fov - 10.0, 10.0))
            textbutton "+" action SetVariable("effekseer_fov", min(effekseer_fov + 10.0, 120.0))

        null height 8

        text "background" size 14
        hbox:
            textbutton "black" action SetVariable("effekseer_bg", "#000")
            textbutton "white" action SetVariable("effekseer_bg", "#fff")
            textbutton "grey" action SetVariable("effekseer_bg", "#808080")

        null height 8

        textbutton "Return" action Return()


label effekseer:

    python:
        effekseer_ok, effekseer_status = effekseer_available()

    if not effekseer_ok:

        "Effekseer test unavailable.\n\n[effekseer_status]"

        return

    "The Effekseer test is about to start.\n\nIf you see nothing, widen the camera distance and field of view -- the defaults are placeholders. Compare the black and white backgrounds to check alpha."

    call screen effekseer_test

    "Effekseer test finished."

    return
