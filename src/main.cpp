#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_main.h>

// the vertex input layout
struct Vertex
{
    float x, y, z;    // vec3 position
    float r, g, b, a; // vec4 color
};

// a list of vertices
static Vertex vertices[]{
    {0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},   // top vertex
    {-0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}, // bottom left vertex
    {0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f}   // bottom right vertex
};

struct UniformBuffer
{
    float time;
    // you can add other properties here
};

static UniformBuffer timeUniform{};

SDL_Window *window;
SDL_GPUDevice *device;
SDL_GPUBuffer *vertexBuffer;
SDL_GPUTransferBuffer *transferBuffer;
SDL_GPUGraphicsPipeline *graphicsPipeline;

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
#if defined(__APPLE__)
    // macOS and iOS use Metal
    const char *deviceName = "Metal";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_METALLIB;
    const char *vertexShaderPath = "src/shaders/simple.vert.metallib";
    const char *fragmentShaderPath = "src/shaders/simple.frag.metallib";
    const char *entryPoint = "main0";
#else
    // Windows, Linux, Android, etc. use SPIR-V
    const char *deviceName = "Vulkan";
    const SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
    const char *vertexShaderPath = "src/shaders/simple.vert.spv";
    const char *fragmentShaderPath = "src/shaders/simple.frag.spv";
    const char *entryPoint = "main";
#endif

    // create a window
    window = SDL_CreateWindow("SDL_GPU_Kit", 1280, 720, SDL_WINDOW_RESIZABLE);

    // create the device
    device = SDL_CreateGPUDevice(shaderFormat, false, deviceName);
    if (!device)
    {
        SDL_Log("Failed to create device: %s", SDL_GetError());
        return SDL_AppResult::SDL_APP_FAILURE;
    }

    SDL_ClaimWindowForGPUDevice(device, window);

    // load the vertex shader code
    size_t vertexCodeSize;
    void *vertexCode = SDL_LoadFile(vertexShaderPath, &vertexCodeSize);

    // create the vertex shader
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code_size = vertexCodeSize;
    vertexInfo.code = (Uint8 *)vertexCode;
    vertexInfo.entrypoint = entryPoint;
    vertexInfo.format = shaderFormat;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    vertexInfo.num_samplers = 0;
    vertexInfo.num_storage_buffers = 0;
    vertexInfo.num_storage_textures = 0;
    vertexInfo.num_uniform_buffers = 0;

    SDL_GPUShader *vertexShader = SDL_CreateGPUShader(device, &vertexInfo);

    // free the file
    SDL_free(vertexCode);

    // load the fragment shader code
    size_t fragmentCodeSize;
    void *fragmentCode = SDL_LoadFile(fragmentShaderPath, &fragmentCodeSize);

    // create the fragment shader
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code_size = fragmentCodeSize;
    fragmentInfo.code = (Uint8 *)fragmentCode;
    fragmentInfo.entrypoint = entryPoint;
    fragmentInfo.format = shaderFormat;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 0;
    fragmentInfo.num_storage_buffers = 0;
    fragmentInfo.num_storage_textures = 0;
    fragmentInfo.num_uniform_buffers = 1;

    SDL_GPUShader *fragmentShader = SDL_CreateGPUShader(device, &fragmentInfo);

    // free the file
    SDL_free(fragmentCode);

    // create the graphics pipeline
    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    // describe the vertex buffers
    SDL_GPUVertexBufferDescription vertexBufferDesctiptions[1];
    vertexBufferDesctiptions[0].slot = 0;
    vertexBufferDesctiptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertexBufferDesctiptions[0].instance_step_rate = 0;
    vertexBufferDesctiptions[0].pitch = sizeof(Vertex);

    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDesctiptions;

    // describe the vertex attribute
    SDL_GPUVertexAttribute vertexAttributes[2];

    // a_position
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].location = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[0].offset = 0;

    // a_color
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].location = 1;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    vertexAttributes[1].offset = sizeof(float) * 3;

    pipelineInfo.vertex_input_state.num_vertex_attributes = 2;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    // describe the color target
    SDL_GPUColorTargetDescription colorTargetDescriptions[1];
    colorTargetDescriptions[0] = {};
    colorTargetDescriptions[0].format = SDL_GetGPUSwapchainTextureFormat(device, window);

    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = colorTargetDescriptions;

    // create the pipeline
    graphicsPipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);

    // we don't need to store the shaders after creating the pipeline
    SDL_ReleaseGPUShader(device, vertexShader);
    SDL_ReleaseGPUShader(device, fragmentShader);

    // create the vertex buffer
    SDL_GPUBufferCreateInfo bufferInfo{};
    bufferInfo.size = sizeof(vertices);
    bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexBuffer = SDL_CreateGPUBuffer(device, &bufferInfo);

    // create a transfer buffer to upload to the vertex buffer
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.size = sizeof(vertices);
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferBuffer = SDL_CreateGPUTransferBuffer(device, &transferInfo);

    // fill the transfer buffer
    Vertex *data = (Vertex *)SDL_MapGPUTransferBuffer(device, transferBuffer, false);

    SDL_memcpy(data, (void *)vertices, sizeof(vertices));

    SDL_UnmapGPUTransferBuffer(device, transferBuffer);

    // start a copy pass
    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    // where is the data
    SDL_GPUTransferBufferLocation location{};
    location.transfer_buffer = transferBuffer;
    location.offset = 0;

    // where to upload the data
    SDL_GPUBufferRegion region{};
    region.buffer = vertexBuffer;
    region.size = sizeof(vertices);
    region.offset = 0;

    // upload the data
    SDL_UploadToGPUBuffer(copyPass, &location, &region, true);

    // end the copy pass
    SDL_EndGPUCopyPass(copyPass);
    SDL_SubmitGPUCommandBuffer(commandBuffer);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    // acquire the command buffer
    SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device);

    // get the swapchain texture
    SDL_GPUTexture *swapchainTexture;
    Uint32 width, height;
    SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchainTexture, &width, &height);

    // end the frame early if a swapchain texture is not available
    if (swapchainTexture == NULL)
    {
        // you must always submit the command buffer
        SDL_SubmitGPUCommandBuffer(commandBuffer);
        return SDL_APP_CONTINUE;
    }

    // create the color target
    SDL_GPUColorTargetInfo colorTargetInfo{};
    colorTargetInfo.clear_color = {240 / 255.0f, 240 / 255.0f, 240 / 255.0f, 255 / 255.0f};
    colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
    colorTargetInfo.texture = swapchainTexture;

    // begin a render pass
    SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, NULL);

    // bind the pipeline
    SDL_BindGPUGraphicsPipeline(renderPass, graphicsPipeline);

    // bind the vertex buffer
    SDL_GPUBufferBinding bufferBindings[1];
    bufferBindings[0].buffer = vertexBuffer;
    bufferBindings[0].offset = 0;

    SDL_BindGPUVertexBuffers(renderPass, 0, bufferBindings, 1);

    // update the time uniform
    timeUniform.time = SDL_GetTicksNS() / 1e9f;
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &timeUniform, sizeof(UniformBuffer));

    // issue a draw call
    SDL_DrawGPUPrimitives(renderPass, 3, 1, 0, 0);

    // end the render pass
    SDL_EndGPURenderPass(renderPass);

    // submit the command buffer
    SDL_SubmitGPUCommandBuffer(commandBuffer);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    // close the window on request
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
    {
        return SDL_APP_SUCCESS;
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    // release buffers
    SDL_ReleaseGPUBuffer(device, vertexBuffer);
    SDL_ReleaseGPUTransferBuffer(device, transferBuffer);

    // release the pipeline
    SDL_ReleaseGPUGraphicsPipeline(device, graphicsPipeline);

    // destroy the GPU device
    SDL_DestroyGPUDevice(device);

    // destroy the window
    SDL_DestroyWindow(window);
}
