#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 512;
constexpr std::uint32_t Height = 256;
constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t Objects = 300;
constexpr std::size_t ObjectBytes = 512;
constexpr std::uint32_t Rgba8Info = 0x8028u;
constexpr std::uint32_t TiledAttrib3 = 0x4dc6c000u;
constexpr std::array<std::array<float, 4>, 3> positions{{{-1.0f, -1.0f, 0.0f, 1.0f}, {3.0f, -1.0f, 0.0f, 1.0f}, {-1.0f, 3.0f, 0.0f, 1.0f}}};
constexpr std::array<std::uint32_t, 11> vertexCode{0xf4080100u, 0xfa000000u, 0x4a0a0a02u, 0x4a0a0b08u, 0x4a0a0a03u, 0xe00c2000u, 0x80010005u, 0xbf8c3f70u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
constexpr std::array<std::uint32_t, 5> pixelCode{0x7e0002f2u, 0x7e020280u, 0xf800180fu, 0x00010100u, 0xbf810000u};

struct Block {
    explicit Block(std::size_t bytes) : bytes(bytes), watched(AgcDriver::GuestMemory::WriteWatched()) {
#ifdef _WIN32
        if (watched) {
            data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
            GuestArena::GuestArenaCommit_nid_postfix(data, bytes, PAGE_READWRITE, bytes);
        } else {
            data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
#else
        if (watched) {
            void* raw = mmap(nullptr, bytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Require(raw != MAP_FAILED, "cannot map the guest block");
            const auto begin = reinterpret_cast<std::uintptr_t>(raw);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(raw, aligned - begin);
            if (aligned + bytes != begin + bytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + bytes), begin + BlockBytes - aligned);
            data = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
        } else {
            data = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, bytes));
        }
#endif
        Require(data != nullptr, "cannot allocate the guest block");
        GuestAllocations::Mutation mutation;
        mutation.Add(data, bytes, true, true);
    }
    ~Block() {
        AgcDriver::Graphics::StorageTexture::FlushPending(Address(), bytes, nullptr, "test release");
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
#ifdef _WIN32
        if (watched) {
            GuestArena::GuestArenaReset_nid_postfix(data, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
        } else {
            VirtualFree(data, 0, MEM_RELEASE);
        }
#else
        if (watched) {
            munmap(data, bytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, bytes);
        } else {
            std::free(data);
        }
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t bytes;
    bool watched;
    std::uint8_t* data = nullptr;
};

AgcDriver::Graphics::ColorTarget DecodeTarget(const Block& block) {
    AgcDriver::Registers cx;
    cx[0x318] = static_cast<std::uint32_t>(block.Address() >> 8u);
    cx[0x31b] = 0;
    cx[0x31c] = Rgba8Info;
    cx[0x31d] = 0;
    cx[0x323] = 0;
    cx[0x324] = 0;
    cx[0x3b0] = ((Width - 1u) << 14u) | (Height - 1u);
    cx[0x3b8] = TiledAttrib3;
    cx[0x390] = static_cast<std::uint32_t>(block.Address() >> 40u);
    auto color = AgcDriver::Graphics::DecodeColorBuffer(cx, 0);
    color.exportIndex = 0;
    return color;
}

struct Scene {
    std::unique_ptr<Block> target;
    std::unique_ptr<Block> objects;
    std::vector<ShaderRecompiler::RecompileResult> vertex;
    ShaderRecompiler::RecompileResult pixel;
    AgcDriver::Graphics::State state{};
    AgcDriver::Pm4::DrawParameters draw{0u, 3u, 0u, 1u, 0u, false};
    std::uint32_t pushBytes = 0;
};

Scene Build(AgcDriver::VulkanDevice& device) {
    Scene scene;
    const AgcDriver::Graphics::ColorTargetLayout layout(Width, Height, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    scene.target = std::make_unique<Block>(layout.Bytes());
    scene.objects = std::make_unique<Block>((Objects * ObjectBytes + BlockBytes - 1) / BlockBytes * BlockBytes);
    std::memset(scene.objects->data, 0, scene.objects->bytes);
    constexpr std::uint32_t stride = 16u;
    constexpr std::uint32_t offset = 1u;
    for (std::size_t object = 0; object < Objects; ++object) {
        auto* table = reinterpret_cast<std::uint32_t*>(scene.objects->data + object * ObjectBytes);
        auto* vertices = scene.objects->data + object * ObjectBytes + 256;
        const auto firstRecord = offset + 1u + 2u;
        for (std::size_t index = 0; index < positions.size(); ++index) std::memcpy(vertices + (firstRecord + index) * stride, positions[index].data(), 16u);
        const auto address = reinterpret_cast<std::uintptr_t>(vertices);
        const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (stride << 16u), 8u, 0x01000facu | (77u << 12u)};
        std::memcpy(table, descriptor.data(), sizeof(descriptor));
        const auto tableAddress = reinterpret_cast<std::uintptr_t>(table);
        std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(tableAddress), static_cast<std::uint32_t>(tableAddress >> 32u), offset, 1u};
        const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{tableAddress, std::as_bytes(std::span(descriptor))}}};
        ShaderRecompiler::ShaderVertexStageInfo vertexInfo{};
        vertexInfo.fetchEmbedded = true;
        vertexInfo.resourcesNum = 1u;
        vertexInfo.resourcesDst[0] = {0, 4, 0, 0};
        ShaderRecompiler::RecompileRequest request{{ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0, {}}, {32u, 0u, userData, std::nullopt, std::nullopt, vertexInfo, memory}, device.Target(), {0u, 0u, 0u, 96u}};
        request.context.vertex->resources[0].fields = descriptor;
        scene.vertex.push_back(ShaderRecompiler::Recompile(request));
        Require(scene.vertex.back().vertexInputs.empty(), "runtime vertex fetch created Vulkan vertex attributes");
    }
    scene.pushBytes = static_cast<std::uint32_t>(scene.vertex.front().pushConstants.size());
    for (const auto& vertex : scene.vertex) Require(vertex.pushConstants.size() == scene.pushBytes, "objects differ in their push constant size");
    ShaderRecompiler::ShaderPixelStageInfo pixelInfo{};
    pixelInfo.wave32 = true;
    pixelInfo.targetOutputMode[0] = 9u;
    pixelInfo.targetExportMapping.fill(0xe4u);
    ShaderRecompiler::RecompileRequest pixelRequest{{ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}}, {32u, 0u, {}, std::nullopt, pixelInfo, std::nullopt, {}}, device.Target(), {0u, 0u, scene.pushBytes, 128u - scene.pushBytes}};
    scene.pixel = ShaderRecompiler::Recompile(pixelRequest);
    auto& state = scene.state;
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = DecodeTarget(*scene.target);
    Require(state.color.tileMode == AgcDriver::Graphics::ColorTileMode::RenderTarget && state.color.elementBytes == 4 && state.color.bytes == layout.Bytes(), "the target did not decode as a tiled RGBA8 surface");
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0.0f, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0.0f, 1.0f};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15u;
    state.blends = {state.blend};
    scene.draw.firstVertex = 1u;
    scene.draw.firstInstance = 1u;
    return scene;
}

void DrawObject(AgcDriver::VulkanDevice& device, const Scene& scene, std::size_t object) {
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{{ShaderStage::Vertex, &scene.vertex[object], 0u}, {ShaderStage::Fragment, &scene.pixel, scene.pushBytes}}};
    device.Draw(scene.state, scene.draw, shaders);
}

void CheckPixels(AgcDriver::VulkanDevice& device, const Scene& scene) {
    AgcDriver::Graphics::StorageTexture::FlushPending(scene.target->Address(), scene.target->bytes, nullptr, "test read-back");
    device.WaitIdle();
    std::vector<std::uint8_t> stored(scene.target->bytes);
    AgcDriver::GuestMemory::Read(scene.target->Address(), std::as_writable_bytes(std::span(stored)), 1);
    for (std::size_t index = 0; index < stored.size(); index += 4u) {
        Require(stored[index] == 255 && stored[index + 1u] == 0 && stored[index + 2u] == 0 && stored[index + 3u] == 255, "object draws produced the wrong triangle at texel " + std::to_string(index / 4u));
    }
}

void Run(AgcDriver::VulkanDevice& device) {
    const auto scene = Build(device);
    std::memset(scene.target->data, 0, scene.target->bytes);
    for (std::size_t round = 0; round < 3; ++round) {
        for (std::size_t object = 0; object < Objects; ++object) DrawObject(device, scene, object);
    }
    CheckPixels(device, scene);
}

void Benchmark(AgcDriver::VulkanDevice& device, std::size_t draws, double seconds) {
    constexpr std::size_t FrameDraws = 5000;
    const auto scene = Build(device);
    std::memset(scene.target->data, 0, scene.target->bytes);
    for (std::size_t object = 0; object < Objects; ++object) DrawObject(device, scene, object);
    device.WaitIdle();
    const auto lookupsBefore = AgcDriver::Graphics::DeviceProcLookups();
    const auto start = std::chrono::steady_clock::now();
    std::size_t issued = 0;
    double recordingUs = 0;
    while (issued < draws || std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        const auto frameStart = std::chrono::steady_clock::now();
        for (std::size_t n = 0; n < FrameDraws; ++n) DrawObject(device, scene, (issued + n) % Objects);
        recordingUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - frameStart).count();
        issued += FrameDraws;
        device.SubmitRecorded();
    }
    const auto recorded = std::chrono::steady_clock::now();
    device.WaitIdle();
    const auto done = std::chrono::steady_clock::now();
    CheckPixels(device, scene);
    const auto us = [](auto from, auto to) { return std::chrono::duration<double, std::micro>(to - from).count(); };
    std::cout << "object draws: " << recordingUs / static_cast<double>(issued) << " us per draw recording, " << us(start, recorded) / static_cast<double>(issued) << " us per draw with submissions, over " << issued << " draws in frames of " << FrameDraws << ", " << us(recorded, done) / 1000.0 << " ms to finish, " << static_cast<double>(AgcDriver::Graphics::DeviceProcLookups() - lookupsBefore) / static_cast<double>(issued) << " device function lookups per draw\n";
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--benchmark") {
            Benchmark(*device, static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)), argc == 4 ? std::strtod(argv[3], nullptr) : 0.0);
            return 0;
        }
        Run(*device);
        std::cout << "object draw tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
