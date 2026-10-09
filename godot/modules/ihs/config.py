def can_build(env, platform):
    # The module drives libgodot from an ivi-homescreen platform view; it only
    # makes sense in a library build on Linux.
    return platform == "linuxbsd"


def configure(env):
    pass


def get_doc_classes():
    return ["IhsHost"]


def get_doc_path():
    return "doc_classes"
