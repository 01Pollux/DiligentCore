/*
 *  Copyright 2019-2025 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "pch.h"

#include "BufferD3D12Impl.hpp"

#include "RenderDeviceD3D12Impl.hpp"
#include "DeviceContextD3D12Impl.hpp"

#include "D3D12TypeConversions.hpp"
#include "GraphicsAccessories.hpp"
#include "DXGITypeConversions.hpp"
#include "EngineMemory.h"
#include "StringTools.hpp"

namespace Diligent
{

BufferD3D12Impl::BufferD3D12Impl(IReferenceCounters*        pRefCounters,
                                 FixedBlockMemoryAllocator& BuffViewObjMemAllocator,
                                 RenderDeviceD3D12Impl*     pRenderDeviceD3D12,
                                 const BufferDesc&          BuffDesc,
                                 const BufferData*          pBuffData /*= nullptr*/) :
    TBufferBase{
        pRefCounters,
        BuffViewObjMemAllocator,
        pRenderDeviceD3D12,
        BuffDesc,
        false,
    }
{
    ValidateBufferInitData(m_Desc, pBuffData);

    if (m_Desc.Usage == USAGE_UNIFIED)
    {
        LOG_ERROR_AND_THROW("Unified resources are not supported in Direct3D12");
    }

    Uint32 BufferAlignment = 1;
    if (m_Desc.BindFlags & BIND_UNIFORM_BUFFER)
        BufferAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

    if (m_Desc.Usage == USAGE_STAGING && m_Desc.CPUAccessFlags == CPU_ACCESS_WRITE)
        BufferAlignment = std::max(BufferAlignment, Uint32{D3D12_TEXTURE_DATA_PITCH_ALIGNMENT});

    m_Desc.Size = AlignUp(m_Desc.Size, BufferAlignment);

    if ((m_Desc.Usage == USAGE_DYNAMIC) &&
        (m_Desc.BindFlags & BIND_UNORDERED_ACCESS) == 0 &&
        (m_Desc.Mode == BUFFER_MODE_UNDEFINED || m_Desc.Mode == BUFFER_MODE_STRUCTURED))
    {
        // Dynamic constant/vertex/index buffers are suballocated in the upload heap when Map() is called.
        // Dynamic buffers with UAV flags as well as formatted buffers need to be allocated in GPU-only memory.
        // Dynamic upload heap buffer is always in D3D12_RESOURCE_STATE_GENERIC_READ state.

        SetState(RESOURCE_STATE_GENERIC_READ);
    }
    else
    {
        VERIFY(m_Desc.Usage != USAGE_DYNAMIC || PlatformMisc::CountOneBits(m_Desc.ImmediateContextMask) <= 1,
               "ImmediateContextMask must contain single set bit, this error should've been handled in ValidateBufferDesc()");

        D3D12_RESOURCE_DESC d3d12BuffDesc{};
        d3d12BuffDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        d3d12BuffDesc.Alignment          = 0;
        d3d12BuffDesc.Width              = m_Desc.Size;
        d3d12BuffDesc.Height             = 1;
        d3d12BuffDesc.DepthOrArraySize   = 1;
        d3d12BuffDesc.MipLevels          = 1;
        d3d12BuffDesc.Format             = DXGI_FORMAT_UNKNOWN;
        d3d12BuffDesc.SampleDesc.Count   = 1;
        d3d12BuffDesc.SampleDesc.Quality = 0;
        // Layout must be D3D12_TEXTURE_LAYOUT_ROW_MAJOR, as buffer memory layouts are
        // understood by applications and row-major texture data is commonly marshaled through buffers.
        d3d12BuffDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d3d12BuffDesc.Flags  = D3D12_RESOURCE_FLAG_NONE;
        if ((m_Desc.BindFlags & BIND_UNORDERED_ACCESS) || (m_Desc.BindFlags & BIND_RAY_TRACING))
            d3d12BuffDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!(m_Desc.BindFlags & BIND_SHADER_RESOURCE) && !(m_Desc.BindFlags & BIND_RAY_TRACING))
            d3d12BuffDesc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        ID3D12Device* pd3d12Device = pRenderDeviceD3D12->GetD3D12Device();

        if (m_Desc.Usage == USAGE_SPARSE)
        {
            HRESULT hr = pd3d12Device->CreateReservedResource(&d3d12BuffDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                              __uuidof(m_pd3d12Resource),
                                                              reinterpret_cast<void**>(static_cast<ID3D12Resource**>(&m_pd3d12Resource)));
            if (FAILED(hr))
                LOG_ERROR_AND_THROW("Failed to create D3D12 buffer");

            if (*m_Desc.Name != 0)
                m_pd3d12Resource->SetName(WidenString(m_Desc.Name).c_str());

            SetState(RESOURCE_STATE_UNDEFINED);
        }
        else
        {
            D3D12_HEAP_PROPERTIES HeapProps{};
            if (m_Desc.Usage == USAGE_STAGING)
                HeapProps.Type = m_Desc.CPUAccessFlags == CPU_ACCESS_READ ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
            else
                HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

            if (HeapProps.Type == D3D12_HEAP_TYPE_READBACK)
                SetState(RESOURCE_STATE_COPY_DEST);
            else if (HeapProps.Type == D3D12_HEAP_TYPE_UPLOAD)
                SetState(RESOURCE_STATE_GENERIC_READ);
            HeapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            HeapProps.CreationNodeMask     = 1;
            HeapProps.VisibleNodeMask      = 1;

            const Uint64 InitialDataSize = (pBuffData != nullptr && pBuffData->pData != nullptr) ?
                std::min(pBuffData->DataSize, d3d12BuffDesc.Width) :
                0;

            if (InitialDataSize > 0)
                SetState(RESOURCE_STATE_COPY_DEST);

            if (!IsInKnownState())
                SetState(RESOURCE_STATE_UNDEFINED);

            const SoftwareQueueIndex CmdQueueInd = pBuffData && pBuffData->pContext ?
                ClassPtrCast<DeviceContextD3D12Impl>(pBuffData->pContext)->GetCommandQueueId() :
                SoftwareQueueIndex{PlatformMisc::GetLSB(m_Desc.ImmediateContextMask)};

            const D3D12_RESOURCE_STATES StateMask = InitialDataSize > 0 ?
                GetSupportedD3D12ResourceStatesForCommandList(pRenderDeviceD3D12->GetCommandQueueType(CmdQueueInd)) :
                static_cast<D3D12_RESOURCE_STATES>(~0u);

            const D3D12_RESOURCE_STATES d3d12State = ResourceStateFlagsToD3D12ResourceStates(GetState()) & StateMask;

            // By default, committed resources and heaps are almost always zeroed upon creation.
            // CREATE_NOT_ZEROED flag allows this to be elided in some scenarios to lower the overhead
            // of creating the heap. No need to zero the resource if we initialize it.
            const D3D12_HEAP_FLAGS d3d12HeapFlags = InitialDataSize > 0 ?
                D3D12_HEAP_FLAG_CREATE_NOT_ZEROED :
                D3D12_HEAP_FLAG_NONE;

            HRESULT hr = pd3d12Device->CreateCommittedResource(
                &HeapProps, d3d12HeapFlags, &d3d12BuffDesc, d3d12State,
                nullptr, // pOptimizedClearValue
                __uuidof(m_pd3d12Resource),
                reinterpret_cast<void**>(static_cast<ID3D12Resource**>(&m_pd3d12Resource)));
            if (FAILED(hr))
                LOG_ERROR_AND_THROW("Failed to create D3D12 buffer");

            if (*m_Desc.Name != 0)
                m_pd3d12Resource->SetName(WidenString(m_Desc.Name).c_str());

            if (InitialDataSize > 0)
            {
                D3D12_HEAP_PROPERTIES UploadHeapProps{};
                UploadHeapProps.Type                 = D3D12_HEAP_TYPE_UPLOAD;
                UploadHeapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                UploadHeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                UploadHeapProps.CreationNodeMask     = 1;
                UploadHeapProps.VisibleNodeMask      = 1;

                d3d12BuffDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

                CComPtr<ID3D12Resource> UploadBuffer;
                hr = pd3d12Device->CreateCommittedResource(
                    &UploadHeapProps, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, // Do not zero the heap
                    &d3d12BuffDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(UploadBuffer),
                    reinterpret_cast<void**>(static_cast<ID3D12Resource**>(&UploadBuffer)));
                if (FAILED(hr))
                    LOG_ERROR_AND_THROW("Failed to create upload buffer");

                const std::wstring UploadBufferName = std::wstring{L"Upload buffer for buffer '"} + WidenString(m_Desc.Name) + L'\'';
                UploadBuffer->SetName(UploadBufferName.c_str());

                void* DestAddress = nullptr;

                hr = UploadBuffer->Map(0, nullptr, &DestAddress);
                if (FAILED(hr))
                    LOG_ERROR_AND_THROW("Failed to map upload buffer");
                memcpy(DestAddress, pBuffData->pData, StaticCast<size_t>(InitialDataSize));
                UploadBuffer->Unmap(0, nullptr);

                RenderDeviceD3D12Impl::PooledCommandContext InitContext = pRenderDeviceD3D12->AllocateCommandContext(CmdQueueInd);
                // copy data to the intermediate upload heap and then schedule a copy from the upload heap to the default buffer
                VERIFY_EXPR(CheckState(RESOURCE_STATE_COPY_DEST));
                // We MUST NOT call TransitionResource() from here, because
                // it will call AddRef() and potentially Release(), while
                // the object is not constructed yet
                InitContext->CopyResource(m_pd3d12Resource, UploadBuffer);

                // Command list fence should only be signaled when submitting cmd list
                // from the immediate context, otherwise the basic requirement will be violated
                // as in the scenario below
                // See http://diligentgraphics.com/diligent-engine/architecture/d3d12/managing-resource-lifetimes/
                //
                //  Signaled Fence  |        Immediate Context               |            InitContext            |
                //                  |                                        |                                   |
                //    N             |  Draw(ResourceX)                       |                                   |
                //                  |  Release(ResourceX)                    |                                   |
                //                  |   - (ResourceX, N) -> Release Queue    |                                   |
                //                  |                                        | CopyResource()                    |
                //   N+1            |                                        | CloseAndExecuteCommandContext()   |
                //                  |                                        |                                   |
                //   N+2            |  CloseAndExecuteCommandContext()       |                                   |
                //                  |   - Cmd list is submitted with number  |                                   |
                //                  |     N+1, but resource it references    |                                   |
                //                  |     was added to the delete queue      |                                   |
                //                  |     with value N                       |                                   |
                pRenderDeviceD3D12->CloseAndExecuteTransientCommandContext(CmdQueueInd, std::move(InitContext));

                // Add reference to the object to the release queue to keep it alive
                // until copy operation is complete. This must be done after
                // submitting command list for execution!
                pRenderDeviceD3D12->SafeReleaseDeviceObject(std::move(UploadBuffer), Uint64{1} << CmdQueueInd);
            }

            if (m_Desc.BindFlags & BIND_UNIFORM_BUFFER)
            {
                m_CBVDescriptorAllocation = pRenderDeviceD3D12->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                CreateCBV(m_CBVDescriptorAllocation.GetCpuHandle());
            }
        }
    }

    m_MemoryProperties = MEMORY_PROPERTY_HOST_COHERENT;
}

static BufferDesc BufferDescFromD3D12Resource(BufferDesc BuffDesc, ID3D12Resource* pd3d12Buffer)
{
    DEV_CHECK_ERR(BuffDesc.Usage != USAGE_DYNAMIC, "Dynamic buffers cannot be attached to native d3d12 resource");

    D3D12_RESOURCE_DESC d3d12BuffDesc = pd3d12Buffer->GetDesc();
    DEV_CHECK_ERR(d3d12BuffDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER, "D3D12 resource is not a buffer");

    DEV_CHECK_ERR(BuffDesc.Size == 0 || BuffDesc.Size == d3d12BuffDesc.Width, "Buffer size specified by the BufferDesc (", BuffDesc.Size, ") does not match d3d12 resource size (", d3d12BuffDesc.Width, ")");
    BuffDesc.Size = StaticCast<Uint32>(d3d12BuffDesc.Width);

    if (d3d12BuffDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    {
        DEV_CHECK_ERR(BuffDesc.BindFlags == 0 || (BuffDesc.BindFlags & BIND_UNORDERED_ACCESS), "BIND_UNORDERED_ACCESS flag is not specified by the BufferDesc, while d3d12 resource was created with D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS flag");
        BuffDesc.BindFlags |= BIND_UNORDERED_ACCESS;
    }
    if (d3d12BuffDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
    {
        DEV_CHECK_ERR(!(BuffDesc.BindFlags & BIND_SHADER_RESOURCE), "BIND_SHADER_RESOURCE flag is specified by the BufferDesc, while d3d12 resource was created with D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE flag");
        BuffDesc.BindFlags &= ~BIND_SHADER_RESOURCE;
    }
    else
        BuffDesc.BindFlags |= BIND_SHADER_RESOURCE;

    if ((BuffDesc.BindFlags & BIND_UNORDERED_ACCESS) || (BuffDesc.BindFlags & BIND_SHADER_RESOURCE))
    {
        if (BuffDesc.Mode == BUFFER_MODE_STRUCTURED || BuffDesc.Mode == BUFFER_MODE_FORMATTED)
        {
            DEV_CHECK_ERR(BuffDesc.ElementByteStride != 0, "Element byte stride cannot be 0 for a structured or a formatted buffer");
        }
        else if (BuffDesc.Mode == BUFFER_MODE_RAW)
        {
        }
        else
        {
            UNEXPECTED("Buffer mode must be structured or formatted");
        }
    }

    // Warning: can not detect sparse buffer

    return BuffDesc;
}

BufferD3D12Impl::BufferD3D12Impl(IReferenceCounters*        pRefCounters,
                                 FixedBlockMemoryAllocator& BuffViewObjMemAllocator,
                                 RenderDeviceD3D12Impl*     pRenderDeviceD3D12,
                                 const BufferDesc&          BuffDesc,
                                 RESOURCE_STATE             InitialState,
                                 ID3D12Resource*            pd3d12Buffer) :
    TBufferBase{
        pRefCounters,
        BuffViewObjMemAllocator,
        pRenderDeviceD3D12,
        BufferDescFromD3D12Resource(BuffDesc, pd3d12Buffer),
        false,
    }
{
    m_pd3d12Resource = pd3d12Buffer;
    SetState(InitialState);

    if (m_Desc.BindFlags & BIND_UNIFORM_BUFFER)
    {
        m_CBVDescriptorAllocation = pRenderDeviceD3D12->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CreateCBV(m_CBVDescriptorAllocation.GetCpuHandle());
    }

    m_MemoryProperties = MEMORY_PROPERTY_HOST_COHERENT;
}

BufferD3D12Impl::~BufferD3D12Impl()
{
    // D3D12 object can only be destroyed when it is no longer used by the GPU
    GetDevice()->SafeReleaseDeviceObject(std::move(m_pd3d12Resource), m_Desc.ImmediateContextMask);
}

void BufferD3D12Impl::CreateViewInternal(const BufferViewDesc& OrigViewDesc, IBufferView** ppView, bool bIsDefaultView)
{
    VERIFY(ppView != nullptr, "Null pointer provided");
    if (!ppView) return;
    VERIFY(*ppView == nullptr, "Overwriting reference to existing object may cause memory leaks");

    *ppView = nullptr;

    try
    {
        RenderDeviceD3D12Impl*     pDeviceD3D12Impl  = GetDevice();
        FixedBlockMemoryAllocator& BuffViewAllocator = pDeviceD3D12Impl->GetBuffViewObjAllocator();
        VERIFY(&BuffViewAllocator == &m_dbgBuffViewAllocator, "Buff view allocator does not match allocator provided at buffer initialization");

        BufferViewDesc ViewDesc = OrigViewDesc;
        if (ViewDesc.ViewType == BUFFER_VIEW_UNORDERED_ACCESS)
        {
            DescriptorHeapAllocation UAVHandleAlloc = pDeviceD3D12Impl->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            CreateUAV(ViewDesc, UAVHandleAlloc.GetCpuHandle());
            *ppView = NEW_RC_OBJ(BuffViewAllocator, "BufferViewD3D12Impl instance", BufferViewD3D12Impl, bIsDefaultView ? this : nullptr)(GetDevice(), ViewDesc, this, std::move(UAVHandleAlloc), bIsDefaultView);
        }
        else if (ViewDesc.ViewType == BUFFER_VIEW_SHADER_RESOURCE)
        {
            DescriptorHeapAllocation SRVHandleAlloc = pDeviceD3D12Impl->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            CreateSRV(ViewDesc, SRVHandleAlloc.GetCpuHandle());
            *ppView = NEW_RC_OBJ(BuffViewAllocator, "BufferViewD3D12Impl instance", BufferViewD3D12Impl, bIsDefaultView ? this : nullptr)(GetDevice(), ViewDesc, this, std::move(SRVHandleAlloc), bIsDefaultView);
        }

        if (!bIsDefaultView && *ppView)
            (*ppView)->AddRef();
    }
    catch (const std::runtime_error&)
    {
        const char* ViewTypeName = GetBufferViewTypeLiteralName(OrigViewDesc.ViewType);
        LOG_ERROR("Failed to create view \"", OrigViewDesc.Name ? OrigViewDesc.Name : "", "\" (", ViewTypeName, ") for buffer \"", m_Desc.Name, "\"");
    }
}

void BufferD3D12Impl::CreateUAV(BufferViewDesc& UAVDesc, D3D12_CPU_DESCRIPTOR_HANDLE UAVDescriptor) const
{
    ValidateAndCorrectBufferViewDesc(m_Desc, UAVDesc, GetDevice()->GetAdapterInfo().Buffer.StructuredBufferOffsetAlignment);

    D3D12_UNORDERED_ACCESS_VIEW_DESC D3D12_UAVDesc;
    BufferViewDesc_to_D3D12_UAV_DESC(m_Desc, UAVDesc, D3D12_UAVDesc);

    ID3D12Device* pd3d12Device = GetDevice()->GetD3D12Device();
    pd3d12Device->CreateUnorderedAccessView(m_pd3d12Resource, nullptr, &D3D12_UAVDesc, UAVDescriptor);
}

void BufferD3D12Impl::CreateSRV(struct BufferViewDesc& SRVDesc, D3D12_CPU_DESCRIPTOR_HANDLE SRVDescriptor) const
{
    ValidateAndCorrectBufferViewDesc(m_Desc, SRVDesc, GetDevice()->GetAdapterInfo().Buffer.StructuredBufferOffsetAlignment);

    D3D12_SHADER_RESOURCE_VIEW_DESC D3D12_SRVDesc;
    BufferViewDesc_to_D3D12_SRV_DESC(m_Desc, SRVDesc, D3D12_SRVDesc);

    ID3D12Device* pd3d12Device = GetDevice()->GetD3D12Device();
    pd3d12Device->CreateShaderResourceView(m_pd3d12Resource, &D3D12_SRVDesc, SRVDescriptor);
}

void BufferD3D12Impl::CreateCBV(D3D12_CPU_DESCRIPTOR_HANDLE CBVDescriptor,
                                Uint64                      Offset,
                                Uint64                      Size) const
{
    VERIFY((Offset % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) == 0, "Offset (", Offset, ") must be ", D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, "-aligned");
    VERIFY(Offset + Size <= m_Desc.Size, "Range is out of bounds");
    if (Size == 0)
    {
        // CBV can be at most 65536 bytes (D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16 bytes)
        Size = std::min(m_Desc.Size - Offset, Uint64{D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT} * 16u);
    }

    D3D12_CONSTANT_BUFFER_VIEW_DESC D3D12_CBVDesc;
    D3D12_CBVDesc.BufferLocation = m_pd3d12Resource->GetGPUVirtualAddress() + Offset;
    D3D12_CBVDesc.SizeInBytes    = StaticCast<UINT>(AlignUp(Size, Uint32{D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}));

    ID3D12Device* pd3d12Device = GetDevice()->GetD3D12Device();
    pd3d12Device->CreateConstantBufferView(&D3D12_CBVDesc, CBVDescriptor);
}

ID3D12Resource* BufferD3D12Impl::GetD3D12Buffer(Uint64& DataStartByteOffset, IDeviceContext* pContext)
{
    ID3D12Resource* pd3d12Resource = GetD3D12Resource();
    if (pd3d12Resource != nullptr)
    {
        VERIFY(m_Desc.Usage != USAGE_DYNAMIC || (m_Desc.BindFlags & (BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS)) != 0, "Expected non-dynamic buffer or a buffer with SRV or UAV bind flags");
        DataStartByteOffset = 0;
        return pd3d12Resource;
    }
    else
    {
        VERIFY(m_Desc.Usage == USAGE_DYNAMIC, "Dynamic buffer is expected");
        DeviceContextD3D12Impl* pCtxD3D12 = ClassPtrCast<DeviceContextD3D12Impl>(pContext);
        return pCtxD3D12->GetDynamicBufferD3D12ResourceAndOffset(this, DataStartByteOffset);
    }
}

void BufferD3D12Impl::SetD3D12ResourceState(D3D12_RESOURCE_STATES state)
{
    SetState(D3D12ResourceStatesToResourceStateFlags(state));
}

D3D12_RESOURCE_STATES BufferD3D12Impl::GetD3D12ResourceState() const
{
    return ResourceStateFlagsToD3D12ResourceStates(GetState());
}

SparseBufferProperties BufferD3D12Impl::GetSparseProperties() const
{
    DEV_CHECK_ERR(m_Desc.Usage == USAGE_SPARSE,
                  "IBuffer::GetSparseProperties() must only be used for sparse buffer");

    ID3D12Device* pd3d12Device = m_pDevice->GetD3D12Device();

    UINT             NumTilesForEntireResource = 0;
    D3D12_TILE_SHAPE StandardTileShapeForNonPackedMips{};
    pd3d12Device->GetResourceTiling(GetD3D12Resource(),
                                    &NumTilesForEntireResource,
                                    nullptr,
                                    &StandardTileShapeForNonPackedMips,
                                    nullptr,
                                    0,
                                    nullptr);

    VERIFY(StandardTileShapeForNonPackedMips.WidthInTexels == D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,
           "Expected to be a standard block size");

    SparseBufferProperties Props;
    Props.AddressSpaceSize = Uint64{NumTilesForEntireResource} * StandardTileShapeForNonPackedMips.WidthInTexels;
    Props.BlockSize        = StandardTileShapeForNonPackedMips.WidthInTexels;
    return Props;
}

} // namespace Diligent
