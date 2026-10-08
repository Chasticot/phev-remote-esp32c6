"""Enable socket diagnostics only for Arduino's TCP client, not the web portal."""

Import("env")
from pathlib import Path


def tcp_debug(build_env, node):
    if node.name != "NetworkClient.cpp":
        return node
    definitions = []
    for definition in build_env["CPPDEFINES"]:
        name = definition[0] if isinstance(definition, (tuple, list)) else str(definition).split("=", 1)[0]
        if name != "CORE_DEBUG_LEVEL":
            definitions.append(definition)
    replacement = str(Path(build_env.subst("$PROJECT_DIR")) / "tools" / "NetworkClientBounded.cpp")
    print("DIAG TCP: NetworkClient de projet, ecriture bornee a 25 ms, CORE_DEBUG_LEVEL=4")
    return build_env.Object(replacement, CPPDEFINES=definitions + [("CORE_DEBUG_LEVEL", 4)])


env.AddBuildMiddleware(tcp_debug, "*NetworkClient.cpp")
