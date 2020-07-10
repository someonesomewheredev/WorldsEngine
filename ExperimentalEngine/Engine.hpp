#pragma once
#include "PCH.hpp"
#include <SDL2/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include "AssetDB.hpp"
#ifdef TRACY_ENABLE
#include "tracy/TracyVulkan.hpp"
#endif

struct WorldObject {
	WorldObject(AssetID material, AssetID mesh) 
		: material(material)
		, mesh(mesh)
		, texScaleOffset(1.0f, 1.0f, 0.0f, 0.0f)
		, materialIndex(0) {}

	AssetID material;
	AssetID mesh;
	int materialIndex;
	glm::vec4 texScaleOffset;
};

struct MVP {
	glm::mat4 model;
	glm::mat4 view;
	glm::mat4 projection;
};

struct VP {
	glm::mat4 view;
	glm::mat4 projection;
};

struct PackedLight {
	glm::vec4 pack0;
	glm::vec4 pack1;
	glm::vec4 pack2;
};

enum class LightType {
	Point,
	Spot,
	Directional
};

struct WorldLight {
	WorldLight() : type(LightType::Point), color(1.0f), spotCutoff(1.35f) {}
	WorldLight(LightType type) : type(type), color(1.0f), spotCutoff(1.35f) {}
	LightType type;
	glm::vec3 color;
	float spotCutoff;
};

struct LightUB {
	glm::vec4 pack0;
	glm::mat4 shadowmapMatrix;
	PackedLight lights[16];
};

struct Vertex {
	glm::vec3 position;
	glm::vec3 normal;
	glm::vec3 tangent;
	glm::vec2 uv;
	float ao;
};

struct PackedMaterial {
	glm::vec4 pack0;
	glm::vec4 pack1;
};

struct ProceduralObject {
	ProceduralObject() : uploaded(false), readyForUpload(false), visible(true) {}
	AssetID material;

	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	bool uploaded;
	bool readyForUpload;
	bool visible;
	vku::VertexBuffer vb;
	vku::IndexBuffer ib;
	uint32_t indexCount;
	vk::IndexType indexType;
};

struct Camera {
	Camera() : 
		position(0.0f), 
		rotation(), 
		verticalFOV(1.25f) {}
	glm::vec3 position;
	glm::quat rotation;
	float verticalFOV;

	glm::mat4 getViewMatrix() {
		return glm::lookAt(position, position + (rotation * glm::vec3(0.0f, 0.0f, 1.0f)), glm::vec3(0.0f, 1.0f, 0.0f));
	}

	glm::mat4 getProjectionMatrix(float aspect) {
		return glm::perspective(verticalFOV, aspect, 0.01f, 2500.0f);
	}
};

extern AssetDB g_assetDB;

struct QueueFamilyIndices {
	uint32_t graphics;
	uint32_t present;
};

class Swapchain {
public:
	Swapchain(vk::PhysicalDevice&, vk::Device&, vk::SurfaceKHR&, QueueFamilyIndices qfi, vk::SwapchainKHR oldSwapchain = vk::SwapchainKHR());
	~Swapchain();
	void getSize(uint32_t* x, uint32_t* y) { *x = width; *y = height; }
	vk::Result acquireImage(vk::Device& device, vk::Semaphore semaphore, uint32_t* imageIndex);
	vk::UniqueSwapchainKHR& getSwapchain() { return swapchain; }
	vk::Format imageFormat() { return format; }
	std::vector<vk::Image> images;
	std::vector<vk::ImageView> imageViews;
private:
	vk::Device& device;
	vk::UniqueSwapchainKHR swapchain;
	vk::Format format;
	uint32_t width;
	uint32_t height;
};

class VKRenderer {
	struct LoadedMeshData {
		vku::VertexBuffer vb;
		vku::IndexBuffer ib;
		uint32_t indexCount;
		vk::IndexType indexType;
	};

	vk::UniqueInstance instance;
	vk::PhysicalDevice physicalDevice;
	vk::UniqueDevice device;
	vk::UniquePipelineCache pipelineCache;
	vk::UniqueDescriptorPool descriptorPool;
	vk::SurfaceKHR surface;
	std::unique_ptr<Swapchain> swapchain;
	vku::DebugCallback dbgCallback;
	uint32_t graphicsQueueFamilyIdx;
	uint32_t computeQueueFamilyIdx;
	uint32_t presentQueueFamilyIdx;
	uint32_t asyncComputeQueueFamilyIdx;
	uint32_t width, height;
	vk::SampleCountFlagBits msaaSamples;
	int32_t numMSAASamples;
	vk::UniqueRenderPass imguiRenderPass;
	std::vector<vk::UniqueFramebuffer> framebuffers;
	vk::UniqueSemaphore imageAcquire;
	vk::UniqueSemaphore commandComplete;
	vk::UniqueCommandPool commandPool;
	std::vector<vk::UniqueCommandBuffer> cmdBufs;
	std::vector<vk::Fence> cmdBufferFences;
	VmaAllocator allocator;
	
	// stuff related to standard geometry rendering
	vku::DepthStencilImage depthStencilImage;
	vk::UniqueRenderPass renderPass;
	vk::UniquePipeline pipeline;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueDescriptorSetLayout dsl;
	vku::UniformBuffer vpUB;
	vku::UniformBuffer lightsUB;
	vku::UniformBuffer materialUB;
	vku::UniformBuffer modelMatrixUB;
	vku::TextureImage2D albedoTex;
	vku::ShaderModule fragmentShader;
	vku::ShaderModule vertexShader;
	vk::UniqueSampler albedoSampler;
	vk::UniqueSampler shadowSampler;
	vk::UniqueFramebuffer renderFb;
	vku::GenericImage polyImage;

	// tonemap related stuff
	vku::ShaderModule tonemapShader;
	vk::UniqueDescriptorSetLayout tonemapDsl;
	vk::UniquePipeline tonemapPipeline;
	vk::UniquePipelineLayout tonemapPipelineLayout;
	vk::DescriptorSet tonemapDescriptorSet;
	vku::GenericImage finalPrePresent;
	vk::UniqueFramebuffer finalPrePresentFB;

	// shadowmapping stuff
	vk::UniqueRenderPass shadowmapPass;
	vk::UniquePipeline shadowmapPipeline;
	vk::UniquePipelineLayout shadowmapPipelineLayout;
	vku::ShaderModule shadowVertexShader;
	vku::ShaderModule shadowFragmentShader;
	vk::UniqueFramebuffer shadowmapFb;
	vku::GenericImage shadowmapImage;
	vk::DescriptorSet shadowmapDescriptorSet;
	vk::UniqueDescriptorSetLayout shadowmapDsl;

	std::vector<vk::DescriptorSet> descriptorSets;
	SDL_Window* window;
	vk::UniqueQueryPool queryPool;
	uint64_t lastRenderTimeTicks;
	float timestampPeriod;

	void createSwapchain(vk::SwapchainKHR oldSwapchain);
	void createFramebuffers();
	void createSCDependents();
	void setupTonemapping();
	void setupImGUI();
	void setupStandard();
	void setupShadowPass();
	void presentNothing(uint32_t imageIndex);
	void loadTex(const char* path, int index);
	void loadAlbedo();
	void doTonemap(vk::UniqueCommandBuffer& cmdBuf, uint32_t imageIndex);
	void renderShadowmap(vk::UniqueCommandBuffer& cmdBuf, entt::registry& reg, uint32_t imageIndex, Camera& cam);
	void renderPolys(vk::UniqueCommandBuffer& cmdBuf, entt::registry& reg, uint32_t imageIndex, Camera& cam);
	void updateTonemapDescriptors();
	vku::ShaderModule loadShaderAsset(AssetID id);

	std::unordered_map<AssetID, LoadedMeshData> loadedMeshes;
	int frameIdx;
#ifdef TRACY_ENABLE
	std::vector<TracyVkCtx> tracyContexts;
#endif
	vku::TextureImage2D textures[64];
	vku::TextureImageCube cubemaps[64];
public:
	double time;
	VKRenderer(SDL_Window* window, bool* success, std::vector<std::string> additionalInstanceExtensions, std::vector<std::string> additionalDeviceExtensions);
	void recreateSwapchain();
	void frame(Camera& cam, entt::registry& reg);
	void preloadMesh(AssetID id);
	void uploadProcObj(ProceduralObject& procObj);
	inline float getLastRenderTime() { return lastRenderTimeTicks * timestampPeriod; }

	~VKRenderer();
};
