#pragma once
#include <stdint.h>

#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
VK_DEFINE_HANDLE(VkCommandBuffer)
VK_DEFINE_HANDLE(VkPipelineLayout)
VK_DEFINE_HANDLE(VkDescriptorSet)
#undef VK_DEFINE_HANDLE

namespace R2::VK
{
    class Buffer;

    enum class IndexType
    {
        Uint16 = 0,
        Uint32 = 1
    };

    struct Viewport
    {
        static Viewport Simple(float w, float h)
        {
            return Viewport{ 0.0f, 0.0f, 0.0f, 1.0f, w, h };
        }

        static Viewport Offset(float x, float y, float w, float h)
        {
            return Viewport{ x, y, 0.0f, 1.0f, w, h };
        }

        float X;
        float Y;
        float MinDepth;
        float MaxDepth;
        float Width;
        float Height;
    };

    struct ScissorRect
    {
        static ScissorRect Simple(uint32_t w, uint32_t h)
        {
            return ScissorRect{ 0, 0, w, h };
        }

        int X;
        int Y;
        uint32_t Width;
        uint32_t Height;
    };

    enum class ShaderStage;

    class Pipeline;
    class PipelineLayout;

    class CommandBuffer
    {
    public:
        CommandBuffer(VkCommandBuffer cb);
        void SetViewport(Viewport vp);
        void SetScissor(ScissorRect rect);
        void BindVertexBuffer(uint32_t location, Buffer* buffer, uint64_t offset);
        void BindIndexBuffer(Buffer* buffer, uint64_t offset, IndexType indexType);
        void BindPipeline(Pipeline* p);
        void BindGraphicsDescriptorSet(VkPipelineLayout pipelineLayout, VkDescriptorSet descriptorSet, uint32_t setNumber);
        void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);

        template <typename T>
        void PushConstants(const T& data, ShaderStage stage, VkPipelineLayout pipelineLayout) { PushConstants(&data, sizeof(data), stage, pipelineLayout); }
        void PushConstants(const void* data, size_t dataSize, ShaderStage stages, VkPipelineLayout pipelineLayout);

        void BeginDebugLabel(const char* label, float r, float g, float b);
        void EndDebugLabel();

        VkCommandBuffer GetNativeHandle();
    private:
        VkCommandBuffer cb;
    };
}