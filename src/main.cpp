#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION

#include "vulkan_app.h"

#include <exception>
#include <iostream>

int main()
{
    try {
        VulkanApp app;
        app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
    return 0;
}
