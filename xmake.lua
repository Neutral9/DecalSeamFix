-- CommonLibSSE-NG: the lib/commonlibsse-ng submodule (git submodule update --init --recursive)
includes("lib/commonlibsse-ng")

set_project("DecalSeamFix")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")

-- the detour on BGSDecalManager::ApplyDecal (a function entry, not a call site)
add_requires("minhook")

-- the target is named after the MO2 mod folder: the plugin rule installs into
-- XSE_TES5_MODS_PATH/<target name>
target("DecalSeamFix")
    add_rules("commonlibsse-ng.plugin", {
        name = "DecalSeamFix",
        author = "Neutral9",
        description = "Ground decals no longer end in a straight cut at land quadrant and cell borders."
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_packages("minhook")
    -- CommonLib headers: nameless structs, zero-sized arrays, macro redefinitions
    add_cxxflags("cl::/wd4200", "cl::/wd4201", "cl::/wd4005")

    -- xmake install: into the author's MO2 mod folder when it is there (otherwise XSE_TES5_MODS_PATH/DecalSeamFix)
    on_config(function (target)
        if os.isdir("E:/SOE/mods") then
            target:set("installdir", "E:/SOE/mods/DecalSeamFix")
        end
    end)
