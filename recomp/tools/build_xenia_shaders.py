#!/usr/bin/env python3
# Ips3: compiles Xenia's built-in Vulkan shaders (GLSL/XeSL -> SPIR-V C headers) the way its
# own build does (xenia-build.py build_shaders, SPIR-V target), with glslangValidator and
# spirv-opt from PATH or VULKAN_SDK.
#
#   build_xenia_shaders.py <xenia checkout>
import os
import subprocess
import sys

VALID_STAGES = ("vs", "hs", "ds", "gs", "ps", "cs")


def tool_wrappers(root):
    """Xenia passes --canonicalize-ids to spirv-opt, which only renumbers IDs; older
    SPIRV-Tools releases lack it. Point the compiler at wrappers that drop it there."""
    import shutil
    help_text = subprocess.run(["spirv-opt", "--help"], capture_output=True, text=True).stdout
    if "--canonicalize-ids" in help_text:
        return None
    sdk = os.path.join(root, "build-tools")
    bin_dir = os.path.join(sdk, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    for tool in ("glslangValidator", "spirv-dis"):
        target = os.path.join(bin_dir, tool)
        if not os.path.exists(target):
            os.symlink(shutil.which(tool), target)
    with open(os.path.join(bin_dir, "spirv-opt"), "w") as f:
        f.write("#!/bin/sh\n"
                "args=\"\"\n"
                "for a in \"$@\"; do [ \"$a\" = --canonicalize-ids ] || args=\"$args '$a'\"; done\n"
                f"eval exec {shutil.which('spirv-opt')} $args\n")
    os.chmod(os.path.join(bin_dir, "spirv-opt"), 0o755)
    return sdk


def main():
    root = sys.argv[1]
    compiler = os.path.join(root, "tools", "build", "compile_shader_spirv.py")
    sdk = tool_wrappers(root)
    env = dict(os.environ, VULKAN_SDK=sdk) if sdk else None
    built = 0
    for shader_dir in ("src/xenia/gpu/shaders", "src/xenia/ui/shaders"):
        src_dir = os.path.join(root, shader_dir)
        out_dir = os.path.join(src_dir, "bytecode", "vulkan_spirv")
        os.makedirs(out_dir, exist_ok=True)
        for name in sorted(os.listdir(src_dir)):
            if not (name.endswith(".glsl") or name.endswith(".xesl")) or len(name) <= 8 or name[-8] != ".":
                continue
            identifier = name[:-5].replace(".", "_")
            if identifier[-2:] not in VALID_STAGES:
                continue
            source = os.path.join(src_dir, name)
            output = os.path.join(out_dir, identifier + ".h")
            if os.path.exists(output) and os.path.getmtime(output) >= os.path.getmtime(source):
                continue
            if subprocess.call([sys.executable, compiler, source, output], env=env) != 0:
                sys.exit(f"failed: {source}")
            built += 1
    print(f"xenia shaders: {built} compiled")


if __name__ == "__main__":
    main()
