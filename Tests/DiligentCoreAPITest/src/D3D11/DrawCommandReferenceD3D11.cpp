/*     Copyright 2019 Diligent Graphics LLC
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF ANY PROPRIETARY RIGHTS.
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

#include "D3D11/TestingEnvironmentD3D11.h"
#include "D3D11/TestingSwapChainD3D11.h"

#include "InlineShaders/DrawCommandTestHLSL.h"

namespace Diligent
{

namespace Testing
{

void RenderDrawCommandReferenceD3D11(ISwapChain* pSwapChain)
{
    auto* pEnvD3D11              = TestingEnvironmentD3D11::GetInstance();
    auto* pd3d11Context          = pEnvD3D11->GetD3D11Context();
    auto* pTestingSwapChainD3D11 = ValidatedCast<TestingSwapChainD3D11>(pSwapChain);

    pd3d11Context->ClearState();

    auto pVS = pEnvD3D11->CreateVertexShader(HLSL::DrawTest_ProceduralTriangleVS);
    ASSERT_NE(pVS, nullptr);

    auto pPS = pEnvD3D11->CreatePixelShader(HLSL::DrawTest_PS);
    ASSERT_NE(pPS, nullptr);

    pd3d11Context->VSSetShader(pVS, nullptr, 0);
    pd3d11Context->PSSetShader(pPS, nullptr, 0);
    pd3d11Context->RSSetState(pEnvD3D11->GetNoCullRS());
    pd3d11Context->OMSetBlendState(pEnvD3D11->GetDefaultBS(), nullptr, 0xFFFFFFFF);
    pd3d11Context->OMSetDepthStencilState(pEnvD3D11->GetDisableDepthDSS(), 0);
    pd3d11Context->IASetInputLayout(nullptr);

    ID3D11RenderTargetView* pd3d11RTVs[] = {pTestingSwapChainD3D11->GetD3D11RTV()};
    pd3d11Context->OMSetRenderTargets(1, pd3d11RTVs, nullptr);
    float ClearColor[] = {0, 0, 0, 0};
    pd3d11Context->ClearRenderTargetView(pd3d11RTVs[0], ClearColor);

    D3D11_VIEWPORT d3dVP = {};

    const auto& SCDesc = pTestingSwapChainD3D11->GetDesc();
    d3dVP.Width        = static_cast<float>(SCDesc.Width);
    d3dVP.Height       = static_cast<float>(SCDesc.Height);
    d3dVP.MaxDepth     = 1;
    pd3d11Context->RSSetViewports(1, &d3dVP);

    pd3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pd3d11Context->Draw(6, 0);

    pd3d11Context->ClearState();
}

} // namespace Testing

} // namespace Diligent
