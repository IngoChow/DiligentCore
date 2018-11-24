/*     Copyright 2015-2018 Egor Yusov
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

#include "pch.h"
#include <sstream>
#include "RenderDeviceVkImpl.h"
#include "DeviceContextVkImpl.h"
#include "SwapChainVk.h"
#include "PipelineStateVkImpl.h"
#include "TextureVkImpl.h"
#include "BufferVkImpl.h"
#include "VulkanTypeConversions.h"
#include "CommandListVkImpl.h"

namespace Diligent
{
    static std::string GetContextObjectName(const char* Object, bool bIsDeferred, Uint32 ContextId)
    {
        std::stringstream ss;
        ss << Object;
        if (bIsDeferred)
            ss << " of deferred context #" << ContextId;
        else
            ss << " of immediate context";
        return ss.str();
    }

    DeviceContextVkImpl::DeviceContextVkImpl( IReferenceCounters*                   pRefCounters, 
                                              RenderDeviceVkImpl*                   pDeviceVkImpl, 
                                              bool                                  bIsDeferred, 
                                              const EngineVkAttribs&                Attribs, 
                                              Uint32                                ContextId,
                                              Uint32                                CommandQueueId,
                                              std::shared_ptr<GenerateMipsVkHelper> GenerateMipsHelper) :
        TDeviceContextBase
        {
            pRefCounters,
            pDeviceVkImpl,
            ContextId,
            CommandQueueId,
            bIsDeferred ? std::numeric_limits<decltype(m_NumCommandsToFlush)>::max() : Attribs.NumCommandsToFlushCmdBuffer,
            bIsDeferred
        },
        m_CmdListAllocator  { GetRawAllocator(), sizeof(CommandListVkImpl), 64 },
        // Command pools must be thread safe because command buffers are returned into pools by release queues
        // potentially running in another thread
        m_CmdPool
        {
            pDeviceVkImpl->GetLogicalDevice().GetSharedPtr(),
            pDeviceVkImpl->GetCommandQueue(CommandQueueId).GetQueueFamilyIndex(),
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
        },
        // Upload heap must always be thread-safe as Finish() may be called from another thread
        m_UploadHeap
        {
            *pDeviceVkImpl,
            GetContextObjectName("Upload heap", bIsDeferred, ContextId),
            Attribs.UploadHeapPageSize
        },
        m_DynamicHeap
        {
            pDeviceVkImpl->GetDynamicMemoryManager(),
            GetContextObjectName("Dynamic heap", bIsDeferred, ContextId),
            Attribs.DynamicHeapPageSize
        },
        m_DynamicDescrSetAllocator
        {
            pDeviceVkImpl->GetDynamicDescriptorPool(),
            GetContextObjectName("Dynamic descriptor set allocator", bIsDeferred, ContextId),
        },
        m_GenerateMipsHelper(std::move(GenerateMipsHelper))
    {
        m_GenerateMipsHelper->CreateSRB(&m_GenerateMipsSRB);

        BufferDesc DummyVBDesc;
        DummyVBDesc.Name          = "Dummy vertex buffer";
        DummyVBDesc.BindFlags     = BIND_VERTEX_BUFFER;
        DummyVBDesc.Usage         = USAGE_DEFAULT;
        DummyVBDesc.uiSizeInBytes = 32;
        m_pDevice->CreateBuffer(DummyVBDesc, BufferData{}, &m_DummyVB);
    }

    DeviceContextVkImpl::~DeviceContextVkImpl()
    {
        if (m_State.NumCommands != 0)
        {
            LOG_ERROR_MESSAGE(m_bIsDeferred ? 
                                "There are outstanding commands in deferred context #", m_ContextId, " being destroyed, which indicates that FinishCommandList() has not been called." :
                                "There are outstanding commands in the immediate context being destroyed, which indicates the context has not been Flush()'ed.",
                              " This is unexpected and may result in synchronization errors");
        }

        if (!m_bIsDeferred)
        {
            Flush();
        }

        // For deferred contexts, m_SubmittedBuffersCmdQueueMask is reset to 0 after every call to FinishFrame().
        // In this case there are no resources to release, so there will be no issues.
        FinishFrame();

        // There must be no stale resources
        DEV_CHECK_ERR(m_UploadHeap.GetStalePagesCount()                  == 0, "All allocated upload heap pages must have been released at this point");
        DEV_CHECK_ERR(m_DynamicHeap.GetAllocatedMasterBlockCount()       == 0, "All allocated dynamic heap master blocks must have been released");
        DEV_CHECK_ERR(m_DynamicDescrSetAllocator.GetAllocatedPoolCount() == 0, "All allocated dynamic descriptor set pools must have been released at this point");

        auto* pDeviceVkImpl = m_pDevice.RawPtr<RenderDeviceVkImpl>();

        auto VkCmdPool = m_CmdPool.Release();
        pDeviceVkImpl->SafeReleaseDeviceObject(std::move(VkCmdPool), ~Uint64{0});

        // The main reason we need to idle the GPU is because we need to make sure that all command buffers are returned to the
        // pool. Upload heap, dynamic heap and dynamic descriptor manager return their resources to global managers and
        // do not really need to wait for GPU to idle.
        pDeviceVkImpl->IdleGPU();
        DEV_CHECK_ERR(m_CmdPool.DvpGetBufferCounter() == 0, "All command buffers must have been returned to the pool");
    }

    IMPLEMENT_QUERY_INTERFACE( DeviceContextVkImpl, IID_DeviceContextVk, TDeviceContextBase )

    void DeviceContextVkImpl::DisposeVkCmdBuffer(Uint32 CmdQueue, VkCommandBuffer vkCmdBuff, Uint64 FenceValue)
    {
        VERIFY_EXPR(vkCmdBuff != VK_NULL_HANDLE);
        class CmdBufferDeleter
        {
        public:
            CmdBufferDeleter(VkCommandBuffer                           _vkCmdBuff, 
                             VulkanUtilities::VulkanCommandBufferPool& _Pool) noexcept :
                vkCmdBuff (_vkCmdBuff),
                Pool      (&_Pool)
            {
                VERIFY_EXPR(vkCmdBuff != VK_NULL_HANDLE);
            }

            CmdBufferDeleter             (const CmdBufferDeleter&)  = delete;
            CmdBufferDeleter& operator = (const CmdBufferDeleter&)  = delete;
            CmdBufferDeleter& operator = (      CmdBufferDeleter&&) = delete;

            CmdBufferDeleter(CmdBufferDeleter&& rhs) noexcept : 
                vkCmdBuff (rhs.vkCmdBuff),
                Pool      (rhs.Pool)
            {
                rhs.vkCmdBuff = VK_NULL_HANDLE;
                rhs.Pool      = nullptr;
            }

            ~CmdBufferDeleter()
            {
                if(Pool != nullptr)
                {
                    Pool->FreeCommandBuffer(std::move(vkCmdBuff));
                }
            }

        private:
            VkCommandBuffer                           vkCmdBuff;
            VulkanUtilities::VulkanCommandBufferPool* Pool;
        };

        auto& ReleaseQueue = m_pDevice.RawPtr<RenderDeviceVkImpl>()->GetReleaseQueue(CmdQueue);
        ReleaseQueue.DiscardResource(CmdBufferDeleter{vkCmdBuff, m_CmdPool}, FenceValue);
    }

    inline void DeviceContextVkImpl::DisposeCurrentCmdBuffer(Uint32 CmdQueue, Uint64 FenceValue)
    {
        VERIFY(m_CommandBuffer.GetState().RenderPass == VK_NULL_HANDLE, "Disposing command buffer with unifinished render pass");
        auto vkCmdBuff = m_CommandBuffer.GetVkCmdBuffer();
        if (vkCmdBuff != VK_NULL_HANDLE)
        {
            DisposeVkCmdBuffer(CmdQueue, vkCmdBuff, FenceValue);
            m_CommandBuffer.Reset();
        }
    }


    void DeviceContextVkImpl::SetPipelineState(IPipelineState *pPipelineState)
    {
        // Never flush deferred context!
        if (!m_bIsDeferred && m_State.NumCommands >= m_NumCommandsToFlush)
        {
            Flush();
        }

        auto* pPipelineStateVk = ValidatedCast<PipelineStateVkImpl>(pPipelineState);
        const auto& PSODesc = pPipelineStateVk->GetDesc();

        bool CommitStates = false;
        bool CommitScissor = false;
        if (!m_pPipelineState)
        {
            // If no pipeline state is bound, we are working with the fresh command
            // list. We have to commit the states set in the context that are not
            // committed by the draw command (render targets, viewports, scissor rects, etc.)
            CommitStates = true;
        }
        else
        {
            const auto& OldPSODesc = m_pPipelineState->GetDesc();
            // Commit all graphics states when switching from compute pipeline 
            // This is necessary because if the command list had been flushed
            // and the first PSO set on the command list was a compute pipeline, 
            // the states would otherwise never be committed (since m_pPipelineState != nullptr)
            CommitStates = OldPSODesc.IsComputePipeline;
            // We also need to update scissor rect if ScissorEnable state was disabled in previous pipeline
            CommitScissor = !OldPSODesc.GraphicsPipeline.RasterizerDesc.ScissorEnable;
        }

        TDeviceContextBase::SetPipelineState( pPipelineStateVk, 0 /*Dummy*/ );
        EnsureVkCmdBuffer();

        if (PSODesc.IsComputePipeline)
        {
            auto vkPipeline = pPipelineStateVk->GetVkPipeline();
            m_CommandBuffer.BindComputePipeline(vkPipeline);
        }
        else
        {
            auto vkPipeline = pPipelineStateVk->GetVkPipeline();
            m_CommandBuffer.BindGraphicsPipeline(vkPipeline);

            if (CommitStates)
            {
                m_CommandBuffer.SetStencilReference(m_StencilRef);
                m_CommandBuffer.SetBlendConstants(m_BlendFactors);
                CommitRenderPassAndFramebuffer();
                CommitViewports();
            }

            if (PSODesc.GraphicsPipeline.RasterizerDesc.ScissorEnable && (CommitStates || CommitScissor))
            {
                CommitScissorRects();
            }
        }

        m_DescrSetBindInfo.Reset();
    }

    void DeviceContextVkImpl::TransitionShaderResources(IPipelineState *pPipelineState, IShaderResourceBinding *pShaderResourceBinding)
    {
        VERIFY_EXPR(pPipelineState != nullptr);

        auto *pPipelineStateVk = ValidatedCast<PipelineStateVkImpl>(pPipelineState);
        pPipelineStateVk->CommitAndTransitionShaderResources(pShaderResourceBinding, this, false, COMMIT_SHADER_RESOURCES_FLAG_TRANSITION_RESOURCES, nullptr);
    }

    void DeviceContextVkImpl::CommitShaderResources(IShaderResourceBinding *pShaderResourceBinding, Uint32 Flags)
    {
        if (!DeviceContextBase::CommitShaderResources(pShaderResourceBinding, Flags, 0 /*Dummy*/))
            return;

        m_pPipelineState->CommitAndTransitionShaderResources(pShaderResourceBinding, this, true, Flags, &m_DescrSetBindInfo);
    }

    void DeviceContextVkImpl::SetStencilRef(Uint32 StencilRef)
    {
        if (TDeviceContextBase::SetStencilRef(StencilRef, 0))
        {
            EnsureVkCmdBuffer();
            m_CommandBuffer.SetStencilReference(m_StencilRef);
        }
    }

    void DeviceContextVkImpl::SetBlendFactors(const float* pBlendFactors)
    {
        if (TDeviceContextBase::SetBlendFactors(pBlendFactors, 0))
        {
            EnsureVkCmdBuffer();
            m_CommandBuffer.SetBlendConstants(m_BlendFactors);
        }
    }

    void DeviceContextVkImpl::TransitionVkVertexBuffers()
    {
        for ( Uint32 Buff = 0; Buff < m_NumVertexStreams; ++Buff )
        {
            auto& CurrStream = m_VertexStreams[Buff];
            auto* pBufferVk = CurrStream.pBuffer.RawPtr();
            if (pBufferVk != nullptr && pBufferVk->IsInKnownState())
            {
                if (!pBufferVk->CheckState(RESOURCE_STATE_VERTEX_BUFFER))
                {
                    TransitionBufferState(*pBufferVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_VERTEX_BUFFER, true);
                }
                VERIFY_EXPR(pBufferVk->CheckAccessFlags(VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT));
            }
        }
    }

    void DeviceContextVkImpl::CommitVkVertexBuffers(bool TransitionBuffers)
    {
#ifdef DEVELOPMENT
        if (m_NumVertexStreams < m_pPipelineState->GetNumBufferSlotsUsed())
            LOG_ERROR("Currently bound pipeline state \"", m_pPipelineState->GetDesc().Name, "\" expects ", m_pPipelineState->GetNumBufferSlotsUsed(), " input buffer slots, but only ", m_NumVertexStreams, " is bound");
#endif
        // Do not initialize array with zeroes for performance reasons
        VkBuffer vkVertexBuffers[MaxBufferSlots];// = {}
        VkDeviceSize Offsets[MaxBufferSlots];
        VERIFY( m_NumVertexStreams <= MaxBufferSlots, "Too many buffers are being set" );
        bool DynamicBufferPresent = false;
        for ( Uint32 slot = 0; slot < m_NumVertexStreams; ++slot )
        {
            auto& CurrStream = m_VertexStreams[slot];
            if (auto* pBufferVk = CurrStream.pBuffer.RawPtr())
            {
                if (pBufferVk->GetDesc().Usage == USAGE_DYNAMIC)
                {
                    DynamicBufferPresent = true;
#ifdef DEVELOPMENT
                    pBufferVk->DvpVerifyDynamicAllocation(this);
#endif
                }

                if (TransitionBuffers)
                {
                    if (pBufferVk->IsInKnownState())
                    {
                        if (!pBufferVk->CheckState(RESOURCE_STATE_VERTEX_BUFFER))
                        {
                            TransitionBufferState(*pBufferVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_VERTEX_BUFFER, true);
                        }
                        VERIFY_EXPR(pBufferVk->CheckAccessFlags(VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT));
                    }
                }
#ifdef DEVELOPMENT
                else
                {
                    if (pBufferVk->IsInKnownState() && !pBufferVk->CheckState(RESOURCE_STATE_VERTEX_BUFFER))
                    {
                        LOG_ERROR_MESSAGE("Buffer '", pBufferVk->GetDesc().Name, "' used as vertex buffer at slot ", slot, " must be in "
                                          "RESOURCE_STATE_VERTEX_BUFFER state. Actual buffer state: ", GetResourceStateString(pBufferVk->GetState()), 
                                          ". Use DRAW_FLAG_TRANSITION_VERTEX_BUFFERS flag or explicitly transition the buffer to the required state.");

                    }
                }
#endif

                // Device context keeps strong references to all vertex buffers.

                vkVertexBuffers[slot] = pBufferVk->GetVkBuffer();
                Offsets[slot] = CurrStream.Offset + pBufferVk->GetDynamicOffset(m_ContextId, this);
            }
            else
            {
                // We can't bind null vertex buffer in Vulkan and have to use a dummy one
                vkVertexBuffers[slot] = m_DummyVB.RawPtr<BufferVkImpl>()->GetVkBuffer();
                Offsets[slot] = 0;
            }
        }

        //GraphCtx.FlushResourceBarriers();
        if (m_NumVertexStreams > 0)
            m_CommandBuffer.BindVertexBuffers( 0, m_NumVertexStreams, vkVertexBuffers, Offsets );

        // GPU offset for a dynamic vertex buffer can change every time a draw command is invoked
        m_State.CommittedVBsUpToDate = !DynamicBufferPresent;
    }

    void DeviceContextVkImpl::DvpLogRenderPass_PSOMismatch()
    {
        std::stringstream ss;
        ss << "Active render pass is incomaptible with PSO '" << m_pPipelineState->GetDesc().Name << "'. "
                "This indicates the mismatch between the number and/or format of bound render targets and/or depth stencil buffer "
                "and the PSO. Vulkand requires exact match.\n"
                "    Bound render targets (" << m_NumBoundRenderTargets << "):";
        Uint32 SampleCount = 0;
        for (Uint32 rt = 0; rt < m_NumBoundRenderTargets; ++rt)
        {
            ss << ' ';
            if (auto* pRTV = m_pBoundRenderTargets[rt].RawPtr())
            {
                VERIFY_EXPR(SampleCount == 0 || SampleCount == pRTV->GetTexture()->GetDesc().SampleCount);
                SampleCount = pRTV->GetTexture()->GetDesc().SampleCount;
                ss << GetTextureFormatAttribs(pRTV->GetDesc().Format).Name;
            }
            else
                ss << "<Not set>";
        }
        ss << "; DSV: ";
        if (m_pBoundDepthStencil)
        {
            VERIFY_EXPR(SampleCount == 0 || SampleCount == m_pBoundDepthStencil->GetTexture()->GetDesc().SampleCount);
            SampleCount = m_pBoundDepthStencil->GetTexture()->GetDesc().SampleCount;
            ss << GetTextureFormatAttribs(m_pBoundDepthStencil->GetDesc().Format).Name;
        }
        else
            ss << "<Not set>";
        ss << "; Sample count: " << SampleCount;

        const auto& GrPipeline = m_pPipelineState->GetDesc().GraphicsPipeline;
        ss << "\n    PSO: render targets (" << Uint32{GrPipeline.NumRenderTargets} << "): ";
        for (Uint32 rt = 0; rt < GrPipeline.NumRenderTargets; ++rt)
            ss << ' ' << GetTextureFormatAttribs(GrPipeline.RTVFormats[rt]).Name;
        ss << "; DSV: " << GetTextureFormatAttribs(GrPipeline.DSVFormat).Name;
        ss << "; Sample count: " << Uint32{GrPipeline.SmplDesc.Count};

        LOG_ERROR_MESSAGE(ss.str());
    }

    void DeviceContextVkImpl::Draw( DrawAttribs& drawAttribs )
    {
#ifdef DEVELOPMENT
        if (!DvpVerifyDrawArguments(drawAttribs))
            return;
#endif

        EnsureVkCmdBuffer();

        if ( drawAttribs.IsIndexed )
        {
#ifdef DEVELOPMENT
            if (m_pIndexBuffer == nullptr)
            {
                LOG_ERROR("Index buffer is not set up for indexed draw command");
                return;
            }
#endif

            BufferVkImpl *pBuffVk = m_pIndexBuffer.RawPtr<BufferVkImpl>();
            if (drawAttribs.Flags & DRAW_FLAG_TRANSITION_INDEX_BUFFER)
            {
                if (pBuffVk->IsInKnownState())
                {
                    if (!pBuffVk->CheckState(RESOURCE_STATE_INDEX_BUFFER))
                    {
                        TransitionBufferState(*pBuffVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_INDEX_BUFFER, true);
                    }
                    VERIFY_EXPR(pBuffVk->CheckAccessFlags(VK_ACCESS_INDEX_READ_BIT));
                }
            }
#ifdef DEVELOPMENT
            else
            {
                if (pBuffVk->IsInKnownState() && !pBuffVk->CheckState(RESOURCE_STATE_INDEX_BUFFER))
                {
                    LOG_ERROR_MESSAGE("Buffer '", pBuffVk->GetDesc().Name, "' used as index buffer must be in RESOURCE_STATE_INDEX_BUFFER "
                                       "state. Actual buffer state: ", GetResourceStateString(pBuffVk->GetState()), 
                                       ". Use DRAW_FLAG_TRANSITION_INDEX_BUFFER flag or explicitly transition the buffer to the required state.");
                }
            }
#endif

            DEV_CHECK_ERR(drawAttribs.IndexType == VT_UINT16 || drawAttribs.IndexType == VT_UINT32, "Unsupported index format. Only R16_UINT and R32_UINT are allowed.");
            VkIndexType vkIndexType = drawAttribs.IndexType == VT_UINT16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            m_CommandBuffer.BindIndexBuffer(pBuffVk->GetVkBuffer(), m_IndexDataStartOffset + pBuffVk->GetDynamicOffset(m_ContextId, this), vkIndexType);
        }

        auto TransitionVertexBuffers = (drawAttribs.Flags & DRAW_FLAG_TRANSITION_VERTEX_BUFFERS) != 0;
        if (m_State.CommittedVBsUpToDate)
        {
            if (TransitionVertexBuffers)
            {
                TransitionVkVertexBuffers();
            }
#ifdef DEVELOPMENT
            else
            {
                for (Uint32 slot = 0; slot < m_NumVertexStreams; ++slot )
                {
                    auto& CurrStream = m_VertexStreams[slot];
                    auto* pBufferVk = CurrStream.pBuffer.RawPtr();
                    if (pBufferVk != nullptr && pBufferVk->IsInKnownState() && !pBufferVk->CheckState(RESOURCE_STATE_VERTEX_BUFFER))
                    {
                        LOG_ERROR_MESSAGE("Buffer '", pBufferVk->GetDesc().Name, "' used as vertex buffer at slot ", slot, " must be in "
                                          "RESOURCE_STATE_VERTEX_BUFFER state. Actual buffer state: ", GetResourceStateString(pBufferVk->GetState()), 
                                          ". Use DRAW_FLAG_TRANSITION_VERTEX_BUFFERS flag or explicitly transition the buffer to the required state.");
                    }
                }
            }
#endif
        }
        else
        {
            CommitVkVertexBuffers(TransitionVertexBuffers);
        }
        if (m_DescrSetBindInfo.DynamicOffsetCount != 0)
            m_pPipelineState->BindDescriptorSetsWithDynamicOffsets(this, m_DescrSetBindInfo);
#if 0
#ifdef _DEBUG
        else
        {
            if ( m_pPipelineState->dbgContainsShaderResources() )
                LOG_ERROR_MESSAGE("Pipeline state \"", m_pPipelineState->GetDesc().Name, "\" contains shader resources, but IDeviceContext::CommitShaderResources() was not called" );
        }
#endif
#endif

        auto* pIndirectDrawAttribsVk = ValidatedCast<BufferVkImpl>(drawAttribs.pIndirectDrawAttribs);
        if (pIndirectDrawAttribsVk != nullptr)
        {
            // Buffer memory barries must be executed outside of render pass
            if(drawAttribs.Flags & DRAW_FLAG_TRANSITION_INDIRECT_ARGS_BUFFER)
            {
                if (pIndirectDrawAttribsVk->IsInKnownState())
                {
                    if (!pIndirectDrawAttribsVk->CheckState(RESOURCE_STATE_INDIRECT_ARGUMENT))
                    {
                        TransitionBufferState(*pIndirectDrawAttribsVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_INDIRECT_ARGUMENT, true);
                    }
                    VERIFY_EXPR(pIndirectDrawAttribsVk->CheckAccessFlags(VK_ACCESS_INDIRECT_COMMAND_READ_BIT));
                }
            }
#ifdef DEVELOPMENT
            else
            {
                if (pIndirectDrawAttribsVk->IsInKnownState() && !pIndirectDrawAttribsVk->CheckState(RESOURCE_STATE_INDIRECT_ARGUMENT))
                {
                    LOG_ERROR_MESSAGE("Buffer '", pIndirectDrawAttribsVk->GetDesc().Name, "' used as indirect draw arguments buffer must be in RESOURCE_STATE_INDIRECT_ARGUMENT "
                                       "state. Actual buffer state: ", GetResourceStateString(pIndirectDrawAttribsVk->GetState()), 
                                       ". Use DRAW_FLAG_TRANSITION_INDIRECT_ARGS_BUFFER flag or explicitly transition the buffer to the required state.");
                }
            }
#endif
        }

#ifdef DEVELOPMENT
        if (m_pPipelineState->GetVkRenderPass() != m_RenderPass)
        {
            DvpLogRenderPass_PSOMismatch();
        }
#endif

        CommitRenderPassAndFramebuffer();

        if (pIndirectDrawAttribsVk != nullptr)
        {
#ifdef DEVELOPMENT
            if (pIndirectDrawAttribsVk->GetDesc().Usage == USAGE_DYNAMIC)
                pIndirectDrawAttribsVk->DvpVerifyDynamicAllocation(this);
#endif

            if ( drawAttribs.IsIndexed )
                m_CommandBuffer.DrawIndexedIndirect(pIndirectDrawAttribsVk->GetVkBuffer(), pIndirectDrawAttribsVk->GetDynamicOffset(m_ContextId, this) + drawAttribs.IndirectDrawArgsOffset, 1, 0);
            else
                m_CommandBuffer.DrawIndirect(pIndirectDrawAttribsVk->GetVkBuffer(), pIndirectDrawAttribsVk->GetDynamicOffset(m_ContextId, this) + drawAttribs.IndirectDrawArgsOffset, 1, 0);
        }
        else
        {
            if ( drawAttribs.IsIndexed )
                m_CommandBuffer.DrawIndexed(drawAttribs.NumIndices, drawAttribs.NumInstances, drawAttribs.FirstIndexLocation, drawAttribs.BaseVertex, drawAttribs.FirstInstanceLocation);
            else
                m_CommandBuffer.Draw(drawAttribs.NumVertices, drawAttribs.NumInstances, drawAttribs.StartVertexLocation, drawAttribs.FirstInstanceLocation );
        }

        ++m_State.NumCommands;
    }

    void DeviceContextVkImpl::DispatchCompute( const DispatchComputeAttribs &DispatchAttrs )
    {
#ifdef DEVELOPMENT
        if (!DvpVerifyDispatchArguments(DispatchAttrs))
            return;
#endif

        EnsureVkCmdBuffer();

        // Dispatch commands must be executed outside of render pass
        if (m_CommandBuffer.GetState().RenderPass != VK_NULL_HANDLE)
            m_CommandBuffer.EndRenderPass();

        if (m_DescrSetBindInfo.DynamicOffsetCount != 0)
            m_pPipelineState->BindDescriptorSetsWithDynamicOffsets(this, m_DescrSetBindInfo);
#if 0
#ifdef _DEBUG
        else
        {
            if ( m_pPipelineState->dbgContainsShaderResources() )
                LOG_ERROR_MESSAGE("Pipeline state \"", m_pPipelineState->GetDesc().Name, "\" contains shader resources, but IDeviceContext::CommitShaderResources() was not called" );
        }
#endif
#endif

        if ( DispatchAttrs.pIndirectDispatchAttribs )
        {
            if ( auto *pBufferVk = ValidatedCast<BufferVkImpl>(DispatchAttrs.pIndirectDispatchAttribs) )
            {
#ifdef DEVELOPMENT
                if (pBufferVk->GetDesc().Usage == USAGE_DYNAMIC)
                    pBufferVk->DvpVerifyDynamicAllocation(this);
#endif

                if(DispatchAttrs.Flags & DISPATCH_FLAG_TRANSITION_INDIRECT_ARGS_BUFFER)
                {
                    // Buffer memory barries must be executed outside of render pass
                    if (pBufferVk->IsInKnownState())
                    {
                        if (!pBufferVk->CheckState(RESOURCE_STATE_INDIRECT_ARGUMENT))
                        {
                            TransitionBufferState(*pBufferVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_INDIRECT_ARGUMENT, true);
                        }
                        VERIFY_EXPR(pBufferVk->CheckAccessFlags(VK_ACCESS_INDIRECT_COMMAND_READ_BIT));
                    }
                }
#ifdef DEVELOPMENT
                else
                {
                    if (pBufferVk->IsInKnownState() && !pBufferVk->CheckState(RESOURCE_STATE_INDIRECT_ARGUMENT))
                    {
                        LOG_ERROR_MESSAGE("Buffer '", pBufferVk->GetDesc().Name, "' used as indirect dispatch arguments buffer must be in RESOURCE_STATE_INDIRECT_ARGUMENT "
                                           "state. Actual buffer state: ", GetResourceStateString(pBufferVk->GetState()), 
                                           ". Use DISPATCH_FLAG_TRANSITION_INDIRECT_ARGS_BUFFER flag or explicitly transition the buffer to the required state.");
                    }
                }
#endif

                m_CommandBuffer.DispatchIndirect(pBufferVk->GetVkBuffer(), pBufferVk->GetDynamicOffset(m_ContextId, this) + DispatchAttrs.DispatchArgsByteOffset);
            }
            else
            {
                LOG_ERROR_MESSAGE("Valid pIndirectDrawAttribs must be provided for indirect dispatch command");
            }
        }
        else
            m_CommandBuffer.Dispatch(DispatchAttrs.ThreadGroupCountX, DispatchAttrs.ThreadGroupCountY, DispatchAttrs.ThreadGroupCountZ);

        ++m_State.NumCommands;
    }

    void DeviceContextVkImpl::ClearDepthStencil( ITextureView* pView, Uint32 ClearFlags, float fDepth, Uint8 Stencil )
    {
        ITextureViewVk* pVkDSV = nullptr;
        if ( pView != nullptr )
        {
            pVkDSV = ValidatedCast<ITextureViewVk>(pView);
#ifdef DEVELOPMENT
            const auto& ViewDesc = pVkDSV->GetDesc();
            if ( ViewDesc.ViewType != TEXTURE_VIEW_DEPTH_STENCIL)
            {
                LOG_ERROR("The type (", GetTexViewTypeLiteralName(ViewDesc.ViewType), ") of texture view '", pView->GetDesc().Name, "' is incorrect for ClearDepthStencil operation. Depth-stencil view (TEXTURE_VIEW_DEPTH_STENCIL) must be provided." );
                return;
            }
#endif
        }
        else
        {
            if (m_pSwapChain)
            {
                pVkDSV = ValidatedCast<ITextureViewVk>(m_pSwapChain->GetDepthBufferDSV());
            }
            else
            {
                LOG_ERROR("Failed to clear default depth stencil buffer: swap chain is not initialized in the device context");
                return;
            }
        }

        EnsureVkCmdBuffer();

        const auto& ViewDesc = pVkDSV->GetDesc();
        VERIFY(ViewDesc.TextureDim != RESOURCE_DIM_TEX_3D, "Depth-stencil view of a 3D texture should've been created as 2D texture array view");

        if (pVkDSV == m_pBoundDepthStencil)
        {
            // Render pass may not be currently committed
            VERIFY_EXPR(m_RenderPass != VK_NULL_HANDLE && m_Framebuffer != VK_NULL_HANDLE);
            CommitRenderPassAndFramebuffer();

            VkClearAttachment ClearAttachment = {};
            ClearAttachment.aspectMask = 0;
            if (ClearFlags & CLEAR_DEPTH_FLAG)   ClearAttachment.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (ClearFlags & CLEAR_STENCIL_FLAG) ClearAttachment.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            // colorAttachment is only meaningful if VK_IMAGE_ASPECT_COLOR_BIT is set in aspectMask
            ClearAttachment.colorAttachment = VK_ATTACHMENT_UNUSED;
            ClearAttachment.clearValue.depthStencil.depth = fDepth;
            ClearAttachment.clearValue.depthStencil.stencil = Stencil;
            VkClearRect ClearRect;
            // m_FramebufferWidth, m_FramebufferHeight are scaled to the proper mip level
            ClearRect.rect = { {0, 0}, {m_FramebufferWidth, m_FramebufferHeight} };
            // The layers [baseArrayLayer, baseArrayLayer + layerCount) count from the base layer of 
            // the attachment image view (17.2), so baseArrayLayer is 0, not ViewDesc.FirstArraySlice
            ClearRect.baseArrayLayer = 0;
            ClearRect.layerCount     = ViewDesc.NumArraySlices;
            // No memory barriers are needed between vkCmdClearAttachments and preceding or 
            // subsequent draw or attachment clear commands in the same subpass (17.2)
            m_CommandBuffer.ClearAttachment(ClearAttachment, ClearRect);
        }
        else
        {
            // End render pass to clear the buffer with vkCmdClearDepthStencilImage
            if (m_CommandBuffer.GetState().RenderPass != VK_NULL_HANDLE)
                m_CommandBuffer.EndRenderPass();

            auto* pTexture = pVkDSV->GetTexture();
            auto* pTextureVk = ValidatedCast<TextureVkImpl>(pTexture);

            // Image layout must be VK_IMAGE_LAYOUT_GENERAL or VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL (17.1)
            if (pTextureVk->IsInKnownState())
            {
                if (!pTextureVk->CheckState(RESOURCE_STATE_COPY_DEST))
                {
                    TransitionTextureState(*pTextureVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COPY_DEST, true);
                }
                VERIFY_EXPR(pTextureVk->GetLayout() == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            }
            
            VkClearDepthStencilValue ClearValue;
            ClearValue.depth = fDepth;
            ClearValue.stencil = Stencil;
            VkImageSubresourceRange Subresource;
            Subresource.aspectMask = 0;
            if (ClearFlags & CLEAR_DEPTH_FLAG)   Subresource.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (ClearFlags & CLEAR_STENCIL_FLAG) Subresource.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            // We are clearing the image, not image view with vkCmdClearDepthStencilImage
            Subresource.baseArrayLayer = ViewDesc.FirstArraySlice;
            Subresource.layerCount     = ViewDesc.NumArraySlices;
            Subresource.baseMipLevel   = ViewDesc.MostDetailedMip;
            Subresource.levelCount     = ViewDesc.NumMipLevels;

            m_CommandBuffer.ClearDepthStencilImage(pTextureVk->GetVkImage(), ClearValue, Subresource);
        }

        ++m_State.NumCommands;
    }

    VkClearColorValue ClearValueToVkClearValue(const float *RGBA, TEXTURE_FORMAT TexFmt)
    {
        VkClearColorValue ClearValue;
        const auto& FmtAttribs = GetTextureFormatAttribs(TexFmt);
        if (FmtAttribs.ComponentType == COMPONENT_TYPE_SINT)
        {
            for (int i=0; i < 4; ++i)
                ClearValue.int32[i] = static_cast<int32_t>(RGBA[i]);
        }
        else if (FmtAttribs.ComponentType == COMPONENT_TYPE_UINT)
        {
            for (int i = 0; i < 4; ++i)
                ClearValue.uint32[i] = static_cast<uint32_t>(RGBA[i]);
        }
        else
        {
            for (int i = 0; i < 4; ++i)
                ClearValue.float32[i] = RGBA[i];
        }

        return ClearValue;
    }

    void DeviceContextVkImpl::ClearRenderTarget( ITextureView *pView, const float *RGBA )
    {
        ITextureViewVk* pVkRTV = nullptr;
        if ( pView != nullptr )
        {
#ifdef DEVELOPMENT
            const auto& ViewDesc = pView->GetDesc();
            if ( ViewDesc.ViewType != TEXTURE_VIEW_RENDER_TARGET)
            {
                LOG_ERROR("The type (", GetTexViewTypeLiteralName(ViewDesc.ViewType), ") of texture view '", pView->GetDesc().Name, "' is incorrect for ClearRenderTarget operation. Render target view (TEXTURE_VIEW_RENDER_TARGET) must be provided." );
                return;
            }
#endif
            pVkRTV = ValidatedCast<ITextureViewVk>(pView);
        }
        else
        {
            if (m_pSwapChain)
            {
                pVkRTV = ValidatedCast<ITextureViewVk>(m_pSwapChain->GetCurrentBackBufferRTV());
            }
            else
            {
                LOG_ERROR("Failed to clear default render target: swap chain is not initialized in the device context");
                return;
            }
        }

        static constexpr float Zero[4] = { 0.f, 0.f, 0.f, 0.f };
        if ( RGBA == nullptr )
            RGBA = Zero;
        
        EnsureVkCmdBuffer();

        const auto& ViewDesc = pVkRTV->GetDesc();
        VERIFY(ViewDesc.TextureDim != RESOURCE_DIM_TEX_3D, "Render target view of a 3D texture should've been created as 2D texture array view");

        // Check if the texture is one of the currently bound render targets
        static constexpr const Uint32 InvalidAttachmentIndex = static_cast<Uint32>(-1);
        Uint32 attachmentIndex = InvalidAttachmentIndex;
        for (Uint32 rt = 0; rt < m_NumBoundRenderTargets; ++rt)
        {
            if (m_pBoundRenderTargets[rt] == pVkRTV)
            {
                attachmentIndex = rt;
                break;
            }
        }
        
        if (attachmentIndex != InvalidAttachmentIndex)
        {
            // Render pass may not be currently committed
            VERIFY_EXPR(m_RenderPass != VK_NULL_HANDLE && m_Framebuffer != VK_NULL_HANDLE);
            CommitRenderPassAndFramebuffer();

            VkClearAttachment ClearAttachment = {};
            ClearAttachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            // colorAttachment is only meaningful if VK_IMAGE_ASPECT_COLOR_BIT is set in aspectMask, 
            // in which case it is an index to the pColorAttachments array in the VkSubpassDescription 
            // structure of the current subpass which selects the color attachment to clear (17.2)
            // It is NOT the render pass attachment index
            ClearAttachment.colorAttachment = attachmentIndex;
            ClearAttachment.clearValue.color = ClearValueToVkClearValue(RGBA, ViewDesc.Format);
            VkClearRect ClearRect;
            // m_FramebufferWidth, m_FramebufferHeight are scaled to the proper mip level
            ClearRect.rect = { {0, 0}, {m_FramebufferWidth, m_FramebufferHeight} };
            // The layers [baseArrayLayer, baseArrayLayer + layerCount) count from the base layer of 
            // the attachment image view (17.2), so baseArrayLayer is 0, not ViewDesc.FirstArraySlice
            ClearRect.baseArrayLayer = 0;
            ClearRect.layerCount     = ViewDesc.NumArraySlices; 
            // No memory barriers are needed between vkCmdClearAttachments and preceding or 
            // subsequent draw or attachment clear commands in the same subpass (17.2)
            m_CommandBuffer.ClearAttachment(ClearAttachment, ClearRect);
        }
        else
        {
            // End current render pass and clear the image with vkCmdClearColorImage
            if (m_CommandBuffer.GetState().RenderPass != VK_NULL_HANDLE)
                m_CommandBuffer.EndRenderPass();

            auto* pTexture = pVkRTV->GetTexture();
            auto* pTextureVk = ValidatedCast<TextureVkImpl>(pTexture);

            // Image layout must be VK_IMAGE_LAYOUT_GENERAL or VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL (17.1)
            if (pTextureVk->IsInKnownState())
            {
                if (!pTextureVk->CheckState(RESOURCE_STATE_COPY_DEST))
                {
                    TransitionTextureState(*pTextureVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COPY_DEST, true);
                }
                VERIFY_EXPR(pTextureVk->GetLayout() == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            }
            auto ClearValue = ClearValueToVkClearValue(RGBA, ViewDesc.Format);
            VkImageSubresourceRange Subresource;
            Subresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            // We are clearing the image, not image view with vkCmdClearColorImage
            Subresource.baseArrayLayer = ViewDesc.FirstArraySlice;
            Subresource.layerCount     = ViewDesc.NumArraySlices;
            Subresource.baseMipLevel   = ViewDesc.MostDetailedMip;
            Subresource.levelCount     = ViewDesc.NumMipLevels;
            VERIFY(ViewDesc.NumMipLevels, "RTV must contain single mip level");

            m_CommandBuffer.ClearColorImage(pTextureVk->GetVkImage(), ClearValue, Subresource);
        }

        ++m_State.NumCommands;
    }

    void DeviceContextVkImpl::FinishFrame()
    {
        if(GetNumCommandsInCtx() != 0)
        {
            LOG_ERROR_MESSAGE(m_bIsDeferred ? 
                "There are outstanding commands in deferred device context #", m_ContextId, " when finishing the frame. This is an error and may cause unpredicted behaviour. Close all deferred contexts and execute them before finishing the frame" :
                "There are outstanding commands in the immediate device context when finishing the frame. This is an error and may cause unpredicted behaviour. Call Flush() to submit all commands for execution before finishing the frame");
        }

        if (!m_MappedTextures.empty())
            LOG_ERROR_MESSAGE("There are mapped textures in the device context when finishing the frame. All dynamic resources must be used in the same frame in which they are mapped.");

        auto& DeviceVkImpl = *m_pDevice.RawPtr<RenderDeviceVkImpl>();

        VERIFY_EXPR(m_bIsDeferred || m_SubmittedBuffersCmdQueueMask == (Uint64{1}<<m_CommandQueueId));

        // Release resources used by the context during this frame.
        
        // Upload heap returns all allocated pages to the global memory manager.
        // Note: as global memory manager is hosted by the render device, the upload heap can be destroyed
        // before the pages are actually returned to the manager.
        m_UploadHeap.ReleaseAllocatedPages(m_SubmittedBuffersCmdQueueMask);
        
        // Dynamic heap returns all allocated master blocks to the global dynamic memory manager.
        // Note: as global dynamic memory manager is hosted by the render device, the dynamic heap can
        // be destroyed before the blocks are actually returned to the global dynamic memory manager.
        m_DynamicHeap.ReleaseMasterBlocks(DeviceVkImpl, m_SubmittedBuffersCmdQueueMask);

        // Dynamic descriptor set allocator returns all allocated pools to the global dynamic descriptor pool manager.
        // Note: as global pool manager is hosted by the render device, the allocator can
        // be destroyed before the pools are actually returned to the global pool manager.
        m_DynamicDescrSetAllocator.ReleasePools(m_SubmittedBuffersCmdQueueMask);

        EndFrame(DeviceVkImpl);
    }

    void DeviceContextVkImpl::Flush()
    {
        if (m_bIsDeferred)
        {
            LOG_ERROR_MESSAGE("Flush() should only be called for immediate contexts");
            return;
        }

        VkSubmitInfo SubmitInfo = {};
        SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        SubmitInfo.pNext = nullptr;

        auto pDeviceVkImpl = m_pDevice.RawPtr<RenderDeviceVkImpl>();
        auto vkCmdBuff = m_CommandBuffer.GetVkCmdBuffer();
        if (vkCmdBuff != VK_NULL_HANDLE )
        {
            if (m_State.NumCommands != 0)
            {
                if (m_CommandBuffer.GetState().RenderPass != VK_NULL_HANDLE)
                {
                    m_CommandBuffer.EndRenderPass();
                }

                m_CommandBuffer.FlushBarriers();
                m_CommandBuffer.EndCommandBuffer();
                
                SubmitInfo.commandBufferCount = 1;
                SubmitInfo.pCommandBuffers = &vkCmdBuff;
            }
        }

        SubmitInfo.waitSemaphoreCount = static_cast<uint32_t>(m_WaitSemaphores.size());
        VERIFY_EXPR(m_WaitSemaphores.size() == m_WaitDstStageMasks.size());
        SubmitInfo.pWaitSemaphores      = SubmitInfo.waitSemaphoreCount != 0 ? m_WaitSemaphores.data() : nullptr;
        SubmitInfo.pWaitDstStageMask    = SubmitInfo.waitSemaphoreCount != 0 ? m_WaitDstStageMasks.data() : nullptr;
        SubmitInfo.signalSemaphoreCount = static_cast<uint32_t>(m_SignalSemaphores.size());
        SubmitInfo.pSignalSemaphores    = SubmitInfo.signalSemaphoreCount != 0 ? m_SignalSemaphores.data() : nullptr;

        // Submit command buffer even if there are no commands to release stale resources.
        //if (SubmitInfo.commandBufferCount != 0 || SubmitInfo.waitSemaphoreCount !=0 || SubmitInfo.signalSemaphoreCount != 0)
        auto SubmittedFenceValue = pDeviceVkImpl->ExecuteCommandBuffer(m_CommandQueueId, SubmitInfo, this, &m_PendingFences);
        
        m_WaitSemaphores.clear();
        m_WaitDstStageMasks.clear();
        m_SignalSemaphores.clear();
        m_PendingFences.clear();

        if (vkCmdBuff != VK_NULL_HANDLE)
        {
            DisposeCurrentCmdBuffer(m_CommandQueueId, SubmittedFenceValue);
        }

        m_State = ContextState{};
        m_DescrSetBindInfo.Reset();
        m_CommandBuffer.Reset();
        m_pPipelineState = nullptr;
    }

    void DeviceContextVkImpl::SetVertexBuffers( Uint32 StartSlot, Uint32 NumBuffersSet, IBuffer **ppBuffers, Uint32 *pOffsets, Uint32 Flags )
    {
        TDeviceContextBase::SetVertexBuffers( StartSlot, NumBuffersSet, ppBuffers, pOffsets, Flags );
        m_State.CommittedVBsUpToDate = false;
    }

    void DeviceContextVkImpl::InvalidateState()
    {
        if (m_State.NumCommands != 0)
            LOG_WARNING_MESSAGE("Invalidating context that has outstanding commands in it. Call Flush() to submit commands for execution");

        TDeviceContextBase::InvalidateState();
        m_State = ContextState{};
        m_RenderPass = VK_NULL_HANDLE;
        m_Framebuffer = VK_NULL_HANDLE;
        m_DescrSetBindInfo.Reset();
        VERIFY(m_CommandBuffer.GetState().RenderPass == VK_NULL_HANDLE, "Invalidating context with unifinished render pass");
        m_CommandBuffer.Reset();
    }

    void DeviceContextVkImpl::SetIndexBuffer( IBuffer *pIndexBuffer, Uint32 ByteOffset )
    {
        TDeviceContextBase::SetIndexBuffer( pIndexBuffer, ByteOffset );
        m_State.CommittedIBUpToDate = false;
    }


    void DeviceContextVkImpl::CommitViewports()
    {
        VkViewport VkViewports[MaxViewports]; // Do not waste time initializing array to zero
        for ( Uint32 vp = 0; vp < m_NumViewports; ++vp )
        {
            VkViewports[vp].x        = m_Viewports[vp].TopLeftX;
            VkViewports[vp].y        = m_Viewports[vp].TopLeftY;
            VkViewports[vp].width    = m_Viewports[vp].Width;
            VkViewports[vp].height   = m_Viewports[vp].Height;
            VkViewports[vp].minDepth = m_Viewports[vp].MinDepth;
            VkViewports[vp].maxDepth = m_Viewports[vp].MaxDepth;

            // Turn the viewport upside down to be consistent with Direct3D. Note that in both APIs,
            // the viewport covers the same texture rows. The difference is that Direct3D invertes 
            // normalized device Y coordinate when transforming NDC to window coordinates. In Vulkan
            // we achieve the same effect by using negative viewport height. Therefore we need to
            // invert normalized device Y coordinate when transforming to texture V
            // 
            //             
            //       Image                Direct3D                                       Image               Vulkan
            //        row                                                                 row   
            //         0 _   (0,0)_______________________(1,0)                  Tex Height _   (0,1)_______________________(1,1)              
            //         1 _       |                       |      |             VP Top + Hght _ _ _ _|   __________          |      A           
            //         2 _       |                       |      |                          .       |  |   .--> +x|         |      |           
            //           .       |                       |      |                          .       |  |   |      |         |      |           
            //           .       |                       |      | V Coord                          |  |   V +y   |         |      | V Coord   
            //     VP Top _ _ _ _|   __________          |      |                    VP Top _ _ _ _|  |__________|         |      |           
            //           .       |  |    A +y  |         |      |                          .       |                       |      |           
            //           .       |  |    |     |         |      |                          .       |                       |      |           
            //           .       |  |    '-->+x|         |      |                        2 _       |                       |      |           
            //           .       |  |__________|         |      |                        1 _       |                       |      |           
            //Tex Height _       |_______________________|      V                        0 _       |_______________________|      |           
            //               (0,1)                       (1,1)                                 (0,0)                       (1,0)              
            //                                                                                
            //

            VkViewports[vp].y = VkViewports[vp].y + VkViewports[vp].height;
            VkViewports[vp].height = -VkViewports[vp].height;
        }
        EnsureVkCmdBuffer();
        // TODO: reinterpret_cast m_Viewports to VkViewports?
        m_CommandBuffer.SetViewports(0, m_NumViewports, VkViewports);
    }

    void DeviceContextVkImpl::SetViewports( Uint32 NumViewports, const Viewport *pViewports, Uint32 RTWidth, Uint32 RTHeight  )
    {
        TDeviceContextBase::SetViewports( NumViewports, pViewports, RTWidth, RTHeight );
        VERIFY( NumViewports == m_NumViewports, "Unexpected number of viewports" );

        CommitViewports();
    }

    void DeviceContextVkImpl::CommitScissorRects()
    {
        VERIFY(m_pPipelineState && m_pPipelineState->GetDesc().GraphicsPipeline.RasterizerDesc.ScissorEnable, "Scissor test must be enabled in the graphics pipeline");

        VkRect2D VkScissorRects[MaxViewports]; // Do not waste time initializing array with zeroes
        for (Uint32 sr = 0; sr < m_NumScissorRects; ++sr)
        {
            const auto &SrcRect = m_ScissorRects[sr];
            VkScissorRects[sr].offset = {SrcRect.left, SrcRect.top};
            VkScissorRects[sr].extent = {static_cast<uint32_t>(SrcRect.right - SrcRect.left), static_cast<uint32_t>(SrcRect.bottom - SrcRect.top)};
        }

        EnsureVkCmdBuffer();
        // TODO: reinterpret_cast m_Viewports to m_Viewports?
        m_CommandBuffer.SetScissorRects(0, m_NumScissorRects, VkScissorRects);
    }


    void DeviceContextVkImpl::SetScissorRects( Uint32 NumRects, const Rect *pRects, Uint32 RTWidth, Uint32 RTHeight  )
    {
        TDeviceContextBase::SetScissorRects(NumRects, pRects, RTWidth, RTHeight);

        // Only commit scissor rects if scissor test is enabled in the rasterizer state. 
        // If scissor is currently disabled, or no PSO is bound, scissor rects will be committed by 
        // the SetPipelineState() when a PSO with enabled scissor test is set.
        if ( m_pPipelineState )
        {
            const auto &PSODesc = m_pPipelineState->GetDesc();
            if (!PSODesc.IsComputePipeline && PSODesc.GraphicsPipeline.RasterizerDesc.ScissorEnable)
            {
                VERIFY(NumRects == m_NumScissorRects, "Unexpected number of scissor rects");
                CommitScissorRects();
            }
        }
    }


    void DeviceContextVkImpl::CommitRenderPassAndFramebuffer()
    {
        const auto& CmdBufferState = m_CommandBuffer.GetState();
        if (CmdBufferState.Framebuffer != m_Framebuffer)
        {
            if (CmdBufferState.RenderPass != VK_NULL_HANDLE)
                m_CommandBuffer.EndRenderPass();
        
            if (m_Framebuffer != VK_NULL_HANDLE)
            {
                VERIFY_EXPR(m_RenderPass != VK_NULL_HANDLE);
                if (m_pBoundDepthStencil)
                {
                    auto* pDSVVk = m_pBoundDepthStencil.RawPtr<TextureViewVkImpl>();
                    auto* pDepthBufferVk = ValidatedCast<TextureVkImpl>(pDSVVk->GetTexture());
                    if (pDepthBufferVk->IsInKnownState())
                    {
                        if (!pDepthBufferVk->CheckState(RESOURCE_STATE_DEPTH_WRITE))
                        {
                            TransitionTextureState(*pDepthBufferVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_DEPTH_WRITE, true);
                        }
                        VERIFY_EXPR(pDepthBufferVk->GetLayout() == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                    }
                }

                for (Uint32 rt=0; rt < m_NumBoundRenderTargets; ++rt)
                {
                    if (ITextureView* pRTV = m_pBoundRenderTargets[rt])
                    {
                        auto* pRTVVk = ValidatedCast<TextureViewVkImpl>(pRTV);
                        auto* pRenderTargetVk = ValidatedCast<TextureVkImpl>(pRTVVk->GetTexture());
                        if (pRenderTargetVk->IsInKnownState())
                        {
                            if (!pRenderTargetVk->CheckState(RESOURCE_STATE_RENDER_TARGET))
                            {
                                TransitionTextureState(*pRenderTargetVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_RENDER_TARGET, true);
                            }
                            VERIFY_EXPR(pRenderTargetVk->GetLayout() == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                        }
                    }
                }
                m_CommandBuffer.BeginRenderPass(m_RenderPass, m_Framebuffer, m_FramebufferWidth, m_FramebufferHeight);
            }
        }
    }

    void DeviceContextVkImpl::SetRenderTargets( Uint32 NumRenderTargets, ITextureView *ppRenderTargets[], ITextureView *pDepthStencil )
    {
        if ( TDeviceContextBase::SetRenderTargets( NumRenderTargets, ppRenderTargets, pDepthStencil ) )
        {
            FramebufferCache::FramebufferCacheKey FBKey;
            RenderPassCache::RenderPassCacheKey RenderPassKey;
            if (m_pBoundDepthStencil)
            {
                auto* pDSVVk = m_pBoundDepthStencil.RawPtr<TextureViewVkImpl>();
                auto* pDepthBuffer = pDSVVk->GetTexture();
                FBKey.DSV = pDSVVk->GetVulkanImageView();
                RenderPassKey.DSVFormat = pDSVVk->GetDesc().Format;
                RenderPassKey.SampleCount = static_cast<Uint8>(pDepthBuffer->GetDesc().SampleCount);
            }
            else
            {
                FBKey.DSV = VK_NULL_HANDLE;
                RenderPassKey.DSVFormat = TEX_FORMAT_UNKNOWN;
            }

            FBKey.NumRenderTargets = m_NumBoundRenderTargets;
            RenderPassKey.NumRenderTargets = static_cast<Uint8>(m_NumBoundRenderTargets);

            for (Uint32 rt=0; rt < m_NumBoundRenderTargets; ++rt)
            {
                if (ITextureView* pRTV = m_pBoundRenderTargets[rt])
                {
                    auto* pRTVVk = ValidatedCast<TextureViewVkImpl>(pRTV);
                    auto* pRenderTarget = pRTVVk->GetTexture();
                    FBKey.RTVs[rt] = pRTVVk->GetVulkanImageView();
                    RenderPassKey.RTVFormats[rt] = pRenderTarget->GetDesc().Format;
                    if (RenderPassKey.SampleCount == 0)
                        RenderPassKey.SampleCount = static_cast<Uint8>(pRenderTarget->GetDesc().SampleCount);
                    else
                        VERIFY(RenderPassKey.SampleCount == pRenderTarget->GetDesc().SampleCount, "Inconsistent sample count");
                }
                else
                {
                    FBKey.RTVs[rt] = VK_NULL_HANDLE;
                    RenderPassKey.RTVFormats[rt] = TEX_FORMAT_UNKNOWN;
                }
            }

            auto pDeviceVkImpl = m_pDevice.RawPtr<RenderDeviceVkImpl>();
            auto& FBCache = pDeviceVkImpl->GetFramebufferCache();
            auto& RPCache = pDeviceVkImpl->GetRenderPassCache();

            m_RenderPass = RPCache.GetRenderPass(RenderPassKey);
            FBKey.Pass = m_RenderPass;
            FBKey.CommandQueueMask = ~Uint64{0};
            m_Framebuffer = FBCache.GetFramebuffer(FBKey, m_FramebufferWidth, m_FramebufferHeight, m_FramebufferSlices);

            // Set the viewport to match the render target size
            SetViewports(1, nullptr, 0, 0);
        }

        EnsureVkCmdBuffer();
        CommitRenderPassAndFramebuffer();
    }

    void DeviceContextVkImpl::ResetRenderTargets()
    {
        TDeviceContextBase::ResetRenderTargets();
        m_RenderPass  = VK_NULL_HANDLE;
        m_Framebuffer = VK_NULL_HANDLE;
    }

    void DeviceContextVkImpl::UpdateBufferRegion(BufferVkImpl* pBuffVk, Uint64 DstOffset, Uint64 NumBytes, VkBuffer vkSrcBuffer, Uint64 SrcOffset)
    {
#ifdef DEVELOPMENT
        if (DstOffset + NumBytes > pBuffVk->GetDesc().uiSizeInBytes)
        {
            LOG_ERROR("Update region is out of buffer bounds which will result in an undefined behavior");
        }
#endif

        EnsureVkCmdBuffer();
        if (pBuffVk->IsInKnownState())
        {
            if (!pBuffVk->CheckState(RESOURCE_STATE_COPY_DEST))
            {
                TransitionBufferState(*pBuffVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COPY_DEST, true);
            }
            VERIFY_EXPR(pBuffVk->CheckAccessFlags(VK_ACCESS_TRANSFER_WRITE_BIT));
        }
        VkBufferCopy CopyRegion;
        CopyRegion.srcOffset = SrcOffset;
        CopyRegion.dstOffset = DstOffset;
        CopyRegion.size = NumBytes;
        VERIFY(pBuffVk->m_VulkanBuffer != VK_NULL_HANDLE, "Copy destination buffer must not be suballocated");
        m_CommandBuffer.CopyBuffer(vkSrcBuffer, pBuffVk->GetVkBuffer(), 1, &CopyRegion);
        ++m_State.NumCommands;
    }

    void DeviceContextVkImpl::UpdateBuffer(IBuffer* pBuffer, Uint32 Offset, Uint32 Size, const PVoid pData)
    {
        TDeviceContextBase::UpdateBuffer(pBuffer, Offset, Size, pData);

        // We must use cmd context from the device context provided, otherwise there will
        // be resource barrier issues in the cmd list in the device context
        auto* pBuffVk = ValidatedCast<BufferVkImpl>(pBuffer);

#ifdef DEVELOPMENT
        if (pBuffVk->GetDesc().Usage == USAGE_DYNAMIC)
        {
            LOG_ERROR("Dynamic buffers must be updated via Map()");
            return;
        }
#endif

        constexpr size_t Alignment = 4;
        // Source buffer offset must be multiple of 4 (18.4)
        auto TmpSpace = m_UploadHeap.Allocate(Size, Alignment);
	    memcpy(TmpSpace.CPUAddress, pData, Size);
        UpdateBufferRegion(pBuffVk, Offset, Size, TmpSpace.vkBuffer, TmpSpace.AlignedOffset);
        // The allocation will stay in the upload heap until the end of the frame at which point all upload
        // pages will be discarded
    }

    void DeviceContextVkImpl::CopyBuffer(IBuffer* pSrcBuffer, IBuffer* pDstBuffer, Uint32 SrcOffset, Uint32 DstOffset, Uint32 Size)
    {
        TDeviceContextBase::CopyBuffer(pSrcBuffer, pDstBuffer, SrcOffset, DstOffset, Size);

        auto *pSrcBuffVk = ValidatedCast<BufferVkImpl>(pSrcBuffer);
        auto *pDstBuffVk = ValidatedCast<BufferVkImpl>(pDstBuffer);

#ifdef DEVELOPMENT
        if (pDstBuffVk->GetDesc().Usage == USAGE_DYNAMIC)
        {
            LOG_ERROR("Dynamic buffers cannot be copy destinations");
            return;
        }
#endif

        EnsureVkCmdBuffer();
        if (pSrcBuffVk->IsInKnownState())
        {
            if (!pSrcBuffVk->CheckState(RESOURCE_STATE_COPY_SOURCE))
            {
                TransitionBufferState(*pSrcBuffVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COPY_SOURCE, true);
            }
            VERIFY_EXPR(pSrcBuffVk->CheckAccessFlags(VK_ACCESS_TRANSFER_READ_BIT));
        }
        if (pDstBuffVk->IsInKnownState())
        {
            if (!pDstBuffVk->CheckState(RESOURCE_STATE_COPY_DEST))
            {
                TransitionBufferState(*pDstBuffVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COPY_DEST, true);
            }
            VERIFY_EXPR(pDstBuffVk->CheckAccessFlags(VK_ACCESS_TRANSFER_WRITE_BIT));
        }

        VkBufferCopy CopyRegion;
        CopyRegion.srcOffset = SrcOffset + pSrcBuffVk->GetDynamicOffset(m_ContextId, this);
        CopyRegion.dstOffset = DstOffset;
        CopyRegion.size = Size;
        VERIFY(pDstBuffVk->m_VulkanBuffer != VK_NULL_HANDLE, "Copy destination buffer must not be suballocated");
        VERIFY_EXPR(pDstBuffVk->GetDynamicOffset(m_ContextId, this) == 0);
        m_CommandBuffer.CopyBuffer(pSrcBuffVk->GetVkBuffer(), pDstBuffVk->GetVkBuffer(), 1, &CopyRegion);
        ++m_State.NumCommands;
    }

    void DeviceContextVkImpl::UpdateTexture(ITexture* pTexture, Uint32 MipLevel, Uint32 Slice, const Box& DstBox, const TextureSubResData& SubresData)
    {
        TDeviceContextBase::UpdateTexture( pTexture, MipLevel, Slice, DstBox, SubresData );

        auto* pTexVk = ValidatedCast<TextureVkImpl>(pTexture);
        // OpenGL backend uses UpdateData() to initialize textures, so we can't check the usage in ValidateUpdateTextureParams()
        DEV_CHECK_ERR( pTexVk->GetDesc().Usage == USAGE_DEFAULT, "Only USAGE_DEFAULT textures should be updated with UpdateData()" );

        //pCtxVk->CopyTextureRegion(SubresData.pSrcBuffer, SubresData.Stride, SubresData.DepthStride, this, DstSubResIndex, DstBox);
        UpdateTextureRegion(SubresData.pData, SubresData.Stride, SubresData.DepthStride, *pTexVk, MipLevel, Slice, DstBox);
    }

    void DeviceContextVkImpl::CopyTexture(ITexture*  pSrcTexture, 
                                          Uint32     SrcMipLevel,
                                          Uint32     SrcSlice,
                                          const Box* pSrcBox,
                                          ITexture*  pDstTexture, 
                                          Uint32     DstMipLevel,
                                          Uint32     DstSlice,
                                          Uint32     DstX,
                                          Uint32     DstY,
                                          Uint32     DstZ)
    {
        TDeviceContextBase::CopyTexture( pSrcTexture, SrcMipLevel, SrcSlice, pSrcBox,
                                         pDstTexture, DstMipLevel, DstSlice, DstX, DstY, DstZ );

        auto* pSrcTexVk = ValidatedCast<TextureVkImpl>( pSrcTexture );
        auto* pDstTexVk = ValidatedCast<TextureVkImpl>( pDstTexture );
        const auto& DstTexDesc = pDstTexVk->GetDesc();
        VkImageCopy CopyRegion = {};
        if( pSrcBox )
        {
            CopyRegion.srcOffset.x = pSrcBox->MinX;
            CopyRegion.srcOffset.y = pSrcBox->MinY;
            CopyRegion.srcOffset.z = pSrcBox->MinZ;
            CopyRegion.extent.width  = pSrcBox->MaxX - pSrcBox->MinX;
            CopyRegion.extent.height = std::max(pSrcBox->MaxY - pSrcBox->MinY, 1u);
            CopyRegion.extent.depth  = std::max(pSrcBox->MaxZ - pSrcBox->MinZ, 1u);
        }
        else
        {
            CopyRegion.srcOffset = VkOffset3D{0,0,0};
            CopyRegion.extent.width  = std::max(DstTexDesc.Width  >> SrcMipLevel, 1u);
            CopyRegion.extent.height = std::max(DstTexDesc.Height >> SrcMipLevel, 1u);
            if(DstTexDesc.Type == RESOURCE_DIM_TEX_3D)
                CopyRegion.extent.depth = std::max(DstTexDesc.Depth >> SrcMipLevel, 1u);
            else
                CopyRegion.extent.depth = 1;
        }

        const auto& FmtAttribs = GetDevice()->GetTextureFormatInfo(DstTexDesc.Format);
        VkImageAspectFlags aspectMask = 0;
        if (FmtAttribs.ComponentType == COMPONENT_TYPE_DEPTH)
            aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        else if (FmtAttribs.ComponentType == COMPONENT_TYPE_DEPTH_STENCIL)
        {
            aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        else
            aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

        CopyRegion.srcSubresource.baseArrayLayer = SrcSlice;
        CopyRegion.srcSubresource.layerCount     = 1;
        CopyRegion.srcSubresource.mipLevel       = SrcMipLevel;
        CopyRegion.srcSubresource.aspectMask     = aspectMask;
    
        CopyRegion.dstSubresource.baseArrayLayer = DstSlice;
        CopyRegion.dstSubresource.layerCount     = 1;
        CopyRegion.dstSubresource.mipLevel       = DstMipLevel;
        CopyRegion.dstSubresource.aspectMask     = aspectMask;

        CopyRegion.dstOffset.x = DstX;
        CopyRegion.dstOffset.y = DstY;
        CopyRegion.dstOffset.z = DstZ;

        CopyTextureRegion(pSrcTexVk, pDstTexVk, CopyRegion);
    }

    void DeviceContextVkImpl::CopyTextureRegion(TextureVkImpl *pSrcTexture, TextureVkImpl *pDstTexture, const VkImageCopy &CopyRegion)
    {
        EnsureVkCmdBuffer();
        if (pSrcTexture->IsInKnownState())
        {
            if (!pSrcTexture->CheckState(RESOURCE_STATE_COPY_SOURCE))
            {
                TransitionTextureState(*pSrcTexture, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COPY_SOURCE, true);
            }
            VERIFY_EXPR(pSrcTexture->GetLayout() == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        if (pDstTexture->IsInKnownState())
        {
            if (!pDstTexture->CheckState(RESOURCE_STATE_COPY_DEST))
            {
                TransitionTextureState(*pDstTexture, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COPY_DEST, true);
            }
            VERIFY_EXPR(pDstTexture->GetLayout() == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
        // srcImageLayout must be VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL
        // dstImageLayout must be VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL (18.3)
        m_CommandBuffer.CopyImage(pSrcTexture->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pDstTexture->GetVkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &CopyRegion);
        ++m_State.NumCommands;
    }

    DeviceContextVkImpl::BufferToTextureCopyInfo DeviceContextVkImpl::GetBufferToTextureCopyInfo(
                                                       const TextureDesc& TexDesc,
                                                       Uint32             MipLevel,
                                                       const Box&         Region)const
    {
        BufferToTextureCopyInfo CopyInfo;
        const auto& FmtAttribs = GetTextureFormatAttribs(TexDesc.Format);
        VERIFY_EXPR(Region.MaxX > Region.MinX && Region.MaxY > Region.MinY && Region.MaxZ > Region.MinZ);
        auto UpdateRegionWidth  = Region.MaxX - Region.MinX;
        auto UpdateRegionHeight = Region.MaxY - Region.MinY;
        auto UpdateRegionDepth  = Region.MaxZ - Region.MinZ;
        if(FmtAttribs.ComponentType == COMPONENT_TYPE_COMPRESSED)
        {
            // Align update region size by the block size. This is only necessary when updating
            // coarse mip levels. Otherwise UpdateRegionWidth/Height should be multiples of block size
            VERIFY_EXPR( (FmtAttribs.BlockWidth  & (FmtAttribs.BlockWidth-1))  == 0 );
            VERIFY_EXPR( (FmtAttribs.BlockHeight & (FmtAttribs.BlockHeight-1)) == 0 );
            const auto BlockAlignedRegionWidth =  (UpdateRegionWidth  + (FmtAttribs.BlockWidth-1))  & ~(FmtAttribs.BlockWidth -1);
            const auto BlockAlignedRegionHeight = (UpdateRegionHeight + (FmtAttribs.BlockHeight-1)) & ~(FmtAttribs.BlockHeight-1);
            CopyInfo.RowSize  = BlockAlignedRegionWidth  / Uint32{FmtAttribs.BlockWidth} * Uint32{FmtAttribs.ComponentSize};
            CopyInfo.RowCount = BlockAlignedRegionHeight / FmtAttribs.BlockHeight;
            
            // (imageExtent.width + imageOffset.x) must be less than or equal to the image subresource width, and
            // (imageExtent.height + imageOffset.y) must be less than or equal to the image subresource height (18.4),
            // so we need to clamp UpdateRegionWidth and Height
            const Uint32 MipWidth  = std::max(TexDesc.Width  >> MipLevel, 1u);
            const Uint32 MipHeight = std::max(TexDesc.Height >> MipLevel, 1u);
            VERIFY_EXPR(MipWidth > Region.MinX);
            UpdateRegionWidth  = std::min(UpdateRegionWidth,  MipWidth  - Region.MinX);
            VERIFY_EXPR(MipHeight > Region.MinY);
            UpdateRegionHeight = std::min(UpdateRegionHeight, MipHeight - Region.MinY);
        }
        else
        {
            CopyInfo.RowSize  = UpdateRegionWidth * Uint32{FmtAttribs.ComponentSize} * Uint32{FmtAttribs.NumComponents};
            CopyInfo.RowCount = UpdateRegionHeight;
        }

        const auto& DeviceLimits = m_pDevice.RawPtr<const RenderDeviceVkImpl>()->GetPhysicalDevice().GetProperties().limits;
        CopyInfo.Stride = Align(CopyInfo.RowSize, static_cast<Uint32>(DeviceLimits.optimalBufferCopyRowPitchAlignment));
        if (FmtAttribs.ComponentType == COMPONENT_TYPE_COMPRESSED)
        {
            // If the calling command's VkImage parameter is a compressed image,
            // bufferRowLength must be a multiple of the compressed texel block width
            // In texels (not even in compressed blocks!)
            CopyInfo.StrideInTexels = CopyInfo.Stride / Uint32{FmtAttribs.ComponentSize} * Uint32{FmtAttribs.BlockWidth};
        }
        else
        {
            CopyInfo.StrideInTexels = CopyInfo.Stride / (Uint32{FmtAttribs.ComponentSize} * Uint32{FmtAttribs.NumComponents});
        }
        CopyInfo.DepthStride = CopyInfo.RowCount * CopyInfo.Stride;
        CopyInfo.MemorySize  = UpdateRegionDepth * CopyInfo.DepthStride;
        CopyInfo.Region      = Region;
        return CopyInfo;
    }


    void DeviceContextVkImpl::UpdateTextureRegion(const void*    pSrcData,
                                                  Uint32         SrcStride,
                                                  Uint32         SrcDepthStride,
                                                  TextureVkImpl& TextureVk,
                                                  Uint32         MipLevel,
                                                  Uint32         Slice,
                                                  const Box&     DstBox)
    {
        const auto& TexDesc = TextureVk.GetDesc();
        VERIFY(TexDesc.SampleCount == 1, "Only single-sample textures can be updated with vkCmdCopyBufferToImage()");
        auto CopyInfo = GetBufferToTextureCopyInfo(TexDesc, MipLevel, DstBox);
        const auto UpdateRegionDepth  = CopyInfo.Region.MaxZ - CopyInfo.Region.MinZ;

        // For UpdateTextureRegion(), use UploadHeap, not dynamic heap
        const auto& DeviceLimits = m_pDevice.RawPtr<const RenderDeviceVkImpl>()->GetPhysicalDevice().GetProperties().limits;
        // Source buffer offset must be multiple of 4 (18.4)
        auto BufferOffsetAlignment = std::max(DeviceLimits.optimalBufferCopyOffsetAlignment, VkDeviceSize{4});
        // If the calling command�s VkImage parameter is a compressed image, bufferOffset must be a multiple of 
        // the compressed texel block size in bytes (18.4)
        const auto& FmtAttribs = GetTextureFormatAttribs(TexDesc.Format);
        if (FmtAttribs.ComponentType == COMPONENT_TYPE_COMPRESSED)
        {
            BufferOffsetAlignment = std::max(BufferOffsetAlignment, VkDeviceSize{FmtAttribs.ComponentSize});
        }
        auto Allocation = m_UploadHeap.Allocate(CopyInfo.MemorySize, BufferOffsetAlignment);
        // The allocation will stay in the upload heap until the end of the frame at which point all upload
        // pages will be discarded
        VERIFY( (Allocation.AlignedOffset % BufferOffsetAlignment) == 0, "Allocation offset must be at least 32-bit algined");

#ifdef _DEBUG
        {
            VERIFY(SrcStride >= CopyInfo.RowSize, "Source data stride (", SrcStride, ") is below the image row size (", CopyInfo.RowSize, ")");
            const Uint32 PlaneSize = SrcStride * CopyInfo.RowCount;
            VERIFY(UpdateRegionDepth == 1 || SrcDepthStride >= PlaneSize, "Source data depth stride (", SrcDepthStride, ") is below the image plane size (", PlaneSize, ")");
        }
#endif
        for(Uint32 DepthSlice = 0; DepthSlice < UpdateRegionDepth; ++DepthSlice)
        {
            for(Uint32 row = 0; row < CopyInfo.RowCount; ++row)
            {
                const auto* pSrcPtr =
                    reinterpret_cast<const Uint8*>(pSrcData)
                    + row        * SrcStride
                    + DepthSlice * SrcDepthStride;
                auto* pDstPtr =
                    reinterpret_cast<Uint8*>(Allocation.CPUAddress)
                    + row        * CopyInfo.Stride
                    + DepthSlice * CopyInfo.DepthStride;
                
                memcpy(pDstPtr, pSrcPtr, CopyInfo.RowSize);
            }
        }
        CopyBufferToTexture(Allocation.vkBuffer,
                            static_cast<Uint32>(Allocation.AlignedOffset),
                            CopyInfo.StrideInTexels,
                            CopyInfo.Region,
                            TextureVk,
                            MipLevel,
                            Slice);
    }

    void DeviceContextVkImpl::CopyBufferToTexture(VkBuffer         vkBuffer,
                                                  Uint32           BufferOffset,
                                                  Uint32           BufferRowStrideInTexels,
                                                  const Box&       Region,
                                                  TextureVkImpl&   TextureVk,
                                                  Uint32           MipLevel,
                                                  Uint32           ArraySlice)
    {
        EnsureVkCmdBuffer();
        if (TextureVk.IsInKnownState())
        {
            if (!TextureVk.CheckState(RESOURCE_STATE_COPY_DEST))
            {
                TransitionTextureState(TextureVk, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COPY_DEST, true);
            }
            VERIFY_EXPR(TextureVk.GetLayout() == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }

        VkBufferImageCopy CopyRegion = {};
        VERIFY( (BufferOffset % 4) == 0, "Source buffer offset must be multiple of 4 (18.4)");
        CopyRegion.bufferOffset = BufferOffset; // must be a multiple of 4 (18.4)

        // bufferRowLength and bufferImageHeight specify the data in buffer memory as a subregion of a larger two- or
        // three-dimensional image, and control the addressing calculations of data in buffer memory. If either of these
        // values is zero, that aspect of the buffer memory is considered to be tightly packed according to the imageExtent (18.4).
        CopyRegion.bufferRowLength = BufferRowStrideInTexels;
        CopyRegion.bufferImageHeight = 0;

        const auto& TexDesc = TextureVk.GetDesc();
        const auto& FmtAttribs = GetTextureFormatAttribs(TexDesc.Format);
        // The aspectMask member of imageSubresource must only have a single bit set (18.4)
        if (FmtAttribs.ComponentType == COMPONENT_TYPE_DEPTH)
            CopyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        else if (FmtAttribs.ComponentType == COMPONENT_TYPE_DEPTH_STENCIL)
        {
            UNSUPPORTED("Updating depth-stencil texture is not currently supported");
            // When copying to or from a depth or stencil aspect, the data in buffer memory uses a layout 
            // that is a (mostly) tightly packed representation of the depth or stencil data.
            // To copy both the depth and stencil aspects of a depth/stencil format, two entries in 
            // pRegions can be used, where one specifies the depth aspect in imageSubresource, and the 
            // other specifies the stencil aspect (18.4)
        }
        else
            CopyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

        CopyRegion.imageSubresource.baseArrayLayer = ArraySlice;
        CopyRegion.imageSubresource.layerCount = 1;
        CopyRegion.imageSubresource.mipLevel = MipLevel;
        // - imageOffset.x and (imageExtent.width + imageOffset.x) must both be greater than or equal to 0 and
        //   less than or equal to the image subresource width (18.4)
        // - imageOffset.y and (imageExtent.height + imageOffset.y) must both be greater than or equal to 0 and
        //   less than or equal to the image subresource height (18.4)
        CopyRegion.imageOffset = 
            VkOffset3D
            {
                static_cast<int32_t>(Region.MinX),
                static_cast<int32_t>(Region.MinY),
                static_cast<int32_t>(Region.MinZ)
            };
        VERIFY(Region.MaxX > Region.MinX && Region.MaxY - Region.MinY && Region.MaxZ > Region.MinZ,
               "[", Region.MinX, " .. ", Region.MaxX, ") x [", Region.MinY, " .. ", Region.MaxY, ") x [", Region.MinZ, " .. ", Region.MaxZ, ") is not a vaild region");
        CopyRegion.imageExtent =
            VkExtent3D
            {
                static_cast<uint32_t>(Region.MaxX - Region.MinX),
                static_cast<uint32_t>(Region.MaxY - Region.MinY),
                static_cast<uint32_t>(Region.MaxZ - Region.MinZ)
            };
        
        m_CommandBuffer.CopyBufferToImage(
            vkBuffer,
            TextureVk.GetVkImage(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // must be VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL or VK_IMAGE_LAYOUT_GENERAL (18.4)
            1,
            &CopyRegion);
    }

    void DeviceContextVkImpl::MapTexture( TextureVkImpl&            TextureVk,
                                          Uint32                    MipLevel,
                                          Uint32                    ArraySlice,
                                          MAP_TYPE                  MapType,
                                          Uint32                    MapFlags,
                                          const Box&                MapRegion,
                                          MappedTextureSubresource& MappedData )
    {
        const auto& TexDesc = TextureVk.GetDesc();
        auto CopyInfo = GetBufferToTextureCopyInfo(TexDesc, MipLevel, MapRegion);
        const auto& DeviceLimits = m_pDevice.RawPtr<RenderDeviceVkImpl>()->GetPhysicalDevice().GetProperties().limits;
        // Source buffer offset must be multiple of 4 (18.4)
        auto Alignment = std::max(DeviceLimits.optimalBufferCopyOffsetAlignment, VkDeviceSize{4});
        // If the calling command�s VkImage parameter is a compressed image, bufferOffset must be a multiple of 
        // the compressed texel block size in bytes (18.4)
        const auto& FmtAttribs = GetTextureFormatAttribs(TexDesc.Format);
        if (FmtAttribs.ComponentType == COMPONENT_TYPE_COMPRESSED)
        {
            Alignment = std::max(Alignment, VkDeviceSize{FmtAttribs.ComponentSize});
        }
        auto Allocation = AllocateDynamicSpace(CopyInfo.MemorySize, static_cast<Uint32>(Alignment));

        MappedData.pData       = reinterpret_cast<Uint8*>(Allocation.pDynamicMemMgr->GetCPUAddress()) + Allocation.AlignedOffset;
        MappedData.Stride      = CopyInfo.Stride;
        MappedData.DepthStride = CopyInfo.DepthStride;

        auto it = m_MappedTextures.emplace(MappedTextureKey{&TextureVk, MipLevel, ArraySlice}, MappedTexture{CopyInfo, std::move(Allocation)});
        if(!it.second)
            LOG_ERROR_MESSAGE("Mip level ", MipLevel, ", slice ", ArraySlice, " of texture '", TexDesc.Name, "' has already been mapped");
    }

    void DeviceContextVkImpl::UnmapTexture( TextureVkImpl& TextureVk,
                                            Uint32         MipLevel,
                                            Uint32         ArraySlice)
    {
        const auto& TexDesc = TextureVk.GetDesc();
        auto UploadSpaceIt = m_MappedTextures.find(MappedTextureKey{&TextureVk, MipLevel, ArraySlice});
        if(UploadSpaceIt != m_MappedTextures.end())
        {
            auto& MappedTex = UploadSpaceIt->second;
            CopyBufferToTexture(MappedTex.Allocation.pDynamicMemMgr->GetVkBuffer(),
                                static_cast<Uint32>(MappedTex.Allocation.AlignedOffset),
                                MappedTex.CopyInfo.StrideInTexels,
                                MappedTex.CopyInfo.Region,
                                TextureVk,
                                MipLevel,
                                ArraySlice);
            m_MappedTextures.erase(UploadSpaceIt);
        }
        else
        {
            LOG_ERROR_MESSAGE("Failed to unmap mip level ", MipLevel, ", slice ", ArraySlice, " of texture '", TexDesc.Name, "'. The texture has either been unmapped already or has not been mapped");
        }
    }

    void DeviceContextVkImpl::FinishCommandList(class ICommandList **ppCommandList)
    {
        if (m_CommandBuffer.GetState().RenderPass != VK_NULL_HANDLE)
        {
            m_CommandBuffer.EndRenderPass();
        }

        auto vkCmdBuff = m_CommandBuffer.GetVkCmdBuffer();
        auto err = vkEndCommandBuffer(vkCmdBuff);
        DEV_CHECK_ERR(err == VK_SUCCESS, "Failed to end command buffer");

        auto* pDeviceVkImpl = m_pDevice.RawPtr<RenderDeviceVkImpl>();
        CommandListVkImpl *pCmdListVk( NEW_RC_OBJ(m_CmdListAllocator, "CommandListVkImpl instance", CommandListVkImpl)
                                                 (pDeviceVkImpl, this, vkCmdBuff) );
        pCmdListVk->QueryInterface( IID_CommandList, reinterpret_cast<IObject**>(ppCommandList) );
        
        m_CommandBuffer.Reset();
        m_State = ContextState{};
        m_DescrSetBindInfo.Reset();
        m_pPipelineState = nullptr;

        InvalidateState();
    }

    void DeviceContextVkImpl::ExecuteCommandList(class ICommandList *pCommandList)
    {
        if (m_bIsDeferred)
        {
            LOG_ERROR_MESSAGE("Only immediate context can execute command list");
            return;
        }

        Flush();
        
        InvalidateState();

        CommandListVkImpl* pCmdListVk = ValidatedCast<CommandListVkImpl>(pCommandList);
        VkCommandBuffer vkCmdBuff = VK_NULL_HANDLE;
        RefCntAutoPtr<IDeviceContext> pDeferredCtx;
        pCmdListVk->Close(vkCmdBuff, pDeferredCtx);
        VERIFY(vkCmdBuff != VK_NULL_HANDLE, "Trying to execute empty command buffer");
        VERIFY_EXPR(pDeferredCtx);
        VkSubmitInfo SubmitInfo = {};
        SubmitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        SubmitInfo.pNext              = nullptr;
        SubmitInfo.commandBufferCount = 1;
        SubmitInfo.pCommandBuffers    = &vkCmdBuff;
        auto pDeviceVkImpl = m_pDevice.RawPtr<RenderDeviceVkImpl>();
        VERIFY_EXPR(m_PendingFences.empty());
        auto pDeferredCtxVkImpl = pDeferredCtx.RawPtr<DeviceContextVkImpl>();
        auto SubmittedFenceValue = pDeviceVkImpl->ExecuteCommandBuffer(m_CommandQueueId, SubmitInfo, this, nullptr);
        // Set the bit in the deferred context cmd queue mask corresponding to cmd queue of this context
        pDeferredCtxVkImpl->m_SubmittedBuffersCmdQueueMask |= Uint64{1} << m_CommandQueueId;
        // It is OK to dispose command buffer from another thread. We are not going to
        // record any commands and only need to add the buffer to the queue
        pDeferredCtxVkImpl->DisposeVkCmdBuffer(m_CommandQueueId, vkCmdBuff, SubmittedFenceValue);
    }

    void DeviceContextVkImpl::SignalFence(IFence* pFence, Uint64 Value)
    {
        VERIFY(!m_bIsDeferred, "Fence can only be signalled from immediate context");
        m_PendingFences.emplace_back( std::make_pair(Value, pFence) );
    };

    void DeviceContextVkImpl::TransitionImageLayout(ITexture* pTexture, VkImageLayout NewLayout)
    {
        VERIFY_EXPR(pTexture != nullptr);
        auto pTextureVk = ValidatedCast<TextureVkImpl>(pTexture);
        if (!pTextureVk->IsInKnownState())
        {
            LOG_ERROR_MESSAGE("Failed to transition layout for texture '", pTextureVk->GetDesc().Name, "' because the texture state is unknown");
            return;
        }
        auto NewState = VkImageLayoutToResourceState(NewLayout);
        if (!pTextureVk->CheckState(NewState))
        {
            TransitionTextureState(*pTextureVk, RESOURCE_STATE_UNKNOWN, NewState, true);
        }
    }

    void DeviceContextVkImpl::TransitionTextureState(TextureVkImpl&           TextureVk,
                                                     RESOURCE_STATE           OldState,
                                                     RESOURCE_STATE           NewState,
                                                     bool                     UpdateTextureState,
                                                     VkImageSubresourceRange* pSubresRange/* = nullptr*/)
    {
        if (OldState == RESOURCE_STATE_UNKNOWN)
        {
            if (TextureVk.IsInKnownState())
            {
                OldState = TextureVk.GetState();
            }
            else
            {
                LOG_ERROR_MESSAGE("Failed to transition the state of texture '", TextureVk.GetDesc().Name, "' because the texture state is unknown and is not explicitly specified");
                return;
            }
        }
        else
        {
            if (TextureVk.IsInKnownState() && TextureVk.GetState() != OldState)
            {
                LOG_ERROR_MESSAGE("The state ", GetResourceStateString(TextureVk.GetState()), " of texture '",
                                   TextureVk.GetDesc().Name, "' does not match the old state ", GetResourceStateString(OldState),
                                   " specified by the barrier");
            }
        }

        EnsureVkCmdBuffer();

        auto vkImg = TextureVk.GetVkImage();
        VkImageSubresourceRange FullSubresRange;
        if (pSubresRange == nullptr)
        {
            pSubresRange = &FullSubresRange;
            FullSubresRange.aspectMask     = 0;
            FullSubresRange.baseArrayLayer = 0;
            FullSubresRange.layerCount     = VK_REMAINING_ARRAY_LAYERS;
            FullSubresRange.baseMipLevel   = 0;
            FullSubresRange.levelCount     = VK_REMAINING_MIP_LEVELS;
        }

        if (pSubresRange->aspectMask == 0)
        {
            const auto& TexDesc = TextureVk.GetDesc();
            const auto& FmtAttribs = GetTextureFormatAttribs(TexDesc.Format);
            if (FmtAttribs.ComponentType == COMPONENT_TYPE_DEPTH)
                pSubresRange->aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            else if (FmtAttribs.ComponentType == COMPONENT_TYPE_DEPTH_STENCIL)
            {
                // If image has a depth / stencil format with both depth and stencil components, then the 
                // aspectMask member of subresourceRange must include both VK_IMAGE_ASPECT_DEPTH_BIT and 
                // VK_IMAGE_ASPECT_STENCIL_BIT (6.7.3)
                pSubresRange->aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            }
            else
                pSubresRange->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }

        auto OldLayout = ResourceStateToVkImageLayout(OldState);
        auto NewLayout = ResourceStateToVkImageLayout(NewState);
        m_CommandBuffer.TransitionImageLayout(vkImg, OldLayout, NewLayout, *pSubresRange);
        if(UpdateTextureState)
        {
            TextureVk.SetState(NewState);
            VERIFY_EXPR(TextureVk.GetLayout() == NewLayout);
        }
    }

    void DeviceContextVkImpl::TransitionImageLayout(TextureVkImpl& TextureVk, VkImageLayout OldLayout, VkImageLayout NewLayout, const VkImageSubresourceRange& SubresRange)
    {
        VERIFY(TextureVk.GetLayout() != NewLayout, "The texture is already transitioned to correct layout");
        EnsureVkCmdBuffer();
        auto vkImg = TextureVk.GetVkImage();
        m_CommandBuffer.TransitionImageLayout(vkImg, OldLayout, NewLayout, SubresRange);
    }

    void DeviceContextVkImpl::BufferMemoryBarrier(IBuffer* pBuffer, VkAccessFlags NewAccessFlags)
    {
        VERIFY_EXPR(pBuffer != nullptr);
        auto pBuffVk = ValidatedCast<BufferVkImpl>(pBuffer);
        if (!pBuffVk->IsInKnownState())
        {
            LOG_ERROR_MESSAGE("Failed to execute buffer memory barrier for buffer '", pBuffVk->GetDesc().Name, "' because the buffer state is unknown");
            return;
        }
        auto NewState = VkAccessFlagsToResourceStates(NewAccessFlags);
        if ((pBuffVk->GetState() & NewState) != NewState)
        {
            TransitionBufferState(*pBuffVk, RESOURCE_STATE_UNKNOWN, NewState, true);
        }
    }

    void DeviceContextVkImpl::TransitionBufferState(BufferVkImpl& BufferVk, RESOURCE_STATE OldState, RESOURCE_STATE NewState, bool UpdateBufferState)
    {
        EnsureVkCmdBuffer();
        if (OldState == RESOURCE_STATE_UNKNOWN)
        {
            if (BufferVk.IsInKnownState())
            {
                OldState = BufferVk.GetState();
            }
            else
            {
                LOG_ERROR_MESSAGE("Failed to transition the state of buffer '", BufferVk.GetDesc().Name, "' because the buffer state is unknown and is not explicitly specified");
                return;
            }
        }
        else
        {
            if (BufferVk.IsInKnownState() && BufferVk.GetState() != OldState)
            {
                LOG_ERROR_MESSAGE("The state ", GetResourceStateString(BufferVk.GetState()), " of buffer '",
                                   BufferVk.GetDesc().Name, "' does not match the old state ", GetResourceStateString(OldState),
                                   " specified by the barrier");
            }
        }

        if ((OldState & NewState) != NewState)
        {
            DEV_CHECK_ERR(BufferVk.m_VulkanBuffer != VK_NULL_HANDLE, "Cannot transition suballocated buffer");
            VERIFY_EXPR(BufferVk.GetDynamicOffset(m_ContextId, this) == 0);

            auto vkBuff = BufferVk.GetVkBuffer();
            auto OldAccessFlags = ResourceStateFlagsToVkAccessFlags(OldState);
            auto NewAccessFlags = ResourceStateFlagsToVkAccessFlags(NewState);
            m_CommandBuffer.BufferMemoryBarrier(vkBuff, OldAccessFlags, NewAccessFlags);
            if (UpdateBufferState)
            {
                BufferVk.SetState(NewState);
            }
        }
    }

    VulkanDynamicAllocation DeviceContextVkImpl::AllocateDynamicSpace(Uint32 SizeInBytes, Uint32 Alignment)
    {
        auto DynAlloc = m_DynamicHeap.Allocate(SizeInBytes, Alignment);
#ifdef DEVELOPMENT
        DynAlloc.dvpFrameNumber = m_ContextFrameNumber;
#endif
        return DynAlloc;
    }

    void DeviceContextVkImpl::TransitionResourceStates(Uint32 BarrierCount, StateTransitionDesc* pResourceBarriers)
    {
        if(BarrierCount == 0)
            return;

        EnsureVkCmdBuffer();

        for(Uint32 i=0; i < BarrierCount; ++i)
        {
            const auto& Barrier = pResourceBarriers[i];
#ifdef DEVELOPMENT
            DvpVerifyStateTransitionDesc(Barrier);
#endif
            if (Barrier.pTexture)
            {
                auto* pTextureVkImpl = ValidatedCast<TextureVkImpl>(Barrier.pTexture);
                VkImageSubresourceRange SubResRange;
                SubResRange.aspectMask     = 0;
                SubResRange.baseMipLevel   = Barrier.FirstMipLevel;
                SubResRange.levelCount     = (Barrier.MipLevelsCount == StateTransitionDesc::RemainingMipLevels) ? VK_REMAINING_MIP_LEVELS : Barrier.MipLevelsCount;
                SubResRange.baseArrayLayer = Barrier.FirstArraySlice;
                SubResRange.layerCount     = (Barrier.ArraySliceCount == StateTransitionDesc::RemainingArraySlices) ? VK_REMAINING_ARRAY_LAYERS : Barrier.ArraySliceCount;
                TransitionTextureState(*pTextureVkImpl, Barrier.OldState, Barrier.NewState, Barrier.UpdateResourceState, &SubResRange);
            }
            else
            {
                VERIFY_EXPR(Barrier.pBuffer != nullptr);
                auto* pBufferVkImpl = ValidatedCast<BufferVkImpl>(Barrier.pBuffer);
                TransitionBufferState(*pBufferVkImpl, Barrier.OldState, Barrier.NewState, Barrier.UpdateResourceState);
            }
        }
    }
}
