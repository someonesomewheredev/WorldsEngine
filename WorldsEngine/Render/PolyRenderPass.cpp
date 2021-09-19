#include "../Core/Engine.hpp"
#include "ImGui/imgui_internal.h"
#include "ImGui/imgui.h"
#include "RenderPasses.hpp"
#include "../Core/Transform.hpp"
#include <openvr.h>
#include "tracy/Tracy.hpp"
#include "Render.hpp"
#include "../Physics/Physics.hpp"
#include "Frustum.hpp"
#include "../Core/Console.hpp"
#include "ShaderCache.hpp"
#include <slib/StaticAllocList.hpp>
#include <Libs/IconsFontAwesome5.h>
#include <Util/MatUtil.hpp>

namespace worlds {
    ConVar showWireframe("r_wireframeMode", "0", "0 - No wireframe; 1 - Wireframe only; 2 - Wireframe + solid");
    ConVar dbgDrawMode("r_dbgDrawMode", "0", "0 = Normal, 1 = Normals, 2 = Metallic, 3 = Roughness, 4 = AO");

    struct StandardPushConstants {
        uint32_t modelMatrixIdx;
        uint32_t materialIdx;
        uint32_t vpIdx;
        uint32_t objectId;

        glm::vec4 cubemapExt;
        glm::vec4 cubemapPos;

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

    struct PickBufCSPushConstants {
        uint32_t clearObjId;
        uint32_t doPicking;
    };

    struct LineVert {
        glm::vec3 pos;
        glm::vec4 col;
    };

    void PolyRenderPass::updateDescriptorSets(RenderContext& ctx) {
        ZoneScoped;
        auto& texSlots = ctx.resources.textures;
        auto& cubemapSlots = ctx.resources.cubemaps;
        vku::DescriptorSetUpdater updater(20, 256, 0);
        size_t i = 0;
        for (VkDescriptorSet& ds : descriptorSets) {
            updater.beginDescriptorSet(ds);

            updater.beginBuffers(0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            updater.buffer(ctx.resources.vpMatrixBuffer->buffer(), 0, sizeof(MultiVP));

            updater.beginBuffers(1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            updater.buffer(lightsUB.buffer(), 0, sizeof(LightUB));

            updater.beginBuffers(2, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            updater.buffer(ctx.resources.materialBuffer->buffer(), 0, sizeof(MaterialsUB));

            updater.beginBuffers(3, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            updater.buffer(modelMatrixUB[i].buffer(), 0, sizeof(ModelMatrices));

            for (uint32_t i = 0; i < texSlots.size(); i++) {
                if (texSlots.isSlotPresent(i)) {
                    updater.beginImages(4, i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
                    updater.image(albedoSampler, texSlots[i].imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }

            updater.beginImages(5, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            updater.image(shadowSampler, ctx.resources.shadowCascades->image.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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
                updater.image(shadowSampler, ctx.resources.additionalShadowImages[i]->image.imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }

            updater.beginBuffers(9, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            updater.buffer(lightTileBuffer.buffer(), 0, sizeof(LightTileBuffer));

            updater.beginBuffers(10, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            updater.buffer(pickingBuffer.buffer(), 0, sizeof(PickingBuffer));

            i++;
        }
        if (!updater.ok())
            fatalErr("updater was not ok");

        updater.update(handles->device);


        dsUpdateNeeded = false;
    }

    PolyRenderPass::PolyRenderPass(
        VulkanHandles* handles,
        RenderTexture* depthStencilImage,
        RenderTexture* polyImage,
        bool enablePicking)
        : depthStencilImage(depthStencilImage)
        , polyImage(polyImage)
        , enablePicking(enablePicking)
        , pickX(0)
        , pickY(0)
        , pickThisFrame(false)
        , awaitingResults(false)
        , setEventNextFrame(false)
        , cullMeshRenderer(nullptr)
        , handles(handles) {

    }

    static ConVar enableDepthPrepass("r_depthPrepass", "1");
    static ConVar enableParallaxMapping("r_doParallaxMapping", "0");
    static ConVar maxParallaxLayers("r_maxParallaxLayers", "32");
    static ConVar minParallaxLayers("r_minParallaxLayers", "4");

    void setupVertexFormat(vku::PipelineMaker& pm) {
        pm.vertexBinding(0, (uint32_t)sizeof(Vertex));
        pm.vertexAttribute(0, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, position));
        pm.vertexAttribute(1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, normal));
        pm.vertexAttribute(2, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)offsetof(Vertex, tangent));
        pm.vertexAttribute(3, 0, VK_FORMAT_R32_SFLOAT, (uint32_t)offsetof(Vertex, bitangentSign));
        pm.vertexAttribute(4, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(Vertex, uv));
    }

    void PolyRenderPass::setup(RenderContext& ctx, VkDescriptorPool descriptorPool) {
        ZoneScoped;

        vku::SamplerMaker sm{};
        sm.magFilter(VK_FILTER_LINEAR).minFilter(VK_FILTER_LINEAR).mipmapMode(VK_SAMPLER_MIPMAP_MODE_LINEAR).anisotropyEnable(true).maxAnisotropy(16.0f).maxLod(VK_LOD_CLAMP_NONE).minLod(0.0f);
        albedoSampler = sm.create(handles->device);

        vku::SamplerMaker ssm{};
        ssm.magFilter(VK_FILTER_LINEAR).minFilter(VK_FILTER_LINEAR).mipmapMode(VK_SAMPLER_MIPMAP_MODE_LINEAR).compareEnable(true).compareOp(VK_COMPARE_OP_GREATER);
        shadowSampler = ssm.create(handles->device);

        vku::DescriptorSetLayoutMaker dslm;
        // VP
        dslm.buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 1);
        // Lights
        dslm.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 1);
        // Materials
        dslm.buffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 1);
        // Model matrices
        dslm.buffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 1);
        // Textures
        dslm.image(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, NUM_TEX_SLOTS);
        dslm.bindFlag(4, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
        // Shadowmap
        dslm.image(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1);
        // Cubemaps
        dslm.image(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, NUM_CUBEMAP_SLOTS);
        dslm.bindFlag(6, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
        // BRDF LUT
        dslm.image(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1);
        // Additional shadow images
        dslm.image(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, NUM_SHADOW_LIGHTS);
        // Light tiles
        dslm.buffer(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 1);
        // Picking
        dslm.buffer(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 1);

        dsl = dslm.create(handles->device);

        vku::PipelineLayoutMaker plm;
        plm.pushConstantRange(VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(StandardPushConstants));
        plm.descriptorSetLayout(dsl);
        pipelineLayout = plm.create(handles->device);

        lightsUB = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            sizeof(LightUB), VMA_MEMORY_USAGE_CPU_TO_GPU, "Lights");

        lightTileBuffer = vku::GenericBuffer(
            handles->device, handles->allocator,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            sizeof(LightTileBuffer), VMA_MEMORY_USAGE_CPU_TO_GPU, "Light Tiles");

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
        lightTilesMapped = (LightTileBuffer*)lightTileBuffer.map(handles->device);

        VkEventCreateInfo eci{ VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
        vkCreateEvent(handles->device, &eci, nullptr, &pickEvent);

        vku::DescriptorSetMaker dsm;
        dsm.layout(dsl);
        dsm.layout(dsl);
        descriptorSets = dsm.create(handles->device, descriptorPool);

        vku::RenderpassMaker rPassMaker;

        rPassMaker.attachmentBegin(VK_FORMAT_B10G11R11_UFLOAT_PACK32);
        rPassMaker.attachmentLoadOp(VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        rPassMaker.attachmentStoreOp(VK_ATTACHMENT_STORE_OP_STORE);
        rPassMaker.attachmentSamples(polyImage->image.info().samples);
        rPassMaker.attachmentFinalLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        rPassMaker.attachmentBegin(VK_FORMAT_D32_SFLOAT);
        rPassMaker.attachmentLoadOp(VK_ATTACHMENT_LOAD_OP_CLEAR);
        rPassMaker.attachmentStencilLoadOp(VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        rPassMaker.attachmentSamples(polyImage->image.info().samples);
        rPassMaker.attachmentFinalLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        rPassMaker.subpassBegin(VK_PIPELINE_BIND_POINT_GRAPHICS);
        rPassMaker.subpassDepthStencilAttachment(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 1);

        rPassMaker.dependencyBegin(VK_SUBPASS_EXTERNAL, 0);
        rPassMaker.dependencySrcStageMask(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        rPassMaker.dependencyDstStageMask(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        rPassMaker.dependencyDstAccessMask(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        rPassMaker.subpassBegin(VK_PIPELINE_BIND_POINT_GRAPHICS);
        rPassMaker.subpassColorAttachment(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0);
        rPassMaker.subpassDepthStencilAttachment(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 1);

        rPassMaker.dependencyBegin(0, 1);
        rPassMaker.dependencySrcStageMask(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        rPassMaker.dependencyDstStageMask(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        rPassMaker.dependencyDstAccessMask(VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);


        // AMD driver bug workaround: shaders that use ViewIndex without a multiview renderpass
        // will crash the driver, so we always set up a renderpass with multiview even if it's only
        // one view.
        VkRenderPassMultiviewCreateInfo renderPassMultiviewCI{ VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO };
        uint32_t viewMasks[2] = { 0b00000001, 0b00000001 };
        uint32_t correlationMask = 0b00000001;

        if (ctx.passSettings.enableVR) {
            viewMasks[0] = 0b00000011;
            viewMasks[1] = 0b00000011;
            correlationMask = 0b00000011;
        }

        renderPassMultiviewCI.subpassCount = 2;
        renderPassMultiviewCI.pViewMasks = viewMasks;
        renderPassMultiviewCI.correlationMaskCount = 1;
        renderPassMultiviewCI.pCorrelationMasks = &correlationMask;
        rPassMaker.setPNext(&renderPassMultiviewCI);

        renderPass = rPassMaker.create(handles->device);

        VkImageView attachments[2] = { polyImage->image.imageView(), depthStencilImage->image.imageView() };

        auto extent = polyImage->image.info().extent;
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.attachmentCount = 2;
        fci.pAttachments = attachments;
        fci.width = extent.width;
        fci.height = extent.height;
        fci.renderPass = this->renderPass;
        fci.layers = 1;

        VKCHECK(vkCreateFramebuffer(handles->device, &fci, nullptr, &renderFb));

        AssetID vsID = AssetDB::pathToId("Shaders/standard.vert.spv");
        AssetID fsID = AssetDB::pathToId("Shaders/standard.frag.spv");
        vertexShader = ShaderCache::getModule(handles->device, vsID);
        fragmentShader = ShaderCache::getModule(handles->device, fsID);

        auto msaaSamples = vku::sampleCountFlags(ctx.passSettings.msaaSamples);

        struct StandardSpecConsts {
            VkBool32 enablePicking = false;
            float parallaxMaxLayers = 32.0f;
            float parallaxMinLayers = 4.0f;
            VkBool32 doParallax = false;
        };

        // standard shader specialization constants
        VkSpecializationMapEntry entries[4] = {
            { 0, offsetof(StandardSpecConsts, enablePicking), sizeof(VkBool32) },
            { 1, offsetof(StandardSpecConsts, parallaxMaxLayers), sizeof(float) },
            { 2, offsetof(StandardSpecConsts, parallaxMinLayers), sizeof(float) },
            { 3, offsetof(StandardSpecConsts, doParallax), sizeof(VkBool32) }
        };

        VkSpecializationInfo standardSpecInfo{ 4, entries, sizeof(StandardSpecConsts) };

        {
            vku::PipelineMaker pm{ extent.width, extent.height };

            StandardSpecConsts spc{
                enablePicking,
                maxParallaxLayers.getFloat(),
                minParallaxLayers.getFloat(),
                (bool)enableParallaxMapping.getInt()
            };

            standardSpecInfo.pData = &spc;

            pm.shader(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "main", &standardSpecInfo);
            pm.shader(VK_SHADER_STAGE_VERTEX_BIT, vertexShader);
            setupVertexFormat(pm);
            pm.cullMode(VK_CULL_MODE_BACK_BIT);

            if ((int)enableDepthPrepass)
                pm.depthWriteEnable(false)
                .depthTestEnable(true)
                .depthCompareOp(VK_COMPARE_OP_EQUAL);
            else
                pm.depthWriteEnable(true).depthTestEnable(true).depthCompareOp(VK_COMPARE_OP_GREATER);

            pm.blendBegin(false);
            pm.frontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE);
            pm.subPass(1);

            pm.rasterizationSamples(msaaSamples);
            pm.alphaToCoverageEnable(true);

            pipeline = pm.create(handles->device, handles->pipelineCache, pipelineLayout, renderPass);
        }

        {
            AssetID fsID = AssetDB::pathToId("Shaders/standard_alpha_test.frag.spv");
            auto atFragmentShader = vku::loadShaderAsset(handles->device, fsID);

            vku::PipelineMaker pm{ extent.width, extent.height };

            // Sadly we can't enable picking for alpha test surfaces as we can't use
            // early fragment tests with them, which leads to strange issues.
            StandardSpecConsts spc{
                (bool)enableDepthPrepass.getInt(),
                maxParallaxLayers.getFloat(),
                minParallaxLayers.getFloat(),
                (bool)enableParallaxMapping.getInt()
            };

            standardSpecInfo.pData = &spc;

            pm.shader(VK_SHADER_STAGE_FRAGMENT_BIT, atFragmentShader, "main", &standardSpecInfo);
            pm.shader(VK_SHADER_STAGE_VERTEX_BIT, vertexShader);
            setupVertexFormat(pm);
            pm.cullMode(VK_CULL_MODE_BACK_BIT);
            if ((int)enableDepthPrepass)
                pm.depthWriteEnable(false)
                .depthTestEnable(true)
                .depthCompareOp(VK_COMPARE_OP_EQUAL);
            else
                pm.depthWriteEnable(true).depthTestEnable(true).depthCompareOp(VK_COMPARE_OP_GREATER);

            pm.blendBegin(false);
            pm.frontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE);
            pm.subPass(1);

            pm.rasterizationSamples(msaaSamples);
            pm.alphaToCoverageEnable(true);

            alphaTestPipeline = pm.create(handles->device, handles->pipelineCache, pipelineLayout, renderPass);
        }

        {
            vku::PipelineMaker pm{ extent.width, extent.height };

            StandardSpecConsts spc{
                enablePicking,
                maxParallaxLayers.getFloat(),
                minParallaxLayers.getFloat(),
                (bool)enableParallaxMapping.getInt()
            };

            standardSpecInfo.pData = &spc;

            pm.shader(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader, "main", &standardSpecInfo);
            pm.shader(VK_SHADER_STAGE_VERTEX_BIT, vertexShader);
            setupVertexFormat(pm);
            pm.cullMode(VK_CULL_MODE_NONE);
            pm.depthWriteEnable(true).depthTestEnable(true).depthCompareOp(VK_COMPARE_OP_GREATER);
            pm.blendBegin(false);
            pm.frontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE);
            pm.subPass(1);

            pm.rasterizationSamples(msaaSamples);
            pm.alphaToCoverageEnable(true);
            noBackfaceCullPipeline = pm.create(handles->device, handles->pipelineCache, pipelineLayout, renderPass);
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
            pm.subPass(1);

            VkPipelineMultisampleStateCreateInfo pmsci{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            pmsci.rasterizationSamples = msaaSamples;
            pm.multisampleState(pmsci);

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
        depthPrepass->setup(ctx, renderPass, pipelineLayout);

        uiPass = new WorldSpaceUIPass(handles);
        uiPass->setup(ctx, renderPass, descriptorPool);

        updateDescriptorSets(ctx);

        if (ctx.passSettings.enableVR) {
            cullMeshRenderer = new VRCullMeshRenderer{ handles };
            cullMeshRenderer->setup(ctx, renderPass, descriptorPool);
        }

        VKCHECK(vkSetEvent(handles->device, pickEvent));
    }

    slib::StaticAllocList<SubmeshDrawInfo> drawInfo{ 8192 };

    void doTileCulling(LightTileBuffer* tileBuf, glm::mat4 proj, glm::mat4 viewMat, int TILE_SIZE, int tileOffset, int screenWidth, int screenHeight, entt::registry& reg) {
        const int xTiles = (screenWidth + (TILE_SIZE - 1)) / TILE_SIZE;
        const int yTiles = (screenHeight + (TILE_SIZE - 1)) / TILE_SIZE;

        const int totalTiles = xTiles * yTiles;
        glm::mat4 invProjView = glm::inverse(proj * viewMat);

        glm::vec2 ndcTileSize = 2.0f * glm::vec2(TILE_SIZE, -TILE_SIZE) / glm::vec2(screenWidth, screenHeight);
        glm::vec3 camPos = getMatrixTranslation(glm::inverse(viewMat));

        JobList& jl = g_jobSys->getFreeJobList();
        jl.begin();

        for (int x = 0; x < xTiles; x++) {
            Job j{ [&, x] {
            for (int y = 0; y < yTiles; y++) {
                int tileIdx = ((y * xTiles) + x);
                tileIdx += tileOffset;

                Frustum tileFrustum;

                glm::vec2 ndcTopLeftCorner{ -1.0f, 1.0f };
                glm::vec2 tileCoords{ x, y };

                glm::vec2 tileCornersNDC[4] =
                {
                    ndcTopLeftCorner + ndcTileSize * tileCoords, // Top left
                    ndcTopLeftCorner + ndcTileSize * (tileCoords + glm::vec2{1, 0}), // Top right
                    ndcTopLeftCorner + ndcTileSize * (tileCoords + glm::vec2{1, 1}), // Bottom right
                    ndcTopLeftCorner + ndcTileSize * (tileCoords + glm::vec2{0, 1}), // Bottom left
                };

                glm::vec4 temp;
                for (int i = 0; i < 4; i++) {
                    // Find the point on the near plane
                    temp = invProjView * glm::vec4(tileCornersNDC[i], 1.0f, 1.0f);
                    tileFrustum.points[i] = glm::vec3(temp) / temp.w;
                    // And also the far plane
                    temp = invProjView * glm::vec4(tileCornersNDC[i], 0.000000001f, 1.0f);
                    tileFrustum.points[i + 4] = glm::vec3(temp) / temp.w;
                }

                glm::vec3 temp_normal;
                for (int i = 0; i < 4; i++) {
                    //Cax+Cby+Ccz+Cd = 0, planes[i] = (Ca, Cb, Cc, Cd)
                    // temp_normal: normal without normalization
                    temp_normal = glm::cross(tileFrustum.points[i] - camPos, tileFrustum.points[i + 1] - camPos);
                    temp_normal = normalize(temp_normal);
                    tileFrustum.planes[i] = glm::vec4(temp_normal, -dot(temp_normal, tileFrustum.points[i]));
                }

                // near plane
                {
                    temp_normal = cross(tileFrustum.points[1] - tileFrustum.points[0], tileFrustum.points[3] - tileFrustum.points[0]);
                    temp_normal = normalize(temp_normal);
                    tileFrustum.planes[4] = glm::vec4(temp_normal, -dot(temp_normal, tileFrustum.points[0]));
                }

                // far plane
                {
                    temp_normal = cross(tileFrustum.points[7] - tileFrustum.points[4], tileFrustum.points[5] - tileFrustum.points[4]);
                    temp_normal = normalize(temp_normal);
                    tileFrustum.planes[5] = glm::vec4(temp_normal, -dot(temp_normal, tileFrustum.points[4]));
                }

                LightTile& currentTile = tileBuf->tiles[tileIdx];
                uint32_t tileLightCount = 0;

                reg.view<WorldLight, Transform>().each([&](auto ent, WorldLight& l, Transform& transform) {
                    float distance = glm::sqrt(1.0f / l.distanceCutoff);
                    if (l.lightIdx == ~0u) return;
                    if ((tileFrustum.containsSphere(transform.position, distance) || l.type == LightType::Directional)) {
                        currentTile.lightIds[tileLightCount] = l.lightIdx;
                        tileLightCount++;
                    }
                    });
                tileBuf->tileLightCount[tileIdx] = tileLightCount;
            }
        } };
            jl.addJob(std::move(j));
        }

        jl.end();
        g_jobSys->signalJobListAvailable();
        jl.wait();
    }

    void PolyRenderPass::prePass(RenderContext& ctx) {
        ZoneScoped;
        auto& resources = ctx.resources;

        glm::mat4 proj = ctx.projMatrices[0];
        Frustum frustum;
        frustum.fromVPMatrix(proj * ctx.viewMatrices[0]);

        Frustum frustumB;

        if (ctx.passSettings.enableVR) {
            frustumB.fromVPMatrix(ctx.projMatrices[1] * ctx.viewMatrices[1]);
        }

        auto& sceneSettings = ctx.registry.ctx<SceneSettings>();

        uint32_t skyboxId = ctx.resources.cubemaps.loadOrGet(sceneSettings.skybox);

        int lightIdx = 0;
        ctx.registry.view<WorldLight, Transform>().each([&](auto ent, WorldLight& l, Transform& transform) {
            float distance = glm::sqrt(1.0f / l.distanceCutoff);
            l.lightIdx = ~0u;
            if (!l.enabled) return;
            if (l.type != LightType::Directional) {
                if (!ctx.passSettings.enableVR) {
                    if (!frustum.containsSphere(transform.position, distance) && !frustumB.containsSphere(transform.position, distance)) {
                        return;
                    }
                } else {
                    if (!frustum.containsSphere(transform.position, distance)) {
                        return;
                    }
                }
            }

            glm::vec3 lightForward = glm::normalize(transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f));
            if (l.type != LightType::Tube) {
                lightMapped->lights[lightIdx] = PackedLight{
                    glm::vec4(l.color * l.intensity, (float)l.type),
                    glm::vec4(lightForward, l.type == LightType::Sphere ? l.spotCutoff : glm::cos(l.spotCutoff)),
                    transform.position, l.shadowmapIdx,
                    distance
                };
            } else {
                glm::vec3 tubeP0 = transform.position + lightForward * l.tubeLength;
                glm::vec3 tubeP1 = transform.position - lightForward * l.tubeLength;
                lightMapped->lights[lightIdx] = PackedLight{
                    glm::vec4(l.color * l.intensity, (float)l.type),
                    glm::vec4(tubeP0, l.tubeRadius),
                    tubeP1, ~0u,
                    distance
                };
            }

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
            l.lightIdx = lightIdx;
            lightIdx++;
            });

        const int TILE_SIZE = 32;
        const int xTiles = (ctx.passWidth + (TILE_SIZE - 1)) / TILE_SIZE;
        const int yTiles = (ctx.passHeight + (TILE_SIZE - 1)) / TILE_SIZE;
        const int totalTiles = xTiles * yTiles;

        int numViews = ctx.passSettings.enableVR ? 2 : 1;

        doTileCulling(lightTilesMapped, ctx.projMatrices[0], ctx.viewMatrices[0], TILE_SIZE, 0, ctx.passWidth, ctx.passHeight, ctx.registry);

        if (ctx.passSettings.enableVR)
            doTileCulling(lightTilesMapped, ctx.projMatrices[1], ctx.viewMatrices[1], TILE_SIZE, totalTiles, ctx.passWidth, ctx.passHeight, ctx.registry);

        lightTilesMapped->tileSize = TILE_SIZE;
        lightTilesMapped->tilesPerEye = xTiles * yTiles;
        lightTilesMapped->numTilesX = xTiles;
        lightTilesMapped->numTilesY = yTiles;

        lightMapped->pack0.x = (float)lightIdx;
        lightMapped->pack0.y = ctx.cascadeInfo.texelsPerUnit[0];
        lightMapped->pack0.z = ctx.cascadeInfo.texelsPerUnit[1];
        lightMapped->pack0.w = ctx.cascadeInfo.texelsPerUnit[2];
        lightMapped->shadowmapMatrices[0] = ctx.cascadeInfo.matrices[0];
        lightMapped->shadowmapMatrices[1] = ctx.cascadeInfo.matrices[1];
        lightMapped->shadowmapMatrices[2] = ctx.cascadeInfo.matrices[2];
        ctx.debugContext.stats->numLightsInView = lightIdx;

        uint32_t aoBoxIdx = 0;
        ctx.registry.view<Transform, ProxyAOComponent>().each([&](auto ent, Transform& t, ProxyAOComponent& pac) {
            lightMapped->box[aoBoxIdx].setScale(pac.bounds);
            glm::mat4 tMat = glm::translate(glm::mat4(1.0f), t.position);
            lightMapped->box[aoBoxIdx].setMatrix(glm::mat4_cast(glm::inverse(t.rotation)) * glm::inverse(tMat));
            lightMapped->box[aoBoxIdx].setEntityId((uint32_t)ent);
            aoBoxIdx++;
            });
        lightMapped->pack1.x = aoBoxIdx;

        uint32_t aoSphereIdx = 0;
        ctx.registry.view<Transform, SphereAOProxy>().each([&](entt::entity entity, Transform& t, SphereAOProxy& sao) {
            lightMapped->sphere[aoSphereIdx].position = t.position;
            lightMapped->sphere[aoSphereIdx].radius = sao.radius;
            lightMapped->sphereIds[aoSphereIdx] = (uint32_t)entity;
            aoSphereIdx++;
            });
        lightMapped->pack1.y = aoSphereIdx;

        if (dsUpdateNeeded) {
            // Update descriptor sets to bring in any new textures
            updateDescriptorSets(ctx);
        }

        drawInfo.clear();

        int matrixIdx = 0;
        bool warned = false;
        {
            ZoneScopedN("PolyRenderPass SDI generation");
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
                if (!ctx.passSettings.enableVR) {
                    if (!frustum.containsSphere(t.position, meshPos->second.sphereRadius * maxScale)) {
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

                modelMatricesMapped[ctx.imageIndex]->modelMatrices[matrixIdx] = t.getMatrix();

                for (int i = 0; i < meshPos->second.numSubmeshes; i++) {
                    auto& currSubmesh = meshPos->second.submeshes[i];

                    SubmeshDrawInfo sdi = { 0 };
                    sdi.ib = meshPos->second.ib.buffer();
                    sdi.vb = meshPos->second.vb.buffer();
                    sdi.indexCount = currSubmesh.indexCount;
                    sdi.indexOffset = currSubmesh.indexOffset;
                    sdi.materialIdx = wo.materialIdx[i];
                    sdi.matrixIdx = matrixIdx;
                    sdi.texScaleOffset = wo.texScaleOffset;
                    sdi.ent = ent;
                    auto& packedMat = resources.materials[wo.materialIdx[i]];
                    sdi.opaque = packedMat.getCutoff() == 0.0f;

                    switch (wo.uvOverride) {
                    default:
                        sdi.drawMiscFlags = 0;
                        break;
                    case UVOverride::XY:
                        sdi.drawMiscFlags = 1024;
                        break;
                    case UVOverride::XZ:
                        sdi.drawMiscFlags = 2048;
                        break;
                    case UVOverride::ZY:
                        sdi.drawMiscFlags = 4096;
                        break;
                    case UVOverride::PickBest:
                        sdi.drawMiscFlags = 8192;
                        break;
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
                                sdi.drawMiscFlags |= 16384; // flag for cubemap parallax correction
                                sdi.cubemapPos = cubeT.position;
                                sdi.cubemapExt = wc.extent;
                            }
                            lastPriority = wc.priority;
                        }
                        });

                    sdi.cubemapIdx = currCubemapIdx;

                    auto& extraDat = resources.materials.getExtraDat(wo.materialIdx[i]);

                    sdi.pipeline = sdi.opaque ? pipeline : alphaTestPipeline;

                    if (extraDat.noCull) {
                        sdi.pipeline = noBackfaceCullPipeline;
                    } else if (extraDat.wireframe || showWireframe.getInt() == 1) {
                        sdi.pipeline = wireframePipeline;
                        sdi.dontPrepass = true;
                    } else if (ctx.registry.has<UseWireframe>(ent) || showWireframe.getInt() == 2) {
                        drawInfo.add(sdi);
                        ctx.debugContext.stats->numTriangles += currSubmesh.indexCount / 3;
                        sdi.pipeline = wireframePipeline;
                        sdi.dontPrepass = true;
                    }
                    ctx.debugContext.stats->numTriangles += currSubmesh.indexCount / 3;

                    drawInfo.add(std::move(sdi));
                }

                matrixIdx++;
                });
        }

        dbgLinesPass->prePass(ctx);
        skyboxPass->prePass(ctx);
        uiPass->prePass(ctx);
    }

    void PolyRenderPass::execute(RenderContext& ctx) {
        ZoneScoped;
        TracyVkZone((*ctx.debugContext.tracyContexts)[ctx.imageIndex], ctx.cmdBuf, "Polys");

        std::array<VkClearValue, 2> clearColours;
        clearColours[0].color.float32[0] = 0.0f;
        clearColours[0].color.float32[1] = 0.0f;
        clearColours[0].color.float32[2] = 0.0f;
        clearColours[0].color.float32[3] = 1.0f;

        clearColours[1].depthStencil.depth = 0.0f;
        clearColours[1].depthStencil.stencil = 0.0f;

        VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };

        rpbi.renderPass = renderPass;
        rpbi.framebuffer = renderFb;
        rpbi.renderArea = VkRect2D{ {0, 0}, {ctx.passWidth, ctx.passHeight} };
        rpbi.clearValueCount = (uint32_t)clearColours.size();
        rpbi.pClearValues = clearColours.data();

        VkCommandBuffer cmdBuf = ctx.cmdBuf;

        lightsUB.barrier(
            cmdBuf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_DEPENDENCY_BY_REGION_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);

        lightTileBuffer.barrier(
            cmdBuf, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_DEPENDENCY_BY_REGION_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT,
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
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        }

        ctx.resources.shadowCascades->image.barrier(cmdBuf, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);

        if (setEventNextFrame) {
            vkCmdSetEvent(cmdBuf, pickEvent, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            setEventNextFrame = false;
        }

        vkCmdBeginRenderPass(cmdBuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        if (ctx.passSettings.enableVR) {
            cullMeshRenderer->draw(cmdBuf);
        }

        vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets[ctx.imageIndex], 0, nullptr);

        uint32_t globalMiscFlags = 0;

        if (pickThisFrame)
            globalMiscFlags |= 1;

        if (dbgDrawMode.getInt() != 0) {
            globalMiscFlags |= (1 << dbgDrawMode.getInt());
        }

        if (!ctx.passSettings.enableShadows) {
            globalMiscFlags |= 16384;
        }

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
        }

        vkCmdNextSubpass(cmdBuf, VK_SUBPASS_CONTENTS_INLINE);

        addDebugLabel(cmdBuf, "Main Pass", 0.466f, 0.211f, 0.639f, 1.0f);

        vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        SubmeshDrawInfo last;
        last.pipeline = pipeline;
        for (const auto& sdi : drawInfo) {
            ZoneScopedN("SDI cmdbuf write");

            if (last.pipeline != sdi.pipeline) {
                vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, sdi.pipeline);
                ctx.debugContext.stats->numPipelineSwitches++;
            }

            StandardPushConstants pushConst{
                .modelMatrixIdx = sdi.matrixIdx,
                .materialIdx = sdi.materialIdx,
                .vpIdx = 0,
                .objectId = (uint32_t)sdi.ent,
                .cubemapExt = glm::vec4(sdi.cubemapExt, 0.0f),
                .cubemapPos = glm::vec4(sdi.cubemapPos, 0.0f),
                .texScaleOffset = sdi.texScaleOffset,
                .screenSpacePickPos = glm::ivec3(pickX, pickY, globalMiscFlags | sdi.drawMiscFlags),
                .cubemapIdx = sdi.cubemapIdx
            };

            vkCmdPushConstants(cmdBuf, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConst), &pushConst);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmdBuf, 0, 1, &sdi.vb, &offset);
            vkCmdBindIndexBuffer(cmdBuf, sdi.ib, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmdBuf, sdi.indexCount, 1, sdi.indexOffset, 0, 0);

            last = sdi;
            ctx.debugContext.stats->numDrawCalls++;
        }
        vkCmdEndDebugUtilsLabelEXT(cmdBuf);

        dbgLinesPass->execute(ctx);
        skyboxPass->execute(ctx);
        uiPass->execute(ctx);

        vkCmdEndRenderPass(cmdBuf);
        polyImage->image.setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        depthStencilImage->image.setCurrentLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

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
        lightTileBuffer.unmap(handles->device);
        delete dbgLinesPass;
        delete skyboxPass;
        delete depthPrepass;
        delete uiPass;

        if (cullMeshRenderer)
            delete cullMeshRenderer;
    }
}
