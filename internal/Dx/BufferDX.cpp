﻿#include "BufferDX.h"

#include <algorithm>
#include <cassert>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>

#include "../../Log.h"
#include "ContextDX.h"

namespace Ray {
namespace Dx {
D3D12_HEAP_TYPE GetDxHeapType(const eBufType type) {
    if (type == eBufType::Upload) {
        return D3D12_HEAP_TYPE_UPLOAD;
    } else if (type == eBufType::Readback) {
        return D3D12_HEAP_TYPE_READBACK;
    }
    return D3D12_HEAP_TYPE_DEFAULT;
}

eResState GetInitialDxResourceState(const eBufType type) {
    if (type == eBufType::Upload) {
        return eResState::CopySrc;
    } else if (type == eBufType::Readback) {
        return eResState::CopyDst;
    }
    return eResState::Undefined;
}
} // namespace Dx
} // namespace Ray

int Ray::Dx::Buffer::g_GenCounter = 0;

Ray::Dx::Buffer::Buffer(const char *name, Context *ctx, const eBufType type, const uint32_t initial_size,
                        const uint32_t suballoc_align)
    : LinearAlloc(std::min(suballoc_align, initial_size), initial_size), ctx_(ctx), name_(name), type_(type), size_(0) {
    Resize(size());
}

Ray::Dx::Buffer::~Buffer() { Free(); }

Ray::Dx::Buffer &Ray::Dx::Buffer::operator=(Buffer &&rhs) noexcept {
    LinearAlloc::operator=(static_cast<LinearAlloc &&>(rhs));

    Free();

    assert(!mapped_ptr_);
    assert(mapped_offset_ == 0xffffffff);
    assert(mapped_size_ == 0);

    ctx_ = exchange(rhs.ctx_, nullptr);
    handle_ = exchange(rhs.handle_, {});
    name_ = std::move(rhs.name_);

    type_ = exchange(rhs.type_, eBufType::Undefined);

    size_ = exchange(rhs.size_, 0);
    mapped_ptr_ = exchange(rhs.mapped_ptr_, nullptr);
    mapped_offset_ = exchange(rhs.mapped_offset_, 0xffffffff);
    mapped_size_ = exchange(rhs.mapped_size_, 0);

#ifndef NDEBUG
    flushed_ranges_ = std::move(rhs.flushed_ranges_);
#endif

    resource_state = exchange(rhs.resource_state, eResState::Undefined);

    return (*this);
}

uint32_t Ray::Dx::Buffer::AllocSubRegion(const uint32_t req_size, const char *tag, const Buffer *init_buf,
                                         void *_cmd_buf, const uint32_t init_off) {
    const uint32_t alloc_off = Alloc(req_size, tag);
    if (alloc_off != 0xffffffff) {
        if (init_buf) {
            assert(init_buf->type_ == eBufType::Upload || init_buf->type_ == eBufType::Readback);
            // auto cmd_buf = reinterpret_cast<VkCommandBuffer>(_cmd_buf);

            // VkPipelineStageFlags src_stages = 0, dst_stages = 0;
            // SmallVector<VkBufferMemoryBarrier, 2> barriers;

            /*if (init_buf->resource_state != eResState::Undefined && init_buf->resource_state != eResState::CopySrc) {
                auto &new_barrier = barriers.emplace_back();
                new_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                new_barrier.srcAccessMask = VKAccessFlagsForState(init_buf->resource_state);
                new_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                new_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                new_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                new_barrier.buffer = init_buf->vk_handle();
                new_barrier.offset = VkDeviceSize{init_off};
                new_barrier.size = VkDeviceSize{req_size};

                src_stages |= VKPipelineStagesForState(init_buf->resource_state);
                dst_stages |= VKPipelineStagesForState(eResState::CopySrc);
            }

            if (this->resource_state != eResState::Undefined && this->resource_state != eResState::CopyDst) {
                auto &new_barrier = barriers.emplace_back();
                new_barrier = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                new_barrier.srcAccessMask = VKAccessFlagsForState(this->resource_state);
                new_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                new_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                new_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                new_barrier.buffer = handle_.buf;
                new_barrier.offset = VkDeviceSize{alloc_off};
                new_barrier.size = VkDeviceSize{req_size};

                src_stages |= VKPipelineStagesForState(this->resource_state);
                dst_stages |= VKPipelineStagesForState(eResState::CopyDst);
            }

            if (!barriers.empty()) {
                vkCmdPipelineBarrier(cmd_buf, src_stages, dst_stages, 0, 0, nullptr, uint32_t(barriers.size()),
                                     barriers.cdata(), 0, nullptr);
            }

            VkBufferCopy region_to_copy = {};
            region_to_copy.srcOffset = VkDeviceSize{init_off};
            region_to_copy.dstOffset = VkDeviceSize{alloc_off};
            region_to_copy.size = VkDeviceSize{req_size};

            vkCmdCopyBuffer(cmd_buf, init_buf->handle_.buf, handle_.buf, 1, &region_to_copy);*/

            init_buf->resource_state = eResState::CopySrc;
            this->resource_state = eResState::CopyDst;
        }

        return alloc_off;
    }

    return 0xffffffff;
}

void Ray::Dx::Buffer::UpdateSubRegion(const uint32_t offset, const uint32_t size, const Buffer &init_buf,
                                      const uint32_t init_off, void *_cmd_buf) {
    assert(init_buf.type_ == eBufType::Upload || init_buf.type_ == eBufType::Readback);
    auto cmd_buf = reinterpret_cast<ID3D12GraphicsCommandList *>(_cmd_buf);

    SmallVector<D3D12_RESOURCE_BARRIER, 2> barriers;

    if (/*init_buf.resource_state != eResState::Undefined &&*/ init_buf.resource_state != eResState::CopySrc) {
        auto &new_barrier = barriers.emplace_back();
        new_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        new_barrier.Transition.pResource = init_buf.dx_resource();
        new_barrier.Transition.StateBefore = DXResourceState(init_buf.resource_state);
        new_barrier.Transition.StateAfter = DXResourceState(eResState::CopySrc);
        new_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }

    if (/*this->resource_state != eResState::Undefined &&*/ this->resource_state != eResState::CopyDst) {
        auto &new_barrier = barriers.emplace_back();
        new_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        new_barrier.Transition.pResource = handle_.buf;
        new_barrier.Transition.StateBefore = DXResourceState(this->resource_state);
        new_barrier.Transition.StateAfter = DXResourceState(eResState::CopyDst);
        new_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }

    if (!barriers.empty()) {
        cmd_buf->ResourceBarrier(UINT(barriers.size()), barriers.data());
    }

    cmd_buf->CopyBufferRegion(handle_.buf, offset, init_buf.dx_resource(), init_off, size);

    init_buf.resource_state = eResState::CopySrc;
    this->resource_state = eResState::CopyDst;
}

bool Ray::Dx::Buffer::FreeSubRegion(const uint32_t offset, const uint32_t size) {
    LinearAlloc::Free(offset, size);
    return true;
}

void Ray::Dx::Buffer::Resize(const uint32_t new_size, const bool keep_content) {
    if (size_ >= new_size) {
        return;
    }

    const uint32_t old_size = size_;

    if (!size_) {
        size_ = new_size;
        assert(size_ > 0);
    }

    while (size_ < new_size) {
        size_ *= 2;
    }

    if (old_size) {
        LinearAlloc::Resize(size_);
        assert(size_ == size());
    }

    ID3D12Device *device = ctx_->device();

    D3D12_HEAP_PROPERTIES heap_properties = {};
    heap_properties.Type = GetDxHeapType(type_);
    heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC res_desc = {};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    res_desc.Alignment = 0;
    res_desc.Width = size_;
    res_desc.Height = 1;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = 1;
    res_desc.Format = DXGI_FORMAT_UNKNOWN;
    res_desc.SampleDesc.Count = 1;
    res_desc.SampleDesc.Quality = 0;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type_ == eBufType::Storage || type_ == eBufType::Indirect) {
        res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    } else {
        res_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    }

    eResState new_buf_state = GetInitialDxResourceState(type_);

    ID3D12Resource *new_buf = nullptr;
    HRESULT hr = device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &res_desc,
                                                 type_ == eBufType::Upload ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                           : DXResourceState(new_buf_state),
                                                 nullptr, IID_PPV_ARGS(&new_buf));
    if (FAILED(hr)) {
        return;
    }

#ifdef ENABLE_OBJ_LABELS
    std::wstring temp_str(name_.begin(), name_.end());
    new_buf->SetName(temp_str.c_str());
#endif

#if 0
    VkDeviceMemory buffer_mem = {};

    res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (buf_alloc_info.memoryTypeIndex != 0xffffffff) {
        res = vkAllocateMemory(ctx_->device(), &buf_alloc_info, nullptr, &buffer_mem);
    }
    if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        ctx_->log()->Warning("Not enough device memory, falling back to CPU RAM!");
        memory_props &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        buf_alloc_info.memoryTypeIndex = FindMemoryType(&ctx_->mem_properties(), memory_requirements.memoryTypeBits,
                                                        memory_props, buf_alloc_info.allocationSize);
        res = vkAllocateMemory(ctx_->device(), &buf_alloc_info, nullptr, &buffer_mem);
        if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            memory_props |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

            buf_alloc_info.memoryTypeIndex = FindMemoryType(&ctx_->mem_properties(), memory_requirements.memoryTypeBits,
                                                            memory_props, buf_alloc_info.allocationSize);
            res = vkAllocateMemory(ctx_->device(), &buf_alloc_info, nullptr, &buffer_mem);
        }
    }
    assert(res == VK_SUCCESS && "Failed to allocate memory!");

    res = vkBindBufferMemory(ctx_->device(), new_buf, buffer_mem, 0 /* offset */);
    assert(res == VK_SUCCESS && "Failed to bind memory!");
#endif
    if (handle_.buf) {
        if (keep_content) {
            ID3D12GraphicsCommandList *cmd_buf = BegSingleTimeCommands(ctx_->device(), ctx_->temp_command_pool());

            cmd_buf->CopyBufferRegion(new_buf, 0, handle_.buf, 0, old_size);

            EndSingleTimeCommands(ctx_->device(), ctx_->graphics_queue(), cmd_buf, ctx_->temp_command_pool());

            // destroy previous buffer
            handle_.buf->Release();
        } else {
            // destroy previous buffer
            ctx_->resources_to_destroy[ctx_->backend_frame].push_back(handle_.buf);
        }
    }

    handle_.buf = new_buf;
    handle_.generation = g_GenCounter++;
    resource_state = new_buf_state;
}

void Ray::Dx::Buffer::Free() {
    assert(mapped_offset_ == 0xffffffff && mapped_size_ == 0 && !mapped_ptr_);
    if (handle_.buf) {
        ctx_->resources_to_destroy[ctx_->backend_frame].push_back(handle_.buf);

        handle_ = {};
        size_ = 0;
        LinearAlloc::Clear();
    }
}

void Ray::Dx::Buffer::FreeImmediate() {
    assert(mapped_offset_ == 0xffffffff && mapped_size_ == 0 && !mapped_ptr_);
    if (handle_.buf) {
        handle_.buf->Release();

        handle_ = {};
        size_ = 0;
        LinearAlloc::Clear();
    }
}

uint8_t *Ray::Dx::Buffer::MapRange(const uint32_t offset, const uint32_t size, const bool persistent) {
    assert(mapped_offset_ == 0xffffffff && mapped_size_ == 0 && !mapped_ptr_);
    assert(offset + size <= size_);
    assert(type_ == eBufType::Upload || type_ == eBufType::Readback);
    assert(offset == AlignMapOffset(offset));
    assert((offset + size) == size_ || (offset + size) == AlignMapOffset(offset + size));

#ifndef NDEBUG
    for (auto it = std::begin(flushed_ranges_); it != std::end(flushed_ranges_);) {
        if (offset + size >= it->range.first && offset < it->range.first + it->range.second) {
            const WaitResult res = it->fence.ClientWaitSync(0);
            assert(res == WaitResult::Success);
            it = flushed_ranges_.erase(it);
        } else {
            ++it;
        }
    }
#endif

    D3D12_RANGE range = {};
    range.Begin = offset;
    range.End = offset + size;

    void *mapped = nullptr;
    HRESULT hr = handle_.buf->Map(0, &range, &mapped);
    if (FAILED(hr)) {
        return nullptr;
    }

    mapped_ptr_ = reinterpret_cast<uint8_t *>(mapped);
    mapped_offset_ = offset;
    mapped_size_ = size;
    return reinterpret_cast<uint8_t *>(mapped);
}

void Ray::Dx::Buffer::FlushMappedRange(uint32_t offset, uint32_t size, const bool autoalign) const {}

void Ray::Dx::Buffer::Unmap() {
    assert(mapped_offset_ != 0xffffffff && mapped_size_ != 0 && mapped_ptr_);

    D3D12_RANGE range = {};
    if (type_ != eBufType::Readback) {
        range.Begin = mapped_offset_;
        range.End = mapped_offset_ + mapped_size_;
    }

    handle_.buf->Unmap(0, &range);
    mapped_ptr_ = nullptr;
    mapped_offset_ = 0xffffffff;
    mapped_size_ = 0;
}

void Ray::Dx::Buffer::Fill(const uint32_t dst_offset, const uint32_t size, const uint32_t data, void *_cmd_buf) {
    auto cmd_buf = reinterpret_cast<ID3D12GraphicsCommandList *>(_cmd_buf);

    ID3D12Device *device = ctx_->device();

    D3D12_DESCRIPTOR_HEAP_DESC temp_cpu_descriptor_heap_desc = {};
    temp_cpu_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    temp_cpu_descriptor_heap_desc.NumDescriptors = 1;
    temp_cpu_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ID3D12DescriptorHeap *temp_cpu_descriptor_heap = nullptr;
    HRESULT hr = device->CreateDescriptorHeap(&temp_cpu_descriptor_heap_desc, IID_PPV_ARGS(&temp_cpu_descriptor_heap));
    if (FAILED(hr)) {
        return;
    }

    D3D12_DESCRIPTOR_HEAP_DESC temp_gpu_descriptor_heap_desc = {};
    temp_gpu_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    temp_gpu_descriptor_heap_desc.NumDescriptors = 1;
    temp_gpu_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ID3D12DescriptorHeap *temp_gpu_descriptor_heap = nullptr;
    hr = device->CreateDescriptorHeap(&temp_gpu_descriptor_heap_desc, IID_PPV_ARGS(&temp_gpu_descriptor_heap));
    if (FAILED(hr)) {
        return;
    }

    ID3D12DescriptorHeap *pp_descriptor_heaps[] = {temp_gpu_descriptor_heap};
    cmd_buf->SetDescriptorHeaps(1, pp_descriptor_heaps);

    D3D12_CPU_DESCRIPTOR_HANDLE temp_buffer_cpu_readable_UAV_handle =
        temp_cpu_descriptor_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_UNORDERED_ACCESS_VIEW_DESC buffer_UAV_desc = {};
    buffer_UAV_desc.Format = DXGI_FORMAT_R32_UINT;
    buffer_UAV_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    buffer_UAV_desc.Buffer.FirstElement = 0;
    buffer_UAV_desc.Buffer.NumElements = size_ / sizeof(uint32_t);
    buffer_UAV_desc.Buffer.StructureByteStride = 0;
    buffer_UAV_desc.Buffer.CounterOffsetInBytes = 0;
    buffer_UAV_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(handle_.buf, nullptr, &buffer_UAV_desc, temp_buffer_cpu_readable_UAV_handle);

    D3D12_CPU_DESCRIPTOR_HANDLE temp_buffer_cpu_UAV_handle =
        temp_gpu_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE temp_buffer_gpu_UAV_handle =
        temp_gpu_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    device->CopyDescriptorsSimple(1, temp_buffer_cpu_UAV_handle, temp_buffer_cpu_readable_UAV_handle,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    const UINT clear_val[4] = {data, data, data, data};
    cmd_buf->ClearUnorderedAccessViewUint(temp_buffer_gpu_UAV_handle, temp_buffer_cpu_readable_UAV_handle, handle_.buf,
                                          clear_val, 0, nullptr);

    ctx_->descriptor_heaps_to_destroy[ctx_->backend_frame].push_back(temp_cpu_descriptor_heap);
    ctx_->descriptor_heaps_to_destroy[ctx_->backend_frame].push_back(temp_gpu_descriptor_heap);
}

void Ray::Dx::Buffer::UpdateImmediate(uint32_t dst_offset, uint32_t size, const void *data, void *_cmd_buf) {
    if (type_ == eBufType::Upload) {
        uint8_t *mapped_ptr = Map();
        memcpy(mapped_ptr, data, size);
        Unmap();
    } else {
        // TODO: Maybe there is a more efficient way to do this
        auto temp_upload_buf = Buffer{"Temp upload buffer", ctx_, eBufType::Upload, size};

        uint8_t *mapped_ptr = temp_upload_buf.Map();
        memcpy(mapped_ptr, data, size);
        temp_upload_buf.Unmap();

        CopyBufferToBuffer(temp_upload_buf, 0, *this, dst_offset, size, _cmd_buf);
    }
}

void Ray::Dx::CopyBufferToBuffer(Buffer &src, const uint32_t src_offset, Buffer &dst, const uint32_t dst_offset,
                                 const uint32_t size, void *_cmd_buf) {
    auto cmd_buf = reinterpret_cast<ID3D12GraphicsCommandList *>(_cmd_buf);

    SmallVector<D3D12_RESOURCE_BARRIER, 2> barriers;

    if (/*dst.resource_state != eResState::Undefined &&*/ src.resource_state != eResState::CopySrc) {
        auto &new_barrier = barriers.emplace_back();
        new_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        new_barrier.Transition.pResource = src.dx_resource();
        new_barrier.Transition.StateBefore = DXResourceState(src.resource_state);
        new_barrier.Transition.StateAfter = DXResourceState(eResState::CopySrc);
        new_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }

    if (/*dst.resource_state != eResState::Undefined &&*/ dst.resource_state != eResState::CopyDst) {
        auto &new_barrier = barriers.emplace_back();
        new_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        new_barrier.Transition.pResource = dst.dx_resource();
        new_barrier.Transition.StateBefore = DXResourceState(dst.resource_state);
        new_barrier.Transition.StateAfter = DXResourceState(eResState::CopyDst);
        new_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }

    if (!barriers.empty()) {
        cmd_buf->ResourceBarrier(UINT(barriers.size()), barriers.data());
    }

    cmd_buf->CopyBufferRegion(dst.dx_resource(), dst_offset, src.dx_resource(), src_offset, size);

    src.resource_state = eResState::CopySrc;
    dst.resource_state = eResState::CopyDst;
}