#include <cmath>
#include "imgui_impl_nvn.hpp"
#include "imgui_hid_mappings.h"
#include "lib.hpp"
#include "helpers.h"
#include "logger/Logger.hpp"

#include "nn/os.h"
#include "nn/hid.h"

#include "helpers/InputHelper.h"
#include "MemoryPoolMaker.h"

#include "files/imgui_shader.h"
#include "files/JetBrainsMonoNL-Regular.h"
#include "init.h"

#define UBOSIZE 0x1000

typedef float Matrix44f[4][4];

// orthographic matrix used for shader
static Matrix44f projMatrix = {};

// orthographic projection matrix calc from glm
void orthoRH_ZO(Matrix44f &Result, float left, float right, float bottom, float top, float zNear, float zFar) {

  Result[0][0] = static_cast<float>(2) / (right - left);
  Result[0][1] = 0.0f;
  Result[0][2] = 0.0f;
  Result[0][3] = 0.0f;

  Result[1][0] = 0.0f;
  Result[1][1] = static_cast<float>(2) / (top - bottom);
  Result[1][2] = 0.0f;
  Result[1][3] = 0.0f;

  Result[2][0] = 0.0f;
  Result[2][1] = 0.0f;
  Result[2][2] = -static_cast<float>(1) / (zFar - zNear);
  Result[2][3] = 0.0f;

  Result[3][0] = -(right + left) / (right - left);
  Result[3][1] = -(top + bottom) / (top - bottom);
  Result[3][2] = -zNear / (zFar - zNear);
  Result[3][3] = 1.0f;
}

namespace ImguiNvnBackend {
  void loadIni();
  void saveIni();

  // doesnt get used anymore really, as back when it was needed i had a simplified shader to test with, but now I just test with the actual imgui shader
  void initTestShader() {

    auto bd = getBackendData();
    bd->testShaderBinary = ImguiShaderCompiler::CompileShader("test");

    bd->testShaderBuffer = IM_NEW(MemoryBuffer)(bd->testShaderBinary.size, bd->testShaderBinary.ptr,
                                                nvn::MemoryPoolFlags::CPU_UNCACHED |
                                                nvn::MemoryPoolFlags::GPU_CACHED |
                                                nvn::MemoryPoolFlags::SHADER_CODE);

    EXL_ASSERT(bd->testShaderBuffer->IsBufferReady(), "Shader Buffer was not ready! unable to continue.");

    BinaryHeader offsetData = BinaryHeader((u32 *) bd->testShaderBinary.ptr);

    nvn::BufferAddress addr = bd->testShaderBuffer->GetBufferAddress();

    nvn::ShaderData &vertShaderData = bd->testShaderDatas[0];
    vertShaderData.data = addr + offsetData.mVertexDataOffset;
    vertShaderData.control = bd->testShaderBinary.ptr + offsetData.mVertexControlOffset;

    nvn::ShaderData &fragShaderData = bd->testShaderDatas[1];
    fragShaderData.data = addr + offsetData.mFragmentDataOffset;
    fragShaderData.control = bd->testShaderBinary.ptr + offsetData.mFragmentControlOffset;

    EXL_ASSERT(bd->testShader.Initialize(bd->device), "Unable to Init Program!");
    EXL_ASSERT(bd->testShader.SetShaders(2, bd->testShaderDatas), "Unable to Set Shaders!");

    Logger::log("Test Shader Setup.\n");

  }

  // neat tool to cycle through all loaded textures in a texture pool
  int texIDSelector() {
    {

      static int curId = 256;
      static int downCounter = 0;
      static int upCounter = 0;

      if (InputHelper::isButtonPress(nn::hid::NpadButton::Left)) {
        curId--;
        Logger::log("ID: %d\n", curId);
      } else if (InputHelper::isButtonHold(nn::hid::NpadButton::Left)) {

        downCounter++;
        if (downCounter > 30) {
          curId--;
          Logger::log("ID: %d\n", curId);
        }
      } else {
        downCounter = 0;
      }

      if (InputHelper::isButtonPress(nn::hid::NpadButton::Right)) {
        curId++;
        Logger::log("ID: %d\n", curId);
      } else if (InputHelper::isButtonHold(nn::hid::NpadButton::Right)) {

        upCounter++;
        if (upCounter > 30) {
          curId++;
          Logger::log("ID: %d\n", curId);
        }
      } else {
        upCounter = 0;
      }

      /* fun values with bd->device->GetTextureHandle(curId, 256):
       * 282 = Window Texture
       * 393 = Some sort of render pass (shadow?) nvm it just seems to be the first occurrence of many more textures like it
       * 257 = debug font texture
       */

      return curId;
    }
  }

  // places ImDrawVerts, starting at startIndex, that use the x,y,width, and height values to define vertex coords
  void createQuad(ImDrawVert *verts, int startIndex, int x, int y, int width, int height, ImU32 col) {

    float minXVal = x;
    float maxXVal = x + width;
    float minYVal = y; // 400
    float maxYVal = y + height; // 400

    // top left
    ImDrawVert p1 = {
        .pos = ImVec2(minXVal, minYVal),
        .uv = ImVec2(0.0f, 0.0f),
        .col = col
    };
    // top right
    ImDrawVert p2 = {
        .pos = ImVec2(minXVal, maxYVal),
        .uv = ImVec2(0.0f, 1.0f),
        .col = col
    };
    // bottom left
    ImDrawVert p3 = {
        .pos = ImVec2(maxXVal, minYVal),
        .uv = ImVec2(1.0f, 0.0f),
        .col = col
    };
    // bottom right
    ImDrawVert p4 = {
        .pos = ImVec2(maxXVal, maxYVal),
        .uv = ImVec2(1.0f, 1.0f),
        .col = col
    };

    verts[startIndex] = p4;
    verts[startIndex + 1] = p2;
    verts[startIndex + 2] = p1;

    verts[startIndex + 3] = p1;
    verts[startIndex + 4] = p3;
    verts[startIndex + 5] = p4;
  }

  // this function is mainly what I used to debug the rendering of ImGui, so code is a bit messier
  void renderTestShader(ImDrawData *drawData) {

    auto bd = getBackendData();
    auto io = ImGui::GetIO();

    constexpr int triVertCount = 3;
    constexpr int quadVertCount = triVertCount * 2;

    int quadCount = 1; // modify to reflect how many quads need to be drawn per frame

    int pointCount = quadVertCount * quadCount;

    size_t totalVtxSize = pointCount * sizeof(ImDrawVert);
    if (!bd->vtxBuffer || bd->vtxBuffer->GetPoolSize() < totalVtxSize) {
      if (bd->vtxBuffer) {
        bd->vtxBuffer->Finalize();
        IM_FREE(bd->vtxBuffer);
      }
      bd->vtxBuffer = IM_NEW(MemoryBuffer)(totalVtxSize);
      Logger::log("(Re)sized Vertex Buffer to Size: %d\n", totalVtxSize);
    }

    if (!bd->vtxBuffer->IsBufferReady()) {
      Logger::log("Cannot Draw Data! Buffers are not Ready.\n");
      return;
    }

    ImDrawVert *verts = (ImDrawVert *) bd->vtxBuffer->GetMemPtr();

    float scale = 3.0f;

    float imageX = 1 * scale; // bd->fontTexture.GetWidth();
    float imageY = 1 * scale; // bd->fontTexture.GetHeight();

    createQuad(verts, 0, (io.DisplaySize.x / 2) - (imageX), (io.DisplaySize.y / 2) - (imageY), imageX, imageY,
               IM_COL32_WHITE);

    bd->cmdBuf->BeginRecording();
    bd->cmdBuf->BindProgram(&bd->shaderProgram, nvn::ShaderStageBits::VERTEX | nvn::ShaderStageBits::FRAGMENT);

    bd->cmdBuf->BindUniformBuffer(nvn::ShaderStage::VERTEX, 0, *bd->uniformMemory, UBOSIZE);
    bd->cmdBuf->UpdateUniformBuffer(*bd->uniformMemory, UBOSIZE, 0, sizeof(projMatrix), &projMatrix);

    bd->cmdBuf->BindVertexBuffer(0, (*bd->vtxBuffer), bd->vtxBuffer->GetPoolSize());

    setRenderStates();

//        bd->cmdBuf->BindTexture(nvn::ShaderStage::FRAGMENT, 0, bd->fontTexHandle);

    bd->cmdBuf->DrawArrays(nvn::DrawPrimitive::TRIANGLES, 0, pointCount);

    auto handle = bd->cmdBuf->EndRecording();
    bd->queue->SubmitCommands(1, &handle);
  }

// backend impl

  NvnBackendData *getBackendData() {
    NvnBackendData *result = ImGui::GetCurrentContext() ? (NvnBackendData *) ImGui::GetIO().BackendRendererUserData
                                                        : nullptr;
    EXL_ASSERT(result, "Backend has not been initialized!");
    return result;
  }

  bool createShaders() {

    auto bd = getBackendData();

    if (false) {
      Logger::log("GLSLC compiler can be used!\n");

      ImguiShaderCompiler::InitializeCompiler();

      bd->imguiShaderBinary = ImguiShaderCompiler::CompileShader("imgui");

    } else {
      Logger::log("Unable to compile shaders at runtime. falling back to pre-compiled shaders.\n");

//      FsHelper::LoadData loadData = {
//          .path = "sd:/mp1r/ShaderData/imgui.bin"
//      };
//
//      FsHelper::loadFileFromPath(loadData);

      void *buf = nn::init::GetAllocator()->Allocate(romfs_ShaderData_imgui_bin_len);
      EXL_ASSERT(buf, "Failed to Allocate Buffer! File Size: %d", romfs_ShaderData_imgui_bin_len);
      memcpy(buf, romfs_ShaderData_imgui_bin, romfs_ShaderData_imgui_bin_len);

      Logger::log("Loaded shader\n");

      bd->imguiShaderBinary.size = romfs_ShaderData_imgui_bin_len;
      bd->imguiShaderBinary.ptr = (u8 *) buf;
    }

    if (bd->imguiShaderBinary.size > 0) {
      return true;
    }

    return false;
  }

  bool setupFont() {

    Logger::log("Setting up ImGui Font.\n");

    auto bd = getBackendData();

    ImGuiIO &io = ImGui::GetIO();

    // init sampler and texture pools

    int sampDescSize = 0;
    bd->device->GetInteger(nvn::DeviceInfo::SAMPLER_DESCRIPTOR_SIZE, &sampDescSize);
    int texDescSize = 0;
    bd->device->GetInteger(nvn::DeviceInfo::TEXTURE_DESCRIPTOR_SIZE, &texDescSize);

    int sampMemPoolSize = sampDescSize * MaxSampDescriptors;
    int texMemPoolSize = texDescSize * MaxTexDescriptors;
    int totalPoolSize = ALIGN_UP(sampMemPoolSize + texMemPoolSize, 0x1000);
    if (!MemoryPoolMaker::createPool(&bd->sampTexMemPool, totalPoolSize)) {
      Logger::log("Failed to Create Texture/Sampler Memory Pool!\n");
      return false;
    }

    if (!bd->samplerPool.Initialize(&bd->sampTexMemPool, 0, MaxSampDescriptors)) {
      Logger::log("Failed to Create Sampler Pool!\n");
      return false;
    }

    if (!bd->texPool.Initialize(&bd->sampTexMemPool, sampMemPoolSize, MaxTexDescriptors)) {
      Logger::log("Failed to Create Texture Pool!\n");
      return false;
    }

    // convert imgui font texels

    unsigned char *pixels;
    int width, height, pixelByteSize;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height, &pixelByteSize);
    int texPoolSize = pixelByteSize * width * height;

    if (!MemoryPoolMaker::createPool(&bd->fontMemPool, ALIGN_UP(texPoolSize, 0x1000),
                                     nvn::MemoryPoolFlags::CPU_UNCACHED | nvn::MemoryPoolFlags::GPU_CACHED)) {
      Logger::log("Failed to Create Font Memory Pool!\n");
      return false;
    }

    bd->texBuilder.SetDefaults()
        .SetDevice(bd->device)
        .SetTarget(nvn::TextureTarget::TARGET_2D)
        .SetFormat(nvn::Format::RGBA8)
        .SetSize2D(width, height)
        .SetStorage(&bd->fontMemPool, 0);

    if (!bd->fontTexture.Initialize(&bd->texBuilder)) {
      Logger::log("Failed to Create Font Texture!\n");
      return false;
    }

    // setup font texture

    nvn::CopyRegion region = {
        .xoffset = 0,
        .yoffset = 0,
        .zoffset = 0,
        .width = bd->fontTexture.GetWidth(),
        .height = bd->fontTexture.GetHeight(),
        .depth = 1
    };

    bd->fontTexture.WriteTexels(nullptr, &region, pixels);
    bd->fontTexture.FlushTexels(nullptr, &region);

    bd->samplerBuilder.SetDefaults()
        .SetDevice(bd->device)
        .SetMinMagFilter(nvn::MinFilter::LINEAR, nvn::MagFilter::LINEAR)
        .SetWrapMode(nvn::WrapMode::CLAMP, nvn::WrapMode::CLAMP, nvn::WrapMode::CLAMP);

    if (!bd->fontSampler.Initialize(&bd->samplerBuilder)) {
      Logger::log("Failed to Init Font Sampler!\n");
      return false;
    }

    bd->textureId = 257;
    bd->samplerId = 257;

    bd->texPool.RegisterTexture(bd->textureId, &bd->fontTexture, nullptr);
    bd->samplerPool.RegisterSampler(bd->samplerId, &bd->fontSampler);

    bd->fontTexHandle = bd->device->GetTextureHandle(bd->textureId, bd->samplerId);
    io.Fonts->SetTexID(&bd->fontTexHandle);

    Logger::log("Finished.\n");

    return true;
  }

  bool setupShaders(u8 *shaderBinary, ulong binarySize) {

    Logger::log("Setting up ImGui Shaders.\n");

    auto bd = getBackendData();

    if (!bd->shaderProgram.Initialize(bd->device)) {
      Logger::log("Failed to Initialize Shader Program!");
      return false;
    }

    bd->shaderMemory = IM_NEW(MemoryBuffer)(binarySize, shaderBinary, nvn::MemoryPoolFlags::CPU_UNCACHED |
                                                                      nvn::MemoryPoolFlags::GPU_CACHED |
                                                                      nvn::MemoryPoolFlags::SHADER_CODE);

    if (!bd->shaderMemory->IsBufferReady()) {
      Logger::log("Shader Memory Pool not Ready! Unable to continue.\n");
      return false;
    }

    BinaryHeader offsetData = BinaryHeader((u32 *) shaderBinary);

    nvn::BufferAddress addr = bd->shaderMemory->GetBufferAddress();

    nvn::ShaderData &vertShaderData = bd->shaderDatas[0];
    vertShaderData.data = addr + offsetData.mVertexDataOffset;
    vertShaderData.control = shaderBinary + offsetData.mVertexControlOffset;

    nvn::ShaderData &fragShaderData = bd->shaderDatas[1];
    fragShaderData.data = addr + offsetData.mFragmentDataOffset;
    fragShaderData.control = shaderBinary + offsetData.mFragmentControlOffset;

    if (!bd->shaderProgram.SetShaders(2, bd->shaderDatas)) {
      Logger::log("Failed to Set shader data for program.\n");
      return false;
    }

    bd->shaderProgram.SetDebugLabel("ImGuiShader");

    // Uniform Block Object Memory Setup

    bd->uniformMemory = IM_NEW(MemoryBuffer)(UBOSIZE);

    if (!bd->uniformMemory->IsBufferReady()) {
      Logger::log("Uniform Memory Pool not Ready! Unable to continue.\n");
      return false;
    }

    // setup vertex attrib & stream

    bd->attribStates[0].SetDefaults().SetFormat(nvn::Format::RG32F, offsetof(ImDrawVert, pos)); // pos
    bd->attribStates[1].SetDefaults().SetFormat(nvn::Format::RG32F, offsetof(ImDrawVert, uv)); // uv
    bd->attribStates[2].SetDefaults().SetFormat(nvn::Format::RGBA8, offsetof(ImDrawVert, col)); // color

    bd->streamState.SetDefaults().SetStride(sizeof(ImDrawVert));

    Logger::log("Finished.\n");

    return true;
  }

  void InitBackend(const NvnBackendInitInfo &initInfo) {
    ImGuiIO &io = ImGui::GetIO();
    EXL_ASSERT(!io.BackendRendererUserData, "Already Initialized Imgui Backend!");

    io.BackendPlatformName = "Switch";
    io.BackendRendererName = "imgui_impl_nvn";
    io.IniFilename = nullptr;
    io.MouseDrawCursor = true;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

    io.DisplaySize = ImVec2(1600, 900); // default size

    // calculate ortho matrix
    orthoRH_ZO(projMatrix, 0.0f, io.DisplaySize.x, io.DisplaySize.y, 0.0f, -1.0f, 1.0f);

    auto *bd = IM_NEW(NvnBackendData)();
    io.BackendRendererUserData = (void *) bd;

    bd->device = initInfo.device;
    bd->queue = initInfo.queue;
    bd->cmdBuf = initInfo.cmdBuf;
    bd->isInitialized = false;

    io.Fonts->AddFontFromMemoryCompressedTTF(
        JetBrainsMonoNL_compressed_data, JetBrainsMonoNL_compressed_size,
        18.f
    );

    if (createShaders()) {
      Logger::log("Shader Binaries Loaded! Setting up Render Data.\n");

      if (bd->isUseTestShader)
        initTestShader();

      if (setupShaders(bd->imguiShaderBinary.ptr, bd->imguiShaderBinary.size) && setupFont()) {
        Logger::log("Rendering Setup!\n");

        bd->isInitialized = true;

      } else {
        Logger::log("Failed to Setup Render Data!\n");
      }
    }
  }

  void ShutdownBackend() {

  }

  void updateMouse(ImGuiIO &io) {
    ImVec2 mousePos(0, 0);
    InputHelper::getMouseCoords(&mousePos.x, &mousePos.y);
    io.AddMousePosEvent(mousePos.x, mousePos.y);

    ImVec2 scrollDelta(0, 0);
    InputHelper::getScrollDelta(&scrollDelta.x, &scrollDelta.y);

    if (scrollDelta.x != 0.0f)
      io.AddMouseWheelEvent(0.0f, scrollDelta.x > 0.0f ? 0.5f : -0.5f);

    for (auto [im_k, nx_k]: mouse_mapping) {
      if (InputHelper::isMousePress((nn::hid::MouseButton) nx_k))
        io.AddMouseButtonEvent((ImGuiMouseButton) im_k, true);
      else if (InputHelper::isMouseRelease((nn::hid::MouseButton) nx_k))
        io.AddMouseButtonEvent((ImGuiMouseButton) im_k, false);
    }
  }

  void updateKeyboard(ImGuiIO &io) {
    for (auto [im_k, nx_k]: key_mapping) {
      if (InputHelper::isKeyPress((nn::hid::KeyboardKey) nx_k)) {
        io.AddKeyEvent((ImGuiKey) im_k, true);
      } else if (InputHelper::isKeyRelease((nn::hid::KeyboardKey) nx_k)) {
        io.AddKeyEvent((ImGuiKey) im_k, false);
      }
    }
  }

  void updateGamepad(ImGuiIO &io) {
    for (auto [im_k, nx_k]: npad_mapping) {
      if (InputHelper::isButtonPress((nn::hid::NpadButton) nx_k))
        io.AddKeyEvent((ImGuiKey) im_k, true);
      else if (InputHelper::isButtonRelease((nn::hid::NpadButton) nx_k))
        io.AddKeyEvent((ImGuiKey) im_k, false);
    }

    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, InputHelper::getLeftStickLeft() > 0.8f, InputHelper::getLeftStickLeft());
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, InputHelper::getLeftStickRight() > 0.8f, InputHelper::getLeftStickRight());
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, InputHelper::getLeftStickUp() > 0.8f, InputHelper::getLeftStickUp());
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, InputHelper::getLeftStickDown() > 0.8f, InputHelper::getLeftStickDown());

    io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickLeft, InputHelper::getRightStickLeft() > 0.8f, InputHelper::getRightStickLeft());
    io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickRight, InputHelper::getRightStickRight() > 0.8f, InputHelper::getRightStickRight());
    io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickUp, InputHelper::getRightStickUp() > 0.8f, InputHelper::getRightStickUp());
    io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickDown, InputHelper::getRightStickDown() > 0.8f, InputHelper::getRightStickDown());
  }

  void updateInput() {

    ImGuiIO &io = ImGui::GetIO();
    updateKeyboard(io);
    updateMouse(io);

    if (InputHelper::isInputToggled()) {
      updateGamepad(io);
    }
  }

  void newFrame() {
    ImGuiIO &io = ImGui::GetIO();
    auto *bd = getBackendData();

    nn::TimeSpan curTick = nn::os::GetSystemTick().ToTimeSpan();
    nn::TimeSpan prevTick(bd->lastTick);
    io.DeltaTime = fabsf((float) (curTick - prevTick).GetNanoSeconds() / 1e9f);

    bd->lastTick = curTick;

    InputHelper::updatePadState(); // update input helper

    updateInput(); // update backend inputs
  }

  void setRenderStates() {

    auto bd = getBackendData();

    nvn::PolygonState polyState;
    polyState.SetDefaults();
    polyState.SetPolygonMode(nvn::PolygonMode::FILL);
    polyState.SetCullFace(nvn::Face::NONE);
    polyState.SetFrontFace(nvn::FrontFace::CCW);
    bd->cmdBuf->BindPolygonState(&polyState);

    nvn::ColorState colorState;
    colorState.SetDefaults();
    colorState.SetLogicOp(nvn::LogicOp::COPY);
    colorState.SetAlphaTest(nvn::AlphaFunc::ALWAYS);
    for (int i = 0; i < 8; ++i) {
      colorState.SetBlendEnable(i, true);
    }
    bd->cmdBuf->BindColorState(&colorState);

    nvn::BlendState blendState;
    blendState.SetDefaults();
    blendState.SetBlendFunc(nvn::BlendFunc::SRC_ALPHA, nvn::BlendFunc::ONE_MINUS_SRC_ALPHA, nvn::BlendFunc::ONE,
                            nvn::BlendFunc::ZERO);
    blendState.SetBlendEquation(nvn::BlendEquation::ADD, nvn::BlendEquation::ADD);
    bd->cmdBuf->BindBlendState(&blendState);

    bd->cmdBuf->BindVertexAttribState(3, bd->attribStates);
    bd->cmdBuf->BindVertexStreamState(1, &bd->streamState);

    bd->cmdBuf->SetTexturePool(&bd->texPool);
    bd->cmdBuf->SetSamplerPool(&bd->samplerPool);
  }

  void renderDrawData(ImDrawData *drawData) {

    // we dont need to process any data if it isnt valid
    if (!drawData->Valid) {
//            Logger::log("Draw Data was Invalid! Skipping Render.");
      return;
    }
    // if we dont have any command lists to draw, we can stop here
    if (drawData->CmdListsCount == 0) {
//            Logger::log("Command List was Empty! Skipping Render.\n");
      return;
    }

    // get both the main backend data and IO from ImGui
    auto bd = getBackendData();
    ImGuiIO &io = ImGui::GetIO();

    // if something went wrong during backend setup, don't try to render anything
    if (!bd->isInitialized) {
      Logger::log("Backend Data was not fully initialized!\n");
      return;
    }

    // disable imgui rendering if we are using the test shader code
    if (bd->isUseTestShader) {
      renderTestShader(drawData);
      return;
    }

    // initializes/resizes buffer used for all vertex data created by ImGui
    size_t totalVtxSize = drawData->TotalVtxCount * sizeof(ImDrawVert);
    if (!bd->vtxBuffer || bd->vtxBuffer->GetPoolSize() < totalVtxSize) {
      if (bd->vtxBuffer) {
        bd->vtxBuffer->Finalize();
        IM_FREE(bd->vtxBuffer);
        Logger::log("Resizing Vertex Buffer to Size: %d\n", totalVtxSize);
      } else {
        Logger::log("Initializing Vertex Buffer to Size: %d\n", totalVtxSize);
      }

      bd->vtxBuffer = IM_NEW(MemoryBuffer)(totalVtxSize);
    }

    // initializes/resizes buffer used for all index data created by ImGui
    size_t totalIdxSize = drawData->TotalIdxCount * sizeof(ImDrawIdx);
    if (!bd->idxBuffer || bd->idxBuffer->GetPoolSize() < totalIdxSize) {
      if (bd->idxBuffer) {

        bd->idxBuffer->Finalize();
        IM_FREE(bd->idxBuffer);

        Logger::log("Resizing Index Buffer to Size: %d\n", totalIdxSize);
      } else {
        Logger::log("Initializing Index Buffer to Size: %d\n", totalIdxSize);
      }

      bd->idxBuffer = IM_NEW(MemoryBuffer)(totalIdxSize);

    }

    // if we fail to resize/init either buffers, end execution before we try to use said invalid buffer(s)
    if (!(bd->vtxBuffer->IsBufferReady() && bd->idxBuffer->IsBufferReady())) {
      Logger::log("Cannot Draw Data! Buffers are not Ready.\n");
      return;
    }

    orthoRH_ZO(projMatrix, 0.0f, io.DisplaySize.x, io.DisplaySize.y, 0.0f, -1.0f, 1.0f);

    bd->cmdBuf->BeginRecording(); // start recording our commands to the cmd buffer

    bd->cmdBuf->BindProgram(&bd->shaderProgram, nvn::ShaderStageBits::VERTEX |
                                                nvn::ShaderStageBits::FRAGMENT); // bind main imgui shader

    bd->cmdBuf->BindUniformBuffer(nvn::ShaderStage::VERTEX, 0, *bd->uniformMemory,
                                  UBOSIZE); // bind uniform block ptr
    bd->cmdBuf->UpdateUniformBuffer(*bd->uniformMemory, UBOSIZE, 0, sizeof(projMatrix),
                                    &projMatrix); // add projection matrix data to uniform data

    setRenderStates(); // sets up the rest of the render state, required so that our shader properly gets drawn to the screen

    size_t vtxOffset = 0, idxOffset = 0;
    nvn::TextureHandle boundTextureHandle = 0;

    // load data into buffers, and process draw commands
    for (int i = 0; i < drawData->CmdListsCount; i++) {

      auto cmdList = drawData->CmdLists[i];

      // calc vertex and index buffer sizes
      size_t vtxSize = cmdList->VtxBuffer.Size * sizeof(ImDrawVert);
      size_t idxSize = cmdList->IdxBuffer.Size * sizeof(ImDrawIdx);

      // bind vtx buffer at the current offset
      bd->cmdBuf->BindVertexBuffer(0, (*bd->vtxBuffer) + vtxOffset, vtxSize);

      // copy data from imgui command list into our gpu dedicated memory
      memcpy(bd->vtxBuffer->GetMemPtr() + vtxOffset, cmdList->VtxBuffer.Data, vtxSize);
      memcpy(bd->idxBuffer->GetMemPtr() + idxOffset, cmdList->IdxBuffer.Data, idxSize);

      for (auto cmd: cmdList->CmdBuffer) {


//                ImVec2 origRes(1280.0f, 720.0f);
//                ImVec2 newRes = io.DisplaySize; // (1600.0f, 900.0f);

        // clipping seems to be a bit more broken here than in my other repo, not exactly sure what's changed, but it's disabled for now.

        ImVec4 clipRect = ImVec4(cmd.ClipRect.x,
                                 cmd.ClipRect.y,
                                 cmd.ClipRect.z,
                                 cmd.ClipRect.w);

        ImVec2 clip_min(clipRect.x, clipRect.y);
        ImVec2 clip_max(clipRect.z, clipRect.w);
        ImVec2 clip_size(clip_max.x - clip_min.x, clip_max.y - clip_min.y);

        if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
          continue;

        bd->cmdBuf->SetViewport(0, 0, io.DisplaySize.x, io.DisplaySize.y);
        bd->cmdBuf->SetScissor((int) clip_min.x, (int) clip_min.y,(int) clip_size.x, (int) clip_size.y);
        //bd->cmdBuf->SetScissor(0, 0, io.DisplaySize.x, io.DisplaySize.y);

        // get texture ID from the command
        nvn::TextureHandle TexID = *(nvn::TextureHandle *) cmd.GetTexID();
        // if our previous handle is different from the current, bind the texture
        if (boundTextureHandle != TexID) {
          boundTextureHandle = TexID;
          bd->cmdBuf->BindTexture(nvn::ShaderStage::FRAGMENT, 0, TexID);
        }
        // draw our vertices using the indices stored in the buffer, offset by the current command index offset,
        // as well as the current offset into our buffer.
        bd->cmdBuf->DrawElementsBaseVertex(nvn::DrawPrimitive::TRIANGLES,
                                           nvn::IndexType::UNSIGNED_SHORT, cmd.ElemCount,
                                           (*bd->idxBuffer) + (cmd.IdxOffset * sizeof(ImDrawIdx)) + idxOffset,
                                           cmd.VtxOffset);
      }

      vtxOffset += vtxSize;
      idxOffset += idxSize;
    }

    // end the command recording and submit to queue.
    auto handle = bd->cmdBuf->EndRecording();
    bd->queue->SubmitCommands(1, &handle);
  }

  void loadIni() {
      FsHelper::LoadData loadData = {
          .path = "sd:/mp1r/imgui.ini"
      };

      if (FsHelper::tryLoadFileFromPath(loadData)) {
        Logger::log("Loaded imgui.ini\n");
        ImGui::LoadIniSettingsFromMemory(static_cast<const char *>(loadData.buffer), loadData.bufSize);
        nn::init::GetAllocator()->Free(loadData.buffer);
      } else {
        Logger::log("Failed to load imgui.ini\n");
      }
  }

  void saveIni() {
    size_t size;
    const char *ini = ImGui::SaveIniSettingsToMemory(&size);

    if (FsHelper::createDirectory("sd:/mp1r/") != 0) {
      ImGui::GetIO().WantSaveIniSettings = false;
      return;
    }

    bool saved = FsHelper::writeFileToPath(ini, size, "sd:/mp1r/imgui.ini");
    if (saved != 0) {
      Logger::log("Failed to save imgui.ini\n");
    }
    ImGui::GetIO().WantSaveIniSettings = false;
  }
}