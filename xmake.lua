set_project("breeze-ui")

set_languages("c++2b")
set_warnings("all")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})
add_rules("mode.releasedbg")
includes("deps/glfw.lua")

add_requires("breeze-glfw", {alias = "glfw"})
add_requires("glad")
add_requires("simdutf")

target("breeze-nanovg")
    set_kind("static")
    add_files("src/nanovg/nanovg.c", "src/nanovg/nanovg_gl_impl.c")
    add_includedirs("src/nanovg", {
        public = true
    })
    add_headerfiles("src/nanovg/*.h")
    add_packages("glad", {
        public = true
    })

target("breeze-nanosvg")
    set_kind("headeronly")
    add_files("src/nanosvg/**.c")
    add_includedirs("src/nanosvg", {
        public = true
    })
    add_headerfiles("src/nanosvg/*.h")

target("breeze_ui")
    set_kind("static")
    add_packages("glfw", "glad", "simdutf", {
        public = true
    })
    add_deps("breeze-nanovg", "breeze-nanosvg", {
        public = true
    })
    add_syslinks("dwmapi", "imm32", "shcore", "windowsapp", "CoreMessaging")
    add_files("src/breeze_ui/*.cc")
    add_headerfiles("src/(breeze_ui/*.h)")
    add_includedirs("src/", {
        public = true
    })
    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    set_encodings("utf-8")

target("flex_grow_test")
    set_kind("binary")
    add_deps("breeze_ui")
    add_files("src/test/flex_grow_test.cc")
    add_includedirs("src/")

target("text_bounds_test")
    set_kind("binary")
    add_deps("breeze_ui")
    add_files("src/test/text_bounds_test.cc")
    add_includedirs("src/")

target("acrylic_demo")
    set_kind("binary")
    add_deps("breeze_ui")
    add_files("src/test/acrylic_demo.cc")
    add_includedirs("src/")
