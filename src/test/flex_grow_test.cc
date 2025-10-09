#include "breeze_ui/widget.h"
#include "breeze_ui/ui.h"
#include <iostream>

// Simple test to verify flex grow functionality
void test_flex_grow() {
    // Create a horizontal flex container
    auto container = std::make_shared<ui::flex_widget>();
    container->horizontal = true;
    container->width->reset_to(400);
    container->height->reset_to(100);
    container->auto_size = false;
    
    // Create three children with different flex grow values
    auto child1 = std::make_shared<ui::widget>();
    child1->width->reset_to(50);
    child1->height->reset_to(50);
    child1->flex_grow = 1.0f;  // Will grow
    
    auto child2 = std::make_shared<ui::widget>();
    child2->width->reset_to(100);
    child2->height->reset_to(50);
    child2->flex_grow = 2.0f;  // Will grow more
    
    auto child3 = std::make_shared<ui::widget>();
    child3->width->reset_to(50);
    child3->height->reset_to(50);
    child3->flex_grow = 0.0f;  // Won't grow
    
    // Add children to container
    container->add_child(child1);
    container->add_child(child2);
    container->add_child(child3);
    
    // Create a mock update context
    ui::update_context ctx{
        .delta_time = 16.67f,
        .mouse_x = 0,
        .mouse_y = 0,
        .mouse_down = false,
        .right_mouse_down = false,
        .window = nullptr,
        .mouse_clicked = false,
        .right_mouse_clicked = false,
        .mouse_up = false,
        .screen = {800, 600, 1.0f},
        .scroll_y = 0,
        .need_repaint = *(new bool{false}),
        .offset_x = 0,
        .offset_y = 0,
        .rt = *(new ui::render_target{}),
        .vg = {}
    };
    
    // Update the layout
    container->update(ctx);
    
    // Check results
    std::cout << "Flex Grow Test Results:" << std::endl;
    std::cout << "Container width: " << container->width->dest() << std::endl;
    std::cout << "Child1 width: " << child1->width->dest() << " (flex_grow: " << child1->flex_grow << ")" << std::endl;
    std::cout << "Child2 width: " << child2->width->dest() << " (flex_grow: " << child2->flex_grow << ")" << std::endl;
    std::cout << "Child3 width: " << child3->width->dest() << " (flex_grow: " << child3->flex_grow << ")" << std::endl;
    
    // Expected behavior:
    // - Total fixed width: 50 + 100 + 50 = 200
    // - Available space: 400 - 200 = 200
    // - Total flex grow: 1 + 2 + 0 = 3
    // - Child1 gets: 50 + (1/3) * 200 = 50 + 66.67 = 116.67
    // - Child2 gets: 100 + (2/3) * 200 = 100 + 133.33 = 233.33
    // - Child3 gets: 50 + (0/3) * 200 = 50
    
    float expected_child1 = 50.0f + (1.0f / 3.0f) * 200.0f;
    float expected_child2 = 100.0f + (2.0f / 3.0f) * 200.0f;
    float expected_child3 = 50.0f;
    
    std::cout << "\nExpected:" << std::endl;
    std::cout << "Child1 width: " << expected_child1 << std::endl;
    std::cout << "Child2 width: " << expected_child2 << std::endl;
    std::cout << "Child3 width: " << expected_child3 << std::endl;
    
    bool test_passed = 
        std::abs(child1->width->dest() - expected_child1) < 1.0f &&
        std::abs(child2->width->dest() - expected_child2) < 1.0f &&
        std::abs(child3->width->dest() - expected_child3) < 1.0f;
    
    std::cout << "\nTest " << (test_passed ? "PASSED" : "FAILED") << std::endl;
}

int main() {
    test_flex_grow();
    return 0;
}