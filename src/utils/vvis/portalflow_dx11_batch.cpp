//      note2self
//                  Current step:
//                      Cleanup (probably done?)
//                      main PortalFlow function, cleaned up (03.03.2025)
//





#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "vis.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

// experimental PortalFlow compute shader (note2self: cleanup?)
const char* g_csSource = R"(
cbuffer CB : register(b0)
{
    int maskSize;
    int totalPortals;
};

StructuredBuffer<int> neighborOffsets : register(t0);
StructuredBuffer<int> neighborCounts : register(t1);
StructuredBuffer<int> neighbors : register(t2);
RWByteAddressBuffer visMasks : register(u0);

[numthreads(64,1,1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    int i = DTid.x;
    if(i >= totalPortals)
        return;
    int offset = neighborOffsets[i];
    int count = neighborCounts[i];
    int base = i * maskSize;
    for (int j = 0; j < maskSize; j++)
    {
        uint unionVal = visMasks.Load(base + j);
        for (int n = 0; n < count; n++)
        {
            int neighborIndex = neighbors[offset + n];
            int nbase = neighborIndex * maskSize;
            uint neighborVal = visMasks.Load(nbase + j);
            unionVal |= neighborVal;
        }
        visMasks.Store(base + j, unionVal);
    }
}
)";

// DirectX11 globals
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pImmediateContext = nullptr;
ID3D11ComputeShader* g_pComputeShader = nullptr;

// initialization starts here & compiles the shader
bool InitDX11Batch()
{
    HRESULT hr = D3D11CreateDevice(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        0,
        NULL,
        0,
        D3D11_SDK_VERSION,
        &g_pd3dDevice,
        NULL,
        &g_pImmediateContext);

    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create device.\n");
        return false;
    }

    // shader compiling
    ID3DBlob* pCSBlob = nullptr;
    ID3DBlob* pErrorBlob = nullptr;
    hr = D3DCompile(g_csSource, strlen(g_csSource), NULL, NULL, NULL, "main", "cs_5_0", 0, 0, &pCSBlob, &pErrorBlob);
    if (FAILED(hr))
    {
        if (pErrorBlob)
        {
            printf("[DX11 Batch] Shader compilation error: %s\n", (char*)pErrorBlob->GetBufferPointer());
            pErrorBlob->Release();
        }
        return false;
    }

    // create the shader
    hr = g_pd3dDevice->CreateComputeShader(pCSBlob->GetBufferPointer(), pCSBlob->GetBufferSize(), NULL, &g_pComputeShader);
    pCSBlob->Release();
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create compute shader.\n");
        return false;
    }
    return true;
}

// clean up unused resources
void CleanupDX11Batch()
{
    if (g_pComputeShader)
    {
        g_pComputeShader->Release();
        g_pComputeShader = nullptr;
    }
    if (g_pImmediateContext)
    {
        g_pImmediateContext->Release();
        g_pImmediateContext = nullptr;
    }
    if (g_pd3dDevice)
    {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

// main function, processes all portalflows at once instead of in a queue
bool GPU_PortalFlow_Batch()
{
    int totalPortals = g_numportals * 2;
    int maskSize = portalbytes;
    size_t bufferSize = totalPortals * maskSize; // note2self: bytes

    // setup host buffer & transfer to GPU
    std::vector<unsigned char> hostVis(bufferSize);
    for (int i = 0; i < totalPortals; i++)
    {
        memcpy(&hostVis[i * maskSize], portals[i].portalflood, maskSize);
    }

    // create neighbor list/array & fill out 
    std::vector<int> neighborCounts(totalPortals);
    std::vector<int> neighborOffsets(totalPortals);
    int totalNeighbors = 0;
    for (int i = 0; i < totalPortals; i++)
    {
        int leafIndex = portals[i].leaf;
        int count = leafs[leafIndex].portals.Count();
        neighborCounts[i] = count - 1;  // exclude self because obviously it can't be its own neighbor lmao
        neighborOffsets[i] = totalNeighbors;
        totalNeighbors += (count - 1);
    }

    std::vector<int> neighbors(totalNeighbors);
    for (int i = 0; i < totalPortals; i++)
    {
        int leafIndex = portals[i].leaf;
        int offset = neighborOffsets[i];
        int idx = 0;
        for (int j = 0; j < leafs[leafIndex].portals.Count(); j++)
        {
            int neighborIndex = (int)(leafs[leafIndex].portals[j] - portals);
            if (neighborIndex == i)
                continue;
            neighbors[offset + idx] = neighborIndex;
            idx++;
        }
    }

    HRESULT hr;

    // create YET ANOTHER buffer (this time for vis masks)
    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.ByteWidth = static_cast<UINT>(bufferSize);
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = hostVis.data();
    ID3D11Buffer* pVisBuffer = nullptr;
    hr = g_pd3dDevice->CreateBuffer(&bufDesc, &initData, &pVisBuffer);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create vis buffer.\n");
        return false;
    }

    // create UAV for vis buffer (the UAV from call of duty, duh)
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = static_cast<UINT>(bufferSize);
    ID3D11UnorderedAccessView* pVisUAV = nullptr;
    hr = g_pd3dDevice->CreateUnorderedAccessView(pVisBuffer, &uavDesc, &pVisUAV);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create UAV for vis buffer.\n");
        pVisBuffer->Release();
        return false;
    }

    // create buffer(s) for neighbor lists/arrays
    D3D11_BUFFER_DESC bufDescInt = {};
    bufDescInt.Usage = D3D11_USAGE_DEFAULT;
    bufDescInt.ByteWidth = totalPortals * sizeof(int);
    bufDescInt.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    initData.pSysMem = neighborOffsets.data();
    ID3D11Buffer* pNeighborOffsetsBuffer = nullptr;
    hr = g_pd3dDevice->CreateBuffer(&bufDescInt, &initData, &pNeighborOffsetsBuffer);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create neighborOffsets buffer.\n");
        return false;
    }

    initData.pSysMem = neighborCounts.data();
    ID3D11Buffer* pNeighborCountsBuffer = nullptr;
    hr = g_pd3dDevice->CreateBuffer(&bufDescInt, &initData, &pNeighborCountsBuffer);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create neighborCounts buffer.\n");
        return false;
    }

    bufDescInt.ByteWidth = totalNeighbors * sizeof(int);
    initData.pSysMem = neighbors.data();
    ID3D11Buffer* pNeighborsBuffer = nullptr;
    hr = g_pd3dDevice->CreateBuffer(&bufDescInt, &initData, &pNeighborsBuffer);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create neighbors buffer.\n");
        return false;
    }

    // create SRVs for neighbor lists/arrays
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_SINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = totalPortals;
    ID3D11ShaderResourceView* pNeighborOffsetsSRV = nullptr;
    hr = g_pd3dDevice->CreateShaderResourceView(pNeighborOffsetsBuffer, &srvDesc, &pNeighborOffsetsSRV);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create SRV for neighborOffsets.\n");
        return false;
    }

    srvDesc.Buffer.NumElements = totalPortals;
    ID3D11ShaderResourceView* pNeighborCountsSRV = nullptr;
    hr = g_pd3dDevice->CreateShaderResourceView(pNeighborCountsBuffer, &srvDesc, &pNeighborCountsSRV);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create SRV for neighborCounts.\n");
        return false;
    }

    srvDesc.Buffer.NumElements = totalNeighbors;
    ID3D11ShaderResourceView* pNeighborsSRV = nullptr;
    hr = g_pd3dDevice->CreateShaderResourceView(pNeighborsBuffer, &srvDesc, &pNeighborsSRV);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create SRV for neighbors.\n");
        return false;
    }

    // create constant buffer (we love 'em)
    struct CB
    {
        int maskSize;
        int totalPortals;
        int pad[2];
    } cbData;
    cbData.maskSize = maskSize;
    cbData.totalPortals = totalPortals;
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.ByteWidth = sizeof(CB);
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    initData.pSysMem = &cbData;
    ID3D11Buffer* pConstantBuffer = nullptr;
    hr = g_pd3dDevice->CreateBuffer(&cbDesc, &initData, &pConstantBuffer);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create constant buffer.\n");
        return false;
    }

    // prepare compute shader resources
    g_pImmediateContext->CSSetShader(g_pComputeShader, NULL, 0);
    g_pImmediateContext->CSSetConstantBuffers(0, 1, &pConstantBuffer);
    ID3D11ShaderResourceView* srvs[3] = { pNeighborOffsetsSRV, pNeighborCountsSRV, pNeighborsSRV };
    g_pImmediateContext->CSSetShaderResources(0, 3, srvs);
    g_pImmediateContext->CSSetUnorderedAccessViews(0, 1, &pVisUAV, NULL);

    // launch compute shader
    UINT groupCount = (totalPortals + 63) / 64;
    g_pImmediateContext->Dispatch(groupCount, 1, 1);
    g_pImmediateContext->Flush();

    // create buffer to read back data from GPU
    D3D11_BUFFER_DESC stagingDesc = {};
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.ByteWidth = static_cast<UINT>(bufferSize);
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* pStagingBuffer = nullptr;
    hr = g_pd3dDevice->CreateBuffer(&stagingDesc, NULL, &pStagingBuffer);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to create staging buffer.\n");
        return false;
    }

    // copy results to the staging buffer
    g_pImmediateContext->CopyResource(pStagingBuffer, pVisBuffer);

    // create map staging buffer and update each portal's vis mask
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = g_pImmediateContext->Map(pStagingBuffer, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        printf("[DX11 Batch] Failed to map staging buffer.\n");
        return false;
    }
    unsigned char* pData = reinterpret_cast<unsigned char*>(mapped.pData);
    for (int i = 0; i < totalPortals; i++)
    {
        memcpy(portals[i].portalvis, pData + i * maskSize, maskSize);
        portals[i].status = stat_done;
    }
    g_pImmediateContext->Unmap(pStagingBuffer, 0);

    // release unused DX11 resources. (everything should hopefully be done by this point?)
    if (pConstantBuffer)     pConstantBuffer->Release();
    if (pNeighborsSRV)       pNeighborsSRV->Release();
    if (pNeighborCountsSRV)    pNeighborCountsSRV->Release();
    if (pNeighborOffsetsSRV)   pNeighborOffsetsSRV->Release();
    if (pNeighborsBuffer)      pNeighborsBuffer->Release();
    if (pNeighborCountsBuffer) pNeighborCountsBuffer->Release();
    if (pNeighborOffsetsBuffer) pNeighborOffsetsBuffer->Release();
    if (pStagingBuffer)        pStagingBuffer->Release();
    if (pVisUAV)             pVisUAV->Release();
    if (pVisBuffer)          pVisBuffer->Release();

    return true;
}

// Main PortalFlow function
// Executed only ONCE per code run, hopefully stiff enough to resist a crash...?
extern "C" void PortalFlow_DX11_Batch(int iThread, int dummyPortalnum)
{
    static LONG processed = 0;

    // for fucks sake, ensure only one thread processes this because the engine won't accept proper synchronization. (or i'm just shit at coding)
    if (InterlockedCompareExchange(&processed, 1, 0) != 0)
    {
        return;
    }

    LARGE_INTEGER freq, tStart, tEnd;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);
    printf("[DX11 Batch] Starting batch processing for all portals...\n");

    if (!InitDX11Batch())
    {
        printf("[DX11 Batch ERROR] DX11 initialization failed.\n");
        return;
    }

    if (!GPU_PortalFlow_Batch())
    {
        printf("[DX11 Batch ERROR] GPU_PortalFlow_Batch failed.\n");
        CleanupDX11Batch();
        return;
    }

    CleanupDX11Batch();
    QueryPerformanceCounter(&tEnd);
    double totalTime = static_cast<double>(tEnd.QuadPart - tStart.QuadPart) / freq.QuadPart;
    printf("DX11 Batch VVIS Finished PortalFlow for all portals in a total of %.6f seconds\n", totalTime);
    // i swear to god if we crash at this point again I'm gonna torch this entire codebase, we're literally done with all the code what the fuck
}
