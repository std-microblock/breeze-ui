set_project("breeze-ui")

set_languages("c++2b")
set_warnings("all")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})
add_rules("mode.releasedbg")
includes("deps/glfw.lua")

add_requires("breeze-glfw", {alias = "glfw"})
add_requires("nanovg", "glad", "nanosvg")

target("breeze_ui")
    set_kind("static")
    add_packages("glfw", "glad", "nanovg", "nanosvg", {
        public = true
    })
    add_syslinks("dwmapi", "shcore")
    add_files("src/breeze_ui/*.cc")
    add_headerfiles("src/(breeze_ui/*.h)")
    add_includedirs("src/", {
        public = true
    })
    set_encodings("utf-8")
