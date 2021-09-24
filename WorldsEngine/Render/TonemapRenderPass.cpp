#include "RenderPasses.hpp"
#include "../Core/Engine.hpp"
#include "Render.hpp"
#include "ShaderCache.hpp"
#include "vku/SamplerMaker.hpp"
#include "vku/PipelineMakers.hpp"
#include "vku/DescriptorSetUtil.hpp"

namespace worlds {
    struct TonemapPushConstants {
        int idx;
        float exposureBias;
    };

    static ConVar exposureBias("r_exposure", "0.5");

    TonemapRenderPass::TonemapRenderPass(
            VulkanHandles* handles,
            RenderTexture* hdrImg,
            RenderTexture* finalPrePresent)
        : finalPrePresent{finalPrePresent}
        , hdrImg {hdrImg}
        , handles {handles} {

    }

    void TonemapRenderPass::setup(RenderContext& ctx, VkDescriptorPool descriptorPool) {
        dsPool = descriptorPool;
        vku::DescriptorSetLayoutMaker tonemapDslm;
        tonemapDslm.image(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_COMPUTE_BIT, 1);
        tonemapDslm.image(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_COMPUTE_BIT, 1);

        dsl = tonemapDslm.create(handles->device);

        auto msaaSamples = hdrImg->image.info().samples;
        std::string shaderName = (int)msaaSamples > 1 ? "tonemap.comp.spv" : "tonemap_nomsaa.comp.spv";
        tonemapShader = ShaderCache::getModule(handles->device, AssetDB::pathToId("Shaders/" + shaderName));

        vku::PipelineLayoutMaker plm;
        plm.descriptorSetLayout(dsl);
        plm.pushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TonemapPushConstants));

        pipelineLayout = plm.create(handles->device);

        vku::ComputePipelineMaker cpm;
        cpm.shader(VK_SHADER_STAGE_COMPUTE_BIT, tonemapShader);
        VkSpecializationMapEntry samplesEntry{ 0, 0, sizeof(int32_t) };
        VkSpecializationInfo si;
        si.dataSize = sizeof(msaaSamples);
        si.mapEntryCount = 1;
        si.pMapEntries = &samplesEntry;
        si.pData = &msaaSamples;
        cpm.specializationInfo(si);

        pipeline = cpm.create(handles->device, handles->pipelineCache, pipelineLayout);

        vku::DescriptorSetMaker dsm;
        dsm.layout(dsl);
        descriptorSet = std::move(dsm.create(handles->device, descriptorPool)[0]);

        vku::SamplerMaker sm;
        sampler = sm.create(handles->device);

        vku::DescriptorSetUpdater dsu;
        dsu.beginDescriptorSet(descriptorSet);

        dsu.beginImages(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        dsu.image(sampler, finalPrePresent->image.imageView(), VK_IMAGE_LAYOUT_GENERAL);

        dsu.beginImages(1, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        dsu.image(sampler, hdrImg->image.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        dsu.update(handles->device);
    }

    void TonemapRenderPass::execute(RenderContext& ctx) {
#ifdef TRACY_ENABLE
        ZoneScoped;
        TracyVkZone((*ctx.debugContext.tracyContexts)[ctx.imageIndex], ctx.cmdBuf, "Tonemap/Postprocessing");
#endif
        auto& cmdBuf = ctx.cmdBuf;

        VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
        label.pLabelName = "Tonemap Render Pass";
        label.color[0] = 0.760f;
        label.color[1] = 0.298f;
        label.color[2] = 0.411f;
        label.color[3] = 1.0f;
        vkCmdBeginDebugUtilsLabelEXT(cmdBuf, &label);

        finalPrePresent->image.setLayout(cmdBuf,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT);

        vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        TonemapPushConstants tpc{ 0, exposureBias.getFloat() };
        vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tpc), &tpc);

        vkCmdDispatch(cmdBuf, (ctx.passWidth + 15) / 16, (ctx.passHeight + 15) / 16, 1);

        if (ctx.passSettings.enableVR) {
            finalPrePresentR->image.setLayout(cmdBuf,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);

            vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &rDescriptorSet, 0, nullptr);
            TonemapPushConstants tpc{ 1, exposureBias.getFloat() };
            vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tpc), &tpc);
            vkCmdDispatch(cmdBuf, (ctx.passWidth + 15) / 16, (ctx.passHeight + 15) / 16, 1);
        }

        finalPrePresent->image.setLayout(cmdBuf,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

        vkCmdEndDebugUtilsLabelEXT(cmdBuf);
    }

    void TonemapRenderPass::setRightFinalImage(RenderTexture* right) {
        vku::DescriptorSetMaker dsm;
        dsm.layout(dsl);
        rDescriptorSet = std::move(dsm.create(handles->device, dsPool)[0]);

        vku::DescriptorSetUpdater dsu;
        dsu.beginDescriptorSet(rDescriptorSet);

        finalPrePresentR = right;

        dsu.beginImages(0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        dsu.image(sampler, finalPrePresentR->image.imageView(), VK_IMAGE_LAYOUT_GENERAL);

        dsu.beginImages(1, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        dsu.image(sampler, hdrImg->image.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        dsu.update(handles->device);
    }

    TonemapRenderPass::~TonemapRenderPass() {
    }
}
