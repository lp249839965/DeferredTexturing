//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"

#include "GraphicsTypes.h"
#include "..\\Exceptions.h"
#include "..\\Utility.h"
#include "..\\Serialization.h"
#include "..\\FileIO.h"

namespace SampleFramework12
{

// == DescriptorHeap ==============================================================================

DescriptorHeap::~DescriptorHeap()
{
    Assert_(Heaps[0] == nullptr);
}

void DescriptorHeap::Init(uint32 numPersistent, uint32 numTemporary, D3D12_DESCRIPTOR_HEAP_TYPE heapType, bool shaderVisible)
{
    Shutdown();

    uint32 totalNumDescriptors = numPersistent + numTemporary;
    Assert_(totalNumDescriptors > 0);

    NumPersistent = numPersistent;
    NumTemporary = numTemporary;
    HeapType = heapType;
    ShaderVisible = shaderVisible;
    if(heapType == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || heapType == D3D12_DESCRIPTOR_HEAP_TYPE_DSV)
        ShaderVisible = false;

    NumHeaps = ShaderVisible ? 2 : 1;

    DeadList.Init(numPersistent);
    for(uint32 i = 0; i < numPersistent; ++i)
        DeadList[i] = uint32(i);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = { };
    heapDesc.NumDescriptors = uint32(totalNumDescriptors);
    heapDesc.Type = heapType;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if(ShaderVisible)
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    for(uint32 i = 0; i < NumHeaps; ++i)
    {
        DXCall(DX12::Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&Heaps[i])));
        CPUStart[i] = Heaps[i]->GetCPUDescriptorHandleForHeapStart();
        if(ShaderVisible)
            GPUStart[i] = Heaps[i]->GetGPUDescriptorHandleForHeapStart();
    }

    DescriptorSize = DX12::Device->GetDescriptorHandleIncrementSize(heapType);
}

void DescriptorHeap::Shutdown()
{
    Assert_(PersistentAllocated == 0);
    for(uint64 i = 0; i < ArraySize_(Heaps); ++i)
        DX12::Release(Heaps[i]);
}

PersistentDescriptorAlloc DescriptorHeap::AllocatePersistent()
{
    Assert_(Heaps[0] != nullptr);

    AcquireSRWLockExclusive(&Lock);

    Assert_(PersistentAllocated < NumPersistent);
    uint32 idx = DeadList[PersistentAllocated];
    ++PersistentAllocated;

    ReleaseSRWLockExclusive(&Lock);

    PersistentDescriptorAlloc alloc;
    alloc.Index = idx;
    for(uint32 i = 0; i < NumHeaps; ++i)
    {
        alloc.Handles[i] = CPUStart[i];
        alloc.Handles[i].ptr += idx * DescriptorSize;
    }

    return alloc;
}

void DescriptorHeap::FreePersistent(uint32& idx)
{
    if(idx == uint32(-1))
        return;

    Assert_(idx < NumPersistent);
    Assert_(Heaps[0] != nullptr);

    AcquireSRWLockExclusive(&Lock);

    Assert_(PersistentAllocated > 0);
    DeadList[PersistentAllocated - 1] = idx;
    --PersistentAllocated;

    ReleaseSRWLockExclusive(&Lock);

    idx = uint32(-1);
}

void DescriptorHeap::FreePersistent(D3D12_CPU_DESCRIPTOR_HANDLE& handle)
{
    Assert_(NumHeaps == 1);
    if(handle.ptr != 0)
    {
        uint32 idx = IndexFromHandle(handle);
        FreePersistent(idx);
        handle = { };
    }
}

void DescriptorHeap::FreePersistent(D3D12_GPU_DESCRIPTOR_HANDLE& handle)
{
    Assert_(NumHeaps == 1);
    if(handle.ptr != 0)
    {
        uint32 idx = IndexFromHandle(handle);
        FreePersistent(idx);
        handle = { };
    }
}

TempDescriptorAlloc DescriptorHeap::AllocateTemporary(uint32 count)
{
    Assert_(Heaps[0] != nullptr);
    Assert_(count > 0);

    uint32 tempIdx = uint32(InterlockedAdd64(&TemporaryAllocated, count)) - count;
    Assert_(tempIdx < NumTemporary);

    uint32 finalIdx = tempIdx + NumPersistent;

    TempDescriptorAlloc alloc;
    alloc.StartCPUHandle = CPUStart[HeapIndex];
    alloc.StartCPUHandle.ptr += finalIdx * DescriptorSize;
    alloc.StartGPUHandle = GPUStart[HeapIndex];
    alloc.StartGPUHandle.ptr += finalIdx * DescriptorSize;
    alloc.StartIndex = finalIdx;

    return alloc;
}

void DescriptorHeap::EndFrame()
{
    Assert_(Heaps[0] != nullptr);
    TemporaryAllocated = 0;
    HeapIndex = (HeapIndex + 1) % NumHeaps;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CPUHandleFromIndex(uint32 descriptorIdx) const
{
    Assert_(Heaps[0] != nullptr);
    Assert_(descriptorIdx < TotalNumDescriptors());
    D3D12_CPU_DESCRIPTOR_HANDLE handle = CPUStart[HeapIndex];
    handle.ptr += descriptorIdx * DescriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GPUHandleFromIndex(uint32 descriptorIdx) const
{
    Assert_(Heaps[0] != nullptr);
    Assert_(descriptorIdx < TotalNumDescriptors());
    Assert_(ShaderVisible);
    D3D12_GPU_DESCRIPTOR_HANDLE handle = GPUStart[HeapIndex];
    handle.ptr += descriptorIdx * DescriptorSize;
    return handle;
}

uint32 DescriptorHeap::IndexFromHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    Assert_(Heaps[0] != nullptr);
    Assert_(handle.ptr >= CPUStart[HeapIndex].ptr);
    Assert_(handle.ptr < CPUStart[HeapIndex].ptr + DescriptorSize * TotalNumDescriptors());
    Assert_((handle.ptr - CPUStart[HeapIndex].ptr) % DescriptorSize == 0);
    return uint32(handle.ptr - CPUStart[HeapIndex].ptr) / DescriptorSize;
}

uint32 DescriptorHeap::IndexFromHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle)
{
    Assert_(Heaps[0] != nullptr);
    Assert_(handle.ptr >= GPUStart[HeapIndex].ptr);
    Assert_(handle.ptr < GPUStart[HeapIndex].ptr + DescriptorSize * TotalNumDescriptors());
    Assert_((handle.ptr - GPUStart[HeapIndex].ptr) % DescriptorSize == 0);
    return uint32(handle.ptr - GPUStart[HeapIndex].ptr) / DescriptorSize;
}

ID3D12DescriptorHeap* DescriptorHeap::CurrentHeap() const
{
    Assert_(Heaps[0] != nullptr);
    return Heaps[HeapIndex];
}

// == Buffer ======================================================================================

Buffer::Buffer()
{
}

Buffer::~Buffer()
{
    Assert_(Resource == nullptr);
}

void Buffer::Initialize(uint64 size, uint64 alignment, bool32 dynamic, bool32 gpuWritable,
                        bool32 allowUAV, const void* initData, D3D12_RESOURCE_STATES initialState,
                        ID3D12Heap* heap, uint64 heapOffset, const wchar* name)
{
    Assert_(size > 0);
    Assert_(alignment > 0);

    Size = AlignTo(size, alignment);
    Alignment = alignment;
    Dynamic = dynamic;
    GPUWritable = gpuWritable;
    CurrBuffer = 0;
    CPUAddress = nullptr;
    GPUAddress = 0;
    Heap = nullptr;
    HeapOffset = 0;

    Assert_(allowUAV == false || dynamic == false);
    Assert_(allowUAV == false || gpuWritable);
    Assert_(dynamic || gpuWritable);

    D3D12_RESOURCE_DESC resourceDesc = { };
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = dynamic ? Size * DX12::RenderLatency : Size;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.Flags = allowUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Alignment = 0;

    const D3D12_HEAP_PROPERTIES* heapProps = gpuWritable ? DX12::GetDefaultHeapProps() : DX12::GetUploadHeapProps() ;
    D3D12_RESOURCE_STATES resourceState = initialState;
    if(gpuWritable == false)
        resourceState = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if(initData)
        resourceState = D3D12_RESOURCE_STATE_COMMON;

    if(heap)
    {
        Heap = heap;
        HeapOffset = heapOffset;
        DXCall(DX12::Device->CreatePlacedResource(heap, heapOffset, &resourceDesc, resourceState,
                                                    nullptr, IID_PPV_ARGS(&Resource)));
    }
    else
    {
        DXCall(DX12::Device->CreateCommittedResource(heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                        resourceState, nullptr, IID_PPV_ARGS(&Resource)));
    }

    if(name)
        Resource->SetName(name);

    GPUAddress = Resource->GetGPUVirtualAddress();

    if(gpuWritable == false)
    {
        D3D12_RANGE readRange = { };
        DXCall(Resource->Map(0, &readRange, reinterpret_cast<void**>(&CPUAddress)));
    }

    if(initData && gpuWritable == false)
    {
        for(uint64 i = 0; i < DX12::RenderLatency; ++i)
        {
            uint8* dstMem = CPUAddress + Size * i;
            memcpy(dstMem, initData, size);
        }

    }
    else if(initData)
    {
        UploadContext uploadContext = DX12::ResourceUploadBegin(resourceDesc.Width);

        memcpy(uploadContext.CPUAddress, initData, size);
        if(dynamic)
            memcpy((uint8*)uploadContext.CPUAddress + size, initData, size);

        uploadContext.CmdList->CopyBufferRegion(Resource, 0, uploadContext.Resource, uploadContext.ResourceOffset, size);

        DX12::ResourceUploadEnd(uploadContext);
    }
}

void Buffer::Shutdown()
{
    DX12::DeferredRelease(Resource);
}

MapResult Buffer::Map()
{
    Assert_(Initialized());
    Assert_(Dynamic);
    Assert_(GPUWritable == false);

    // Make sure that we only do this at most once per frame
    Assert_(UploadFrame != DX12::CurrentCPUFrame);
    UploadFrame = DX12::CurrentCPUFrame;
    CurrBuffer = (CurrBuffer + 1) % DX12::RenderLatency;

    MapResult result;
    result.ResourceOffset = CurrBuffer * Size;
    result.CPUAddress = CPUAddress + CurrBuffer * Size;
    result.GPUAddress = GPUAddress + CurrBuffer * Size;
    result.Resource = Resource;
    return result;
}

MapResult Buffer::MapAndSetData(const void* data, uint64 dataSize)
{
    Assert_(dataSize <= Size);
    MapResult result = Map();
    memcpy(result.CPUAddress, data, dataSize);
    return result;
}

uint64 Buffer::UpdateData(const void* srcData, uint64 srcSize, uint64 dstOffset, bool allowOverwrite)
{
    Assert_(GPUWritable && Dynamic);
    Assert_(dstOffset + srcSize <= Size);

    if(UploadFrame != DX12::CurrentCPUFrame)
    {
        UploadFrame = DX12::CurrentCPUFrame;
        CurrBuffer = (CurrBuffer + 1) % DX12::RenderLatency;
    }
    else
    {
        // Make sure that we only do this at most once per frame if allowOverwrite is false
        Assert_(allowOverwrite);
    }

    uint64 currOffset = CurrBuffer * Size;

    UploadContext uploadContext = DX12::ResourceUploadBegin(srcSize);

    memcpy(uploadContext.CPUAddress, srcData, srcSize);

    uploadContext.CmdList->CopyBufferRegion(Resource, currOffset + dstOffset, uploadContext.Resource, uploadContext.ResourceOffset, srcSize);

    DX12::ResourceUploadEnd(uploadContext);

    return GPUAddress + currOffset;
}

void Buffer::Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const
{
    Assert_(Resource != nullptr);
    DX12::TransitionResource(cmdList, Resource, before, after);
}

void Buffer::MakeReadable(ID3D12GraphicsCommandList* cmdList) const
{
    Assert_(Resource != nullptr);
    DX12::TransitionResource(cmdList, Resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
}

void Buffer::MakeWritable(ID3D12GraphicsCommandList* cmdList) const
{
    Assert_(Resource != nullptr);
    DX12::TransitionResource(cmdList, Resource, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void Buffer::UAVBarrier(ID3D12GraphicsCommandList* cmdList) const
{
    Assert_(Resource != nullptr);
    D3D12_RESOURCE_BARRIER barrier = { };
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = Resource;
    cmdList->ResourceBarrier(1, &barrier);
}

#if UseAsserts_

bool Buffer::ReadyForBinding() const
{
    return Initialized();
}

#endif

// == ConstantBuffer ==============================================================================

ConstantBuffer::ConstantBuffer()
{
}

ConstantBuffer::~ConstantBuffer()
{
}

void ConstantBuffer::Initialize(const ConstantBufferInit& init)
{
    InternalBuffer.Initialize(init.Size, DX12::ConstantBufferAlignment, init.Dynamic, init.GPUWritable,
                              false, init.InitData, init.InitialState, init.Heap, init.HeapOffset, init.Name);
}

void ConstantBuffer::Shutdown()
{
    InternalBuffer.Shutdown();
}

void ConstantBuffer::SetAsGfxRootParameter(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter) const
{
    Assert_(InternalBuffer.ReadyForBinding());
    cmdList->SetGraphicsRootConstantBufferView(rootParameter, CurrentGPUAddress);
}

void ConstantBuffer::SetAsComputeRootParameter(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter) const
{
    Assert_(InternalBuffer.ReadyForBinding());
    cmdList->SetComputeRootConstantBufferView(rootParameter, CurrentGPUAddress);
}

void* ConstantBuffer::Map()
{
    MapResult mapResult = InternalBuffer.Map();
    CurrentGPUAddress = mapResult.GPUAddress;
    return mapResult.CPUAddress;
}

void ConstantBuffer::MapAndSetData(const void* data, uint64 dataSize)
{
    Assert_(dataSize <= InternalBuffer.Size);
    void* cpuAddr = Map();
    memcpy(cpuAddr, data, dataSize);
}

void ConstantBuffer::UpdateData(const void* srcData, uint64 srcSize, uint64 dstOffset, bool allowOverwrite)
{
    CurrentGPUAddress = InternalBuffer.UpdateData(srcData, srcSize, dstOffset, allowOverwrite);
}

// == StructuredBuffer ============================================================================

StructuredBuffer::StructuredBuffer()
{

}

StructuredBuffer::~StructuredBuffer()
{
    Assert_(NumElements == 0);
}

void StructuredBuffer::Initialize(const StructuredBufferInit& init)
{
    Shutdown();

    Assert_(init.Stride > 0);
    Assert_(init.NumElements > 0);
    Stride = init.Stride;
    NumElements = init.NumElements;

    InternalBuffer.Initialize(Stride * NumElements, Stride, init.Dynamic, init.GPUWritable, init.CreateUAV,
                              init.InitData, init.InitialState, init.Heap, init.HeapOffset, init.Name);
    GPUAddress = InternalBuffer.GPUAddress;

    PersistentDescriptorAlloc srvAlloc = DX12::SRVDescriptorHeap.AllocatePersistent();
    SRV = srvAlloc.Index;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srvDesc.Buffer.NumElements = uint32(NumElements);
    srvDesc.Buffer.StructureByteStride = uint32(Stride);

    for(uint32 i = 0; i < DX12::SRVDescriptorHeap.NumHeaps; ++i)
    {
        if(init.Dynamic)
            srvDesc.Buffer.FirstElement = uint32(NumElements * i);
        DX12::Device->CreateShaderResourceView(InternalBuffer.Resource, &srvDesc, srvAlloc.Handles[i]);
    }

    if(init.CreateUAV)
    {
        Assert_(init.Dynamic == false);

        ID3D12Resource* counterRes = nullptr;
        if(init.UseCounter)
        {
            D3D12_RESOURCE_DESC resourceDesc = { };
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Width = sizeof(uint32);
            resourceDesc.Height = 1;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
            resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.SampleDesc.Quality = 0;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            resourceDesc.Alignment = 0;
            DXCall(DX12::Device->CreateCommittedResource(DX12::GetDefaultHeapProps(), D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&CounterResource)));

            counterRes = CounterResource;

            CounterUAV = DX12::UAVDescriptorHeap.AllocatePersistent().Handles[0];

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uavDesc.Format = DXGI_FORMAT_UNKNOWN;
            uavDesc.Buffer.CounterOffsetInBytes = 0;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
            uavDesc.Buffer.NumElements = 1;
            uavDesc.Buffer.StructureByteStride = sizeof(uint32);
            DX12::Device->CreateUnorderedAccessView(counterRes, nullptr, &uavDesc, CounterUAV);
        }

        UAV = DX12::UAVDescriptorHeap.AllocatePersistent().Handles[0];

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = { };
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        uavDesc.Buffer.NumElements = uint32(NumElements);
        uavDesc.Buffer.StructureByteStride = uint32(Stride);
        DX12::Device->CreateUnorderedAccessView(InternalBuffer.Resource, counterRes, &uavDesc, UAV);
    }
}

void StructuredBuffer::Shutdown()
{
    DX12::SRVDescriptorHeap.FreePersistent(SRV);
    DX12::UAVDescriptorHeap.FreePersistent(UAV);
    DX12::UAVDescriptorHeap.FreePersistent(CounterUAV);
    InternalBuffer.Shutdown();
    DX12::DeferredRelease(CounterResource);
    Stride = 0;
    NumElements = 0;
}

D3D12_VERTEX_BUFFER_VIEW StructuredBuffer::VBView() const
{
    Assert_(InternalBuffer.ReadyForBinding());
    D3D12_VERTEX_BUFFER_VIEW vbView = { };
    vbView.BufferLocation = GPUAddress;
    vbView.StrideInBytes = uint32(Stride);
    vbView.SizeInBytes = uint32(InternalBuffer.Size);
    return vbView;
}

void* StructuredBuffer::Map()
{
    MapResult mapResult = InternalBuffer.Map();
    GPUAddress = mapResult.GPUAddress;

    return mapResult.CPUAddress;
}

void StructuredBuffer::MapAndSetData(const void* data, uint64 numElements)
{
    Assert_(numElements <= NumElements);
    void* cpuAddr = Map();
    memcpy(cpuAddr, data, numElements * Stride);
}

void StructuredBuffer::UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset, bool allowOverwrite)
{
    GPUAddress = InternalBuffer.UpdateData(srcData, srcNumElements * Stride, dstElemOffset * Stride, allowOverwrite);
}

void StructuredBuffer::Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const
{
    InternalBuffer.Transition(cmdList, before, after);
}

void StructuredBuffer::MakeReadable(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.MakeReadable(cmdList);
}

void StructuredBuffer::MakeWritable(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.MakeWritable(cmdList);
}

void StructuredBuffer::UAVBarrier(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.UAVBarrier(cmdList);
}

// == FormattedBuffer ============================================================================

FormattedBuffer::FormattedBuffer()
{
}

FormattedBuffer::~FormattedBuffer()
{
    Assert_(NumElements == 0);
    Shutdown();
}

void FormattedBuffer::Initialize(const FormattedBufferInit& init)
{
    Shutdown();

    Assert_(init.Format != DXGI_FORMAT_UNKNOWN);
    Assert_(init.NumElements > 0);
    Stride = DirectX::BitsPerPixel(init.Format) / 8;
    NumElements = init.NumElements;
    Format = init.Format;

    InternalBuffer.Initialize(Stride * NumElements, Stride, init.Dynamic, init.GPUWritable, init.CreateUAV,
                              init.InitData, init.InitialState, init.Heap, init.HeapOffset, init.Name);
    GPUAddress = InternalBuffer.GPUAddress;

    PersistentDescriptorAlloc srvAlloc = DX12::SRVDescriptorHeap.AllocatePersistent();
    SRV = srvAlloc.Index;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = { };
    srvDesc.Format = Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srvDesc.Buffer.NumElements = uint32(NumElements);

    for(uint32 i = 0; i < DX12::SRVDescriptorHeap.NumHeaps; ++i)
    {
        if(init.Dynamic)
            srvDesc.Buffer.FirstElement = uint32(NumElements * i);
        DX12::Device->CreateShaderResourceView(InternalBuffer.Resource, &srvDesc, srvAlloc.Handles[i]);
    }


    if(init.CreateUAV)
    {
        Assert_(init.Dynamic == false);

        UAV = DX12::UAVDescriptorHeap.AllocatePersistent().Handles[0];

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = { };
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = Format;
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        uavDesc.Buffer.NumElements = uint32(NumElements);

        DX12::Device->CreateUnorderedAccessView(InternalBuffer.Resource, nullptr, &uavDesc, UAV);
    }
}

void FormattedBuffer::Shutdown()
{
    DX12::SRVDescriptorHeap.FreePersistent(SRV);
    DX12::SRVDescriptorHeap.FreePersistent(SRV);
    InternalBuffer.Shutdown();
    Stride = 0;
    NumElements = 0;
}

D3D12_INDEX_BUFFER_VIEW FormattedBuffer::IBView() const
{
    Assert_(Format == DXGI_FORMAT_R16_UINT || Format == DXGI_FORMAT_R32_UINT);
    D3D12_INDEX_BUFFER_VIEW ibView = { };
    ibView.BufferLocation = GPUAddress;
    ibView.Format = Format;
    ibView.SizeInBytes = uint32(InternalBuffer.Size);
    return ibView;
}

void* FormattedBuffer::Map()
{
    MapResult mapResult = InternalBuffer.Map();

    GPUAddress = mapResult.GPUAddress;
    return mapResult.CPUAddress;
}

void FormattedBuffer::MapAndSetData(const void* data, uint64 numElements)
{
    Assert_(numElements <= NumElements);
    void* cpuAddr = Map();
    memcpy(cpuAddr, data, numElements * Stride);
}

void FormattedBuffer::UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset, bool allowOverwrite)
{
    GPUAddress = InternalBuffer.UpdateData(srcData, srcNumElements * Stride, dstElemOffset * Stride, allowOverwrite);
}

void FormattedBuffer::Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const
{
    InternalBuffer.Transition(cmdList, before, after);
}

void FormattedBuffer::MakeReadable(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.MakeReadable(cmdList);
}

void FormattedBuffer::MakeWritable(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.MakeWritable(cmdList);
}

void FormattedBuffer::UAVBarrier(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.UAVBarrier(cmdList);
}

// == RawBuffer ============================================================================

RawBuffer::RawBuffer()
{
}

RawBuffer::~RawBuffer()
{
    Assert_(NumElements == 0);
    Shutdown();
}

void RawBuffer::Initialize(const RawBufferInit& init)
{
    Shutdown();

    Assert_(init.NumElements > 0);
    NumElements = init.NumElements;

    InternalBuffer.Initialize(Stride * NumElements, Stride, init.Dynamic, init.GPUWritable, init.CreateUAV, init.InitData,
                              init.InitialState, init.Heap, init.HeapOffset, init.Name);
    GPUAddress = InternalBuffer.GPUAddress;

    PersistentDescriptorAlloc srvAlloc = DX12::SRVDescriptorHeap.AllocatePersistent();
    SRV = srvAlloc.Index;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = { };
    srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    srvDesc.Buffer.NumElements = uint32(NumElements);
    for(uint32 i = 0; i < DX12::SRVDescriptorHeap.NumHeaps; ++i)
    {
        if(init.Dynamic)
            srvDesc.Buffer.FirstElement = uint32(NumElements * i);
        DX12::Device->CreateShaderResourceView(InternalBuffer.Resource, &srvDesc, srvAlloc.Handles[i]);
    }

    if(init.CreateUAV)
    {
        Assert_(init.Dynamic == false);

        UAV = DX12::UAVDescriptorHeap.AllocatePersistent().Handles[0];

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = { };
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        uavDesc.Buffer.NumElements = uint32(NumElements);
        DX12::Device->CreateUnorderedAccessView(InternalBuffer.Resource, nullptr, &uavDesc, UAV);
    }
}

void RawBuffer::Shutdown()
{
    DX12::SRVDescriptorHeap.FreePersistent(SRV);
    DX12::UAVDescriptorHeap.FreePersistent(UAV);
    InternalBuffer.Shutdown();
    NumElements = 0;
}

void* RawBuffer::Map()
{
    MapResult mapResult = InternalBuffer.Map();
    GPUAddress = mapResult.GPUAddress;
    return mapResult.CPUAddress;
}

void RawBuffer::MapAndSetData(const void* data, uint64 numElements)
{
    Assert_(numElements <= NumElements);
    void* cpuAddr = Map();
    memcpy(cpuAddr, data, numElements * Stride);
}

void RawBuffer::UpdateData(const void* srcData, uint64 srcNumElements, uint64 dstElemOffset, bool allowOverwrite)
{
    GPUAddress = InternalBuffer.UpdateData(srcData, srcNumElements * Stride, dstElemOffset * Stride, allowOverwrite);
}

void RawBuffer::Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const
{
    InternalBuffer.Transition(cmdList, before, after);
}

void RawBuffer::MakeReadable(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.MakeReadable(cmdList);
}

void RawBuffer::MakeWritable(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.MakeWritable(cmdList);
}

void RawBuffer::UAVBarrier(ID3D12GraphicsCommandList* cmdList) const
{
    InternalBuffer.UAVBarrier(cmdList);
}

// == ReadbackBuffer ==============================================================================

ReadbackBuffer::ReadbackBuffer()
{
}

ReadbackBuffer::~ReadbackBuffer()
{
    Assert_(Resource == nullptr);
}

void ReadbackBuffer::Initialize(uint64 size)
{
    Assert_(size > 0);
    Size = size;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = uint32(size);
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Alignment = 0;

    DXCall(DX12::Device->CreateCommittedResource(DX12::GetReadbackHeapProps(), D3D12_HEAP_FLAG_NONE, &resourceDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&Resource)));
}

void ReadbackBuffer::Shutdown()
{
    DX12::DeferredRelease(Resource);
    Size = 0;
}

void* ReadbackBuffer::Map()
{
    Assert_(Resource != nullptr);
    void* data = nullptr;
    Resource->Map(0, nullptr, &data);
    return data;
}

void ReadbackBuffer::Unmap()
{
    Assert_(Resource != nullptr);
    Resource->Unmap(0, nullptr);
}

// == Fence =======================================================================================

Fence::~Fence()
{
    Assert_(D3DFence == nullptr);
    Shutdown();
}

void Fence::Init(uint64 initialValue)
{
    DXCall(DX12::Device->CreateFence(initialValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&D3DFence)));
    FenceEvent = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
    Win32Call(FenceEvent != 0);
}

void Fence::Shutdown()
{
    DX12::DeferredRelease(D3DFence);
}

void Fence::Signal(ID3D12CommandQueue* queue, uint64 fenceValue)
{
    Assert_(D3DFence != nullptr);
    DXCall(queue->Signal(D3DFence, fenceValue));
}

void Fence::Wait(uint64 fenceValue)
{
    Assert_(D3DFence != nullptr);
    if(D3DFence->GetCompletedValue() < fenceValue)
    {
        DXCall(D3DFence->SetEventOnCompletion(fenceValue, FenceEvent));
        WaitForSingleObject(FenceEvent, INFINITE);
    }
}


bool Fence::Signaled(uint64 fenceValue)
{
    Assert_(D3DFence != nullptr);
    return D3DFence->GetCompletedValue() >= fenceValue;
}

void Fence::Clear(uint64 fenceValue)
{
    Assert_(D3DFence != nullptr);
    D3DFence->Signal(fenceValue);
}

// == Texture ====================================================================================

Texture::Texture()
{
}

Texture::~Texture()
{
    Assert_(Resource == nullptr);
}

void Texture::Shutdown()
{
    DX12::SRVDescriptorHeap.FreePersistent(SRV);
    DX12::DeferredRelease(Resource);
}

// == RenderTexture ===============================================================================

RenderTexture::RenderTexture()
{
}

RenderTexture::~RenderTexture()
{
    Assert_(RTV.ptr == 0);
}

void RenderTexture::Initialize(uint64 width, uint64 height, DXGI_FORMAT format, uint64 msaaSamples, uint64 arraySize, bool32 createUAV, D3D12_RESOURCE_STATES initialState)
{
    Shutdown();

    Assert_(width > 0);
    Assert_(height > 0);
    Assert_(msaaSamples > 0);

    D3D12_RESOURCE_DESC textureDesc = { };
    textureDesc.MipLevels = 1;
    textureDesc.Format = format;
    textureDesc.Width = uint32(width);
    textureDesc.Height = uint32(height);
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(createUAV)
        textureDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(msaaSamples == 1)
        textureDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    textureDesc.DepthOrArraySize = uint16(arraySize);
    textureDesc.SampleDesc.Count = uint32(msaaSamples);
    textureDesc.SampleDesc.Quality = msaaSamples > 1 ? DX12::StandardMSAAPattern : 0;
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Alignment = 0;

    D3D12_CLEAR_VALUE clearValue = { };
    clearValue.Format = format;
    DXCall(DX12::Device->CreateCommittedResource(DX12::GetDefaultHeapProps(), D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                 initialState, &clearValue, IID_PPV_ARGS(&Texture.Resource)));

    PersistentDescriptorAlloc srvAlloc = DX12::SRVDescriptorHeap.AllocatePersistent();
    Texture.SRV = srvAlloc.Index;
    for(uint32 i = 0; i < DX12::SRVDescriptorHeap.NumHeaps; ++i)
        DX12::Device->CreateShaderResourceView(Texture.Resource, nullptr, srvAlloc.Handles[i]);

    Texture.Width = uint32(width);
    Texture.Height = uint32(height);
    Texture.Depth = 1;
    Texture.NumMips = 1;
    Texture.ArraySize = uint32(arraySize);
    Texture.Format = format;
    Texture.Cubemap = false;
    MSAASamples = uint32(msaaSamples);
    MSAAQuality = uint32(textureDesc.SampleDesc.Quality);

    RTV = DX12::RTVDescriptorHeap.AllocatePersistent().Handles[0];
    DX12::Device->CreateRenderTargetView(Texture.Resource, nullptr, RTV);

    if(arraySize > 1)
    {
        ArrayRTVs.Init(arraySize);

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = { };
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Format = format;
        if(msaaSamples > 1)
            rtvDesc.Texture2DMSArray.ArraySize = 1;
        else
            rtvDesc.Texture2DArray.ArraySize = 1;

        for(uint64 i = 0; i < arraySize; ++i)
        {
            if(msaaSamples > 1)
                rtvDesc.Texture2DMSArray.FirstArraySlice = uint32(i);
            else
                rtvDesc.Texture2DArray.FirstArraySlice = uint32(i);

            ArrayRTVs[i] = DX12::RTVDescriptorHeap.AllocatePersistent().Handles[0];
            DX12::Device->CreateRenderTargetView(Texture.Resource, &rtvDesc, ArrayRTVs[i]);
        }
    }

    if(createUAV)
    {
        UAV = DX12::UAVDescriptorHeap.AllocatePersistent().Handles[0];
        DX12::Device->CreateUnorderedAccessView(Texture.Resource, nullptr, nullptr, UAV);
    }
}

void RenderTexture::Shutdown()
{
    DX12::RTVDescriptorHeap.FreePersistent(RTV);
    DX12::UAVDescriptorHeap.FreePersistent(UAV);
    for(uint64 i = 0; i < ArrayRTVs.Size(); ++i)
        DX12::RTVDescriptorHeap.FreePersistent(ArrayRTVs[i]);
    ArrayRTVs.Shutdown();
    Texture.Shutdown();
}

void RenderTexture::Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint64 mipLevel, uint64 arraySlice) const
{
    uint32 subResourceIdx = mipLevel == uint64(-1) || arraySlice == uint64(-1) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
                                                                               : uint32(SubResourceIndex(mipLevel, arraySlice));
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, before, after, subResourceIdx);
}

void RenderTexture::MakeReadable(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel, uint64 arraySlice) const
{
    uint32 subResourceIdx = mipLevel == uint64(-1) || arraySlice == uint64(-1) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
                                                                               : uint32(SubResourceIndex(mipLevel, arraySlice));
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subResourceIdx);
}

void RenderTexture::MakeWritable(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel, uint64 arraySlice) const
{
    uint32 subResourceIdx = mipLevel == uint64(-1) || arraySlice == uint64(-1) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
                                                                               : uint32(SubResourceIndex(mipLevel, arraySlice));
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, subResourceIdx);
}

// == VolumeTexture ===============================================================================

VolumeTexture::VolumeTexture()
{
}

VolumeTexture::~VolumeTexture()
{
    Assert_(UAV.ptr == 0);
}

void VolumeTexture::Initialize(uint64 width, uint64 height, uint64 depth, DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState)
{
    Shutdown();

    Assert_(width > 0);
    Assert_(height > 0);
    Assert_(depth > 0);

    D3D12_RESOURCE_DESC textureDesc = { };
    textureDesc.MipLevels = 1;
    textureDesc.Format = format;
    textureDesc.Width = uint32(width);
    textureDesc.Height = uint32(height);
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    textureDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    textureDesc.DepthOrArraySize = uint16(depth);
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Alignment = 0;

    DXCall(DX12::Device->CreateCommittedResource(DX12::GetDefaultHeapProps(), D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                 initialState, nullptr, IID_PPV_ARGS(&Texture.Resource)));

    PersistentDescriptorAlloc srvAlloc = DX12::SRVDescriptorHeap.AllocatePersistent();
    Texture.SRV = srvAlloc.Index;
    for(uint32 i = 0; i < DX12::SRVDescriptorHeap.NumHeaps; ++i)
        DX12::Device->CreateShaderResourceView(Texture.Resource, nullptr, srvAlloc.Handles[i]);

    Texture.Width = uint32(width);
    Texture.Height = uint32(height);
    Texture.Depth = uint32(depth);
    Texture.NumMips = 1;
    Texture.ArraySize = 1;
    Texture.Format = format;
    Texture.Cubemap = false;

    UAV = DX12::UAVDescriptorHeap.AllocatePersistent().Handles[0];
    DX12::Device->CreateUnorderedAccessView(Texture.Resource, nullptr, nullptr, UAV);
}

void VolumeTexture::Shutdown()
{
    DX12::UAVDescriptorHeap.FreePersistent(UAV);
    Texture.Shutdown();
}

void VolumeTexture::Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const
{
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, before, after, 0);
}

void VolumeTexture::MakeReadable(ID3D12GraphicsCommandList* cmdList) const
{
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ, 0);
}

void VolumeTexture::MakeWritable(ID3D12GraphicsCommandList* cmdList) const
{
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0);
}

// == DepthBuffer ===============================================================================

DepthBuffer::DepthBuffer()
{
}

DepthBuffer::~DepthBuffer()
{
    Assert_(DSVFormat == DXGI_FORMAT_UNKNOWN);
    Shutdown();
}

void DepthBuffer::Initialize(uint64 width, uint64 height, DXGI_FORMAT format, uint64 msaaSamples, uint64 arraySize, D3D12_RESOURCE_STATES initialState)
{
    Shutdown();

    Assert_(width > 0);
    Assert_(height > 0);
    Assert_(msaaSamples > 0);

    DXGI_FORMAT texFormat = format;
    DXGI_FORMAT srvFormat = format;
    if(format == DXGI_FORMAT_D16_UNORM)
    {
        texFormat = DXGI_FORMAT_R16_TYPELESS;
        srvFormat = DXGI_FORMAT_R16_UNORM;
    }
    else if(format == DXGI_FORMAT_D24_UNORM_S8_UINT)
    {
        texFormat = DXGI_FORMAT_R24G8_TYPELESS;
        srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    }
    else if(format == DXGI_FORMAT_D32_FLOAT)
    {
        texFormat = DXGI_FORMAT_R32_TYPELESS;
        srvFormat = DXGI_FORMAT_R32_FLOAT;
    }
    else if(format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
    {
        texFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
        srvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    }
    else
    {
        AssertFail_("Invalid depth buffer format!");
    }

    D3D12_RESOURCE_DESC textureDesc = { };
    textureDesc.MipLevels = 1;
    textureDesc.Format = texFormat;
    textureDesc.Width = uint32(width);
    textureDesc.Height = uint32(height);
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    textureDesc.DepthOrArraySize = uint16(arraySize);
    textureDesc.SampleDesc.Count = uint32(msaaSamples);
    textureDesc.SampleDesc.Quality = msaaSamples > 1 ? DX12::StandardMSAAPattern : 0;
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Alignment = 0;

    D3D12_CLEAR_VALUE clearValue = { };
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;
    clearValue.Format = format;

    DXCall(DX12::Device->CreateCommittedResource(DX12::GetDefaultHeapProps(), D3D12_HEAP_FLAG_NONE, &textureDesc,
                                                 initialState, &clearValue, IID_PPV_ARGS(&Texture.Resource)));

    PersistentDescriptorAlloc srvAlloc = DX12::SRVDescriptorHeap.AllocatePersistent();
    Texture.SRV = srvAlloc.Index;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = { };
    srvDesc.Format = srvFormat;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if(msaaSamples == 1 && arraySize == 1)
    {
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.PlaneSlice = 0;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    }
    else if(msaaSamples == 1 && arraySize > 1)
    {
        srvDesc.Texture2DArray.ArraySize = uint32(arraySize);
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.PlaneSlice = 0;
        srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    }
    else if(msaaSamples > 1 && arraySize == 1)
    {
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    }
    else if(msaaSamples > 1 && arraySize > 1)
    {
        srvDesc.Texture2DMSArray.FirstArraySlice = 0;
        srvDesc.Texture2DMSArray.ArraySize = uint32(arraySize);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
    }

    for(uint32 i = 0; i < DX12::SRVDescriptorHeap.NumHeaps; ++i)
        DX12::Device->CreateShaderResourceView(Texture.Resource, &srvDesc, srvAlloc.Handles[i]);

    Texture.Width = uint32(width);
    Texture.Height = uint32(height);
    Texture.Depth = 1;
    Texture.NumMips = 1;
    Texture.ArraySize = uint32(arraySize);
    Texture.Format = srvFormat;
    Texture.Cubemap = false;
    MSAASamples = uint32(msaaSamples);
    MSAAQuality = uint32(textureDesc.SampleDesc.Quality);

    DSV = DX12::DSVDescriptorHeap.AllocatePersistent().Handles[0];

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = { };
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    dsvDesc.Format = format;

    if(msaaSamples == 1 && arraySize == 1)
    {
        dsvDesc.Texture2D.MipSlice = 0;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    }
    else if(msaaSamples == 1 && arraySize > 1)
    {
        dsvDesc.Texture2DArray.ArraySize = uint32(arraySize);
        dsvDesc.Texture2DArray.FirstArraySlice = 0;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    }
    else if(msaaSamples > 1 && arraySize == 1)
    {
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    }
    else if(msaaSamples > 1 && arraySize > 1)
    {
        dsvDesc.Texture2DMSArray.ArraySize = uint32(arraySize);
        dsvDesc.Texture2DMSArray.FirstArraySlice = 0;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
    }

    DX12::Device->CreateDepthStencilView(Texture.Resource, &dsvDesc, DSV);

    bool hasStencil = format == DXGI_FORMAT_D24_UNORM_S8_UINT || format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

    ReadOnlyDSV = DX12::DSVDescriptorHeap.AllocatePersistent().Handles[0];
    dsvDesc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    if(hasStencil)
        dsvDesc.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    DX12::Device->CreateDepthStencilView(Texture.Resource, &dsvDesc, ReadOnlyDSV);

    if(arraySize > 1)
    {
        ArrayDSVs.Init(arraySize);

        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        if(msaaSamples > 1)
            dsvDesc.Texture2DMSArray.ArraySize = 1;
        else
            dsvDesc.Texture2DArray.ArraySize = 1;

        for(uint64 i = 0; i < arraySize; ++i)
        {
            if(msaaSamples > 1)
                dsvDesc.Texture2DMSArray.FirstArraySlice = uint32(i);
            else
                dsvDesc.Texture2DArray.FirstArraySlice = uint32(i);

            ArrayDSVs[i] = DX12::DSVDescriptorHeap.AllocatePersistent().Handles[0];
            DX12::Device->CreateDepthStencilView(Texture.Resource, &dsvDesc, ArrayDSVs[i]);
        }
    }

    DSVFormat = format;
}

void DepthBuffer::Shutdown()
{
    DX12::DSVDescriptorHeap.FreePersistent(DSV);
    DX12::DSVDescriptorHeap.FreePersistent(ReadOnlyDSV);
    for(uint64 i = 0; i < ArrayDSVs.Size(); ++i)
        DX12::DSVDescriptorHeap.FreePersistent(ArrayDSVs[i]);
    ArrayDSVs.Shutdown();
    Texture.Shutdown();
    DSVFormat = DXGI_FORMAT_UNKNOWN;
}

void DepthBuffer::Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint64 arraySlice) const
{
    uint32 subResourceIdx = arraySlice == uint64(-1) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : uint32(arraySlice);
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, before, after, subResourceIdx);
}

void DepthBuffer::MakeReadable(ID3D12GraphicsCommandList* cmdList, uint64 arraySlice) const
{
    uint32 subResourceIdx = arraySlice == uint64(-1) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : uint32(arraySlice);
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subResourceIdx);
}

void DepthBuffer::MakeWritable(ID3D12GraphicsCommandList* cmdList, uint64 arraySlice) const
{
    uint32 subResourceIdx = arraySlice == uint64(-1) ? D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES : uint32(arraySlice);
    Assert_(Texture.Resource != nullptr);
    DX12::TransitionResource(cmdList, Texture.Resource, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                             D3D12_RESOURCE_STATE_DEPTH_WRITE, subResourceIdx);
}

}