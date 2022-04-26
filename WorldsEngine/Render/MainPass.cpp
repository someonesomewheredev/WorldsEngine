#include "RenderPasses.hpp"
#include <Core/Console.hpp>

namespace ShaderFlags {
    const int DBG_FLAG_NORMALS = 2;
    const int DBG_FLAG_METALLIC = 4;
    const int DBG_FLAG_ROUGHNESS = 8;
    const int DBG_FLAG_AO = 16;
    const int DBG_FLAG_NORMAL_MAP = 32;
    const int DBG_FLAG_LIGHTING_ONLY = 64;
    const int DBG_FLAG_UVS = 128;
    const int DBG_FLAG_SHADOW_CASCADES = 256;
    const int DBG_FLAG_ALBEDO = 512;
    const int DBG_FLAG_LIGHT_TILES = 1024;

    const int MISC_FLAG_UV_XY = 2048;
    const int MISC_FLAG_UV_XZ = 4096;
    const int MISC_FLAG_UV_ZY = 8192;
    const int MISC_FLAG_UV_PICK = 16384;
    const int MISC_FLAG_CUBEMAP_PARALLAX = 32768;
    const int MISC_FLAG_DISABLE_SHADOWS = 65536;
}

namespace worlds {
    MainPass::MainPass(VulkanHandles* handles, vku::PipelineLayout& pipelineLayout) 
        : RenderPass(handles)
        , pipelineLayout(pipelineLayout) {

    }

    void MainPass::execute(RenderContext& ctx, slib::StaticAllocList<SubmeshDrawInfo>& drawInfo, bool pickThisFrame, int pickX, int pickY) {
#ifdef TRACY_ENABLE
        ZoneScoped;
#endif
        TracyVkZone((*ctx.debugContext.tracyContexts)[ctx.frameIndex], ctx.cmdBuf, "Main Pass");

        VkCommandBuffer cmdBuf = ctx.cmdBuf;

        uint32_t globalMiscFlags = 0;

        if (pickThisFrame)
            globalMiscFlags |= 1;

        static worlds::ConVar* dbgDrawMode = g_console->getConVar("r_dbgdrawmode");

        if (dbgDrawMode->getInt() != 0) {
            globalMiscFlags |= (1 << dbgDrawMode->getInt());
        }

        if (!ctx.passSettings.enableCascadeShadows) {
            globalMiscFlags |= ShaderFlags::MISC_FLAG_DISABLE_SHADOWS;
        }

        addDebugLabel(cmdBuf, "Main Pass", 0.466f, 0.211f, 0.639f, 1.0f);
        VkPipeline lastPipeline = VK_NULL_HANDLE;
        for (const auto& sdi : drawInfo) {
            if (lastPipeline != sdi.pipeline) {
                vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, sdi.pipeline);
                ctx.debugContext.stats->numPipelineSwitches++;
            }

            StandardPushConstants pushConst{
                .modelMatrixIdx = sdi.matrixIdx,
                .materialIdx = sdi.materialIdx,
                .vpIdx = 0,
                .objectId = (uint32_t)sdi.ent,
                .cubemapIdx2 = sdi.cubemapIdx2,
                .cubemapBoost = sdi.cubemapIdx == 0 ? ctx.registry.ctx<SceneSettings>().skyboxBoost : 1.0f,
                .skinningOffset = sdi.boneMatrixOffset,
                .texScaleOffset = sdi.texScaleOffset,
                .screenSpacePickPos = glm::ivec3(pickX, pickY, globalMiscFlags | sdi.drawMiscFlags),
                .cubemapIdx = sdi.cubemapIdx
            };

            vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConst), &pushConst);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmdBuf, 0, 1, &sdi.vb, &offset);

            if (sdi.skinned) {
                vkCmdBindVertexBuffers(cmdBuf, 1, 1, &sdi.boneVB, &offset);
            }

            vkCmdBindIndexBuffer(cmdBuf, sdi.ib, 0, sdi.indexType);
            vkCmdDrawIndexed(cmdBuf, sdi.indexCount, 1, sdi.indexOffset, 0, 0);

            lastPipeline = sdi.pipeline;
            ctx.debugContext.stats->numDrawCalls++;
        }
        vkCmdEndDebugUtilsLabelEXT(cmdBuf);
    }
}
