#pragma once
#include "TextureLoader.hpp"

namespace worlds {
    struct CubemapData {
        TextureData faceData[6];
        std::string debugName;
    };

    CubemapData loadCubemapData(AssetID id);
    vku::TextureImageCube uploadCubemapVk(VulkanHandles& ctx, CubemapData& cd, VkCommandBuffer cb, uint32_t imageIndex);
    vku::TextureImageCube uploadCubemapVk(VulkanHandles& ctx, CubemapData& cd);
    void destroyTempCubemapBuffers(uint32_t imageIndex);
}