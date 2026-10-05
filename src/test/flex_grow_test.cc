#include "breeze_ui/ui.h"
#include "breeze_ui/widget.h"
#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(const char *name, float actual, float expected) {
    const bool ok = std::abs(actual - expected) < 1.0f;
    failures += !ok;
    std::cout << (ok ? "[ok] " : "[FAIL] ") << name << ": " << actual
              << " (expected " << expected << ")" << std::endl;
}

void layout(const std::shared_ptr<ui::widget> &root) {
    root->prepare_layout_tree(nullptr);
    YGNodeCalculateLayout(root->node, YGUndefined, YGUndefined,
                          YGDirectionLTR);
    root->apply_layout_tree();
}

std::shared_ptr<ui::widget> box(float w, float h, float grow = 0) {
    auto child = std::make_shared<ui::widget>();
    child->width->reset_to(w);
    child->height->reset_to(h);
    child->flex_grow = grow;
    return child;
}

void test_flex_grow() {
    auto container = std::make_shared<ui::flex_widget>();
    container->horizontal = true;
    container->width->reset_to(400);
    container->height->reset_to(100);
    container->auto_size = false;

    auto child1 = box(50, 50, 1), child2 = box(100, 50, 2),
         child3 = box(50, 50);
    container->add_child(child1);
    container->add_child(child2);
    container->add_child(child3);
    layout(container);

    expect("grow child1", child1->width->dest(), 50.0f + 200.0f / 3.0f);
    expect("grow child2", child2->width->dest(), 100.0f + 400.0f / 3.0f);
    expect("grow child3", child3->width->dest(), 50.0f);
    expect("grow child3 x", child3->x->dest(), 350.0f);
}

void test_auto_size_and_empty() {
    auto container = std::make_shared<ui::flex_widget>();
    container->gap = 10;
    container->padding_left->reset_to(5);
    container->padding_right->reset_to(5);
    container->justify_content = ui::flex_widget::justify::space_evenly;
    layout(container);
    expect("empty width", container->width->dest(), 10);
    expect("empty height", container->height->dest(), 0);

    container->add_child(box(30, 20));
    container->add_child(box(60, 20));
    layout(container);
    expect("auto width", container->width->dest(), 70);
    expect("auto height", container->height->dest(), 50);
}

void test_stable_user_size() {
    auto container = std::make_shared<ui::flex_widget>();
    container->horizontal = true;
    container->auto_size = false;
    container->width->reset_to(300);
    container->height->reset_to(40);
    auto grown = box(50, 20, 1);
    container->add_child(grown);
    container->add_child(box(100, 20));
    for (int i = 0; i < 3; ++i)
        layout(container);
    expect("grow is idempotent", grown->width->dest(), 200);
    container->width->reset_to(200);
    layout(container);
    expect("shrinks back with parent", grown->width->dest(), 100);
}

void test_nested_auto_size() {
    auto outer = std::make_shared<ui::flex_widget>();
    outer->gap = 10;
    auto row = outer->emplace_child<ui::flex_widget>();
    row->horizontal = true;
    row->gap = 6;
    row->padding_top->reset_to(8);
    row->padding_bottom->reset_to(8);
    row->add_child(box(3, 15));
    row->add_child(box(14, 14));
    auto section = outer->emplace_child<ui::flex_widget>();
    section->add_child(box(40, 20));
    section->add_child(box(60, 30));
    for (int i = 0; i < 3; ++i)
        layout(outer);
    expect("nested row width", row->width->dest(), 23);
    expect("nested row height", row->height->dest(), 31);
    expect("nested column height", section->height->dest(), 50);
    expect("nested column y", section->y->dest(), 41);
    expect("outer height", outer->height->dest(), 91);
}

void test_fixed_width_column() {
    auto column = std::make_shared<ui::flex_widget>();
    column->fixed_width = true;
    column->width->reset_to(500);
    column->gap = 20;
    column->align_items = ui::flex_widget::align::stretch;
    column->add_child(box(100, 24));
    auto section = column->emplace_child<ui::flex_widget>();
    section->gap = 10;
    section->add_child(box(40, 20));
    section->add_child(box(60, 30));
    auto last = column->emplace_child<ui::flex_widget>();
    last->add_child(box(10, 10));
    for (int i = 0; i < 3; ++i)
        layout(column);
    expect("fixed-width section height", section->height->dest(), 60);
    expect("fixed-width section width", section->width->dest(), 500);
    expect("fixed-width last y", last->y->dest(), 124);
    expect("fixed-width column width", column->width->dest(), 500);
    expect("fixed-width column height", column->height->dest(), 134);
}
} // namespace

int main() {
    test_flex_grow();
    test_auto_size_and_empty();
    test_stable_user_size();
    test_nested_auto_size();
    test_fixed_width_column();
    std::cout << (failures ? "FAILED" : "PASSED") << std::endl;
    return failures;
}