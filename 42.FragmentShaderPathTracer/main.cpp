// Copyright (C) 2018-2020 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

#define _NBL_STATIC_LIB_
#include <nabla.h>

#include "../common/CommonAPI.h"
#include "../common/Camera.hpp"
#include "../common/QToQuitEventReceiver.h"
#include "nbl/ext/ScreenShot/ScreenShot.h"

using namespace nbl;
using namespace core;
using namespace ui;


using namespace nbl;
using namespace core;
using namespace asset;
using namespace video;

smart_refctd_ptr<IGPUImageView> createHDRImageView(nbl::core::smart_refctd_ptr<nbl::video::ILogicalDevice> device, asset::E_FORMAT colorFormat, uint32_t width, uint32_t height)
{
	smart_refctd_ptr<IGPUImageView> gpuImageViewColorBuffer;
	{
		IGPUImage::SCreationParams imgInfo;
		imgInfo.format = colorFormat;
		imgInfo.type = IGPUImage::ET_2D;
		imgInfo.extent.width = width;
		imgInfo.extent.height = height;
		imgInfo.extent.depth = 1u;
		imgInfo.mipLevels = 1u;
		imgInfo.arrayLayers = 1u;
		imgInfo.samples = asset::ICPUImage::ESCF_1_BIT;
		imgInfo.flags = static_cast<asset::IImage::E_CREATE_FLAGS>(0u);

		auto image = device->createGPUImageOnDedMem(std::move(imgInfo),device->getDeviceLocalGPUMemoryReqs());

		IGPUImageView::SCreationParams imgViewInfo;
		imgViewInfo.image = std::move(image);
		imgViewInfo.format = colorFormat;
		imgViewInfo.viewType = IGPUImageView::ET_2D;
		imgViewInfo.flags = static_cast<IGPUImageView::E_CREATE_FLAGS>(0u);
		imgViewInfo.subresourceRange.baseArrayLayer = 0u;
		imgViewInfo.subresourceRange.baseMipLevel = 0u;
		imgViewInfo.subresourceRange.layerCount = 1u;
		imgViewInfo.subresourceRange.levelCount = 1u;

		gpuImageViewColorBuffer = device->createGPUImageView(std::move(imgViewInfo));
	}

	return gpuImageViewColorBuffer;
}

struct ShaderParameters
{
	const uint32_t MaxDepthLog2 = 4; //5
	const uint32_t MaxSamplesLog2 = 10; //18
} kShaderParameters;

enum E_LIGHT_GEOMETRY
{
	ELG_SPHERE,
	ELG_TRIANGLE,
	ELG_RECTANGLE
};

struct DispatchInfo_t
{
	uint32_t workGroupCount[3];
};

_NBL_STATIC_INLINE_CONSTEXPR uint32_t DEFAULT_WORK_GROUP_SIZE = 16u;

DispatchInfo_t getDispatchInfo(uint32_t imgWidth, uint32_t imgHeight) {
	DispatchInfo_t ret = {};
	ret.workGroupCount[0] = (uint32_t)core::ceil<float>((float)imgWidth / (float)DEFAULT_WORK_GROUP_SIZE);
	ret.workGroupCount[1] = (uint32_t)core::ceil<float>((float)imgHeight / (float)DEFAULT_WORK_GROUP_SIZE);
	ret.workGroupCount[2] = 1;
	return ret;
}

int main()
{
	constexpr uint32_t WIN_W = 1280;
	constexpr uint32_t WIN_H = 720;
	constexpr uint32_t FBO_COUNT = 2u;
	constexpr uint32_t FRAMES_IN_FLIGHT = 5u;
	static_assert(FRAMES_IN_FLIGHT>FBO_COUNT);

	auto initOutput = CommonAPI::Init<WIN_W, WIN_H, FBO_COUNT>(video::EAT_OPENGL, "Physics Simulation", asset::EF_D32_SFLOAT);
	auto system = std::move(initOutput.system);
	auto window = std::move(initOutput.window);
	auto gl = std::move(initOutput.apiConnection);
	auto surface = std::move(initOutput.surface);
	auto gpuPhysicalDevice = std::move(initOutput.physicalDevice);
	auto device = std::move(initOutput.logicalDevice);
	auto queues = std::move(initOutput.queues);
	auto graphicsQueue = queues[decltype(initOutput)::EQT_GRAPHICS];
	auto transferUpQueue = queues[decltype(initOutput)::EQT_TRANSFER_UP];
	auto swapchain = std::move(initOutput.swapchain);
	auto renderpass = std::move(initOutput.renderpass);
	auto fbo = std::move(initOutput.fbo);
	auto commandPool = std::move(initOutput.commandPool);
	auto assetManager = std::move(initOutput.assetManager);
	auto cpu2gpuParams = std::move(initOutput.cpu2gpuParams);
	auto logger = std::move(initOutput.logger);
	auto inputSystem = std::move(initOutput.inputSystem);

	nbl::video::IGPUObjectFromAssetConverter CPU2GPU;
	
	core::smart_refctd_ptr<nbl::video::IGPUCommandBuffer> cmdbuf[FRAMES_IN_FLIGHT];
	device->createCommandBuffers(commandPool.get(), nbl::video::IGPUCommandBuffer::EL_PRIMARY, FRAMES_IN_FLIGHT, cmdbuf);
	
	constexpr uint32_t maxDescriptorCount = 256u;
	constexpr uint32_t PoolSizesCount = 5u;
	nbl::video::IDescriptorPool::SDescriptorPoolSize poolSizes[PoolSizesCount] = {
		{ EDT_STORAGE_BUFFER, 1},
		{ EDT_STORAGE_IMAGE, 8},
		{ EDT_COMBINED_IMAGE_SAMPLER, 2},
		{ EDT_UNIFORM_TEXEL_BUFFER, 1},
		{ EDT_UNIFORM_BUFFER, 1},
	};

	auto descriptorPool = device->createDescriptorPool(static_cast<nbl::video::IDescriptorPool::E_CREATE_FLAGS>(0), maxDescriptorCount, PoolSizesCount, poolSizes);

	// Camera 
	core::vectorSIMDf cameraPosition(0, 5, -10);
	matrix4SIMD proj = matrix4SIMD::buildProjectionMatrixPerspectiveFovRH(core::radians(60), float(WIN_W) / WIN_H, 0.01f, 500.0f);
	Camera cam = Camera(cameraPosition, core::vectorSIMDf(0, 0, 0), proj);

	IGPUDescriptorSetLayout::SBinding descriptorSet0Bindings[] = {
		{ 0u, EDT_STORAGE_IMAGE, 1u, IGPUSpecializedShader::ESS_COMPUTE, nullptr },
	};
	IGPUDescriptorSetLayout::SBinding uboBinding {0, EDT_UNIFORM_BUFFER, 1u, IGPUSpecializedShader::ESS_FRAGMENT, nullptr};
	IGPUDescriptorSetLayout::SBinding descriptorSet3Bindings[] = {
		{ 0u, EDT_COMBINED_IMAGE_SAMPLER, 1u, IGPUSpecializedShader::ESS_COMPUTE, nullptr },
		{ 1u, EDT_UNIFORM_TEXEL_BUFFER, 1u, IGPUSpecializedShader::ESS_COMPUTE, nullptr },
		{ 2u, EDT_COMBINED_IMAGE_SAMPLER, 1u, IGPUSpecializedShader::ESS_COMPUTE, nullptr }
	};
	
	auto gpuDescriptorSetLayout0 = device->createGPUDescriptorSetLayout(descriptorSet0Bindings, descriptorSet0Bindings + 1u);
	auto gpuDescriptorSetLayout1 = device->createGPUDescriptorSetLayout(&uboBinding, &uboBinding + 1u);
	auto gpuDescriptorSetLayout3 = device->createGPUDescriptorSetLayout(descriptorSet3Bindings, descriptorSet3Bindings+3u);

	auto createGpuResources = [&](std::string pathToShader) -> core::smart_refctd_ptr<video::IGPUComputePipeline>
	{
		auto cpuComputeSpecializedShader = core::smart_refctd_ptr_static_cast<asset::ICPUSpecializedShader>(assetManager->getAsset(pathToShader, {}).getContents().begin()[0]);


		ISpecializedShader::SInfo info = cpuComputeSpecializedShader->getSpecializationInfo();
		info.m_backingBuffer = core::make_smart_refctd_ptr<ICPUBuffer>(sizeof(ShaderParameters));
		memcpy(info.m_backingBuffer->getPointer(),&kShaderParameters,sizeof(ShaderParameters));
		info.m_entries = core::make_refctd_dynamic_array<core::smart_refctd_dynamic_array<ISpecializedShader::SInfo::SMapEntry>>(2u);
		for (uint32_t i=0; i<2; i++)
			info.m_entries->operator[](i) = {i,i*sizeof(uint32_t),sizeof(uint32_t)};


		cpuComputeSpecializedShader->setSpecializationInfo(std::move(info));

		auto gpuComputeSpecializedShader = CPU2GPU.getGPUObjectsFromAssets(&cpuComputeSpecializedShader, &cpuComputeSpecializedShader + 1, cpu2gpuParams)->front();

		auto gpuPipelineLayout = device->createGPUPipelineLayout(nullptr, nullptr, core::smart_refctd_ptr(gpuDescriptorSetLayout0), core::smart_refctd_ptr(gpuDescriptorSetLayout1), nullptr, core::smart_refctd_ptr(gpuDescriptorSetLayout3));

		auto gpuPipeline = device->createGPUComputePipeline(nullptr, std::move(gpuPipelineLayout), std::move(gpuComputeSpecializedShader));

		return gpuPipeline;
	};

	E_LIGHT_GEOMETRY lightGeom = ELG_SPHERE;
	constexpr const char* shaderPaths[] = {"../litBySphere.comp","../litByTriangle.comp","../litByRectangle.comp"};
	auto gpuComputePipeline = createGpuResources(shaderPaths[lightGeom]);
	
	DispatchInfo_t dispatchInfo = getDispatchInfo(WIN_W, WIN_H);

	auto createGPUImageView = [&](std::string pathToOpenEXRHDRIImage)
	{
		auto pathToTexture = pathToOpenEXRHDRIImage;
		IAssetLoader::SAssetLoadParams lp(0ull, nullptr, IAssetLoader::ECF_DONT_CACHE_REFERENCES);
		auto cpuTexture = assetManager->getAsset(pathToTexture, lp);
		auto cpuTextureContents = cpuTexture.getContents();

		auto asset = *cpuTextureContents.begin();

		ICPUImageView::SCreationParams viewParams;
		viewParams.flags = static_cast<ICPUImageView::E_CREATE_FLAGS>(0u);
		viewParams.image = core::smart_refctd_ptr_static_cast<asset::ICPUImage>(asset);
		viewParams.format = viewParams.image->getCreationParameters().format;
		viewParams.viewType = IImageView<ICPUImage>::ET_2D;
		viewParams.subresourceRange.baseArrayLayer = 0u;
		viewParams.subresourceRange.layerCount = 1u;
		viewParams.subresourceRange.baseMipLevel = 0u;
		viewParams.subresourceRange.levelCount = 1u;

		auto cpuImageView = ICPUImageView::create(std::move(viewParams));
		auto gpuImageView = CPU2GPU.getGPUObjectsFromAssets(&cpuImageView, &cpuImageView + 1u, cpu2gpuParams)->front();

		return gpuImageView;
	};

	auto gpuEnvmapImageView = createGPUImageView("../../media/envmap/envmap_0.exr");

	smart_refctd_ptr<IGPUBufferView> gpuSequenceBufferView;
	{
		const uint32_t MaxDimensions = 3u<<kShaderParameters.MaxDepthLog2;
		const uint32_t MaxSamples = 1u<<kShaderParameters.MaxSamplesLog2;

		auto sampleSequence = core::make_smart_refctd_ptr<asset::ICPUBuffer>(sizeof(uint32_t)*MaxDimensions*MaxSamples);
		
		core::OwenSampler sampler(MaxDimensions, 0xdeadbeefu);
		//core::SobolSampler sampler(MaxDimensions);

		auto out = reinterpret_cast<uint32_t*>(sampleSequence->getPointer());
		for (auto dim=0u; dim<MaxDimensions; dim++)
		for (uint32_t i=0; i<MaxSamples; i++)
		{
			out[i*MaxDimensions+dim] = sampler.sample(dim,i);
		}
		auto gpuSequenceBuffer = device->createFilledDeviceLocalGPUBufferOnDedMem(graphicsQueue, sampleSequence->getSize(), sampleSequence->getPointer());
		gpuSequenceBufferView = device->createGPUBufferView(gpuSequenceBuffer.get(), asset::EF_R32G32B32_UINT);
	}

	smart_refctd_ptr<IGPUImageView> gpuScrambleImageView;
	{
		IGPUImage::SCreationParams imgParams;
		imgParams.flags = static_cast<IImage::E_CREATE_FLAGS>(0u);
		imgParams.type = IImage::ET_2D;
		imgParams.format = EF_R32G32_UINT;
		imgParams.extent = {WIN_W, WIN_H,1u};
		imgParams.mipLevels = 1u;
		imgParams.arrayLayers = 1u;
		imgParams.samples = IImage::ESCF_1_BIT;

		IGPUImage::SBufferCopy region;
		region.imageExtent = imgParams.extent;
		region.imageSubresource.layerCount = 1u;

		constexpr auto ScrambleStateChannels = 2u;
		const auto renderPixelCount = imgParams.extent.width*imgParams.extent.height;
		core::vector<uint32_t> random(renderPixelCount*ScrambleStateChannels);
		{
			core::RandomSampler rng(0xbadc0ffeu);
			for (auto& pixel : random)
				pixel = rng.nextSample();
		}
		auto buffer = device->createFilledDeviceLocalGPUBufferOnDedMem(graphicsQueue, random.size()*sizeof(uint32_t), random.data());

		IGPUImageView::SCreationParams viewParams;
		viewParams.flags = static_cast<IGPUImageView::E_CREATE_FLAGS>(0u);
		viewParams.image = device->createFilledDeviceLocalGPUImageOnDedMem(graphicsQueue, std::move(imgParams), buffer.get(), 1u, &region);
		viewParams.viewType = IGPUImageView::ET_2D;
		viewParams.format = EF_R32G32_UINT;
		viewParams.subresourceRange.levelCount = 1u;
		viewParams.subresourceRange.layerCount = 1u;
		gpuScrambleImageView = device->createGPUImageView(std::move(viewParams));
	}
	
	// Create Out Image TODO
	smart_refctd_ptr<IGPUImageView> outHDRImageViews[FBO_COUNT] = {};
	for(uint32_t i = 0; i < FBO_COUNT; ++i) {
		outHDRImageViews[i] = createHDRImageView(device, asset::EF_R16G16B16A16_SFLOAT, WIN_W, WIN_H);
	}

	core::smart_refctd_ptr<IGPUDescriptorSet> descriptorSets0[FBO_COUNT] = {};
	for(uint32_t i = 0; i < FBO_COUNT; ++i)
	{
		auto & descSet = descriptorSets0[i];
		descSet = device->createGPUDescriptorSet(descriptorPool.get(), core::smart_refctd_ptr(gpuDescriptorSetLayout0));
		video::IGPUDescriptorSet::SWriteDescriptorSet writeDescriptorSet;
		writeDescriptorSet.dstSet = descSet.get();
		writeDescriptorSet.binding = 0;
		writeDescriptorSet.count = 1u;
		writeDescriptorSet.arrayElement = 0u;
		writeDescriptorSet.descriptorType = asset::EDT_STORAGE_IMAGE;
		video::IGPUDescriptorSet::SDescriptorInfo info;
		{
			info.desc = outHDRImageViews[i];
			info.image.sampler = nullptr;
			info.image.imageLayout = static_cast<asset::E_IMAGE_LAYOUT>(0u);;
		}
		writeDescriptorSet.info = &info;
		device->updateDescriptorSets(1u, &writeDescriptorSet, 0u, nullptr);
	}

	auto gpuubo = device->createDeviceLocalGPUBufferOnDedMem(sizeof(SBasicViewParameters));
	auto uboDescriptorSet1 = device->createGPUDescriptorSet(descriptorPool.get(), core::smart_refctd_ptr(gpuDescriptorSetLayout1));
	{
		video::IGPUDescriptorSet::SWriteDescriptorSet uboWriteDescriptorSet;
		uboWriteDescriptorSet.dstSet = uboDescriptorSet1.get();
		uboWriteDescriptorSet.binding = 0;
		uboWriteDescriptorSet.count = 1u;
		uboWriteDescriptorSet.arrayElement = 0u;
		uboWriteDescriptorSet.descriptorType = asset::EDT_UNIFORM_BUFFER;
		video::IGPUDescriptorSet::SDescriptorInfo info;
		{
			info.desc = gpuubo;
			info.buffer.offset = 0ull;
			info.buffer.size = sizeof(SBasicViewParameters);
		}
		uboWriteDescriptorSet.info = &info;
		device->updateDescriptorSets(1u, &uboWriteDescriptorSet, 0u, nullptr);
	}

	auto descriptorSet3 = device->createGPUDescriptorSet(descriptorPool.get(), core::smart_refctd_ptr(gpuDescriptorSetLayout3));
	{
		constexpr auto kDescriptorCount = 3;
		IGPUDescriptorSet::SWriteDescriptorSet samplerWriteDescriptorSet[kDescriptorCount];
		IGPUDescriptorSet::SDescriptorInfo samplerDescriptorInfo[kDescriptorCount];
		for (auto i=0; i<kDescriptorCount; i++)
		{
			samplerWriteDescriptorSet[i].dstSet = descriptorSet3.get();
			samplerWriteDescriptorSet[i].binding = i;
			samplerWriteDescriptorSet[i].arrayElement = 0u;
			samplerWriteDescriptorSet[i].count = 1u;
			samplerWriteDescriptorSet[i].info = samplerDescriptorInfo+i;
		}
		samplerWriteDescriptorSet[0].descriptorType = EDT_COMBINED_IMAGE_SAMPLER;
		samplerWriteDescriptorSet[1].descriptorType = EDT_UNIFORM_TEXEL_BUFFER;
		samplerWriteDescriptorSet[2].descriptorType = EDT_COMBINED_IMAGE_SAMPLER;

		samplerDescriptorInfo[0].desc = gpuEnvmapImageView;
		{
			ISampler::SParams samplerParams = { ISampler::ETC_CLAMP_TO_EDGE, ISampler::ETC_CLAMP_TO_EDGE, ISampler::ETC_CLAMP_TO_EDGE, ISampler::ETBC_FLOAT_OPAQUE_BLACK, ISampler::ETF_LINEAR, ISampler::ETF_LINEAR, ISampler::ESMM_LINEAR, 0u, false, ECO_ALWAYS };
			samplerDescriptorInfo[0].image.sampler = device->createGPUSampler(samplerParams);
			samplerDescriptorInfo[0].image.imageLayout = EIL_SHADER_READ_ONLY_OPTIMAL;
		}
		samplerDescriptorInfo[1].desc = gpuSequenceBufferView;
		samplerDescriptorInfo[2].desc = gpuScrambleImageView;
		{
			ISampler::SParams samplerParams = { ISampler::ETC_CLAMP_TO_EDGE, ISampler::ETC_CLAMP_TO_EDGE, ISampler::ETC_CLAMP_TO_EDGE, ISampler::ETBC_INT_OPAQUE_BLACK, ISampler::ETF_NEAREST, ISampler::ETF_NEAREST, ISampler::ESMM_NEAREST, 0u, false, ECO_ALWAYS };
			samplerDescriptorInfo[2].image.sampler = device->createGPUSampler(samplerParams);
			samplerDescriptorInfo[2].image.imageLayout = EIL_SHADER_READ_ONLY_OPTIMAL;
		}
		device->updateDescriptorSets(kDescriptorCount, samplerWriteDescriptorSet, 0u, nullptr);
	}

	// auto blitFBO = driver->addFrameBuffer();
	// blitFBO->attach(video::EFAP_COLOR_ATTACHMENT0, std::move(outImgView));
	
	auto lastTime = std::chrono::system_clock::now();
	constexpr uint32_t FRAME_COUNT = 500000u;
	constexpr uint64_t MAX_TIMEOUT = 99999999999999ull;

	core::smart_refctd_ptr<video::IGPUFence> frameComplete[FRAMES_IN_FLIGHT] = { nullptr };
	core::smart_refctd_ptr<video::IGPUSemaphore> imageAcquire[FRAMES_IN_FLIGHT] = { nullptr };
	core::smart_refctd_ptr<video::IGPUSemaphore> renderFinished[FRAMES_IN_FLIGHT] = { nullptr };
	for (uint32_t i=0u; i<FRAMES_IN_FLIGHT; i++)
	{
		imageAcquire[i] = device->createSemaphore();
		renderFinished[i] = device->createSemaphore();
	}

	// Render
	constexpr size_t MaxFramesToAverage = 100ull;
	bool frameDataFilled = false;
	size_t frame_count = 0ull;
	double time_sum = 0;
	double dtList[MaxFramesToAverage] = {};
	for(size_t i = 0ull; i < MaxFramesToAverage; ++i) {
		dtList[i] = 0.0;
	}

	double dt = 0;
	
	// polling for events!
	QToQuitEventReceiver escaper;
	CommonAPI::InputSystem::ChannelReader<IMouseEventChannel> mouse;
	CommonAPI::InputSystem::ChannelReader<IKeyboardEventChannel> keyboard;
	
	uint32_t resourceIx = 0;
	while(escaper.keepOpen())
	{
		resourceIx++;
		if(resourceIx >= FRAMES_IN_FLIGHT) {
			resourceIx = 0;
		}
		
		// Timing
		auto renderStart = std::chrono::system_clock::now();
		dt = std::chrono::duration_cast<std::chrono::milliseconds>(renderStart-lastTime).count();
		lastTime = renderStart;
		
		// Calculate Simple Moving Average for FrameTime
		{
			time_sum -= dtList[frame_count];
			time_sum += dt;
			dtList[frame_count] = dt;
			frame_count++;
			if(frame_count >= MaxFramesToAverage) {
				frame_count = 0;
				frameDataFilled = true;
			}
		}
		double averageFrameTime = (frameDataFilled) ? (time_sum / (double)MaxFramesToAverage) : (time_sum / frame_count);
		// logger->log("averageFrameTime = %f",system::ILogger::ELL_INFO, averageFrameTime);
		
		// Calculate Next Presentation Time Stamp
		auto averageFrameTimeDuration = std::chrono::duration<double, std::milli>(averageFrameTime);
		auto nextPresentationTime = renderStart + averageFrameTimeDuration;
		auto nextPresentationTimeStamp = std::chrono::duration_cast<std::chrono::microseconds>(nextPresentationTime.time_since_epoch());
		
		// Input 
		inputSystem->getDefaultMouse(&mouse);
		inputSystem->getDefaultKeyboard(&keyboard);

		cam.beginInputProcessing(nextPresentationTimeStamp);
		mouse.consumeEvents([&](const IMouseEventChannel::range_t& events) -> void { cam.mouseProcess(events); }, logger.get());
		keyboard.consumeEvents([&](const IKeyboardEventChannel::range_t& events) -> void {
			cam.keyboardProcess(events); 
			escaper.process(events); 
		}, logger.get());
		cam.endInputProcessing(nextPresentationTimeStamp);
		
		auto& cb = cmdbuf[resourceIx];
		auto& fence = frameComplete[resourceIx];
		if (fence)
		while (device->waitForFences(1u,&fence.get(),false,MAX_TIMEOUT)==video::IGPUFence::ES_TIMEOUT)
		{
		}
		else
			fence = device->createFence(static_cast<video::IGPUFence::E_CREATE_FLAGS>(0));
		
		const auto viewMatrix = cam.getViewMatrix();
		const auto viewProjectionMatrix = cam.getConcatenatedMatrix();
				
		// safe to proceed
		cb->begin(0);
		{
			asset::SViewport vp;
			vp.minDepth = 1.f;
			vp.maxDepth = 0.f;
			vp.x = 0u;
			vp.y = 0u;
			vp.width = WIN_W;
			vp.height = WIN_H;
			cb->setViewport(0u, 1u, &vp);
		}

		// renderpass 
		uint32_t imgnum = 0u;
		swapchain->acquireNextImage(MAX_TIMEOUT,imageAcquire[resourceIx].get(),nullptr,&imgnum);
		{
			video::IGPUCommandBuffer::SRenderpassBeginInfo info;
			asset::SClearValue clearValues[2] ={};
			asset::VkRect2D area;
			clearValues[0].color.float32[0] = 0.1f;
			clearValues[0].color.float32[1] = 0.1f;
			clearValues[0].color.float32[2] = 0.1f;
			clearValues[0].color.float32[3] = 1.f;

			clearValues[1].depthStencil.depth = 0.0f;
			clearValues[1].depthStencil.stencil = 0.0f;

			info.renderpass = renderpass;
			info.framebuffer = fbo[imgnum];
			info.clearValueCount = 2u;
			info.clearValues = clearValues;
			info.renderArea.offset = { 0, 0 };
			info.renderArea.extent = { WIN_W, WIN_H };
			cb->beginRenderPass(&info,asset::ESC_INLINE);
		}

		{
			auto mv = viewMatrix;
			auto mvp = viewProjectionMatrix;
			core::matrix3x4SIMD normalMat;
			mv.getSub3x3InverseTranspose(normalMat);

			SBasicViewParameters uboData;
			memcpy(uboData.MV, mv.pointer(), sizeof(mv));
			memcpy(uboData.MVP, mvp.pointer(), sizeof(mvp));
			memcpy(uboData.NormalMat, normalMat.pointer(), sizeof(normalMat));
			
			asset::SBufferRange<video::IGPUBuffer> range;
			range.buffer = gpuubo;
			range.offset = 0ull;
			range.size = sizeof(uboData);
			device->updateBufferRangeViaStagingBuffer(graphicsQueue, range, &uboData);
		}

		// cube envmap handle
		{
			cb->bindComputePipeline(gpuComputePipeline.get());
			cb->bindDescriptorSets(EPBP_COMPUTE, gpuComputePipeline->getLayout(), 0u, 1u, &descriptorSets0[imgnum].get(), nullptr);
			cb->bindDescriptorSets(EPBP_COMPUTE, gpuComputePipeline->getLayout(), 1u, 1u, &uboDescriptorSet1.get(), nullptr);
			cb->bindDescriptorSets(EPBP_COMPUTE, gpuComputePipeline->getLayout(), 3u, 1u, &descriptorSet3.get(), nullptr);
			cb->dispatch(dispatchInfo.workGroupCount[0], dispatchInfo.workGroupCount[1], dispatchInfo.workGroupCount[2]);
		}
		// TODO: tone mapping and stuff

		// Copy HDR Image to SwapChain
		auto srcImgViewCreationParams = outHDRImageViews[imgnum]->getCreationParameters();
		auto dstImgViewCreationParams = fbo[imgnum]->getCreationParameters().attachments[0]->getCreationParameters();

		SImageBlit blit = {};
		blit.srcOffsets[0] = {0, 0, 0};
		blit.srcOffsets[1] = {WIN_W, WIN_H, 1};
		blit.srcSubresource.aspectMask = srcImgViewCreationParams.subresourceRange.aspectMask;
		blit.srcSubresource.mipLevel = srcImgViewCreationParams.subresourceRange.baseMipLevel;
		blit.srcSubresource.baseArrayLayer = srcImgViewCreationParams.subresourceRange.baseArrayLayer;
		blit.srcSubresource.layerCount = srcImgViewCreationParams.subresourceRange.layerCount;
		blit.dstOffsets[0] = {0, 0, 0};
		blit.dstOffsets[1] = {WIN_W, WIN_H, 1};
		blit.dstSubresource.aspectMask = dstImgViewCreationParams.subresourceRange.aspectMask;
		blit.dstSubresource.mipLevel = dstImgViewCreationParams.subresourceRange.baseMipLevel;
		blit.dstSubresource.baseArrayLayer = dstImgViewCreationParams.subresourceRange.baseArrayLayer;
		blit.dstSubresource.layerCount = dstImgViewCreationParams.subresourceRange.layerCount;

		auto srcImg = srcImgViewCreationParams.image;
		auto dstImg = dstImgViewCreationParams.image;
		cb->blitImage(srcImg.get(), EIL_GENERAL, dstImg.get(), EIL_COLOR_ATTACHMENT_OPTIMAL, 1u, &blit , ISampler::ETF_NEAREST);

		cb->endRenderPass();
		cb->end();
		
		CommonAPI::Submit(device.get(), swapchain.get(), cb.get(), graphicsQueue, imageAcquire[resourceIx].get(), renderFinished[resourceIx].get(), fence.get());
		CommonAPI::Present(device.get(), swapchain.get(), graphicsQueue, renderFinished[resourceIx].get(), imgnum);
	}
	
	const auto& fboCreationParams = fbo[0]->getCreationParameters();
	auto gpuSourceImageView = fboCreationParams.attachments[0];

	bool status = ext::ScreenShot::createScreenShot(device.get(), queues[decltype(initOutput)::EQT_TRANSFER_UP], renderFinished[0].get(), gpuSourceImageView.get(), assetManager.get(), "ScreenShot.png");
	assert(status);
}
