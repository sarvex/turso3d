// For conditions of distribution and use, see copyright notice in License.txt

#include "../Graphics/FrameBuffer.h"
#include "../Graphics/Graphics.h"
#include "../Graphics/IndexBuffer.h"
#include "../Graphics/RenderBuffer.h"
#include "../Graphics/Shader.h"
#include "../Graphics/ShaderProgram.h"
#include "../Graphics/Texture.h"
#include "../Graphics/UniformBuffer.h"
#include "../Graphics/VertexBuffer.h"
#include "../IO/Log.h"
#include "../Math/Random.h"
#include "../Resource/ResourceCache.h"
#include "../Scene/Scene.h"
#include "AnimatedModel.h"
#include "Animation.h"
#include "Batch.h"
#include "Camera.h"
#include "DebugRenderer.h"
#include "Light.h"
#include "Material.h"
#include "Model.h"
#include "Octree.h"
#include "Renderer.h"
#include "StaticModel.h"

#include <algorithm>
#include <cstring>
#include <tracy/Tracy.hpp>

static const size_t DRAWABLES_PER_BATCH_TASK = 128;

inline bool CompareLights(LightDrawable* lhs, LightDrawable* rhs)
{
    return lhs->Distance() < rhs->Distance();
}

void ThreadOctantResult::Clear()
{
    drawableAcc = 0;
    taskOctantIdx = 0;
    batchTaskIdx = 0;
    lights.clear();
    octants.clear();
}

void ThreadBatchResult::Clear()
{
    minZ = M_MAX_FLOAT;
    maxZ = 0.0f;
    geometryBounds.Undefine();
    opaqueBatches.clear();
    alphaBatches.clear();
}

ShadowMap::ShadowMap()
{
    // Construct texture but do not define its size yet
    texture = new Texture();
    fbo = new FrameBuffer();
}

ShadowMap::~ShadowMap()
{
}

void ShadowMap::Clear()
{
    freeQueueIdx = 0;
    freeCasterListIdx = 0;
    allocator.Reset(texture->Width(), texture->Height(), 0, 0, false);
    shadowViews.clear();
    instanceTransforms.clear();

    for (auto it = shadowBatches.begin(); it != shadowBatches.end(); ++it)
        it->Clear();
    for (auto it = shadowCasters.begin(); it != shadowCasters.end(); ++it)
        it->clear();
}

Renderer::Renderer() :
    graphics(Subsystem<Graphics>()),
    workQueue(Subsystem<WorkQueue>()),
    frameNumber(0),
    clusterFrustumsDirty(true),
    lastPerMaterialUniforms(0),
    depthBiasMul(1.0f),
    slopeScaleBiasMul(1.0f)
{
    assert(graphics && graphics->IsInitialized());
    assert(workQueue);

    RegisterSubsystem(this);
    RegisterRendererLibrary();

    hasInstancing = graphics->HasInstancing();
    if (hasInstancing)
    {
        instanceVertexBuffer = new VertexBuffer();
        instanceVertexElements.push_back(VertexElement(ELEM_VECTOR4, SEM_TEXCOORD, 3));
        instanceVertexElements.push_back(VertexElement(ELEM_VECTOR4, SEM_TEXCOORD, 4));
        instanceVertexElements.push_back(VertexElement(ELEM_VECTOR4, SEM_TEXCOORD, 5));
    }

    clusterTexture = new Texture();
    clusterTexture->Define(TEX_3D, IntVector3(NUM_CLUSTER_X, NUM_CLUSTER_Y, NUM_CLUSTER_Z), FMT_RGBA32U, 1);
    clusterTexture->DefineSampler(FILTER_POINT, ADDRESS_CLAMP, ADDRESS_CLAMP, ADDRESS_CLAMP);

    clusterFrustums = new Frustum[NUM_CLUSTER_X * NUM_CLUSTER_Y * NUM_CLUSTER_Z];
    clusterBoundingBoxes = new BoundingBox[NUM_CLUSTER_X * NUM_CLUSTER_Y * NUM_CLUSTER_Z];
    clusterData = new unsigned char[MAX_LIGHTS_CLUSTER * NUM_CLUSTER_X * NUM_CLUSTER_Y * NUM_CLUSTER_Z];
    lightData = new LightData[MAX_LIGHTS + 1];

    perViewDataBuffer = new UniformBuffer();
    perViewDataBuffer->Define(USAGE_DYNAMIC, sizeof(PerViewUniforms));

    lightDataBuffer = new UniformBuffer();
    lightDataBuffer->Define(USAGE_DYNAMIC, MAX_LIGHTS * sizeof(LightData));

    octantResults.resize(NUM_OCTANTS + 1);
    batchResults.resize(workQueue->NumThreads());

    for (size_t i = 0; i < NUM_OCTANTS + 1; ++i)
        collectOctantsTasks[i] = new CollectOctantsTask(this, &Renderer::CollectOctantsWork);

    for (size_t z = 0; z < NUM_CLUSTER_Z; ++z)
    {
        cullLightsTasks[z] = new CullLightsTask(this, &Renderer::CullLightsToFrustumWork);
        cullLightsTasks[z]->z = z;
    }

    processLightsTask = new MemberFunctionTask<Renderer>(this, &Renderer::ProcessLightsWork);
    processShadowCastersTask = new MemberFunctionTask<Renderer>(this, &Renderer::ProcessShadowCastersWork);
}

Renderer::~Renderer()
{
    RemoveSubsystem(this);
}

void Renderer::SetupShadowMaps(int dirLightSize, int lightAtlasSize, ImageFormat format)
{
    shadowMaps.resize(2);

    for (size_t i = 0; i < shadowMaps.size(); ++i)
    {
        ShadowMap& shadowMap = shadowMaps[i];

        shadowMap.texture->Define(TEX_2D, i == 0 ? IntVector2(dirLightSize * 2, dirLightSize) : IntVector2(lightAtlasSize, lightAtlasSize), format, 1);
        shadowMap.texture->DefineSampler(COMPARE_BILINEAR, ADDRESS_CLAMP, ADDRESS_CLAMP, ADDRESS_CLAMP, 1);
        shadowMap.fbo->Define(nullptr, shadowMap.texture);
    }

    staticObjectShadowBuffer = new RenderBuffer();
    staticObjectShadowBuffer->Define(IntVector2(lightAtlasSize, lightAtlasSize), format, 1);
    staticObjectShadowFbo = new FrameBuffer();
    staticObjectShadowFbo->Define(nullptr, staticObjectShadowBuffer);

    DefineFaceSelectionTextures();

    shadowMapsDirty = true;
}

void Renderer::SetShadowDepthBiasMul(float depthBiasMul_, float slopeScaleBiasMul_)
{
    depthBiasMul = depthBiasMul_;
    slopeScaleBiasMul = slopeScaleBiasMul_;
    
    // Need to rerender all shadow maps with changed bias
    shadowMapsDirty = true;
}

void Renderer::PrepareView(Scene* scene_, Camera* camera_, bool drawShadows_)
{
    ZoneScoped;

    if (!scene_ || !camera_)
        return;

    scene = scene_;
    camera = camera_;
    octree = scene->FindChild<Octree>();
    if (!octree)
        return;

    // Framenumber is never 0
    ++frameNumber;
    if (!frameNumber)
        ++frameNumber;

    drawShadows = shadowMaps.size() ? drawShadows_ : false;
    frustum = camera->WorldFrustum();
    viewMask = camera->ViewMask();

    // Clear results from last frame
    dirLight = nullptr;
    lastCamera = nullptr;
    rootLevelOctants.clear();
    opaqueBatches.Clear();
    alphaBatches.Clear();
    lights.clear();
    instanceTransforms.clear();
    
    minZ = M_MAX_FLOAT;
    maxZ = 0.0f;
    geometryBounds.Undefine();

    for (size_t i = 0; i < octantResults.size(); ++i)
        octantResults[i].Clear();
    for (size_t i = 0; i < batchResults.size(); ++i)
        batchResults[i].Clear();
    for (auto it = shadowMaps.begin(); it != shadowMaps.end(); ++it)
        it->Clear();

    // First process moved / animated objects' octree reinsertions
    octree->Update(frameNumber);

    // Enable threaded update during geometry / light gathering in case nodes' OnPrepareRender() causes further reinsertion queuing
    octree->SetThreadedUpdate(workQueue->NumThreads() > 1);

    // Find the starting points for octree traversal. Include the root if it contains nodes that didn't fit elsewhere
    Octant* rootOctant = octree->Root();
    if (rootOctant->drawables.size())
        rootLevelOctants.push_back(rootOctant);

    for (size_t i = 0; i < NUM_OCTANTS; ++i)
    {
        if (rootOctant->children[i])
            rootLevelOctants.push_back(rootOctant->children[i]);
    }

    // Keep track of both batch + octant task progress before main batches can be sorted (batch tasks will add to the counter when queued)
    numPendingBatchTasks.store((int)rootLevelOctants.size());
    numPendingShadowViews[0].store(0);
    numPendingShadowViews[1].store(0);

    // Find octants in view and their plane masks for node frustum culling. At the same time, find lights and process them
    // When octant collection tasks complete, they queue tasks for collecting batches from those octants.
    for (size_t i = 0; i < rootLevelOctants.size(); ++i)
    {
        collectOctantsTasks[i]->startOctant = rootLevelOctants[i];
        collectOctantsTasks[i]->subtreeIdx = i;
        processLightsTask->AddDependency(collectOctantsTasks[i]);
    }

    // Ensure shadow view processing doesn't happen before lights have been found and processed
    processShadowCastersTask->AddDependency(processLightsTask);

    workQueue->QueueTasks(rootLevelOctants.size(), reinterpret_cast<Task**>(&collectOctantsTasks[0]));

    // Execute tasks until can sort the main batches. Perform that in the main thread to potentially run faster
    while (numPendingBatchTasks.load() > 0)
        workQueue->TryComplete();

    SortMainBatches();

    // Finish remaining view preparation tasks (shadowcaster batches, light culling to frustum grid)
    workQueue->Complete();

    // No more threaded reinsertion will take place
    octree->SetThreadedUpdate(false);
}

void Renderer::RenderShadowMaps()
{
    ZoneScoped;

    // Unbind shadow textures before rendering to
    Texture::Unbind(TU_DIRLIGHTSHADOW);
    Texture::Unbind(TU_SHADOWATLAS);

    for (size_t i = 0; i < shadowMaps.size(); ++i)
    {
        ShadowMap& shadowMap = shadowMaps[i];
        if (shadowMap.shadowViews.empty())
            continue;

        UpdateInstanceTransforms(shadowMap.instanceTransforms);

        shadowMap.fbo->Bind();

        // First render static objects for those shadowmaps that need to store static objects. Do all of them to avoid FBO changes
        for (size_t j = 0; j < shadowMap.shadowViews.size(); ++j)
        {
            ShadowView* view = shadowMap.shadowViews[j];
            LightDrawable* light = view->light;

            if (view->renderMode == RENDER_STATIC_LIGHT_STORE_STATIC)
            {
                graphics->Clear(false, true, view->viewport);

                BatchQueue& batchQueue = shadowMap.shadowBatches[view->staticQueueIdx];
                if (batchQueue.HasBatches())
                {
                    graphics->SetViewport(view->viewport);
                    graphics->SetDepthBias(light->DepthBias() * depthBiasMul, light->SlopeScaleBias() * slopeScaleBiasMul);
                    RenderBatches(view->shadowCamera, batchQueue);
                }
            }
        }

        // Now do the shadowmap -> static shadowmap storage blits as necessary
        for (size_t j = 0; j < shadowMap.shadowViews.size(); ++j)
        {
            ShadowView* view = shadowMap.shadowViews[j];

            if (view->renderMode == RENDER_STATIC_LIGHT_STORE_STATIC)
                graphics->Blit(staticObjectShadowFbo, view->viewport, shadowMap.fbo, view->viewport, false, true, FILTER_POINT);
        }

        // Rebind shadowmap
        shadowMap.fbo->Bind();

        // First do all the clears or static shadowmap -> shadowmap blits
        for (size_t j = 0; j < shadowMap.shadowViews.size(); ++j)
        {
            ShadowView* view = shadowMap.shadowViews[j];

            if (view->renderMode == RENDER_DYNAMIC_LIGHT)
                graphics->Clear(false, true, view->viewport);
            else if (view->renderMode == RENDER_STATIC_LIGHT_RESTORE_STATIC)
                graphics->Blit(shadowMap.fbo, view->viewport, staticObjectShadowFbo, view->viewport, false, true, FILTER_POINT);
        }

        // Finally render the dynamic objects
        for (size_t j = 0; j < shadowMap.shadowViews.size(); ++j)
        {
            ShadowView* view = shadowMap.shadowViews[j];
            LightDrawable* light = view->light;

            if (view->renderMode != RENDER_STATIC_LIGHT_CACHED)
            {
                BatchQueue& batchQueue = shadowMap.shadowBatches[view->dynamicQueueIdx];
                if (batchQueue.HasBatches())
                {
                    graphics->SetViewport(view->viewport);
                    graphics->SetDepthBias(light->DepthBias() * depthBiasMul, light->SlopeScaleBias() * slopeScaleBiasMul);
                    RenderBatches(view->shadowCamera, batchQueue);
                }
            }
        }
    }

    graphics->SetDepthBias(0.0f, 0.0f);
}

void Renderer::RenderOpaque()
{
    ZoneScoped;

    // Update main batches' instance transforms & light data
    UpdateInstanceTransforms(instanceTransforms);
    ImageLevel clusterLevel(IntVector3(NUM_CLUSTER_X, NUM_CLUSTER_Y, NUM_CLUSTER_Z), FMT_RG32U, clusterData);
    clusterTexture->SetData(0, IntBox(0, 0, 0, NUM_CLUSTER_X, NUM_CLUSTER_Y, NUM_CLUSTER_Z), clusterLevel);
    lightDataBuffer->SetData(0, lights.size() * sizeof(LightData), lightData);

    if (shadowMaps.size())
    {
        shadowMaps[0].texture->Bind(TU_DIRLIGHTSHADOW);
        shadowMaps[1].texture->Bind(TU_SHADOWATLAS);
        faceSelectionTexture1->Bind(TU_FACESELECTION1);
        faceSelectionTexture2->Bind(TU_FACESELECTION2);
    }

    clusterTexture->Bind(TU_LIGHTCLUSTERDATA);
    lightDataBuffer->Bind(UB_LIGHTDATA);

    RenderBatches(camera, opaqueBatches);
}

void Renderer::RenderAlpha()
{
    ZoneScoped;

    if (shadowMaps.size())
    {
        shadowMaps[0].texture->Bind(TU_DIRLIGHTSHADOW);
        shadowMaps[1].texture->Bind(TU_SHADOWATLAS);
        faceSelectionTexture1->Bind(TU_FACESELECTION1);
        faceSelectionTexture2->Bind(TU_FACESELECTION2);
    }

    clusterTexture->Bind(TU_LIGHTCLUSTERDATA);
    lightDataBuffer->Bind(UB_LIGHTDATA);

    RenderBatches(camera, alphaBatches);
}

void Renderer::RenderDebug()
{
    ZoneScoped;

    DebugRenderer* debug = Subsystem<DebugRenderer>();
    if (!debug)
        return;

    for (auto it = lights.begin(); it != lights.end(); ++it)
        (*it)->OnRenderDebug(debug);

    for (auto it = octantResults.begin(); it != octantResults.end(); ++it)
    {
        for (auto oIt = it->octants.begin(); oIt != it->octants.end(); ++oIt)
        {
            Octant* octant = oIt->first;
            octant->OnRenderDebug(debug);

            for (auto dIt = octant->drawables.begin(); dIt != octant->drawables.end(); ++dIt)
            {
                Drawable* drawable = *dIt;
                if (drawable->TestFlag(DF_GEOMETRY) && drawable->LastFrameNumber() == frameNumber)
                    drawable->OnRenderDebug(debug);
            }
        }
    }
}

Texture* Renderer::ShadowMapTexture(size_t index) const
{
    return index < shadowMaps.size() ? shadowMaps[index].texture : nullptr;
}

void Renderer::CollectOctantsAndLights(Octant* octant, ThreadOctantResult& result, bool threaded, bool recursive, unsigned char planeMask)
{
    if (planeMask)
    {
        planeMask = frustum.IsInsideMasked(octant->cullingBox, planeMask);
        if (planeMask == 0xff)
            return;
    }

    for (auto it = octant->drawables.begin(); it != octant->drawables.end(); ++it)
    {
        Drawable* drawable = *it;

        if (drawable->TestFlag(DF_LIGHT))
        {
            if ((drawable->LayerMask() & viewMask) && (!planeMask || frustum.IsInsideMaskedFast(drawable->WorldBoundingBox(), planeMask)))
            {
                if (drawable->OnPrepareRender(frameNumber, camera))
                {
                    LightDrawable* light = static_cast<LightDrawable*>(drawable);
                    result.lights.push_back(light);
                }
            }
        }
        // Lights are sorted first in octants, so break when first geometry encountered. Store the octant for batch collecting
        else
        {
            result.octants.push_back(std::make_pair(octant, planeMask));
            result.drawableAcc += octant->drawables.end() - it;
            break;
        }
    }

    // Setup and queue batches collection task if over the drawable limit now. Note: if not threaded, defer to the end
    if (threaded && result.drawableAcc >= DRAWABLES_PER_BATCH_TASK)
    {
        if (result.collectBatchesTasks.size() <= result.batchTaskIdx)
            result.collectBatchesTasks.push_back(new CollectBatchesTask(this, &Renderer::CollectBatchesWork));

        CollectBatchesTask* batchTask = result.collectBatchesTasks[result.batchTaskIdx];
        batchTask->octants.clear();
        batchTask->octants.insert(batchTask->octants.end(), result.octants.begin() + result.taskOctantIdx, result.octants.end());
        processShadowCastersTask->AddDependency(batchTask);
        numPendingBatchTasks.fetch_add(1);
        workQueue->QueueTask(batchTask);

        result.drawableAcc = 0;
        result.taskOctantIdx = result.octants.size();
        ++result.batchTaskIdx;
    }

    if (recursive)
    {
        for (size_t i = 0; i < NUM_OCTANTS; ++i)
        {
            if (octant->children[i])
                CollectOctantsAndLights(octant->children[i], result, threaded, true, planeMask);
        }
    }
}

bool Renderer::AllocateShadowMap(LightDrawable* light)
{
    size_t index = light->GetLightType() == LIGHT_DIRECTIONAL ? 0 : 1;
    ShadowMap& shadowMap = shadowMaps[index];

    IntVector2 request = light->TotalShadowMapSize();

    // If light already has its preferred shadow rect from the previous frame, try to reallocate it for shadow map caching
    IntRect oldRect = light->ShadowRect();
    if (request.x == oldRect.Width() && request.y == oldRect.Height())
    {
        if (shadowMap.allocator.AllocateSpecific(oldRect))
        {
            light->SetShadowMap(shadowMaps[index].texture, light->ShadowRect());
            return true;
        }
    }

    IntRect shadowRect;
    size_t retries = 3;

    while (retries--)
    {
        int x, y;
        if (shadowMap.allocator.Allocate(request.x, request.y, x, y))
        {
            light->SetShadowMap(shadowMaps[index].texture, IntRect(x, y, x + request.x, y + request.y));
            return true;
        }

        request.x /= 2;
        request.y /= 2;
    }

    // No room in atlas
    light->SetShadowMap(nullptr);
    return false;
}

void Renderer::SortMainBatches()
{
    ZoneScoped;

    for (auto it = batchResults.begin(); it != batchResults.end(); ++it)
    {
        if (it->opaqueBatches.size())
            opaqueBatches.batches.insert(opaqueBatches.batches.end(), it->opaqueBatches.begin(), it->opaqueBatches.end());
        if (it->alphaBatches.size())
            alphaBatches.batches.insert(alphaBatches.batches.end(), it->alphaBatches.begin(), it->alphaBatches.end());
    }

    opaqueBatches.Sort(instanceTransforms, SORT_STATE_AND_DISTANCE, hasInstancing);
    alphaBatches.Sort(instanceTransforms, SORT_DISTANCE, hasInstancing);
}

void Renderer::SortShadowBatches(ShadowMap& shadowMap)
{
    ZoneScoped;

    for (size_t i = 0; i < shadowMap.shadowViews.size(); ++i)
    {
        ShadowView& view = *shadowMap.shadowViews[i];
        LightDrawable* light = view.light;

        // Check if view was discarded during shadowcaster collecting
        if (!light)
            continue;

        BatchQueue* destStatic = (view.renderMode == RENDER_STATIC_LIGHT_STORE_STATIC) ? &shadowMap.shadowBatches[view.staticQueueIdx] : nullptr;
        BatchQueue* destDynamic = &shadowMap.shadowBatches[view.dynamicQueueIdx];

        if (destStatic && destStatic->HasBatches())
            destStatic->Sort(shadowMap.instanceTransforms, SORT_STATE, hasInstancing);

        if (destDynamic->HasBatches())
            destDynamic->Sort(shadowMap.instanceTransforms, SORT_STATE, hasInstancing);
    }
}

void Renderer::UpdateInstanceTransforms(const std::vector<Matrix3x4>& transforms)
{
    if (hasInstancing && transforms.size())
    {
        if (instanceVertexBuffer->NumVertices() < transforms.size())
            instanceVertexBuffer->Define(USAGE_DYNAMIC, transforms.size(), instanceVertexElements, &transforms[0]);
        else
            instanceVertexBuffer->SetData(0, transforms.size(), &transforms[0]);
    }
}

void Renderer::RenderBatches(Camera* camera_, const BatchQueue& queue)
{
    ZoneScoped;

    lastMaterial = nullptr;
    lastPass = nullptr;

    if (camera_ != lastCamera)
    {
        perViewData.projectionMatrix = camera_->ProjectionMatrix();
        perViewData.viewMatrix = camera_->ViewMatrix();
        perViewData.viewProjMatrix = perViewData.projectionMatrix * perViewData.viewMatrix;
        perViewData.depthParameters = Vector4(camera_->NearClip(), camera_->FarClip(), camera_->IsOrthographic() ? 0.5f : 0.0f, camera_->IsOrthographic() ? 0.5f : 1.0f / camera_->FarClip());

        size_t dataSize = sizeof(Matrix3x4) + 2 * sizeof(Matrix4) + 5 * sizeof(Vector4);

        // Set the dir light parameters only in the main view
        if (!dirLight || camera_ != camera)
        {
            perViewData.dirLightData[0] = Vector4::ZERO;
            perViewData.dirLightData[1] = Vector4::ZERO;
            perViewData.dirLightData[3] = Vector4::ONE;
        }
        else
        {
            perViewData.dirLightData[0] = Vector4(-dirLight->WorldDirection(), 0.0f);
            perViewData.dirLightData[1] = dirLight->GetColor().Data();

            if (dirLight->ShadowMap())
            {
                Vector2 cascadeSplits = dirLight->ShadowCascadeSplits();
                float farClip = camera->FarClip();
                float firstSplit = cascadeSplits.x / farClip;
                float secondSplit = cascadeSplits.y / farClip;

                perViewData.dirLightData[2] = Vector4(firstSplit, secondSplit, dirLight->ShadowFadeStart() * secondSplit, 1.0f / (secondSplit - dirLight->ShadowFadeStart() * secondSplit));
                perViewData.dirLightData[3] = dirLight->ShadowParameters();
                if (dirLight->ShadowViews().size() >= 2)
                {
                    *reinterpret_cast<Matrix4*>(&perViewData.dirLightData[4]) = dirLight->ShadowViews()[0].shadowMatrix;
                    *reinterpret_cast<Matrix4*>(&perViewData.dirLightData[8]) = dirLight->ShadowViews()[1].shadowMatrix;
                    dataSize += 8 * sizeof(Vector4);
                }
            }
            else
                perViewData.dirLightData[3] = Vector4::ONE;
        }

        perViewDataBuffer->SetData(0, dataSize, &perViewData);

        lastCamera = camera_;
    }

    perViewDataBuffer->Bind(UB_PERVIEWDATA);

    for (auto it = queue.batches.begin(); it != queue.batches.end(); ++it)
    {
        const Batch& batch = *it;
        unsigned char geometryBits = batch.programBits & SP_GEOMETRYBITS;

        ShaderProgram* program = batch.pass->GetShaderProgram(batch.programBits);
        if (!program->Bind())
            continue;

        Material* material = batch.pass->Parent();
        if (batch.pass != lastPass)
        {
            if (material != lastMaterial)
            {
                for (size_t i = 0; i < MAX_MATERIAL_TEXTURE_UNITS; ++i)
                {
                    Texture* texture = material->GetTexture(i);
                    if (texture)
                        texture->Bind(i);
                }

                lastMaterial = material;
                ++lastPerMaterialUniforms;
                if (!lastPerMaterialUniforms)
                    ++lastPerMaterialUniforms;
            }

            CullMode cullMode = material->GetCullMode();
            if (camera_->UseReverseCulling())
            {
                if (cullMode == CULL_BACK)
                    cullMode = CULL_FRONT;
                else if (cullMode == CULL_FRONT)
                    cullMode = CULL_BACK;
            }

            graphics->SetRenderState(batch.pass->GetBlendMode(), cullMode, batch.pass->GetDepthTest(), batch.pass->GetColorWrite(), batch.pass->GetDepthWrite());

            lastPass = batch.pass;
        }

        if (program->lastPerMaterialUniforms != lastPerMaterialUniforms)
        {
            const std::map<PresetUniform, Vector4>& uniformValues = material->UniformValues();
            for (auto uIt = uniformValues.begin(); uIt != uniformValues.end(); ++uIt)
                graphics->SetUniform(program, uIt->first, uIt->second);

            program->lastPerMaterialUniforms = lastPerMaterialUniforms;
        }

        Geometry* geometry = batch.geometry;
        VertexBuffer* vb = geometry->vertexBuffer;
        IndexBuffer* ib = geometry->indexBuffer;
        vb->Bind(program->Attributes());
        if (ib)
            ib->Bind();

        if (geometryBits == GEOM_INSTANCED)
        {
            if (ib)
                graphics->DrawIndexedInstanced(PT_TRIANGLE_LIST, geometry->drawStart, geometry->drawCount, instanceVertexBuffer, batch.instanceStart, batch.instanceCount);
            else
                graphics->DrawInstanced(PT_TRIANGLE_LIST, geometry->drawStart, geometry->drawCount, instanceVertexBuffer, batch.instanceStart, batch.instanceCount);

            it += batch.instanceCount - 1;
        }
        else
        {
            if (!geometryBits)
                graphics->SetUniform(program, U_WORLDMATRIX, *batch.worldTransform);
            else
                batch.drawable->OnRender(program, batch.geomIndex);

            if (ib)
                graphics->DrawIndexed(PT_TRIANGLE_LIST, geometry->drawStart, geometry->drawCount);
            else
                graphics->Draw(PT_TRIANGLE_LIST, geometry->drawStart, geometry->drawCount);
        }
    }
}

void Renderer::DefineFaceSelectionTextures()
{
    if (faceSelectionTexture1 && faceSelectionTexture2)
        return;

    faceSelectionTexture1 = new Texture();
    faceSelectionTexture2 = new Texture();

    const float faceSelectionData1[] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    const float faceSelectionData2[] = {
        -0.5f, 0.5f, 0.5f, 1.5f,
        0.5f, 0.5f, 0.5f, 0.5f,
        -0.5f, 0.5f, 1.5f, 1.5f,
        -0.5f, -0.5f, 1.5f, 0.5f,
        0.5f, 0.5f, 2.5f, 1.5f,
        -0.5f, 0.5f, 2.5f, 0.5f
    };

    std::vector<ImageLevel> faces1;
    std::vector<ImageLevel> faces2;

    for (size_t i = 0; i < MAX_CUBE_FACES; ++i)
    {
        faces1.push_back(ImageLevel(IntVector2(1, 1), FMT_RGBA32F, &faceSelectionData1[4 * i]));
        faces2.push_back(ImageLevel(IntVector2(1, 1), FMT_RGBA32F, &faceSelectionData2[4 * i]));
    }

    faceSelectionTexture1->Define(TEX_CUBE, IntVector3(1, 1, MAX_CUBE_FACES), FMT_RGBA32F, 1, 1, &faces1[0]);
    faceSelectionTexture1->DefineSampler(FILTER_POINT, ADDRESS_CLAMP, ADDRESS_CLAMP, ADDRESS_CLAMP);

    faceSelectionTexture2->Define(TEX_CUBE, IntVector3(1, 1, MAX_CUBE_FACES), FMT_RGBA32F, 1, 1, &faces2[0]);
    faceSelectionTexture2->DefineSampler(FILTER_POINT, ADDRESS_CLAMP, ADDRESS_CLAMP, ADDRESS_CLAMP);
}

void Renderer::DefineClusterFrustums()
{
    Matrix4 cameraProj = camera->ProjectionMatrix(false);
    if (lastClusterFrustumProj != cameraProj)
        clusterFrustumsDirty = true;

    if (clusterFrustumsDirty)
    {
        ZoneScoped;

        Matrix4 cameraProjInverse = cameraProj.Inverse();
        float cameraNearClip = camera->NearClip();
        float cameraFarClip = camera->FarClip();
        size_t idx = 0;

        float xStep = 2.0f / NUM_CLUSTER_X;
        float yStep = 2.0f / NUM_CLUSTER_Y;
        float zStep = 1.0f / NUM_CLUSTER_Z;

        for (size_t z = 0; z < NUM_CLUSTER_Z; ++z)
        {
            Vector4 nearVec = cameraProj * Vector4(0.0f, 0.0f, z > 0 ? powf(z * zStep, 2.0f) * cameraFarClip : cameraNearClip, 1.0f);
            Vector4 farVec = cameraProj * Vector4(0.0f, 0.0f, powf((z + 1) * zStep, 2.0f) * cameraFarClip, 1.0f);
            float near = nearVec.z / nearVec.w;
            float far = farVec.z / farVec.w;

            for (size_t y = 0; y < NUM_CLUSTER_Y; ++y)
            {
                for (size_t x = 0; x < NUM_CLUSTER_X; ++x)
                {
                    clusterFrustums[idx].vertices[0] = cameraProjInverse * Vector3(-1.0f + xStep * (x + 1), 1.0f - yStep * y, near);
                    clusterFrustums[idx].vertices[1] = cameraProjInverse * Vector3(-1.0f + xStep * (x + 1), 1.0f - yStep * (y + 1), near);
                    clusterFrustums[idx].vertices[2] = cameraProjInverse * Vector3(-1.0f + xStep * x, 1.0f - yStep * (y + 1), near);
                    clusterFrustums[idx].vertices[3] = cameraProjInverse * Vector3(-1.0f + xStep * x, 1.0f - yStep * y, near);
                    clusterFrustums[idx].vertices[4] = cameraProjInverse * Vector3(-1.0f + xStep * (x + 1), 1.0f - yStep * y, far);
                    clusterFrustums[idx].vertices[5] = cameraProjInverse * Vector3(-1.0f + xStep * (x + 1), 1.0f - yStep * (y + 1), far);
                    clusterFrustums[idx].vertices[6] = cameraProjInverse * Vector3(-1.0f + xStep * x, 1.0f - yStep * (y + 1), far);
                    clusterFrustums[idx].vertices[7] = cameraProjInverse * Vector3(-1.0f + xStep * x, 1.0f - yStep * y, far);
                    clusterFrustums[idx].UpdatePlanes();
                    clusterBoundingBoxes[idx].Define(clusterFrustums[idx]);
                    ++idx;
                }
            }
        }

        lastClusterFrustumProj = cameraProj;
        clusterFrustumsDirty = false;
    }
}

void Renderer::CollectOctantsWork(Task* task_, unsigned)
{
    ZoneScoped;

    CollectOctantsTask* task = static_cast<CollectOctantsTask*>(task_);

    // Go through octants in this task's octree branch
    Octant* octant = task->startOctant;
    ThreadOctantResult& result = octantResults[task->subtreeIdx];
    
    CollectOctantsAndLights(octant, result, workQueue->NumThreads() > 1, octant != octree->Root());

    // Queue final batch task for leftover nodes if needed
    if (result.drawableAcc)
    {
        if (result.collectBatchesTasks.size() <= result.batchTaskIdx)
            result.collectBatchesTasks.push_back(new CollectBatchesTask(this, &Renderer::CollectBatchesWork));
            
        CollectBatchesTask* batchTask = result.collectBatchesTasks[result.batchTaskIdx];
        batchTask->octants.clear();
        batchTask->octants.insert(batchTask->octants.end(), result.octants.begin() + result.taskOctantIdx, result.octants.end());
        processShadowCastersTask->AddDependency(batchTask);
        numPendingBatchTasks.fetch_add(1);
        workQueue->QueueTask(batchTask);
    }

    numPendingBatchTasks.fetch_add(-1);
}

void Renderer::ProcessLightsWork(Task*, unsigned)
{
    ZoneScoped;

    // Merge the light collection results
    for (size_t i = 0; i < rootLevelOctants.size(); ++i)
        lights.insert(lights.end(), octantResults[i].lights.begin(), octantResults[i].lights.end());

    // Find the directional light if any
    for (auto it = lights.begin(); it != lights.end(); )
    {
        LightDrawable* light = *it;
        if (light->GetLightType() == LIGHT_DIRECTIONAL)
        {
            if (!dirLight || light->GetColor().Average() > dirLight->GetColor().Average())
                dirLight = light;
            it = lights.erase(it);
        }
        else
            ++it;
    }

    // Sort localized lights by increasing distance
    std::sort(lights.begin(), lights.end(), CompareLights);

    // Clamp to maximum supported
    if (lights.size() > MAX_LIGHTS)
        lights.resize(MAX_LIGHTS);

    // Pre-step for shadow map caching: reallocate all lights' shadow map rectangles which are non-zero at this point.
    // If shadow maps were dirtied (size or bias change) reset all allocations instead
    for (auto it = lights.begin(); it != lights.end(); ++it)
    {
        LightDrawable* light = *it;
        if (shadowMapsDirty)
            light->SetShadowMap(nullptr);
        else if (drawShadows && light->ShadowStrength() < 1.0f && light->ShadowRect() != IntRect::ZERO)
            AllocateShadowMap(light);
    }

    // Check if directional light needs shadows
    if (dirLight)
    {
        if (shadowMapsDirty)
            dirLight->SetShadowMap(nullptr);

        if (!drawShadows || dirLight->ShadowStrength() >= 1.0f || !AllocateShadowMap(dirLight))
            dirLight->SetShadowMap(nullptr);
    }

    shadowMapsDirty = false;

    size_t lightTaskIdx = 0;

    // Go through lights and setup shadowcaster collection tasks
    for (size_t i = 0; i < lights.size(); ++i)
    {
        ShadowMap& shadowMap = shadowMaps[1];

        LightDrawable* light = lights[i];
        float cutoff = light->GetLightType() == LIGHT_SPOT ? cosf(light->Fov() * 0.5f * M_DEGTORAD) : 0.0f;

        lightData[i].position = Vector4(light->WorldPosition(), 1.0f);
        lightData[i].direction = Vector4(-light->WorldDirection(), 0.0f);
        lightData[i].attenuation = Vector4(1.0f / Max(light->Range(), M_EPSILON), cutoff, 1.0f / (1.0f - cutoff), 1.0f);
        lightData[i].color = light->EffectiveColor();
        lightData[i].shadowParameters = Vector4::ONE; // Assume unshadowed

        // Check if not shadowcasting or beyond shadow range
        if (!drawShadows || light->ShadowStrength() >= 1.0f)
        {
            light->SetShadowMap(nullptr);
            continue;
        }

        // Now retry shadow map allocation if necessary. If it's a new allocation, must rerender the shadow map
        if (!light->ShadowMap())
        {
            if (!AllocateShadowMap(light))
                continue;
        }

        light->InitShadowViews();
        std::vector<ShadowView>& shadowViews = light->ShadowViews();

        // Preallocate shadowcaster list
        size_t casterListIdx = shadowMap.freeCasterListIdx++;
        if (shadowMap.shadowCasters.size() < shadowMap.freeCasterListIdx)
            shadowMap.shadowCasters.resize(shadowMap.freeCasterListIdx);

        for (size_t j = 0; j < shadowViews.size(); ++j)
        {
            ShadowView& view = shadowViews[j];

            // Preallocate shadow batch queues
            view.casterListIdx = casterListIdx;

            if (light->IsStatic())
            {
                view.staticQueueIdx = shadowMap.freeQueueIdx++;
                view.dynamicQueueIdx = shadowMap.freeQueueIdx++;
            }
            else
                view.dynamicQueueIdx = shadowMap.freeQueueIdx++;

            if (shadowMap.shadowBatches.size() < shadowMap.freeQueueIdx)
                shadowMap.shadowBatches.resize(shadowMap.freeQueueIdx);

            shadowMap.shadowViews.push_back(&view);
        }

        if (collectShadowCastersTasks.size() <= lightTaskIdx)
            collectShadowCastersTasks.push_back(new CollectShadowCastersTask(this, &Renderer::CollectShadowCastersWork));

        collectShadowCastersTasks[lightTaskIdx]->light = light;
        processShadowCastersTask->AddDependency(collectShadowCastersTasks[lightTaskIdx]);
        ++lightTaskIdx;
    }

    if (dirLight && dirLight->ShadowMap())
    {
        ShadowMap& shadowMap = shadowMaps[0];

        dirLight->InitShadowViews();
        std::vector<ShadowView>& shadowViews = dirLight->ShadowViews();

        for (size_t i = 0; i < shadowViews.size(); ++i)
        {
            ShadowView& view = shadowViews[i];

            // Directional light needs a new frustum query for each split, as the shadow cameras are typically far outside the main view
            // But queries are only performed later when the shadow map can be focused to visible scene
            view.casterListIdx = shadowMap.freeCasterListIdx++;
            if (shadowMap.shadowCasters.size() < shadowMap.freeCasterListIdx)
                shadowMap.shadowCasters.resize(shadowMap.freeCasterListIdx);

            view.dynamicQueueIdx = shadowMap.freeQueueIdx++;
            if (shadowMap.shadowBatches.size() < shadowMap.freeQueueIdx)
                shadowMap.shadowBatches.resize(shadowMap.freeQueueIdx);

            shadowMap.shadowViews.push_back(&view);
        }
    }

    // Now queue all shadowcaster collection tasks
    if (lightTaskIdx > 0)
        workQueue->QueueTasks(lightTaskIdx, reinterpret_cast<Task**>(&collectShadowCastersTasks[0]));
}

void Renderer::CollectBatchesWork(Task* task_, unsigned threadIndex)
{
    ZoneScoped;

    CollectBatchesTask* task = static_cast<CollectBatchesTask*>(task_);
    ThreadBatchResult& result = batchResults[threadIndex];
    bool threaded = workQueue->NumThreads() > 1;

    std::vector<std::pair<Octant*, unsigned char> >& octants = task->octants;
    std::vector<Batch>& opaqueQueue = threaded ? result.opaqueBatches : opaqueBatches.batches;
    std::vector<Batch>& alphaQueue = threaded ? result.alphaBatches : alphaBatches.batches;

    const Matrix3x4& viewMatrix = camera->ViewMatrix();
    Vector3 viewZ = Vector3(viewMatrix.m20, viewMatrix.m21, viewMatrix.m22);
    Vector3 absViewZ = viewZ.Abs();
    float farClipMul = 32767.0f / camera->FarClip();

    // Scan octants for geometries
    for (auto it = octants.begin(); it != octants.end(); ++it)
    {
        Octant* octant = it->first;
        unsigned char planeMask = it->second;
        std::vector<Drawable*>& drawables = octant->drawables;

        for (auto dIt = drawables.begin(); dIt != drawables.end(); ++dIt)
        {
            Drawable* drawable = *dIt;

            if (drawable->TestFlag(DF_GEOMETRY) && (drawable->LayerMask() & viewMask) && (!planeMask || frustum.IsInsideMaskedFast(drawable->WorldBoundingBox(), planeMask)))
            {
                if (drawable->OnPrepareRender(frameNumber, camera))
                {
                    const BoundingBox& geometryBox = drawable->WorldBoundingBox();
                    result.geometryBounds.Merge(geometryBox);

                    Vector3 center = geometryBox.Center();
                    Vector3 edge = geometryBox.Size() * 0.5f;

                    float viewCenterZ = viewZ.DotProduct(center) + viewMatrix.m23;
                    float viewEdgeZ = absViewZ.DotProduct(edge);
                    result.minZ = Min(result.minZ, viewCenterZ - viewEdgeZ);
                    result.maxZ = Max(result.maxZ, viewCenterZ + viewEdgeZ);
 
                    Batch newBatch;

                    unsigned short distance = (unsigned short)(drawable->Distance() * farClipMul);
                    const SourceBatches& batches = static_cast<GeometryDrawable*>(drawable)->batches;
                    size_t numGeometries = batches.NumGeometries();
        
                    for (size_t j = 0; j < numGeometries; ++j)
                    {
                        Material* material = batches.GetMaterial(j);

                        // Assume opaque first
                        newBatch.pass = material->GetPass(PASS_OPAQUE);
                        newBatch.geometry = batches.GetGeometry(j);
                        newBatch.programBits = (unsigned char)(drawable->Flags() & DF_GEOMETRY_TYPE_BITS);
                        newBatch.geomIndex = (unsigned char)j;

                        if (!newBatch.programBits)
                            newBatch.worldTransform = &drawable->WorldTransform();
                        else
                            newBatch.drawable = static_cast<GeometryDrawable*>(drawable);

                        if (newBatch.pass)
                        {
                            // Perform distance sort in addition to state sort
                            if (newBatch.pass->lastSortKey.first != frameNumber || newBatch.pass->lastSortKey.second > distance)
                            {
                                newBatch.pass->lastSortKey.first = frameNumber;
                                newBatch.pass->lastSortKey.second = distance;
                            }
                            if (newBatch.geometry->lastSortKey.first != frameNumber || newBatch.geometry->lastSortKey.second > distance + (unsigned short)j)
                            {
                                newBatch.geometry->lastSortKey.first = frameNumber;
                                newBatch.geometry->lastSortKey.second = distance + (unsigned short)j;
                            }

                            opaqueQueue.push_back(newBatch);
                        }
                        else
                        {
                            // If not opaque, try transparent
                            newBatch.pass = material->GetPass(PASS_ALPHA);
                            if (!newBatch.pass)
                                continue;

                            newBatch.distance = drawable->Distance();
                            alphaQueue.push_back(newBatch);
                        }
                    }
                }
            }
        }
    }

    numPendingBatchTasks.fetch_add(-1);
}

void Renderer::CollectShadowCastersWork(Task* task, unsigned)
{
    ZoneScoped;

    LightDrawable* light = static_cast<CollectShadowCastersTask*>(task)->light;
    LightType lightType = light->GetLightType();
    std::vector<ShadowView>& shadowViews = light->ShadowViews();

    // Directional lights perform queries later, here only point & spot lights (in shadow atlas) are considered
    ShadowMap& shadowMap = shadowMaps[1];

    if (lightType == LIGHT_POINT)
    {
        // Point light: perform only one sphere query, then check which of the point light sides are visible
        for (size_t i = 0; i < shadowViews.size(); ++i)
        {
            // Check if each of the sides is in view. Do not process if isn't. Rendering will be no-op this frame, but cached contents are discarded once comes into view again
            light->SetupShadowView(i, camera);
            ShadowView& view = shadowViews[i];

            if (!frustum.IsInsideFast(BoundingBox(view.shadowFrustum)))
            {
                view.renderMode = RENDER_STATIC_LIGHT_CACHED;
                view.viewport = IntRect::ZERO;
                view.lastViewport = IntRect::ZERO;
            }
        }

        std::vector<Drawable*>& shadowCasters = shadowMap.shadowCasters[shadowViews[0].casterListIdx];
        octree->FindDrawables(shadowCasters, light->WorldSphere(), DF_GEOMETRY | DF_CAST_SHADOWS);
    }
    else if (lightType == LIGHT_SPOT)
    {
        // Spot light: perform query for the spot frustum
        light->SetupShadowView(0, camera);
        ShadowView& view = shadowViews[0];

        std::vector<Drawable*>& shadowCasters = shadowMap.shadowCasters[view.casterListIdx];
        octree->FindDrawablesMasked(shadowCasters, view.shadowFrustum, DF_GEOMETRY | DF_CAST_SHADOWS);
    }
}

void Renderer::ProcessShadowCastersWork(Task*, unsigned)
{
    ZoneScoped;

    // Shadow batches collection needs accurate scene min / max Z results, combine them from per-thread data
    for (auto it = batchResults.begin(); it != batchResults.end(); ++it)
    {
        minZ = Min(minZ, it->minZ);
        maxZ = Max(maxZ, it->maxZ);
        if (it->geometryBounds.IsDefined())
            geometryBounds.Merge(it->geometryBounds);
    }

    minZ = Max(minZ, camera->NearClip());

    // Queue shadow batch collection tasks. These will also perform shadow batch sorting tasks when done
    size_t shadowTaskIdx = 0;
    LightDrawable* lastLight = nullptr;

    for (size_t i = 0; i < shadowMaps.size(); ++i)
    {
        ShadowMap& shadowMap = shadowMaps[i];
        for (size_t j = 0; j < shadowMap.shadowViews.size(); ++j)
        {
            LightDrawable* light = shadowMap.shadowViews[j]->light;
            // For a point light, make only one task that will handle all of the views and skip rest
            if (light->GetLightType() == LIGHT_POINT && light == lastLight)
                continue;

            lastLight = light;

            if (collectShadowBatchesTasks.size() <= shadowTaskIdx)
                collectShadowBatchesTasks.push_back(new CollectShadowBatchesTask(this, &Renderer::CollectShadowBatchesWork));
            collectShadowBatchesTasks[shadowTaskIdx]->shadowMapIdx = i;
            collectShadowBatchesTasks[shadowTaskIdx]->viewIdx = j;
            numPendingShadowViews[i].fetch_add(1);
            ++shadowTaskIdx;
        }
    }

    if (shadowTaskIdx > 0)
        workQueue->QueueTasks(shadowTaskIdx, reinterpret_cast<Task**>(&collectShadowBatchesTasks[0]));

    // Clear per-cluster light data from previous frame, update cluster frustums and bounding boxes if camera changed, then queue light culling tasks for the needed scene range
    DefineClusterFrustums();
    memset(numClusterLights, 0, sizeof numClusterLights);
    memset(clusterData, 0, MAX_LIGHTS_CLUSTER * NUM_CLUSTER_X * NUM_CLUSTER_Y * NUM_CLUSTER_Z);
    for (size_t z = 0; z < NUM_CLUSTER_Z; ++z)
    {
        size_t idx = z * NUM_CLUSTER_X * NUM_CLUSTER_Y;
        if (minZ > clusterFrustums[idx].vertices[4].z || maxZ < clusterFrustums[idx].vertices[0].z)
            continue;
        workQueue->QueueTask(cullLightsTasks[z]);
    }

    // Finally copy correct shadow matrices for the localized light data
    // Note: directional light shadow matrices may still be pending, but they are not included here
    for (size_t i = 0; i < lights.size(); ++i)
    {
        LightDrawable* light = lights[i];

        if (light->ShadowMap())
        {
            lightData[i].shadowParameters = light->ShadowParameters();
            lightData[i].shadowMatrix = light->ShadowViews()[0].shadowMatrix;
        }
    }
}

void Renderer::CollectShadowBatchesWork(Task* task_, unsigned)
{
    ZoneScoped;

    CollectShadowBatchesTask* task = static_cast<CollectShadowBatchesTask*>(task_);
    ShadowMap& shadowMap = shadowMaps[task->shadowMapIdx];
    size_t viewIdx = task->viewIdx;

    for (;;)
    {
        ShadowView& view = *shadowMap.shadowViews[viewIdx];

        LightDrawable* light = view.light;
        LightType lightType = light->GetLightType();

        float splitMinZ = minZ, splitMaxZ = maxZ;

        // Focus directional light shadow camera to the visible geometry combined bounds, and query for shadowcasters late
        if (lightType == LIGHT_DIRECTIONAL)
        {
            if (!light->SetupShadowView(viewIdx, camera, &geometryBounds))
                view.viewport = IntRect::ZERO;
            else
            {
                splitMinZ = Max(splitMinZ, view.splitMinZ);
                splitMaxZ = Min(splitMaxZ, view.splitMaxZ);

                // Before querying (which is potentially expensive), check for degenerate depth range or frustum outside split
                if (splitMinZ >= splitMaxZ || splitMinZ > view.splitMaxZ || splitMaxZ < view.splitMinZ)
                    view.viewport = IntRect::ZERO;
                else
                    octree->FindDrawablesMasked(shadowMap.shadowCasters[view.casterListIdx], view.shadowFrustum, DF_GEOMETRY | DF_CAST_SHADOWS);
            }
        }

        // Skip view? (no geometry, out of range or point light face not in view)
        if (view.viewport == IntRect::ZERO)
        {
            view.renderMode = RENDER_STATIC_LIGHT_CACHED;
            view.lastViewport = IntRect::ZERO;
        }
        else
        {
            const Frustum& shadowFrustum = view.shadowFrustum;
            const Matrix3x4& lightView = view.shadowCamera->ViewMatrix();
            const std::vector<Drawable*>& initialShadowCasters = shadowMap.shadowCasters[view.casterListIdx];

            bool dynamicOrDirLight = lightType == LIGHT_DIRECTIONAL || !light->IsStatic();
            bool dynamicCastersMoved = false;
            bool staticCastersMoved = false;

            size_t totalShadowCasters = 0;
            size_t staticShadowCasters = 0;

            Frustum lightViewFrustum = camera->WorldSplitFrustum(splitMinZ, splitMaxZ).Transformed(lightView);
            BoundingBox lightViewFrustumBox(lightViewFrustum);

            BatchQueue* destStatic = !dynamicOrDirLight ? &shadowMap.shadowBatches[view.staticQueueIdx] : nullptr;
            BatchQueue* destDynamic = &shadowMap.shadowBatches[view.dynamicQueueIdx];

            for (auto it = initialShadowCasters.begin(); it != initialShadowCasters.end(); ++it)
            {
                Drawable* drawable = *it;
                const BoundingBox& geometryBox = drawable->WorldBoundingBox();

                bool inView = drawable->InView(frameNumber);
                bool staticNode = drawable->IsStatic();

                // Check shadowcaster frustum visibility for point lights; may be visible in view, but not in each cube map face
                if (lightType == LIGHT_POINT && !shadowFrustum.IsInsideFast(geometryBox))
                    continue;

                // Furthermore, check by bounding box extrusion if out-of-view or directional light shadowcaster actually contributes to visible geometry shadowing or if it can be skipped
                // This is done only for dynamic objects or dynamic lights' shadows; cached static shadowmap needs to render everything
                if ((!staticNode || dynamicOrDirLight) && !inView)
                {
                    BoundingBox lightViewBox = geometryBox.Transformed(lightView);

                    if (lightType == LIGHT_DIRECTIONAL)
                    {
                        lightViewBox.max.z = Max(lightViewBox.max.z, lightViewFrustumBox.max.z);
                        if (!lightViewFrustum.IsInsideFast(lightViewBox))
                            continue;
                    }
                    else
                    {
                        // For perspective lights, extrusion direction depends on the position of the shadow caster
                        Vector3 center = lightViewBox.Center();
                        Ray extrusionRay(center, center);

                        float extrusionDistance = view.shadowCamera->FarClip();
                        float originalDistance = Clamp(center.Length(), M_EPSILON, extrusionDistance);

                        // Because of the perspective, the bounding box must also grow when it is extruded to the distance
                        float sizeFactor = extrusionDistance / originalDistance;

                        // Calculate the endpoint box and merge it to the original. Because it's axis-aligned, it will be larger
                        // than necessary, so the test will be conservative
                        Vector3 newCenter = extrusionDistance * extrusionRay.direction;
                        Vector3 newHalfSize = lightViewBox.Size() * sizeFactor * 0.5f;
                        BoundingBox extrudedBox(newCenter - newHalfSize, newCenter + newHalfSize);
                        lightViewBox.Merge(extrudedBox);

                        if (!lightViewFrustum.IsInsideFast(lightViewBox))
                            continue;
                    }
                }

                // If not in view, let the node prepare itself for render now
                if (!inView)
                {
                    if (!drawable->OnPrepareRender(frameNumber, camera))
                        continue;
                }

                ++totalShadowCasters;

                if (staticNode)
                {
                    ++staticShadowCasters;
                    if (drawable->LastUpdateFrameNumber() == frameNumber)
                        staticCastersMoved = true;
                }
                else
                {
                    if (drawable->LastUpdateFrameNumber() == frameNumber)
                        dynamicCastersMoved = true;
                }

                // If did not allocate a static queue, just put everything to dynamic
                BatchQueue& dest = destStatic ? (staticNode ? *destStatic : *destDynamic) : *destDynamic;
                const SourceBatches& batches = static_cast<GeometryDrawable*>(drawable)->batches;
                size_t numGeometries = batches.NumGeometries();

                Batch newBatch;

                for (size_t j = 0; j < numGeometries; ++j)
                {
                    Material* material = batches.GetMaterial(j);
                    newBatch.pass = material->GetPass(PASS_SHADOW);
                    if (!newBatch.pass)
                        continue;

                    newBatch.geometry = batches.GetGeometry(j);
                    newBatch.programBits = (unsigned char)(drawable->Flags() & DF_GEOMETRY_TYPE_BITS);
                    newBatch.geomIndex = (unsigned char)j;

                    if (!newBatch.programBits)
                        newBatch.worldTransform = &drawable->WorldTransform();
                    else
                        newBatch.drawable = static_cast<GeometryDrawable*>(drawable);

                    dest.batches.push_back(newBatch);
                }
            }

            // Now determine which kind of caching can be used for the shadow map
            // Dynamic or directional lights
            if (dynamicOrDirLight)
            {
                // If light atlas allocation changed, light moved, or amount of objects in view changed, render an optimized shadow map
                if (view.lastViewport != view.viewport || !view.lastShadowMatrix.Equals(view.shadowMatrix, 0.0001f) || view.lastNumGeometries != totalShadowCasters || dynamicCastersMoved || staticCastersMoved)
                    view.renderMode = RENDER_DYNAMIC_LIGHT;
                else
                    view.renderMode = RENDER_STATIC_LIGHT_CACHED;
            }
            // Static lights
            else
            {
                // If light atlas allocation has changed, or the static light changed, render a full shadow map now that can be cached next frame
                if (view.lastViewport != view.viewport || !view.lastShadowMatrix.Equals(view.shadowMatrix, 0.0001f))
                    view.renderMode = RENDER_STATIC_LIGHT_STORE_STATIC;
                else
                {
                    view.renderMode = RENDER_STATIC_LIGHT_CACHED;

                    // If static shadowcasters updated themselves (e.g. LOD change), render shadow map fully
                    // If dynamic casters moved, need to restore shadowmap and rerender
                    if (staticCastersMoved)
                        view.renderMode = RENDER_STATIC_LIGHT_STORE_STATIC;
                    else
                    {
                        if (dynamicCastersMoved || view.lastNumGeometries != totalShadowCasters)
                            view.renderMode = staticShadowCasters > 0 ? RENDER_STATIC_LIGHT_RESTORE_STATIC : RENDER_DYNAMIC_LIGHT;
                    }
                }
            }

            // If no rendering to be done, use the last rendered shadow projection matrix to avoid artifacts when rotating camera
            if (view.renderMode == RENDER_STATIC_LIGHT_CACHED)
                view.shadowMatrix = view.lastShadowMatrix;
            else
            {
                view.lastViewport = view.viewport;
                view.lastNumGeometries = totalShadowCasters;
                view.lastShadowMatrix = view.shadowMatrix;

                // Clear static batch queue if not needed
                if (destStatic && view.renderMode != RENDER_STATIC_LIGHT_STORE_STATIC)
                    destStatic->Clear();
            }
        }

        // For a point light, process all its views in the same task
        if (lightType == LIGHT_POINT && viewIdx < shadowMap.shadowViews.size() - 1 && shadowMap.shadowViews[viewIdx + 1]->light == light)
            ++viewIdx;
        else
            break;
    }

    // Sort shadow batches if was the last
    if (numPendingShadowViews[task->shadowMapIdx].fetch_add(-1) == 1)
        SortShadowBatches(shadowMap);
}

void Renderer::CullLightsToFrustumWork(Task* task, unsigned)
{
    ZoneScoped;

    // Cull lights against each cluster frustum on the given Z-level
    size_t z = static_cast<CullLightsTask*>(task)->z;
    const Matrix3x4& cameraView = camera->ViewMatrix();

    for (size_t i = 0; i < lights.size(); ++i)
    {
        LightDrawable* light = lights[i];
        LightType lightType = light->GetLightType();

        if (lightType == LIGHT_POINT)
        {
            Sphere bounds(cameraView * light->WorldPosition(), light->Range());
            float minViewZ = bounds.center.z - light->Range();
            float maxViewZ = bounds.center.z + light->Range();

            size_t idx = z * NUM_CLUSTER_X * NUM_CLUSTER_Y;
            if (minViewZ > clusterFrustums[idx].vertices[4].z || maxViewZ < clusterFrustums[idx].vertices[0].z || numClusterLights[idx] >= MAX_LIGHTS_CLUSTER)
                continue;

            for (size_t y = 0; y < NUM_CLUSTER_Y; ++y)
            {
                for (size_t x = 0; x < NUM_CLUSTER_X; ++x)
                {
                    if (bounds.IsInsideFast(clusterBoundingBoxes[idx]) && clusterFrustums[idx].IsInsideFast(bounds))
                        clusterData[(idx << 4) + numClusterLights[idx]++] = (unsigned char)(i + 1);

                    ++idx;
                }
            }
        }
        else if (lightType == LIGHT_SPOT)
        {
            Frustum bounds(light->WorldFrustum().Transformed(cameraView));
            BoundingBox boundsBox(bounds);
            float minViewZ = boundsBox.min.z;
            float maxViewZ = boundsBox.max.z;

            size_t idx = z * NUM_CLUSTER_X * NUM_CLUSTER_Y;
            if (minViewZ > clusterFrustums[idx].vertices[4].z || maxViewZ < clusterFrustums[idx].vertices[0].z || numClusterLights[idx] >= MAX_LIGHTS_CLUSTER)
                continue;

            for (size_t y = 0; y < NUM_CLUSTER_Y; ++y)
            {
                for (size_t x = 0; x < NUM_CLUSTER_X; ++x)
                {
                    if (bounds.IsInsideFast(clusterBoundingBoxes[idx]) && clusterFrustums[idx].IsInsideFast(boundsBox))
                        clusterData[(idx << 4) + numClusterLights[idx]++] = (unsigned char)(i + 1);

                    ++idx;
                }
            }
        }
    }
}

void RegisterRendererLibrary()
{
    static bool registered = false;
    if (registered)
        return;

    // Scene node base attributes are needed
    RegisterSceneLibrary();
    Octree::RegisterObject();
    Camera::RegisterObject();
    OctreeNode::RegisterObject();
    GeometryNode::RegisterObject();
    StaticModel::RegisterObject();
    Bone::RegisterObject();
    AnimatedModel::RegisterObject();
    Light::RegisterObject();
    Material::RegisterObject();
    Model::RegisterObject();
    Animation::RegisterObject();

    registered = true;
}
