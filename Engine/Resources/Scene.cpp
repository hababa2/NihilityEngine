#include "Scene.hpp"

#include "Engine.hpp"
#include "Resources.hpp"
#include "Mesh.hpp"

#include "Rendering\Renderer.hpp"
#include "Rendering\Pipeline.hpp"
#include "Rendering\RenderingDefines.hpp"
#include "Platform\Settings.hpp"
#include "Math\Physics.hpp"
#include "Core\Events.hpp"

Entity::Entity() : id(Scene::Invalid), scene(nullptr) {}

Entity::Entity(U32 id, Scene* scene) : id(id), scene(scene) {}

void Scene::Create(CameraType cameraType)
{
	componentBitPosition.Reserve(128);

	switch (cameraType)
	{
	case CAMERA_TYPE_PERSPECTIVE: {
#ifdef NH_DEBUG
		flyCamera.SetPerspective(0.01f, 1000.0f, 45.0f, 1.7777778f);
#else
		camera.SetPerspective(0.01f, 1000.0f, 45.0f, 1.7777778f);
#endif
	} break;
	case CAMERA_TYPE_ORTHOGRAPHIC: {
#ifdef NH_DEBUG
		flyCamera.SetOrthograpic(-100.0f, 100.0f, 240.0f, 135.0f, 0.0f);
#else
		camera.SetOrthograpic(-100.0f, 100.0f, 240.0f, 135.0f, 0.0f);
#endif
	} break;
	}

	for (U32 i = 0; i < VERTEX_TYPE_COUNT - 1; ++i)
	{
		vertexBuffers[i] = Renderer::CreateBuffer(Megabytes(32), BUFFER_USAGE_VERTEX_BUFFER | BUFFER_USAGE_TRANSFER_DST, BUFFER_MEMORY_TYPE_GPU_LOCAL, name + "_vertex_buffer_" + i);
	}

	stagingBuffer = Renderer::CreateBuffer(Megabytes(32), BUFFER_USAGE_TRANSFER_SRC, BUFFER_MEMORY_TYPE_CPU_VISIBLE | BUFFER_MEMORY_TYPE_CPU_COHERENT, name + "_staging_buffer");
	entitiesBuffer = Renderer::CreateBuffer(Megabytes(32), BUFFER_USAGE_STORAGE_BUFFER | BUFFER_USAGE_TRANSFER_DST, BUFFER_MEMORY_TYPE_GPU_LOCAL, name + "_entities_buffer");
	instanceBuffer = Renderer::CreateBuffer(Megabytes(32), BUFFER_USAGE_VERTEX_BUFFER | BUFFER_USAGE_TRANSFER_DST, BUFFER_MEMORY_TYPE_GPU_LOCAL, name + "_instance_buffer");
	indexBuffer = Renderer::CreateBuffer(Megabytes(64), BUFFER_USAGE_INDEX_BUFFER | BUFFER_USAGE_TRANSFER_DST, BUFFER_MEMORY_TYPE_GPU_LOCAL, name + "_index_buffer");
	drawBuffer = Renderer::CreateBuffer(Megabytes(32), BUFFER_USAGE_STORAGE_BUFFER | BUFFER_USAGE_INDIRECT_BUFFER | BUFFER_USAGE_TRANSFER_DST, BUFFER_MEMORY_TYPE_GPU_LOCAL, name + "_draw_buffer");
	countsBuffer = Renderer::CreateBuffer(sizeof(U32) * 32, BUFFER_USAGE_STORAGE_BUFFER | BUFFER_USAGE_INDIRECT_BUFFER | BUFFER_USAGE_TRANSFER_DST, BUFFER_MEMORY_TYPE_GPU_LOCAL, name + "_counts_buffer");
}

void Scene::Destroy()
{
	name.Destroy();

	indexWrites.Destroy();
	drawWrites.Destroy();
	countsWrites.Destroy();

	meshDraws.Destroy();
	entities.Destroy();

	pipelines.Destroy();

	for (Renderpass& renderpass : renderpasses) { Renderer::DestroyRenderPassInstant(&renderpass); }
	renderpasses.Destroy();

	for (U32 i = 0; i < VERTEX_TYPE_COUNT - 1; ++i)
	{
		vertexWrites[i].Destroy();
		Renderer::DestroyBuffer(vertexBuffers[i]);
	}

	Renderer::DestroyBuffer(stagingBuffer);
	Renderer::DestroyBuffer(entitiesBuffer);
	Renderer::DestroyBuffer(instanceBuffer);
	Renderer::DestroyBuffer(indexBuffer);
	Renderer::DestroyBuffer(drawBuffer);
	Renderer::DestroyBuffer(countsBuffer);
}

Entity Scene::CreateEntity()
{
	U32 id = Invalid;

	//TODO: Use freelist
	if (availableEntities.Size() == 0)
	{
		id = maxEntityID++;
	}
	else
	{
		id = availableEntities.Back();
		availableEntities.Pop();
	}

	entityMasks.Add(id, {});

	return { id, this };
}

void Scene::DestroyEntity(Entity& entity)
{
	Bitset& mask = GetEntityMask(entity.id);

	for (int i = 0; i < MaxComponents; i++)
	{
		if (mask[i] == 1) { componentPools[i]->Remove(entity.id); }
	}

	entityMasks.Remove(entity.id);
	availableEntities.Push(entity.id);

	entity.id = Invalid;
}

void Scene::SetSkybox(const ResourceRef<Skybox>& skybox)
{
	SkyboxData* skyboxData = Renderer::GetSkyboxData();
	GlobalData* globalData = Renderer::GetGlobalData();

	skyboxData->skyboxIndex = (U32)skybox->texture->Handle();
	globalData->skyboxIndex = (U32)skybox->texture->Handle();

	if (!hasSkybox)
	{
		hasSkybox = true;
		PushConstant pushConstant = { 0, sizeof(SkyboxData), Renderer::GetSkyboxData() };
		ResourceRef<Pipeline> pipeline = Resources::LoadPipeline("pipelines/skybox.nhpln", 1, &pushConstant);

		AddPipeline(pipeline);
	}
}

void Scene::SetPostProcessing(const PostProcessData& data)
{
	Copy(Renderer::GetPostProcessData(), &data, 1);

	if (!hasPostProcessing)
	{
		hasPostProcessing = true;
		PushConstant pushConstant = { 0, sizeof(PostProcessData), Renderer::GetPostProcessData() };
		ResourceRef<Pipeline> pipeline = Resources::LoadPipeline("pipelines/postProcess.nhpln", 1, &pushConstant);
		pipeline->AddDescriptor({ Renderer::defaultRenderTarget->imageView, IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, Renderer::defaultRenderTarget->sampler.vkSampler });

		AddPipeline(pipeline);
	}
}

void Scene::AddMesh(ResourceRef<Mesh>& mesh)
{
	if (mesh->index != U32_MAX) { return; }

	mesh->index = (U32)meshDraws.Size();
	MeshDraw& draw = meshDraws.Push({});

	//Indices
	if (mesh->indicesSize) { indexWrites.Push(CreateWrite(indexOffset * sizeof(U32), 0, mesh->indicesSize, mesh->indices)); }
	
	draw.indexCount = mesh->indicesSize / sizeof(U32);
	draw.indexOffset = indexOffset;
	indexOffset += draw.indexCount;

	vertexOffset = (U32)NextMultipleOf(vertexOffset, mesh->buffers[0].stride);
	draw.vertexOffset = vertexOffset / mesh->buffers[0].stride;
	
	U32 verticesSize = 0;
	for (VertexBuffer& buffer : mesh->buffers)
	{
		verticesSize = Math::Max(verticesSize, buffer.size);
		vertexWrites[buffer.type].Push(CreateWrite(vertexOffset, 0, buffer.size, buffer.buffer));
	}

	vertexOffset += verticesSize;
}

void Scene::AddInstance(MeshInstance& instance)
{
	AddMesh(instance.mesh);

	MeshDraw& draw = meshDraws[instance.mesh->index];

	bool first = true;

	for (ResourceRef<Pipeline>& pipeline : instance.material->effect->processing)
	{
		AddPipeline(pipeline);

		if (!pipeline->drawCount)
		{
			pipeline->drawOffset = drawOffset;
			pipeline->countOffset = countsOffset;
			if(pipeline->useIndices) { drawOffset += sizeof(VkDrawIndexedIndirectCommand) * 128; }
			else { drawOffset += sizeof(VkDrawIndirectCommand) * 128; }
			countsOffset += sizeof(U32);
		}

		if (!draw.instanceCount)
		{
			if (pipeline->useIndices) { draw.drawOffset = pipeline->drawOffset + sizeof(VkDrawIndexedIndirectCommand) * pipeline->drawCount; }
			else { draw.drawOffset = pipeline->drawOffset + sizeof(VkDrawIndirectCommand) * pipeline->drawCount; }
			++pipeline->drawCount;

			countsWrites.Push(CreateWrite(pipeline->countOffset, 0, sizeof(U32), &pipeline->drawCount));

			if (first)
			{
				first = false;
				draw.instanceOffset = (U32)NextMultipleOf(instanceOffset, pipeline->instanceStride);
				instanceOffset += pipeline->instanceStride * 1024;
			}
		}

		if (pipeline->useIndices)
		{
			VkDrawIndexedIndirectCommand drawCommand{};
			drawCommand.indexCount = draw.indexCount;
			drawCommand.instanceCount = draw.instanceCount + 1;
			drawCommand.firstIndex = draw.indexOffset;
			drawCommand.vertexOffset = draw.vertexOffset;
			drawCommand.firstInstance = draw.instanceOffset / pipeline->instanceStride;

			drawWrites.Push(CreateWrite(draw.drawOffset, 0, sizeof(VkDrawIndexedIndirectCommand), &drawCommand));
		}
		else
		{
			VkDrawIndirectCommand drawCommand{};
			drawCommand.vertexCount = instance.mesh->vertexCount;
			drawCommand.instanceCount = draw.instanceCount + 1;
			drawCommand.firstVertex = draw.vertexOffset;
			drawCommand.firstInstance = draw.instanceOffset / pipeline->instanceStride;

			drawWrites.Push(CreateWrite(draw.drawOffset, 0, sizeof(VkDrawIndirectCommand), &drawCommand));
		}
	}

	ResourceRef<Pipeline>& pipeline = instance.material->effect->processing[0];

	instance.instanceOffset = draw.instanceOffset + draw.instanceCount * pipeline->instanceStride;

	instanceWrites.Push(CreateWrite(instance.instanceOffset, 0, pipeline->instanceStride, instance.instanceData.data));

	++draw.instanceCount;
}

void Scene::UpdateInstance(MeshInstance& instance)
{
	ResourceRef<Pipeline>& pipeline = instance.material->effect->processing[0];

	instanceWrites.Push(CreateWrite(instance.instanceOffset, 0, pipeline->instanceStride, instance.instanceData.data));
}

void Scene::AddPipeline(ResourceRef<Pipeline>& pipeline)
{
	if (!pipeline->loaded)
	{
		pipeline->loaded = true;
		U32 i = 0;
		for (const ResourceRef<Pipeline>& p : pipelines)
		{
			if (p == pipeline) { return; }

			if (pipeline->renderOrder <= p->renderOrder) { break; }

			++i;
		}

		pipeline->SetBuffers(vertexBuffers, instanceBuffer);

		if (loaded) { BreakPoint; } //TODO: runtime renderpass edits

		pipelines.Insert(i, pipeline);
	}
}

void Scene::Load()
{
	if (!loaded)
	{
		for (SparseSetBase* pool : componentPools)
		{
			pool->Load(this);
		}

		loaded = true;

		U32 renderpass = 0;
		U32 subpass = 0;
		bool first = true;
		bool firstColor = true;
		bool firstDepth = true;

		RenderpassInfo renderpassInfos[8]{};
		
		for (ResourceRef<Pipeline>& pipeline : pipelines)
		{
			//TODO: Maybe only need to check if we need MORE rendertargets, not if the count doesn't match
			if (first || pipeline->clearTypes || (pipeline->sizeX && (pipeline->sizeX != renderpassInfos[renderpass].renderArea.extent.x || pipeline->sizeY != renderpassInfos[renderpass].renderArea.extent.y)) ||
				pipeline->outputCount != renderpassInfos[renderpass].renderTargetCount || (pipeline->depthEnable && !renderpassInfos[renderpass].depthStencilTarget))
			{
				if (!first) { ++renderpass; }

				subpass = 0;
		
				RenderpassInfo& renderpassInfo = renderpassInfos[renderpass];
		
				renderpassInfo.Reset();
				renderpassInfo.name = name + "_renderpass" + renderpass;
				renderpassInfo.resize = pipeline->sizeX == 0;
		
				if (renderpassInfo.resize)
				{
					if (pipeline->outputCount)
					{
						renderpassInfo.AddRenderTarget(Renderer::defaultRenderTarget);

						TextureInfo textureInfo{};
						Settings::GetSetting(WindowWidth, textureInfo.width);
						Settings::GetSetting(WindowHeight, textureInfo.height);
						textureInfo.depth = 1;
						textureInfo.flags = TEXTURE_FLAG_RENDER_TARGET;
						textureInfo.type = VK_IMAGE_TYPE_2D;
						textureInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
						String n = "default_render_target";

						for (U32 i = 1; i < pipeline->outputCount; ++i)
						{
							textureInfo.name = n + i;

							renderpassInfo.AddRenderTarget(Resources::CreateTexture(textureInfo));
						}
					}

					if (pipeline->depthEnable) { renderpassInfo.SetDepthStencilTarget(Renderer::defaultDepthTarget); }

					renderpassInfo.renderArea = { { 0, 0 }, { Renderer::defaultRenderTarget->width, Renderer::defaultRenderTarget->height } };
				}
				else
				{
					TextureInfo textureInfo{};
					textureInfo.width = pipeline->sizeX;
					textureInfo.height = pipeline->sizeY;
					textureInfo.depth = 1;
					textureInfo.flags = TEXTURE_FLAG_RENDER_TARGET;
					textureInfo.type = VK_IMAGE_TYPE_2D;

					String n = renderpassInfo.name + "_render_target";
					renderpassInfo.renderArea = { { 0, 0 }, { pipeline->sizeX, pipeline->sizeY } };

					for (U32 i = 0; i < pipeline->outputCount; ++i)
					{
						textureInfo.name = n + i;
						textureInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;

						renderpassInfo.AddRenderTarget(Resources::CreateTexture(textureInfo));
					}

					if (pipeline->depthEnable)
					{
						textureInfo.name = renderpassInfo.name + "_depth_target";
						textureInfo.format = VK_FORMAT_D32_SFLOAT;

						renderpassInfo.SetDepthStencilTarget(Resources::CreateTexture(textureInfo));
					}
				}
		
				if ((firstColor || (pipeline->clearTypes & CLEAR_TYPE_COLOR)) && pipeline->outputCount)
				{
					firstColor = false;
					renderpassInfo.colorLoadOp = ATTACHMENT_LOAD_OP_CLEAR;
				}
				else { renderpassInfo.colorLoadOp = ATTACHMENT_LOAD_OP_LOAD; }

				if ((firstDepth || (pipeline->clearTypes & CLEAR_TYPE_DEPTH)) && pipeline->depthEnable)
				{
					firstDepth = false;
					renderpassInfo.depthLoadOp = ATTACHMENT_LOAD_OP_CLEAR;
				}
				else { renderpassInfo.depthLoadOp = ATTACHMENT_LOAD_OP_LOAD; }
		
				renderpassInfo.subpassCount = 1;
		
				if (pipeline->subpass.inputAttachmentCount) { Logger::Error("First Shader In Renderpass Cannot Have Input Attachments!"); }
			}
		
			pipeline->renderpassIndex = renderpass;
		
			if (pipeline->subpass.inputAttachmentCount)
			{
				++subpass;
		
				renderpassInfos[renderpass].AddSubpass(pipeline->subpass);
			}
		
			pipeline->subpassIndex = subpass;
		
			first = false;
		}
		
		Renderpass* prevRenderpass = nullptr;

		for (ResourceRef<Pipeline>& pipeline : pipelines)
		{
			for (U8 i = 0; i < pipeline->dependancyCount; ++i)
			{
				Dependancy& dependancy = pipeline->dependancies[i];
				dependancy.descriptor = pipeline->descriptorCount;

				switch (dependancy.type)
				{
				case DEPENDANCY_RENDER_TARGET: {
					ResourceRef<Texture>& target = renderpassInfos[dependancy.pipeline->renderpassIndex].renderTargets[dependancy.index];
					target->flags |= TEXTURE_FLAG_RENDER_SAMPLED;
					pipeline->AddDescriptor({ target });
				} break;
				case DEPENDANCY_DEPTH_TARGET: {
					ResourceRef<Texture>& target = renderpassInfos[dependancy.pipeline->renderpassIndex].depthStencilTarget;
					target->flags |= TEXTURE_FLAG_RENDER_SAMPLED;
					pipeline->AddDescriptor({ target });
				} break;
				case DEPENDANCY_ENTITY_BUFFER: {
					pipeline->AddDescriptor({ entitiesBuffer.vkBuffer });
				} break;
				}
			}
		}
		
		for (U32 i = 0; i <= renderpass; ++i)
		{
			Renderpass* rp = &renderpasses.Push({});
			Renderer::CreateRenderpass(rp, renderpassInfos[i]);
			prevRenderpass = rp;
		}

		for (ResourceRef<Pipeline>& pipeline : pipelines)
		{
			pipeline->Build(&renderpasses[pipeline->renderpassIndex]);
		}
	}
}

void Scene::Unload()
{
	if (loaded)
	{
		loaded = false;
	}
}

void Scene::Update()
{
	static F32 lightVal = 0.0f;

	ShadowData* shadowData = Renderer::GetShadowData();
	SkyboxData* skyboxData = Renderer::GetSkyboxData();
	GlobalData* globalData = Renderer::GetGlobalData();

	lightVal += (F32)Time::DeltaTime();

	globalData->lightPos.x = Math::Cos(lightVal) * 0.2f;
	globalData->lightPos.y = 1.0f;
	globalData->lightPos.z = Math::Sin(lightVal) * 0.2f;

	Matrix4 perspective;
	perspective.SetPerspective(45.0f, 1.0f, 0.01f, 1000.0f);

	Matrix4 look;
	look.LookAt(globalData->lightPos, Vector3Zero, Vector3Up);

	shadowData->depthViewProjection = perspective * look;

	globalData->lightSpace = shadowData->depthViewProjection;

#ifdef NH_DEBUG
	if (Engine::InEditor()) { flyCamera.Update(); }
	else { flyCamera.GetCamera().Update(); }

	globalData->viewProjection = flyCamera.ViewProjection();
	globalData->eye = flyCamera.Eye();

	skyboxData->invViewProjection = flyCamera.ViewProjection().Inverse(); //TODO: fix floating point inaccuracy with position
#else
	camera.Update();
	globalData->viewProjection = camera.ViewProjection();
	globalData->eye = camera.Eye();

	skyboxData->invViewProjection = camera.ViewProjection().Inverse();
#endif

	VkBufferCopy region{};
	region.dstOffset = 0;
	region.size = sizeof(GlobalData);
	region.srcOffset = 0;

	Renderer::FillBuffer(Renderer::globalsBuffer, sizeof(GlobalData), globalData, 1, &region);

	//TODO: Update components

	for (Entity& entity : entities)
	{
		//entityWrites.Push(CreateWrite(entity.id * sizeof(Matrix4), 0, sizeof(Matrix4), &entity.transform.WorldMatrix()));
	}

	if (entityWrites.Size())
	{
		Renderer::FillBuffer(entitiesBuffer, stagingBuffer, (U32)entityWrites.Size(), (VkBufferCopy*)entityWrites.Data());
		entityWrites.Clear();
	}

	if (indexWrites.Size())
	{
		Renderer::FillBuffer(indexBuffer, stagingBuffer, (U32)indexWrites.Size(), (VkBufferCopy*)indexWrites.Data());
		indexWrites.Clear();
	}

	for (U32 i = 0; i < VERTEX_TYPE_COUNT - 1; ++i)
	{
		Vector<BufferCopy>& writes = vertexWrites[i];

		if (writes.Size())
		{
			Renderer::FillBuffer(vertexBuffers[i], stagingBuffer, (U32)writes.Size(), (VkBufferCopy*)writes.Data());
			writes.Clear();
		}
	}

	if (instanceWrites.Size())
	{
		Renderer::FillBuffer(instanceBuffer, stagingBuffer, (U32)instanceWrites.Size(), (VkBufferCopy*)instanceWrites.Data());
		instanceWrites.Clear();
	}

	if (countsWrites.Size())
	{
		Renderer::FillBuffer(countsBuffer, stagingBuffer, (U32)countsWrites.Size(), (VkBufferCopy*)countsWrites.Data());
		countsWrites.Clear();
	}

	if (drawWrites.Size())
	{
		Renderer::FillBuffer(drawBuffer, stagingBuffer, (U32)drawWrites.Size(), (VkBufferCopy*)drawWrites.Data());
		drawWrites.Clear();
	}

	stagingBuffer.allocationOffset = 0;
}

void Scene::Render(CommandBuffer* commandBuffer)
{
	U32 renderpass = U32_MAX;
	U32 subpass = 0;

	for (ResourceRef<Pipeline>& pipeline : pipelines)
	{
		if (pipeline->renderpassIndex != renderpass)
		{
			if (renderpass != U32_MAX)
			{
				commandBuffer->EndRenderpass();

				if (renderpasses[renderpass].depthStencilTarget && renderpasses[renderpass].depthStencilTarget->flags & TEXTURE_FLAG_RENDER_SAMPLED)
				{
					VkImageMemoryBarrier2 barrier = Renderer::ImageBarrier(renderpasses[renderpass].depthStencilTarget->image, 
						VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
						VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
				
					commandBuffer->PipelineBarrier(0, 0, nullptr, 1, &barrier);
				}
			}

			commandBuffer->BeginRenderpass(&renderpasses[pipeline->renderpassIndex]);
			renderpass = pipeline->renderpassIndex;
			subpass = 0;
		}
		else if (pipeline->subpassIndex != subpass)
		{
			commandBuffer->NextSubpass();
			subpass = pipeline->subpassIndex;
		}

		pipeline->Run(commandBuffer, indexBuffer, drawBuffer, countsBuffer);
	}

	commandBuffer->EndRenderpass();
}

void Scene::Resize()
{
	for (Renderpass& renderpass : renderpasses)
	{
		Renderer::RecreateRenderpass(&renderpass);
	}
}

BufferCopy Scene::CreateWrite(U64 dstOffset, U64 srcOffset, U64 size, const void* data)
{
	BufferCopy region{};
	region.srcOffset = stagingBuffer.allocationOffset;
	region.dstOffset = dstOffset;
	region.size = size;
	stagingBuffer.allocationOffset += size;

	Copy((U8*)stagingBuffer.data + region.srcOffset, (U8*)data + srcOffset, size);

	return region;
}

Bitset& Scene::GetEntityMask(U32 id)
{
	Bitset* mask = entityMasks.Get(id);
	return *mask;
}