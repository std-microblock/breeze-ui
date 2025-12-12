set_project("breeze-ui")

set_languages("c++2b")
set_warnings("all")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})
add_rules("mode.releasedbg")
includes("deps/glfw.lua")

add_requires("breeze-glfw", {alias = "glfw"})
add_requires("glad")

target("nanovg")
    set_kind("static")
    add_files("src/nanovg/**.c")
    add_includedirs("src/nanovg", {
        public = true
    })
    add_headerfiles("src/nanovg/*.h")

target("nanosvg")
    set_kind("headeronly")
    add_files("src/nanosvg/**.c")
    add_includedirs("src/nanosvg", {
        public = true
    })
    add_headerfiles("src/nanosvg/*.h")

target("breeze_ui")
    set_kind("static")
    add_packages("glfw", "glad", {
        public = true
    })
    add_deps("nanovg", "nanosvg", {
        public = true
    })
    add_syslinks("dwmapi", "shcore")
    add_files("src/breeze_ui/*.cc")
    add_headerfiles("src/(breeze_ui/*.h)")
    add_includedirs("src/", {
        public = true
    })
    set_encodings("utf-8")

target("flex_grow_test")
    set_kind("binary")
    add_deps("breeze_ui")
    add_files("src/test/flex_grow_test.cc")
    add_includedirs("src/")
