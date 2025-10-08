#pragma once
#include "breeze_ui/nanovg_wrapper.h"
#include "breeze_ui/widget.h"
#include "glad/glad.h"
namespace ui {
NVGImage LoadBitmapImage(nanovg_context ctx, void *hbitmap);
};