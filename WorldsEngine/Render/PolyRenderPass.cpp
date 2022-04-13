#include <openvr.h>
#include <slib/StaticAllocList.hpp>

#include <Core/Console.hpp>
#include <Core/Engine.hpp>
#include <Core/Transform.hpp>
#include <ImGui/imgui_internal.h>
#include <ImGui/imgui.h>
#include <Physics/Physics.hpp>
#include <tracy/Tracy.hpp>
#include <Util/MatUtil.hpp>
#include "RenderPasses.hpp"
#include "Render.hpp"
#include "Frustum.hpp"
#include "ShaderCache.hpp"
#include "vku/RenderpassMaker.hpp"
#include "vku/DescriptorSetUtil.hpp"
#include "ShaderReflector.hpp"

namespace ShaderFlags {
    enum ShaderFlag {
        DBG_FLAG_NORMALS = 2,
        DBG_FLAG_METALLIC = 4,
        DBG_FLAG_ROUGHNESS = 8,
        DBG_FLAG_AO = 16,
        DBG_FLAG_NORMAL_MAP = 32,
        DBG_FLAG_LIGHTING_ONLY = 64,
        DBG_FLAG_UVS = 128,
        DBG_FLAG_SHADOW_CASCADES = 256,
        DBG_FLAG_ALBEDO = 512,
        DBG_FLAG_LIGHT_TILES = 1024,

        MISC_FLAG_UV_XY = 2048,
        MISC_FLAG_UV_XZ = 4096,
        MISC_FLAG_UV_ZY = 8192,
        MISC_FLAG_UV_PICK = 16384,
        MISC_FLAG_CUBEMAP_PARALLAX = 32768,
        MISC_FLAG_DISABLE_SHADOWS = 65536,
        MISC_FLAG_SELECTION_GLOW = 131072
    };
}

namespace worlds {
    ConVar showWireframe("r_wireframeMode", "0", "0 - No wireframe; 1 - Wireframe only; 2 - Wireframe + solid");
    ConVar dbgDrawMode("r_dbgDrawMode", "0", "0 = Normal, 1 = Normals, 2 = Metallic, 3 = Roughness, 4 = AO");
    ConVar enableProxyAO("r_enableProxyAO", "1");
    ConVar enableDepthPrepass("r_depthPrepass", "1");
    ConVar enableParallaxMapping("r_doParallaxMapping", "0");
    ConVar maxParallaxLayers("r_maxParallaxLayers", "32");
    ConVar minParallaxLayers("r_minParallaxLayers", "4");

    struct StandardSpecConsts {
        VkBool32 enablePicking = false;
        float parallaxMaxLayers = 32.0f;
        float parallaxMinLayers = 4.0f;
        VkBool32 doParallax = false;
        VkBool32 enableProxyAO = false;
    };

    void setupVertexFormat(vku::PipelineMaker& pm) {
        pm.vertexBinding(0, (uint32_t)sizeof(Vertex));
        pm.vertexAttribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, position));
        pm.vertexAttribute(1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, normal));
        pm.vertexAttribute(2, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, tangent));
        pm.vertexAttribute(3, 0, VK_FORMAT_R32_SFLOAT, (uint32_t)offsetof(Vertex, bitangentSign));
        pm.vertexAttribute(4, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(Vertex, uv));
    }

    void setupSkinningVertexFormat(vku::PipelineMaker& pm) {
        pm.vertexBinding(1, (uint32_t)sizeof(VertSkinningInfo));
        pm.vertexAttribute(5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(VertSkinningInfo, weights));
        pm.vertexAttribute(6, 1, VK_FORMAT_R32G32B32A32_UINT, (uint32_t)offsetof(VertSkinningInfo, boneIds));
    }

    class StandardPipelineMaker {
    public:
        StandardPipelineMaker(AssetID vertexShader, AssetID fragmentShader) {
            vs = vertexShader;
            fs = fragmentShader;
        }

        StandardPipelineMaker& setMsaaSamples(int val) {
            msaaSamples = val;
            return *this;
        }

        StandardPipelineMaker& setPickingEnabled(bool val) {
            enablePicking = val;
            return *this;
        }

        StandardPipelineMaker& setCullMode(VkCullModeFlags val) {
            cullFlags = val;
            return *this;
        }

        StandardPipelineMaker& setUseSkinningAttributes(bool val) {
            useSkinningAttributes = val;
            return *this;
        }

        vku::Pipeline createPipeline(VulkanHandles* handles, VkPipelineLayout layout, VkRenderPass renderPass) {
            VkSpecializationMapEntry entries[5] = {
                { 0, offsetof(StandardSpecConsts, enablePicking),     sizeof(VkBool32) },
                { 1, offsetof(StandardSpecConsts, parallaxMaxLayers), sizeof(float) },
                { 2, offsetof(StandardSpecConsts, parallaxMinLayers), sizeof(float) },
                { 3, offsetof(StandardSpecConsts, doParallax),        sizeof(VkBool32) },
                { 4, offsetof(StandardSpecConsts, enableProxyAO),     sizeof(VkBool32) }
            };

            VkSpecializationInfo standardSpecInfo{ 5, entries, sizeof(StandardSpecConsts) };

            vku::PipelineMaker pm{ 1600, 900 };
            VkShaderModule fragmentShader = ShaderCache::getModule(handles->device, fs);
            VkShaderModule vertexShader = ShaderCache::getModule(handles->device, vs);

            StandardSpecConsts spc{
                enablePicking,
                maxParallaxLayers.getFloat(),
                minParallaxLayers.getFloat(),
                (bool)enableParallaxMapping.getInt(),
                (bool)enableProxyAO.getInt()
            };

            standardSpecInfo.pData = &spc;

            pm.shader(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "main", &standardSpecInfo);
            pm.shader(VK_SHADER_STAGE_VERTEX_BIT, vertexShader);
            setupVertexFormat(pm);

            if (useSkinningAttributes)
                setupSkinningVertexFormat(pm);

            pm.cullMode(cullFlags);

            if ((int)enableDepthPrepass)
                pm.depthWriteEnable(false)
                .depthTestEnable(true)
                .depthCompareOp(VK_COMPARE_OP_EQUAL);
            else
                pm.depthWriteEnable(true).depthTestEnable(true).depthCompareOp(VK_COMPARE_OP_GREATER);

            pm.blendBegin(false);
            pm.frontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE);

            pm.rasterizationSamples(vku::sampleCountFlags(msaaSamples));

            pm.dynamicState(VK_DYNAMIC_STATE_VIEWPORT);
            pm.dynamicState(VK_DYNAMIC_STATE_SCISSOR);

            if (handles->hasOutOfOrderRasterization)
                pm.rasterizationOrderAMD(VK_RASTERIZATION_ORDER_RELAXED_AMD);

            return pm.create(handles->device, handles->pipelineCache, layout, renderPass);
        }
    private:
        AssetID vs;
        AssetID fs;
        int msaaSamples = 1;
        bool enablePicking = false;
        bool useSkinningAttributes = false;
        VkCullModeFlags cullFlags = VK_CULL_MODE_BACK_BIT;
    };

    struct StandardPushConstants {
        uint32_t modelMatrixIdx;
        uint32_t materialIdx;
        uint32_t vpIdx;
        uint32_t objectId;

        glm::vec3 cubemapExt;
        uint32_t skinningOffset;
        glm::vec3 cubemapPos;
        float cubemapBoost;

        glm::vec4 texScaleOffset;

        glm::ivec3 screenSpacePickPos;
        uint32_t cubemapIdx;
    };

    struct SkyboxPushConstants {
        // (x: vp index, y: cubemap index)
        glm::ivec4 ubIndices;
    };

    struct PickingBuffer {
        uint32_t objectID;
    };

    struct LineVert {
        glm::vec3 pos;
        glm::vec4 col;
    };

    RenderPass::RenderPass(VulkanHandles* handles) : handles(handles) {}
    RenderPass::~RenderPass() {}

    void PolyRenderPass::updateDescriptorSet(RenderContext& ctx, size_t dsIdx, vku::DescriptorSetUpdater& updater) {
        VkDescriptorSet ds = descriptorSets[dsIdx];
        auto& texSlots = ctx.resources.textures;
        auto& cubemapSlots = ctx.resources.cubemaps;
        updater.beginDescriptorSet(ds);

        updater.beginBuffers(0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        updater.buffer(ctx.resources.vpMatrixBuffer->buffer(), 0, sizeof(MultiVP));

        updater.beginBuffers(1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        updater.buffer(lightsUB.buffer(), 0, sizeof(LightUB));

        updater.beginBuffers(2, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        updater.buffer(ctx.resources.materialBuffer->buffer(), 0, sizeof(MaterialsUB));

        updater.beginBuffers(3, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        updater.buffer(modelMatrixUB[dsIdx].buffer(), 0, sizeof(ModelMatrices));

        for (uint32_t i = 0; i < texSlots.size(); i++) {
            if (texSlots.isSlotPresent(i)) {
                updater.beginImages(4, i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                updater.image(albedoSampler, texSlots[i].imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }

        updater.beginImages(5, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        updater.image(shadowSampler, ctx.resources.shadowCascades->image().imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        for (uint32_t i = 0; i < cubemapSlots.size(); i++) {
            if (cubemapSlots.isSlotPresent(i)) {
                updater.beginImages(6, i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                updater.image(albedoSampler, cubemapSlots[i].imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }

        updater.beginImages(7, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        updater.image(albedoSampler, ctx.resources.brdfLut->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        for (int i = 0; i < NUM_SHADOW_LIGHTS; i++) {
            updater.beginImages(8, i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            updater.image(shadowSampler, ctx.resources.additionalShadowImages[i]->image().imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        updater.beginBuffers(9, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        updater.buffer(lightTileInfoBuffer.buffer(), 0, sizeof(LightTileInfoBuffer));

        updater.beginBuffers(10, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        updater.buffer(lightTileLightCountBuffer.buffer(), 0, sizeof(uint32_t) * numLightTiles);

        updater.beginBuffers(11, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        updater.buffer(lightTilesBuffer.buffer(), 0, sizeof(LightingTile) * numLightTiles);

        updater.beginBuffers(12, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        updater.buffer(skinningMatrixUB.buffer(), 0, sizeof(glm::mat4) * 512);

        updater.beginBuffers(13, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        updater.buffer(pickingBuffer.buffer(), 0, sizeof(PickingBuffer));

        updater.beginImages(14, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        updater.image(albedoSampler, ctx.resources.blueNoiseTexture->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    void PolyRenderPass::updateDescriptorSets(RenderContext& ctx) {
        ZoneScoped;
        vku::DescriptorSetUpdater updater(10 * descriptorSets.size(), 200 * descriptorSets.size(), 0);

        for (size_t i = 0; i < descriptorSets.size(); i++) {
            updateDescriptorSet(ctx, i, updater);
        }

        if (!updater.ok())
            fatalErr("updater was not ok");

        updater.update(handles->device);

        dsUpdateNeeded = false;
    }

    PolyRenderPass::PolyRenderPass(
        VulkanHandles* handles,
        RenderResource* depthStencilImage,
        RenderResource* polyImage,
        RenderResource* bloomTarget,
        bool enablePicking)
        : RenderPass(handles)
        , depthResource(depthStencilImage)
        , colourResource(polyImage)
        , bloomResource(bloomTarget)
        , enablePicking(enablePicking)
        , pickX(0)
        , pickY(0)
        , pickThisFrame(false)
        , awaitingResults(false)
        , setEventNextFrame(false)
        , cullMeshRenderer(nullptr) {

    }

    void PolyRenderPass::setup(RenderContext& ctx, VkDescriptorPool descriptorPool) {
        ZoneScoped;

        int tileSize = LightUB::LIGHT_TILE_SIZE;
        const int xTiles = (ctx.passWidth + (tileSize - 1)) / tileSize;
        const int yTiles = (ctx.passHeight + (tileSize - 1)) / tileSize;
        numLightTiles = xTiles * yTiles;

        if (ctx.passSettings.enableVr)
            numLightTiles *= 2;

        vku::SamplerMaker sm{};
        sm.magFilter(VK_FILTER_LINEAR).minFilter(VK_FILTER_LINEAR).mipmapMode(VK_SAMPLER_MIPMAP_MODE_LINEAR).anisotropyEnable(true).maxAnisotropy(16.0f).maxLod(VK_LOD_CLAMP_NONE).minLod(0.0f);
        albedoSampler = sm.create(handles->device);

        vku::SamplerMaker ssm{};
        ssm.magFilter(VK_FILTER_LINEAR).minFilter(VK_FILTER_LINEAR).mipmapMode(VK_SAMPLER_MIPMAP_MODE_LINEAR);
        shadowSampler = ssm.create(handles->device);

        AssetID fsID = AssetDB::pathToId("Shaders/standard.frag.spv");
        ShaderReflector reflector { fsID };
        dsl = reflector.createDescriptorSetLayout(handles->device, 0);

        vku::PipelineLayoutMaker plm;
        plm.pushConstantRange(VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(StandardPushConstants));
        plm.descriptorSetLayout(dsl);
        pipelineLayout = plm.create(handles->device);

        lightsUB = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            sizeof(LightUB), VMA_MEMORY_USAGE_CPU_TO_GPU, "Lights");

        lightTileInfoBuffer = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            sizeof(LightTileInfoBuffer), VMA_MEMORY_USAGE_CPU_TO_GPU, "Light Tile Info");

        lightTilesBuffer = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            sizeof(LightingTile) * numLightTiles, VMA_MEMORY_USAGE_GPU_ONLY, "Light Tiles"
        );

        lightTileLightCountBuffer = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            sizeof(uint32_t) * numLightTiles, VMA_MEMORY_USAGE_GPU_ONLY, "Light Tile Light Counts"
        );

        skinningMatrixUB = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            sizeof(glm::mat4) * 512, VMA_MEMORY_USAGE_CPU_TO_GPU, "Skinning Matrices"
        );


        for (int i = 0; i < ctx.maxSimultaneousFrames; i++) {
            modelMatrixUB.push_back(vku::GenericBuffer(
                handles->device, handles->allocator,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                sizeof(ModelMatrices), VMA_MEMORY_USAGE_CPU_TO_GPU, "Model matrices"));
        }

        pickingBuffer = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            sizeof(PickingBuffer), VMA_MEMORY_USAGE_CPU_ONLY, "Picking buffer");

        for (vku::GenericBuffer& matrixUB : modelMatrixUB) {
            modelMatricesMapped.push_back((ModelMatrices*)matrixUB.map(handles->device));
        }
        lightMapped = (LightUB*)lightsUB.map(handles->device);
        lightTileInfoMapped = (LightTileInfoBuffer*)lightTileInfoBuffer.map(handles->device);

        skinningMatricesMapped = (glm::mat4*)skinningMatrixUB.map(handles->device);

        pickEvent = vku::Event{handles->device};

        vku::DescriptorSetMaker dsm;
        for (int i = 0; i < ctx.maxSimultaneousFrames; i++)
            dsm.layout(dsl);
        descriptorSets = dsm.create(handles->device, descriptorPool);

        vku::RenderpassMaker rPassMaker;

        rPassMaker.attachmentBegin(VK_FORMAT_B10G11R11_UFLOAT_PACK32);
        rPassMaker.attachmentLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR);
        rPassMaker.attachmentStoreOp(VK_ATTACHMENT_STORE_OP_STORE); 
        rPassMaker.attachmentSamples(colourResource->image().info().samples);
        rPassMaker.attachmentFinalLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        rPassMaker.attachmentBegin(VK_FORMAT_D32_SFLOAT);
        rPassMaker.attachmentLoadOp(VK_ATTACHMENT_LOAD_OP_LOAD);
        rPassMaker.attachmentStencilLoadOp(VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        rPassMaker.attachmentStoreOp(VK_ATTACHMENT_STORE_OP_DONT_CARE);
        rPassMaker.attachmentSamples(colourResource->image().info().samples);
        rPassMaker.attachmentInitialLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        rPassMaker.attachmentFinalLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        rPassMaker.subpassBegin(VK_PIPELINE_BIND_POINT_GRAPHICS);
        rPassMaker.subpassColorAttachment(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
        rPassMaker.subpassDepthStencilAttachment(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 1);

        // Dependency on the depth prepass
        rPassMaker.dependencyBegin(VK_SUBPASS_EXTERNAL, 0);
        rPassMaker.dependencySrcStageMask(VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        rPassMaker.dependencySrcAccessMask(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        rPassMaker.dependencyDstStageMask(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        rPassMaker.dependencyDstAccessMask(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

        // Dependency on the previous write
        rPassMaker.dependencyBegin(VK_SUBPASS_EXTERNAL, 0);
        rPassMaker.dependencySrcStageMask(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        rPassMaker.dependencyDstStageMask(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        rPassMaker.dependencySrcAccessMask(0);
        rPassMaker.dependencyDstAccessMask(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

        // Dependency for post-processing
        rPassMaker.dependencyBegin(0, VK_SUBPASS_EXTERNAL);
        rPassMaker.dependencySrcStageMask(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        rPassMaker.dependencyDstStageMask(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        rPassMaker.dependencySrcAccessMask(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        rPassMaker.dependencyDstAccessMask(VK_ACCESS_SHADER_READ_BIT);

        vku::RenderpassMaker depthPassMaker;

        depthPassMaker.attachmentBegin(VK_FORMAT_D32_SFLOAT);
        depthPassMaker.attachmentLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR);
        depthPassMaker.attachmentStencilLoadOp(VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        depthPassMaker.attachmentStoreOp(VK_ATTACHMENT_STORE_OP_STORE);
        depthPassMaker.attachmentSamples(colourResource->image().info().samples);
        depthPassMaker.attachmentFinalLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        depthPassMaker.subpassBegin(VK_PIPELINE_BIND_POINT_GRAPHICS);
        depthPassMaker.subpassDepthStencilAttachment(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0);

        depthPassMaker.dependencyBegin(0, VK_SUBPASS_EXTERNAL);
        depthPassMaker.dependencyDstStageMask(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        depthPassMaker.dependencyDstAccessMask(VK_ACCESS_SHADER_READ_BIT);
        depthPassMaker.dependencySrcStageMask(VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        depthPassMaker.dependencySrcAccessMask(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        // AMD driver bug workaround: shaders that use ViewIndex without a multiview renderpass
        // will crash the driver, so we always set up a renderpass with multiview even if it's only
        // one view.
        VkRenderPassMultiviewCreateInfo renderPassMultiviewCI{ VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO };
        uint32_t viewMasks[2] = { 0b00000001, 0b00000001 };
        uint32_t correlationMask = 0b00000001;

        if (ctx.passSettings.enableVr) {
            viewMasks[0] = 0b00000011;
            viewMasks[1] = 0b00000011;
            correlationMask = 0b00000011;
        }

        renderPassMultiviewCI.subpassCount = 1;
        renderPassMultiviewCI.pViewMasks = viewMasks;
        renderPassMultiviewCI.correlationMaskCount = 1;
        renderPassMultiviewCI.pCorrelationMasks = &correlationMask;
        rPassMaker.setPNext(&renderPassMultiviewCI);
        depthPassMaker.setPNext(&renderPassMultiviewCI);

        renderPass = rPassMaker.create(handles->device);
        depthPass = depthPassMaker.create(handles->device);

        vku::GenericImage& colourImage = colourResource->image();
        vku::GenericImage& depthImage = depthResource->image();

        VkImageView attachments[2] = { colourImage.imageView(), depthImage.imageView() };

        auto extent = colourImage.info().extent;
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.attachmentCount = 2;
        fci.pAttachments = attachments;
        fci.width = extent.width;
        fci.height = extent.height;
        fci.renderPass = renderPass;
        fci.layers = 1;

        VKCHECK(vku::createFramebuffer(handles->device, &fci, &renderFb));

        VkImageView depthAttachment = depthImage.imageView();
        fci.attachmentCount = 1;
        fci.pAttachments = &depthAttachment;
        fci.renderPass = depthPass;

        VKCHECK(vku::createFramebuffer(handles->device, &fci, &depthFb));

        AssetID vsID = AssetDB::pathToId("Shaders/standard.vert.spv");
        vertexShader = ShaderCache::getModule(handles->device, vsID);
        fragmentShader = ShaderCache::getModule(handles->device, fsID);

        {
            StandardPipelineMaker pm{ vsID, fsID };
            pm
                .setMsaaSamples(ctx.passSettings.msaaLevel)
                .setPickingEnabled(enablePicking);
            pipeline = pm.createPipeline(handles, pipelineLayout, renderPass);
        }

        {
            AssetID skinnedVSID = AssetDB::pathToId("Shaders/standard_skinned.vert.spv");
            StandardPipelineMaker pm{ skinnedVSID, fsID };
            pm
                .setMsaaSamples(ctx.passSettings.msaaLevel)
                .setPickingEnabled(enablePicking)
                .setUseSkinningAttributes(true);
            skinnedPipeline = pm.createPipeline(handles, pipelineLayout, renderPass);
        }

        {
            StandardPipelineMaker pm{ vsID, fsID };
            pm
                .setMsaaSamples(ctx.passSettings.msaaLevel)
                .setPickingEnabled(enablePicking)
                .setCullMode(VK_CULL_MODE_NONE);
            noBackfaceCullPipeline = pm.createPipeline(handles, pipelineLayout, renderPass);
        }

        {
            AssetID wvsID = AssetDB::pathToId("Shaders/wire_obj.vert.spv");
            AssetID wfsID = AssetDB::pathToId("Shaders/wire_obj.frag.spv");
            wireVertexShader = ShaderCache::getModule(handles->device, wvsID);
            wireFragmentShader = ShaderCache::getModule(handles->device, wfsID);

            vku::PipelineMaker pm{ extent.width, extent.height };
            pm.shader(VK_SHADER_STAGE_FRAGMENT_BIT, wireFragmentShader);
            pm.shader(VK_SHADER_STAGE_VERTEX_BIT, wireVertexShader);
            pm.depthWriteEnable(true).depthTestEnable(true).depthCompareOp(VK_COMPARE_OP_GREATER);
            pm.vertexBinding(0, (uint32_t)sizeof(Vertex));
            pm.vertexAttribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, position));
            pm.vertexAttribute(1, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(Vertex, uv));
            pm.polygonMode(VK_POLYGON_MODE_LINE);
            pm.lineWidth(2.0f);

            VkPipelineMultisampleStateCreateInfo pmsci{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            pmsci.rasterizationSamples = vku::sampleCountFlags(ctx.passSettings.msaaLevel);
            pm.multisampleState(pmsci);

            pm.dynamicState(VK_DYNAMIC_STATE_VIEWPORT);
            pm.dynamicState(VK_DYNAMIC_STATE_SCISSOR);

            vku::PipelineLayoutMaker plm;
            plm.pushConstantRange(VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(StandardPushConstants));
            plm.descriptorSetLayout(dsl);
            wireframePipelineLayout = plm.create(handles->device);

            wireframePipeline = pm.create(handles->device, handles->pipelineCache, wireframePipelineLayout, renderPass);
        }

        dbgLinesPass = new DebugLinesPass(handles);
        dbgLinesPass->setup(ctx, renderPass, descriptorPool);

        skyboxPass = new SkyboxPass(handles);
        skyboxPass->setup(ctx, renderPass, descriptorPool);

        depthPrepass = new DepthPrepass(handles);
        depthPrepass->setup(ctx, depthPass, pipelineLayout);

        uiPass = new WorldSpaceUIPass(handles);
        uiPass->setup(ctx, renderPass, descriptorPool);

        lightCullPass = new LightCullPass(handles, depthResource);
        lightCullPass->setup(ctx, lightsUB.buffer(), lightTileInfoBuffer.buffer(), lightTilesBuffer.buffer(), lightTileLightCountBuffer.buffer(), descriptorPool);

        mainPass = new MainPass(handles, pipelineLayout);

        bloomPass = new BloomRenderPass(handles, colourResource, bloomResource);
        bloomPass->setup(ctx, descriptorPool);

        updateDescriptorSets(ctx);

        if (ctx.passSettings.enableVr) {
            cullMeshRenderer = new VRCullMeshRenderer{ handles };
            cullMeshRenderer->setup(ctx, depthPass, descriptorPool);
        }

        VKCHECK(vkSetEvent(handles->device, pickEvent));
    }

    void PolyRenderPass::resizeInternalBuffers(RenderContext& ctx) {
        DeletionQueue::queueObjectDeletion(renderFb.release(), VK_OBJECT_TYPE_FRAMEBUFFER);
        DeletionQueue::queueObjectDeletion(depthFb.release(), VK_OBJECT_TYPE_FRAMEBUFFER);
        vku::GenericImage& colourImage = colourResource->image();
        vku::GenericImage& depthImage = depthResource->image();

        VkImageView attachments[2] = { colourImage.imageView(), depthImage.imageView() };
        auto extent = colourImage.info().extent;
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.attachmentCount = 2;
        fci.pAttachments = attachments;
        fci.width = extent.width;
        fci.height = extent.height;
        fci.renderPass = renderPass;
        fci.layers = 1;

        VKCHECK(vku::createFramebuffer(handles->device, &fci, &renderFb));

        VkImageView depthAttachment = depthImage.imageView();
        fci.attachmentCount = 1;
        fci.pAttachments = &depthAttachment;
        fci.renderPass = depthPass;

        VKCHECK(vku::createFramebuffer(handles->device, &fci, &depthFb));

        int tileSize = LightUB::LIGHT_TILE_SIZE;
        const int xTiles = (ctx.passWidth + (tileSize - 1)) / tileSize;
        const int yTiles = (ctx.passHeight + (tileSize - 1)) / tileSize;
        numLightTiles = xTiles * yTiles;

        if (ctx.passSettings.enableVr)
            numLightTiles *= 2;

        lightTilesBuffer = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            sizeof(LightingTile) * numLightTiles, VMA_MEMORY_USAGE_GPU_ONLY, "Light Tiles"
        );

        lightTileLightCountBuffer = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            sizeof(uint32_t) * numLightTiles, VMA_MEMORY_USAGE_GPU_ONLY, "Light Tile Light Counts"
        );

        updateDescriptorSets(ctx);

        bloomPass->resizeInternalBuffers(ctx);
        lightCullPass->changeLightTileBuffers(ctx, lightTilesBuffer.buffer(), lightTileLightCountBuffer.buffer());
        lightCullPass->resizeInternalBuffers(ctx);
    }

    glm::mat4 getBoneTransform(LoadedMeshData& meshData, Pose& pose, int boneIdx) {
        glm::mat4 transform = pose.boneTransforms[boneIdx];

        uint32_t parentId = meshData.meshBones[boneIdx].parentIdx;

        while (parentId != ~0u) {
            transform = pose.boneTransforms[parentId] * transform;
            parentId = meshData.meshBones[parentId].parentIdx;
        }

        return transform;
    }

    void updateSkinningMatrices(LoadedMeshData& meshData, Pose& pose, glm::mat4* skinningMatricesMapped, int skinningOffset) {
        for (int i = 0; i < meshData.meshBones.size(); i++) {
            skinningMatricesMapped[i + skinningOffset] = getBoneTransform(meshData, pose, i) * meshData.meshBones[i].inverseBindPose;
        }
    }

    void PolyRenderPass::generateDrawInfo(RenderContext& ctx) {
        ZoneScoped;

        Frustum frustum;
        frustum.fromVPMatrix(ctx.projMatrices[0] * ctx.viewMatrices[0]);

        Frustum frustumB;
        if (ctx.passSettings.enableVr) {
            frustumB.fromVPMatrix(ctx.projMatrices[1] * ctx.viewMatrices[1]);
        }

        auto& resources = ctx.resources;
        auto& sceneSettings = ctx.registry.ctx<SceneSettings>();
        // skybox should always be loaded
        // if it isn't, something has already gone terribly wrong
        uint32_t skyboxId = ctx.resources.cubemaps.loadOrGet(sceneSettings.skybox);

        drawInfo.clear();

        int matrixIdx = 0;
        bool warned = false;
        ctx.registry.view<Transform, WorldObject>().each([&](entt::entity ent, Transform& t, WorldObject& wo) {
            if (matrixIdx == ModelMatrices::SIZE - 1) {
                if (!warned) {
                    logWarn("Out of model matrices!");
                    warned = true;
                }
                return;
            }

            auto meshPos = resources.meshes.find(wo.mesh);

            if (meshPos == resources.meshes.end()) {
                // Haven't loaded the mesh yet
                matrixIdx++;
                logWarn(WELogCategoryRender, "Missing mesh");
                return;
            }

            float maxScale = glm::max(t.scale.x, glm::max(t.scale.y, t.scale.z));
            if (!ctx.passSettings.enableVr) {
                if (!frustum.containsSphere(t.position, meshPos->second.sphereRadius * maxScale)) {
                    ctx.debugContext.stats->numCulledObjs++;
                    return;
                }

                glm::vec3 aabbMin{FLT_MAX};
                glm::vec3 aabbMax{-FLT_MAX};

                glm::vec3 mi = meshPos->second.aabbMin * t.scale;
                glm::vec3 ma = meshPos->second.aabbMax * t.scale;

                glm::vec3 points[] = {
                    mi,
                    glm::vec3(ma.x, mi.y, mi.z),
                    glm::vec3(mi.x, ma.y, mi.z),
                    glm::vec3(ma.x, ma.y, mi.z),
                    glm::vec3(mi.x, mi.y, ma.z),
                    glm::vec3(ma.x, mi.y, ma.z),
                    glm::vec3(mi.x, ma.y, ma.z),
                    glm::vec3(ma.x, ma.y, ma.z)
                };

                for (int i = 0; i < 8; i++) {
                    glm::vec3 p = t.transformPoint(points[i]);
                    aabbMin = glm::min(aabbMin, p);
                    aabbMax = glm::max(aabbMax, p);
                }

                if (!frustum.containsAABB(aabbMin, aabbMax)) {
                    ctx.debugContext.stats->numCulledObjs++;
                    return;
                }
            } else {
                if (!frustum.containsSphere(t.position, meshPos->second.sphereRadius * maxScale) &&
                    !frustumB.containsSphere(t.position, meshPos->second.sphereRadius * maxScale)) {
                    ctx.debugContext.stats->numCulledObjs++;
                    return;
                }
            }

            modelMatricesMapped[ctx.frameIndex]->modelMatrices[matrixIdx] = t.getMatrix();

            for (int i = 0; i < meshPos->second.numSubmeshes; i++) {
                auto& currSubmesh = meshPos->second.submeshes[i];

                SubmeshDrawInfo sdi{};
                sdi.ib = meshPos->second.ib.buffer();
                sdi.vb = meshPos->second.vb.buffer();
                sdi.indexCount = currSubmesh.indexCount;
                sdi.indexOffset = currSubmesh.indexOffset;
                if (wo.presentMaterials[i])
                    sdi.materialIdx = ctx.resources.materials.get(wo.materials[i]);
                else
                    sdi.materialIdx = ctx.resources.materials.get(wo.materials[0]);
                sdi.matrixIdx = matrixIdx;
                sdi.texScaleOffset = wo.texScaleOffset;
                sdi.ent = ent;
                auto& packedMat = resources.materials[sdi.materialIdx];
                sdi.opaque = packedMat.getCutoff() < 0.004f;

                switch (wo.uvOverride) {
                default:
                    sdi.drawMiscFlags = 0;
                    break;
                case UVOverride::XY:
                    sdi.drawMiscFlags = ShaderFlags::MISC_FLAG_UV_XY;
                    break;
                case UVOverride::XZ:
                    sdi.drawMiscFlags = ShaderFlags::MISC_FLAG_UV_XZ;
                    break;
                case UVOverride::ZY:
                    sdi.drawMiscFlags = ShaderFlags::MISC_FLAG_UV_ZY;
                    break;
                case UVOverride::PickBest:
                    sdi.drawMiscFlags = ShaderFlags::MISC_FLAG_UV_PICK;
                    break;
                }

                if (ctx.registry.has<EditorGlow>(ent)) {
                    sdi.drawMiscFlags |= ShaderFlags::MISC_FLAG_SELECTION_GLOW;
                }

                uint32_t currCubemapIdx = skyboxId;
                int lastPriority = INT32_MIN;

                ctx.registry.view<WorldCubemap, Transform>().each([&](WorldCubemap& wc, Transform& cubeT) {
                    glm::vec3 cPos = t.position;
                    glm::vec3 ma = wc.extent + cubeT.position;
                    glm::vec3 mi = cubeT.position - wc.extent;

                    if (cPos.x < ma.x && cPos.x > mi.x &&
                        cPos.y < ma.y && cPos.y > mi.y &&
                        cPos.z < ma.z && cPos.z > mi.z && wc.priority > lastPriority) {
                        currCubemapIdx = resources.cubemaps.get(wc.cubemapId);
                        sdi.cubemapPos = cubeT.position;
                        sdi.cubemapExt = wc.extent;
                        if (wc.cubeParallax) {
                            sdi.drawMiscFlags |= ShaderFlags::MISC_FLAG_CUBEMAP_PARALLAX; // flag for cubemap parallax correction
                        }
                        lastPriority = wc.priority;
                    }
                    });

                sdi.cubemapIdx = currCubemapIdx;

                auto& extraDat = resources.materials.getExtraDat(sdi.materialIdx);

                sdi.pipeline = pipeline;

                if (extraDat.noCull) {
                    sdi.pipeline = noBackfaceCullPipeline;
                } else if (extraDat.wireframe || showWireframe.getInt() == 1) {
                    sdi.pipeline = wireframePipeline;
                    sdi.dontPrepass = true;
                } else if (ctx.registry.has<UseWireframe>(ent) || showWireframe.getInt() == 2) {
                    drawInfo.add(sdi);
                    //ctx.debugContext.stats->numTriangles += currSubmesh.indexCount / 3;
                    sdi.pipeline = wireframePipeline;
                    sdi.dontPrepass = true;
                }
                //ctx.debugContext.stats->numTriangles += currSubmesh.indexCount / 3;

                drawInfo.add(std::move(sdi));
            }

            matrixIdx++;
            });

        int skinningOffset = 0;
        ctx.registry.view<Transform, SkinnedWorldObject>().each([&](entt::entity ent, Transform& t, SkinnedWorldObject& wo) {
            auto meshPos = resources.meshes.find(wo.mesh);

            if (matrixIdx == ModelMatrices::SIZE - 1) {
                if (!warned) {
                    logWarn("Out of model matrices!");
                    warned = true;
                }
                return;
            }

            if (meshPos == resources.meshes.end()) {
                // Haven't loaded the mesh yet
                logWarn(WELogCategoryRender, "Missing mesh");
                return;
            }

            updateSkinningMatrices(meshPos->second, wo.currentPose, skinningMatricesMapped, skinningOffset);

            modelMatricesMapped[ctx.frameIndex]->modelMatrices[matrixIdx] = t.getMatrix();

            //float maxScale = glm::max(t.scale.x, glm::max(t.scale.y, t.scale.z));
            //if (!ctx.passSettings.enableVR) {
            //    if (!frustum.containsSphere(t.position, meshPos->second.sphereRadius * maxScale)) {
            //        ctx.debugContext.stats->numCulledObjs++;
            //        return;
            //    }
            //} else {
            //    if (!frustum.containsSphere(t.position, meshPos->second.sphereRadius * maxScale) &&
            //        !frustumB.containsSphere(t.position, meshPos->second.sphereRadius * maxScale)) {
            //        ctx.debugContext.stats->numCulledObjs++;
            //        return;
            //    }
            //}

            for (int i = 0; i < meshPos->second.numSubmeshes; i++) {
                auto& currSubmesh = meshPos->second.submeshes[i];

                SubmeshDrawInfo sdi{};
                sdi.ib = meshPos->second.ib.buffer();
                sdi.vb = meshPos->second.vb.buffer();
                sdi.indexCount = currSubmesh.indexCount;
                sdi.indexOffset = currSubmesh.indexOffset;
                sdi.materialIdx = ctx.resources.materials.get(wo.materials[i]);
                sdi.matrixIdx = matrixIdx;
                sdi.texScaleOffset = wo.texScaleOffset;
                sdi.ent = ent;
                sdi.skinned = true;
                sdi.boneVB = meshPos->second.vertexSkinWeights.buffer();
                sdi.boneMatrixOffset = skinningOffset;
                auto& packedMat = resources.materials[sdi.materialIdx];
                sdi.opaque = packedMat.getCutoff() == 0.0f;

                switch (wo.uvOverride) {
                default:
                    sdi.drawMiscFlags = 0;
                    break;
                case UVOverride::XY:
                    sdi.drawMiscFlags = ShaderFlags::MISC_FLAG_UV_XY;
                    break;
                case UVOverride::XZ:
                    sdi.drawMiscFlags = ShaderFlags::MISC_FLAG_UV_XZ;
                    break;
                case UVOverride::ZY:
                    sdi.drawMiscFlags = ShaderFlags::MISC_FLAG_UV_ZY;
                    break;
                case UVOverride::PickBest:
                    sdi.drawMiscFlags = ShaderFlags::MISC_FLAG_UV_PICK;
                    break;
                }

                if (ctx.registry.has<EditorGlow>(ent)) {
                    sdi.drawMiscFlags |= ShaderFlags::MISC_FLAG_SELECTION_GLOW;
                }

                uint32_t currCubemapIdx = skyboxId;
                int lastPriority = INT32_MIN;

                ctx.registry.view<WorldCubemap, Transform>().each([&](WorldCubemap& wc, Transform& cubeT) {
                    glm::vec3 cPos = t.position;
                    glm::vec3 ma = wc.extent + cubeT.position;
                    glm::vec3 mi = cubeT.position - wc.extent;

                    if (cPos.x < ma.x && cPos.x > mi.x &&
                        cPos.y < ma.y && cPos.y > mi.y &&
                        cPos.z < ma.z && cPos.z > mi.z && wc.priority > lastPriority) {
                        currCubemapIdx = resources.cubemaps.get(wc.cubemapId);
                        if (wc.cubeParallax) {
                            sdi.drawMiscFlags |= ShaderFlags::MISC_FLAG_CUBEMAP_PARALLAX; // flag for cubemap parallax correction
                            sdi.cubemapPos = cubeT.position;
                            sdi.cubemapExt = wc.extent;
                        }
                        lastPriority = wc.priority;
                    }
                    });

                sdi.cubemapIdx = currCubemapIdx;
                sdi.pipeline = skinnedPipeline;

                //ctx.debugContext.stats->numTriangles += currSubmesh.indexCount / 3;

                drawInfo.add(std::move(sdi));
            }
            skinningOffset += meshPos->second.meshBones.size();
            matrixIdx++;

            });
    }

    void PolyRenderPass::prePass(RenderContext& ctx) {
        ZoneScoped;

        Frustum frustum;
        frustum.fromVPMatrix(ctx.projMatrices[0] * ctx.viewMatrices[0]);

        Frustum frustumB;
        if (ctx.passSettings.enableVr) {
            frustumB.fromVPMatrix(ctx.projMatrices[1] * ctx.viewMatrices[1]);
        }

        int lightIdx = 0;
        ctx.registry.view<WorldLight, Transform>().each([&](auto ent, WorldLight& l, Transform& transform) {
            l.lightIdx = ~0u;

            // Don't overrun the end of the buffer, just give up if there are too many lights
            if (lightIdx >= LightUB::MAX_LIGHTS - 1) {
                return;
            }

            // If the light isn't enabled, just early out
            if (!l.enabled) return;

            // Do a coarse cull stage against the frustum
            float distance = l.maxDistance;
            if (l.type != LightType::Directional) {
                bool inFrustum = frustum.containsSphere(transform.position, distance);

                if (ctx.passSettings.enableVr)
                    inFrustum |= frustumB.containsSphere(transform.position, distance);
                if (!inFrustum) {
                    return;
                }
            }

            // Convert the color of the light to linear space
            glm::vec3 colLinear = glm::pow(l.color, glm::vec3(2.2));

            // Find the forward direction of the light in world space
            glm::vec3 lightForward = glm::normalize(transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f));
            if (l.type != LightType::Tube) {
                // Set up the light in the correct position in the buffer
                lightMapped->lights[lightIdx] = PackedLight{
                    colLinear * l.intensity, 0,
                    glm::vec4(lightForward, l.type == LightType::Sphere ? l.spotCutoff : glm::cos(l.spotCutoff)),
                    transform.position,
                    distance
                };
                lightMapped->lights[lightIdx].setLightType(l.type);
                lightMapped->lights[lightIdx].setShadowmapIndex(l.shadowmapIdx);
            } else {
                // Tube lights use the struct layout slightly differently
                glm::vec3 tubeP0 = transform.position + lightForward * l.tubeLength;
                glm::vec3 tubeP1 = transform.position - lightForward * l.tubeLength;
                lightMapped->lights[lightIdx] = PackedLight{
                    colLinear * l.intensity, 0,
                    glm::vec4(tubeP0, l.tubeRadius),
                    tubeP1,
                    distance
                };
                lightMapped->lights[lightIdx].setLightType(l.type);
                lightMapped->lights[lightIdx].setShadowmapIndex(~0u);
            }

            // If shadows are enabled, construct the appropriate
            // matrices and put those in the buffer too
            if (l.enableShadows && l.shadowmapIdx != ~0u) {
                Camera shadowCam;
                shadowCam.position = transform.position;
                shadowCam.rotation = transform.rotation;
                shadowCam.near = l.shadowNear;
                shadowCam.far = l.shadowFar;
                float fov = l.spotCutoff * 2.0f;
                shadowCam.verticalFOV = fov;
                lightMapped->additionalShadowMatrices[l.shadowmapIdx] = shadowCam.getProjectMatrixNonInfinite(1.0f) * shadowCam.getViewMatrix();
            }

            // Set indices and increment for the next light
            l.lightIdx = lightIdx;
            lightIdx++;
        });

        int tileSize = LightUB::LIGHT_TILE_SIZE;
        const int xTiles = (ctx.passWidth + (tileSize - 1)) / tileSize;
        const int yTiles = (ctx.passHeight + (tileSize - 1)) / tileSize;
        const int totalTiles = xTiles * yTiles;

        lightTileInfoMapped->tileSize = tileSize;
        lightTileInfoMapped->tilesPerEye = xTiles * yTiles;
        lightTileInfoMapped->numTilesX = xTiles;
        lightTileInfoMapped->numTilesY = yTiles;

        int realTotalTiles = totalTiles;

        if (ctx.passSettings.enableVr)
            realTotalTiles *= 2;

        if (realTotalTiles > MAX_LIGHT_TILES)
            fatalErr("Too many lighting tiles");

        lightMapped->lightCount = lightIdx;
        for (int i = 0; i < 4; i++) {
            lightMapped->cascadeTexelsPerUnit[i] = ctx.cascadeInfo.texelsPerUnit[i];
            lightMapped->shadowmapMatrices[i] = ctx.cascadeInfo.matrices[i];
        }
        ctx.debugContext.stats->numLightsInView = lightIdx;

        uint32_t aoBoxIdx = 0;
        ctx.registry.sort<ProxyAOComponent>([&](const entt::entity a, const entt::entity b) {
            const Transform& ta = ctx.registry.get<Transform>(a);
            const Transform& tb = ctx.registry.get<Transform>(b);
            return glm::length2(ta.position - ctx.camera.position) < glm::length2(tb.position - ctx.camera.position);
        });

        ctx.registry.view<ProxyAOComponent, Transform>().each([&](auto ent, ProxyAOComponent& pac, Transform& t) {
            if (aoBoxIdx >= 128) return;
            glm::vec3 aabbMin{FLT_MAX};
            glm::vec3 aabbMax{-FLT_MAX};

            glm::vec3 mi = -pac.bounds;
            glm::vec3 ma = pac.bounds;

            glm::vec3 points[] = {
                mi,
                glm::vec3(ma.x, mi.y, mi.z),
                glm::vec3(mi.x, ma.y, mi.z),
                glm::vec3(ma.x, ma.y, mi.z),
                glm::vec3(mi.x, mi.y, ma.z),
                glm::vec3(ma.x, mi.y, ma.z),
                glm::vec3(mi.x, ma.y, ma.z),
                glm::vec3(ma.x, ma.y, ma.z)
            };

            for (int i = 0; i < 8; i++) {
                glm::vec3 p = t.transformPoint(points[i]);
                aabbMin = glm::min(aabbMin, p);
                aabbMax = glm::max(aabbMax, p);
            }

            if (ctx.passSettings.enableVr) {
                if (!frustum.containsAABB(aabbMin, aabbMax) && !frustumB.containsAABB(aabbMin, aabbMax)) return;
            } else {
                if (!frustum.containsAABB(aabbMin, aabbMax)) return;
            }

            Transform ct = t;
            ct.scale = glm::vec3(1.0f);
            lightMapped->box[aoBoxIdx].setScale(pac.bounds);
            //glm::mat4 tMat = glm::translate(glm::mat4(1.0f), t.position);
            //lightMapped->box[aoBoxIdx].setMatrix(glm::mat4_cast(glm::inverse(t.rotation)) * glm::inverse(tMat));
            lightMapped->box[aoBoxIdx].setMatrix(ct.getMatrix());
            //lightMapped->box[aoBoxIdx].setTranslation(t.position);
            lightMapped->box[aoBoxIdx].setEntityId((uint32_t)ent);
            aoBoxIdx++;
            });
        lightMapped->aoBoxCount = aoBoxIdx;

        uint32_t aoSphereIdx = 0;
        ctx.registry.view<SphereAOProxy, Transform>().each([&](entt::entity entity, SphereAOProxy& sao, Transform& t) {
            lightMapped->sphere[aoSphereIdx].position = t.position;
            lightMapped->sphere[aoSphereIdx].radius = sao.radius;
            lightMapped->sphereIds[aoSphereIdx] = (uint32_t)entity;
            aoSphereIdx++;
            });
        lightMapped->aoSphereCount = aoSphereIdx;

        if (dsUpdateNeeded) {
            // Update descriptor sets to bring in any new textures
            updateDescriptorSets(ctx);
        }

        generateDrawInfo(ctx);

        dbgLinesPass->prePass(ctx);
        skyboxPass->prePass(ctx);
        uiPass->prePass(ctx);
    }

    void PolyRenderPass::execute(RenderContext& ctx) {
        ZoneScoped;
        TracyVkZone((*ctx.debugContext.tracyContexts)[ctx.frameIndex], ctx.cmdBuf, "Polys");

        std::array<VkClearValue, 2> clearValues{
            vku::makeColorClearValue(0.0f, 0.0f, 0.0f, 1.0f),
            vku::makeDepthStencilClearValue(0.0f, 0)
        };

        VkClearValue depthClearValue = vku::makeDepthStencilClearValue(0.0f, 0);

        VkCommandBuffer cmdBuf = ctx.cmdBuf;

        lightsUB.barrier(
            cmdBuf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_DEPENDENCY_BY_REGION_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);

        modelMatrixUB[ctx.frameIndex].barrier(
            cmdBuf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_DEPENDENCY_BY_REGION_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);

        if (pickThisFrame) {
            pickingBuffer.barrier(cmdBuf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT,
                VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);

            PickingBuffer pb;
            pb.objectID = ~0u;
            vkCmdUpdateBuffer(cmdBuf, pickingBuffer.buffer(), 0, sizeof(pb), &pb);

            pickingBuffer.barrier(cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        }

        ctx.resources.shadowCascades->image()
            .barrier(cmdBuf,
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);

        if (setEventNextFrame) {
            vkCmdSetEvent(cmdBuf, pickEvent, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            setEventNextFrame = false;
        }

        VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };

        rpbi.renderPass = depthPass;
        rpbi.framebuffer = depthFb;
        rpbi.renderArea = VkRect2D{ {0, 0}, {ctx.passWidth, ctx.passHeight} };
        rpbi.clearValueCount = 1;
        rpbi.pClearValues = &depthClearValue;

        vkCmdBeginRenderPass(cmdBuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = ctx.passWidth;
        vp.height = ctx.passHeight;
        vkCmdSetViewport(cmdBuf, 0, 1, &vp);

        VkRect2D scissor{};
        scissor.extent.width = ctx.passWidth;
        scissor.extent.height = ctx.passHeight;
        vkCmdSetScissor(cmdBuf, 0, 1, &scissor);

        if (ctx.passSettings.enableVr) {
            cullMeshRenderer->draw(cmdBuf);
        }

        vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets[ctx.frameIndex], 0, nullptr);

        std::sort(drawInfo.begin(), drawInfo.end(), [&](const SubmeshDrawInfo& sdiA, const SubmeshDrawInfo& sdiB) {
            if (sdiA.opaque && !sdiB.opaque)
                return true;
            else if (sdiB.opaque && !sdiA.opaque)
                return false;

            return sdiA.pipeline > sdiB.pipeline;
            });

        if ((int)enableDepthPrepass) {
            ZoneScopedN("Depth prepass");
            depthPrepass->execute(ctx, drawInfo);
            depthResource->image().setCurrentLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }

        vkCmdEndRenderPass(cmdBuf);

        {
            lightTilesBuffer.barrier(cmdBuf,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);

            lightTileLightCountBuffer.barrier(cmdBuf,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);

            lightCullPass->execute(ctx, LightUB::LIGHT_TILE_SIZE);

            lightTilesBuffer.barrier(cmdBuf,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);

            lightTileLightCountBuffer.barrier(cmdBuf,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_DEPENDENCY_BY_REGION_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        }

        rpbi.clearValueCount = (uint32_t)clearValues.size();
        rpbi.pClearValues = clearValues.data();
        rpbi.renderPass = renderPass;
        rpbi.framebuffer = renderFb;

        vkCmdBeginRenderPass(cmdBuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        mainPass->execute(ctx, drawInfo, pickThisFrame, pickX, pickY);

        dbgLinesPass->execute(ctx);
        skyboxPass->execute(ctx);
        uiPass->execute(ctx);

        vkCmdEndRenderPass(cmdBuf);

        colourResource->image().setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        depthResource->image().setCurrentLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        static ConVar forceDisableBloom {"r_forceDisableBloom", "0"};

        if (ctx.passSettings.enableBloom && !forceDisableBloom.getInt()) {
            bloomPass->execute(ctx);
        } else {
            VkClearColorValue clearVal { {.0f, .0f, .0f, .0f} };
            VkImageSubresourceRange subresourceRange {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = ctx.passSettings.enableVr ? 2u : 1u
            };

            bloomResource->image().setLayout(cmdBuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdClearColorImage(cmdBuf, bloomResource->image().image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &subresourceRange);
            bloomResource->image().setLayout(cmdBuf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        }

        if (pickThisFrame) {
            vkCmdResetEvent(cmdBuf, pickEvent, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            pickThisFrame = false;
        }
    }

    void PolyRenderPass::requestEntityPick() {
        if (awaitingResults) return;
        pickThisFrame = true;
        awaitingResults = true;
    }

    bool PolyRenderPass::getPickedEnt(uint32_t* entOut) {
        VkResult pickEvtRes = vkGetEventStatus(handles->device, pickEvent);

        if (pickEvtRes != VK_EVENT_RESET)
            return false;

        PickingBuffer* pickBuf = (PickingBuffer*)pickingBuffer.map(handles->device);
        *entOut = pickBuf->objectID;

        pickingBuffer.unmap(handles->device);

        setEventNextFrame = true;
        awaitingResults = false;

        return true;
    }

    PolyRenderPass::~PolyRenderPass() {
        for (vku::GenericBuffer& matrixUB : modelMatrixUB) {
            matrixUB.unmap(handles->device);
        }

        lightsUB.unmap(handles->device);
        lightTileInfoBuffer.unmap(handles->device);
        skinningMatrixUB.unmap(handles->device);

        delete dbgLinesPass;
        delete skyboxPass;
        delete depthPrepass;
        delete uiPass;
        delete lightCullPass;
        delete mainPass;
        delete bloomPass;

        if (cullMeshRenderer)
            delete cullMeshRenderer;
    }
}
