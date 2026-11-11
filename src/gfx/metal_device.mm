#include "nxmac/gfx/metal_device.h"

#import <Metal/Metal.h>

namespace nxmac::gfx {

std::string DefaultGpuArchitecture() {
    std::string name;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device != nil) {
            if (const char* s = device.architecture.name.UTF8String) name = s;
        }
    }
    return name;
}

}  // namespace nxmac::gfx
