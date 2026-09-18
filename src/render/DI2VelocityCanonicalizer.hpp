#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include "PDAFWPlugin.h"

// DI2 UE4.25 writes linearly encoded RG16 velocity. FixUEObjectMotion guesses
// its encoding using a fixed 0.001 UV tolerance: small linear velocities can
// be mistaken for nonlinear ones and almost erased. Normalize the encoding
// before that shader, including the history it reads on the next eye.
class DI2VelocityCanonicalizer {
public:
    bool convert(ID3D12GraphicsCommandList* cmd, pd::D3D12RendererAPI* renderer,
                 const pd::TextureDesc& source, const pd::TextureDesc& target) {
        if (!cmd || !renderer || !source.pTexture || !target.pTexture ||
            source.srvPos < 0 || target.uavPos < 0) return false;
        const auto src = source.pTexture->GetDesc();
        const auto dst = target.pTexture->GetDesc();
        if (src.Format != DXGI_FORMAT_R16G16_UNORM || dst.Format != DXGI_FORMAT_R16G16B16A16_UNORM ||
            src.Width != dst.Width || src.Height != dst.Height) return false;
        if (!initialize(renderer->GetDevice())) return false;

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {target.pTexture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              target.initialState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        if (target.initialState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) cmd->ResourceBarrier(1, &barrier);
        auto heap = renderer->GetViewHeap();
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(m_root.Get());
        cmd->SetPipelineState(m_pipeline.Get());
        cmd->SetComputeRootDescriptorTable(0, renderer->GetGPUDescriptorHandle(source.srvPos));
        cmd->SetComputeRootDescriptorTable(1, renderer->GetGPUDescriptorHandle(target.uavPos));
        cmd->Dispatch(static_cast<UINT>((src.Width + 7) / 8), (src.Height + 7) / 8, 1);
        if (target.initialState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter = target.initialState;
        } else {
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = target.pTexture;
        }
        cmd->ResourceBarrier(1, &barrier);
        return true;
    }

private:
    bool initialize(ID3D12Device* device) {
        if (m_device.Get() == device && m_pipeline) return true;
        if (!device) return false;
        m_pipeline.Reset(); m_root.Reset(); m_device = device;
        static constexpr char shader[] = R"(
Texture2D<float2> Input : register(t0);
RWTexture2D<float4> Output : register(u0);
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height; Input.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 raw = Input.Load(int3(id.xy, 0));
    if (raw.x <= 0.0) { Output[id.xy] = 0.0; return; }
    const float bias = 32767.0 / 65535.0;
    float2 linearNDC = (raw - bias) / 0.2495;
    float2 canonical = bias + sign(linearNDC) * sqrt(abs(linearNDC)) * 0.35284629464149475;
    Output[id.xy] = float4(canonical, 0.0, 0.0);
})";
        Microsoft::WRL::ComPtr<ID3DBlob> code, error, signature;
        if (FAILED(D3DCompile(shader, sizeof(shader) - 1, "DI2VelocityCanonicalizer", nullptr, nullptr,
                             "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) return false;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        for (unsigned i = 0; i != 2; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable = {1, &ranges[i]};
            parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters = 2; desc.pParameters = parameters;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) return false;
        if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_root)))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = m_root.Get();
        pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        return SUCCEEDED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_pipeline)));
    }
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
};
