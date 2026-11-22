// SPDX-License-Identifier: GPL-3.0-or-later

#include <deque>
#include <functional>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

class TestEnvironment final : public Shader::Environment {
public:
    TestEnvironment() { stage = Shader::Stage::VertexB; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

// Runs CollectShaderInfoPass on a vertex shader whose entry block `body` fills, followed by a
// second block `tail` fills.
bool OnFarPlane(const std::function<void(IR::IREmitter&)>& body,
                const std::function<void(IR::IREmitter&)>& tail = {},
                Shader::Stage stage = Shader::Stage::VertexB) {
    Shader::ObjectPool<IR::Inst> pool{64};
    std::deque<IR::Block> blocks;
    IR::Block& entry{blocks.emplace_back(pool)};
    IR::Block& exit{blocks.emplace_back(pool)};
    entry.AddBranch(&exit);
    IR::IREmitter entry_ir{entry};
    body(entry_ir);
    IR::IREmitter exit_ir{exit};
    if (tail) {
        tail(exit_ir);
    }
    exit_ir.Epilogue();

    IR::Program program;
    program.stage = stage;
    program.blocks = {&entry, &exit};
    program.post_order_blocks = {&exit, &entry};
    TestEnvironment env;
    Shader::Optimization::CollectShaderInfoPass(env, program);
    return program.info.position_on_far_plane;
}

IR::F32 Depth(IR::IREmitter& ir) {
    return ir.BitCast<IR::F32>(ir.GetCbuf(ir.Imm32(1), ir.Imm32(0)));
}
} // Anonymous namespace

TEST_CASE("A vertex shader storing one value to position z and w is on the far plane",
          "[shader_recompiler][far_plane]") {
    REQUIRE(OnFarPlane([](IR::IREmitter& ir) {
        const IR::F32 depth{Depth(ir)};
        ir.SetAttribute(IR::Attribute::PositionZ, depth, ir.Imm32(0));
        ir.SetAttribute(IR::Attribute::PositionW, depth, ir.Imm32(0));
    }));
    REQUIRE(OnFarPlane([](IR::IREmitter& ir) {
        ir.SetAttribute(IR::Attribute::PositionZ, ir.Imm32(1.0f), ir.Imm32(0));
        ir.SetAttribute(IR::Attribute::PositionW, ir.Imm32(1.0f), ir.Imm32(0));
    }));
}

TEST_CASE("Other position stores are not on the far plane", "[shader_recompiler][far_plane]") {
    // Different values
    REQUIRE_FALSE(OnFarPlane([](IR::IREmitter& ir) {
        ir.SetAttribute(IR::Attribute::PositionZ, Depth(ir), ir.Imm32(0));
        ir.SetAttribute(IR::Attribute::PositionW, ir.Imm32(1.0f), ir.Imm32(0));
    }));
    // z or w never stored
    REQUIRE_FALSE(OnFarPlane([](IR::IREmitter& ir) {
        ir.SetAttribute(IR::Attribute::PositionW, Depth(ir), ir.Imm32(0));
    }));
    // z stored twice
    REQUIRE_FALSE(OnFarPlane([](IR::IREmitter& ir) {
        const IR::F32 depth{Depth(ir)};
        ir.SetAttribute(IR::Attribute::PositionZ, depth, ir.Imm32(0));
        ir.SetAttribute(IR::Attribute::PositionW, depth, ir.Imm32(0));
        ir.SetAttribute(IR::Attribute::PositionZ, ir.Imm32(0.5f), ir.Imm32(0));
    }));
    // Stores in different blocks
    REQUIRE_FALSE(OnFarPlane(
        [](IR::IREmitter& ir) {
            ir.SetAttribute(IR::Attribute::PositionZ, ir.Imm32(1.0f), ir.Imm32(0));
        },
        [](IR::IREmitter& ir) {
            ir.SetAttribute(IR::Attribute::PositionW, ir.Imm32(1.0f), ir.Imm32(0));
        }));
    // Indexed attribute stores can reach position too
    REQUIRE_FALSE(OnFarPlane([](IR::IREmitter& ir) {
        const IR::F32 depth{Depth(ir)};
        ir.SetAttribute(IR::Attribute::PositionZ, depth, ir.Imm32(0));
        ir.SetAttribute(IR::Attribute::PositionW, depth, ir.Imm32(0));
        ir.SetAttributeIndexed(ir.Imm32(0x78), depth, ir.Imm32(0));
    }));
    // The draw may land in another viewport
    REQUIRE_FALSE(OnFarPlane([](IR::IREmitter& ir) {
        const IR::F32 depth{Depth(ir)};
        ir.SetAttribute(IR::Attribute::PositionZ, depth, ir.Imm32(0));
        ir.SetAttribute(IR::Attribute::PositionW, depth, ir.Imm32(0));
        ir.SetAttribute(IR::Attribute::ViewportIndex, ir.Imm32(1.0f), ir.Imm32(0));
    }));
    // Only vertex shaders are marked
    REQUIRE_FALSE(OnFarPlane(
        [](IR::IREmitter& ir) {
            const IR::F32 depth{Depth(ir)};
            ir.SetAttribute(IR::Attribute::PositionZ, depth, ir.Imm32(0));
            ir.SetAttribute(IR::Attribute::PositionW, depth, ir.Imm32(0));
        },
        {}, Shader::Stage::Geometry));
}
