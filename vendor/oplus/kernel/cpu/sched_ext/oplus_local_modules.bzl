load("//build/kernel/kleaf:kernel.bzl", "ddk_headers")
load("//build/kernel/oplus:oplus_modules_define.bzl", "define_oplus_ddk_module")
load("//build/kernel/oplus:oplus_modules_dist.bzl", "ddk_copy_to_dist_dir")

def define_oplus_local_modules():

    define_oplus_ddk_module(
        name = "oplus_bsp_sched_ext",
        srcs = native.glob([
            "main.c",
            "**/*.h",
        ]),
        conditional_srcs = {
            "CONFIG_HMBIRD_SCHED": {
                True:  [
                    "cpufreq_scx_main.c",
                    "scx_shadow_tick.c",
                    "hmbird/hmbird_main.c",
                    "hmbird/hmbird_misc.c",
                    "hmbird/hmbird_sysctrl.c",
                    "hmbird/hmbird_util_track.c",
                ],
            },
            "CONFIG_HMBIRD_SCHED_GKI": {
                True:  [
                    "cpufreq_scx_main.c",
                    "scx_shadow_tick.c",
                    "hmbird_gki/scx_main.c",
                    "hmbird_gki/scx_sched_gki.c",
                    "hmbird_gki/scx_util_track.c",
                    "hmbird_gki/scx_game.c",
                ],
            },
        },
        includes = ["."],
        local_defines = [
            "CONFIG_SCX_GAME_OPT_ENABLE",
        ],
	    ko_deps = [
            "//vendor/oplus/kernel/cpu/game_opt:oplus_bsp_game_opt",
        ],
        header_deps = [
            "//vendor/oplus/kernel/cpu/game_opt:config_headers",
        ],
        copts = select({
            "//build/kernel/kleaf:kocov_is_true": ["-fprofile-arcs", "-ftest-coverage"],
            "//conditions:default": [],
        }),
    )
    ddk_headers(
        name = "config_headers",
        hdrs  = native.glob([
            "**/*.h",
        ]),
        includes = ["."],
    )
    ddk_copy_to_dist_dir(
        name = "oplus_bsp_sched_ext",
        module_list = [
            "oplus_bsp_sched_ext",
        ],
    )
