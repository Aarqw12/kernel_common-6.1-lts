# SPDX-License-Identifier: GPL-2.0-or-later

"""Defines helper functions for creating debug and staging build configs."""

load("@bazel_skylib//lib:paths.bzl", "paths")
load("//build/kernel/kleaf:hermetic_tools.bzl", "hermetic_toolchain")
load(
    "//build/kernel/kleaf:kernel.bzl",
    "kernel_module",
)

def _extracted_system_dlkm(ctx):
    hermetic_tools = hermetic_toolchain.get(ctx)

    inputs = []

    system_dlkm_archive = None
    for f in ctx.files.images:
        if f.basename == "system_dlkm_staging_archive.tar.gz":
            inputs.append(f)
            system_dlkm_archive = f
            break

    outs = []
    for m in ctx.attr.gki_modules:
        outs.append(ctx.actions.declare_file(m))
    common_outs = outs[0].dirname

    intermediates_dir = paths.join(
        ctx.bin_dir.path,
        paths.dirname(ctx.build_file_path),
        ctx.attr.name + "_intermediates",
    )

    command = hermetic_tools.setup
    command += """
        # Extract GKI modules
        mkdir -p {intermediates_dir}

        tar xf {system_dlkm_archive} -C {intermediates_dir}
        find {intermediates_dir} -name '*.ko' -exec cp -t {common_outs} {{}} \\+

        # Verify the outputs. We don't care if there are more modules extracted
        # than used. For example, we don't use zram.ko.
        all_modules=({all_modules})
        for m in "${{all_modules[@]}}"; do
            if ! [[ -f "{common_outs}/${{m}}" ]]; then
                echo "${{m}} is missing from $(basename {system_dlkm_archive})" >&2
                exit 1
            fi
        done
    """.format(
        all_modules = " ".join([m.basename for m in outs]),
        common_outs = common_outs,
        system_dlkm_archive = system_dlkm_archive.path,
        intermediates_dir = intermediates_dir,
    )

    ctx.actions.run_shell(
        mnemonic = "ExtractedSystemDlkm",
        inputs = inputs,
        outputs = outs,
        tools = hermetic_tools.deps,
        progress_message = "Extracting GKI modules",
        command = command,
    )

    return [DefaultInfo(files = depset(outs))]

extracted_system_dlkm = rule(
    doc = """Extracts the system_dlkm archive so that they can be copied to the dist_dir""",
    implementation = _extracted_system_dlkm,
    attrs = {
        "images": attr.label(
            doc = "The kernel_images target that contains the system_dlkm archive.",
            allow_files = True,
            mandatory = True,
        ),
        "gki_modules": attr.string_list(
            doc = "A list of GKI modules",
            allow_empty = False,
            mandatory = True,
        ),
    },
    toolchains = [hermetic_toolchain.type],
)

def lto_dependant_kernel_module(
        name,
        outs,
        lto_outs,
        test_outs = [],
        srcs = None,
        kernel_build = None,
        makefile = None,
        deps = None,
        **kwargs):
    """Wrapper over kernel_module to conditionally modify outs when LTO is set differently.

    Args:
        name: name of the module
        outs: See kernel_module.outs
        lto_outs: Like outs, but only appended to outs when LTO is not set to none
        test_outs: KUnit test output modules
        srcs: sources for the kernel module
        kernel_build: See kernel_module.kernel_build
        makefile: See kernel_module.makefile
        deps: See kernel_module.deps
        **kwargs: common kwargs for all rules
    """
    kwargs_with_private_visibility = dict(
        kwargs,
        visibility = ["//visibility:private"],
    )

    kernel_module(
        name = name + "_internal",
        srcs = srcs,
        outs = outs + lto_outs,
        kernel_build = kernel_build,
        makefile = makefile,
        deps = deps,
        **kwargs_with_private_visibility
    )

    kernel_module(
        name = name + "_lto_none",
        srcs = srcs,
        outs = outs,
        kernel_build = kernel_build,
        makefile = makefile,
        deps = deps,
        **kwargs_with_private_visibility
    )

    kernel_module(
        name = name + "_kunit",
        srcs = srcs,
        outs = outs + test_outs + lto_outs,
        kernel_build = kernel_build,
        makefile = makefile,
        deps = deps,
        **kwargs_with_private_visibility
    )

    native.alias(
        name = name,
        actual = select({
            "//private/devices/google/common:lto_none": name + "_lto_none",
            "//private/devices/google/common:kunit_enabled": name + "_kunit",
            "//conditions:default": name + "_internal",
        }),
        **kwargs
    )
