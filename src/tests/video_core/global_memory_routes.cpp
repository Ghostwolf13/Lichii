// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/profile.h"
#include "video_core/renderer_vulkan/pipeline_shader_emission.h"

namespace {
using namespace Shader;

class NativeEnvironment final : public Environment {
public:
    NativeEnvironment() {
        stage = Stage::Compute;
    }
    bool IsNativeProgram() const override { return true; }
    bool IsCbufStorageBufferPointer(u32, u32 offset) override { return offset < 0x200; }
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    TextureType ReadTextureType(u32) override { return {}; }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

// OpSelectionMerge count of each function in a SPIR-V module, in order
std::vector<size_t> SelectionsPerFunction(const std::vector<u32>& code) {
    static constexpr u32 op_function{54};
    static constexpr u32 op_selection_merge{247};
    std::vector<size_t> result;
    for (size_t i = 5; i < code.size();) {
        const u32 count{code[i] >> 16};
        REQUIRE(count > 0);
        const u32 opcode{code[i] & 0xffff};
        if (opcode == op_function) {
            result.push_back(0);
        } else if (opcode == op_selection_merge && !result.empty()) {
            ++result.back();
        }
        i += count;
    }
    return result;
}

// EDEN_GLOBAL_MEMORY_ROUTES_TEST_OUTPUT=<dir>: write the modules for spirv-val.
void Dump(const std::vector<u32>& code, std::string_view name) {
    const char* const directory{std::getenv("EDEN_GLOBAL_MEMORY_ROUTES_TEST_OUTPUT")};
    if (!directory) {
        return;
    }
    std::filesystem::create_directories(directory);
    std::ofstream file{std::filesystem::path{directory} / (std::string{name} + ".spv"),
                       std::ios::binary};
    file.write(reinterpret_cast<const char*>(code.data()),
               static_cast<std::streamsize>(code.size() * sizeof(u32)));
    REQUIRE(file.good());
}

// The functions marked DontInline, and the functions that call each of them
std::vector<std::pair<u32, std::set<u32>>> NotInlinedCallers(const std::vector<u32>& code) {
    static constexpr u32 op_function{54};
    static constexpr u32 op_function_call{57};
    static constexpr u32 dont_inline{2};
    std::vector<std::pair<u32, std::set<u32>>> result;
    for (size_t i = 5; i < code.size(); i += code[i] >> 16) {
        if ((code[i] & 0xffff) == op_function && (code[i + 3] & dont_inline) != 0) {
            result.emplace_back(code[i + 2], std::set<u32>{});
        }
    }
    u32 function{};
    for (size_t i = 5; i < code.size(); i += code[i] >> 16) {
        const u32 opcode{code[i] & 0xffff};
        if (opcode == op_function) {
            function = code[i + 2];
        } else if (opcode == op_function_call) {
            for (auto& [callee, callers] : result) {
                if (code[i + 3] == callee) {
                    callers.insert(function);
                }
            }
        }
    }
    return result;
}

// Three pointers in constant buffer 0. "routed" is built from the first or the second one,
// "fallback" from the first, the second or a value the pointer search does not follow, and
// "tracked" from the third alone.
struct Fixture {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    IR::Inst* routed{};
    IR::Inst* fallback{};

    Fixture() {
        IR::IREmitter ir{block};
        const auto word = [&](u32 offset) { return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset)); };
        const auto address = [&](const IR::U32& low, const IR::U32& high) {
            return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(low, high))};
        };
        const IR::U1 first{ir.INotEqual(word(0x200), ir.Imm32(0))};
        const IR::U1 second{ir.INotEqual(word(0x204), ir.Imm32(0))};
        const IR::U32 low{ir.Select(first, word(0x100), word(0x108))};
        const IR::U32 high{ir.Select(first, word(0x104), word(0x10c))};
        const IR::U32 value{ir.LoadGlobal32(address(low, high))};
        routed = value.InstRecursive();
        const IR::U32 other{ir.IMul(word(0x208), ir.Imm32(3))};
        const IR::U32 any_low{ir.Select(second, low, other)};
        ir.WriteGlobal32(address(any_low, high), value);
        fallback = &block.Instructions().back();
        ir.WriteGlobal32(address(word(0x110), word(0x114)), value);

        program.stage = Stage::Compute;
        program.workgroup_size = {1, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        HostTranslateInfo host{};
        host.min_ssbo_alignment = 16;
        host.support_int64 = true;
        NativeEnvironment env;
        Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
        Optimization::CollectShaderInfoPass(env, program);
        program.info.constant_buffer_descriptors = {{0, 1}};
    }
};
} // namespace

TEST_CASE("A native access built from its own candidates is routed among them alone",
          "[shader][global_memory_routes]") {
    Fixture fixture;
    const auto& info{fixture.program.info};
    REQUIRE(info.native_global_bounds);
    REQUIRE(info.storage_buffers_descriptors.size() == 3);
    REQUIRE(fixture.routed->GetOpcode() == IR::Opcode::LoadGlobal32);
    REQUIRE(fixture.fallback->GetOpcode() == IR::Opcode::WriteGlobal32);

    const auto offset_of = [&](u32 descriptor) {
        return info.storage_buffers_descriptors[descriptor].cbuf_offset;
    };
    REQUIRE(fixture.program.global_memory_routes.size() == 1);
    std::vector<u32> offsets;
    for (const u32 descriptor : fixture.program.global_memory_routes.front()) {
        offsets.push_back(offset_of(descriptor));
    }
    CHECK(offsets == std::vector<u32>{0x100, 0x108});
    CHECK(fixture.program.global_memory_route.at(fixture.routed) == 0);
    // A path through a multiplication can carry any pointer: every bound buffer
    CHECK_FALSE(fixture.program.global_memory_route.contains(fixture.fallback));

    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    const auto selections{
        SelectionsPerFunction(Backend::SPIRV::EmitSPIRV(profile, fixture.program))};
    // The functions through every buffer branch once per buffer, the routed 32-bit load and store
    // once per candidate
    CHECK(std::ranges::count(selections, size_t{3}) >= 2);
    CHECK(std::ranges::count(selections, size_t{2}) == 2);
}

TEST_CASE("Selecting the buffer routes every access among all bound buffers",
          "[shader][global_memory_routes]") {
    Fixture fixture;
    REQUIRE(fixture.program.global_memory_routes.size() == 1);
    Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    const size_t routed{
        SelectionsPerFunction(Backend::SPIRV::EmitSPIRV(profile, fixture.program)).size()};
    profile.select_global_memory_buffer = true;
    const auto selections{
        SelectionsPerFunction(Backend::SPIRV::EmitSPIRV(profile, fixture.program))};
    // The buffer search, the 32, 64 and 128-bit load and store through every buffer, and the entry
    // point: the routed load and store add none
    CHECK(selections.size() == 8);
    CHECK(routed == 9);
}

TEST_CASE("Selecting the buffer searches the bound buffers in one function that is not inlined",
          "[shader][global_memory_routes]") {
    Fixture fixture;
    Profile profile{};
    // SPIR-V 1.6 as on NVIDIA: the routed loads select a vector with a scalar condition (1.4)
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    CHECK(NotInlinedCallers(Backend::SPIRV::EmitSPIRV(profile, fixture.program)).empty());
    profile.select_global_memory_buffer = true;
    const auto code{Backend::SPIRV::EmitSPIRV(profile, fixture.program)};
    Dump(code, "select");
    const auto searches{NotInlinedCallers(code)};
    REQUIRE(searches.size() == 1);
    // The 32, 64 and 128-bit load and store each call it
    CHECK(searches.front().second.size() == 6);
}

TEST_CASE("NVIDIA compute uses single-candidate routes except for the #619 shaders",
          "[shader][global_memory_routes]") {
    constexpr u64 ordinary{0x1234};
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    profile.select_global_memory_buffer = true;
    const auto emit = [&](Stage stage, u64 hash, bool single, bool use_pipeline_policy) {
        Fixture fixture;
        fixture.program.stage = stage;
        fixture.program.info.is_task = stage == Stage::Task;
        fixture.program.info.is_mesh = stage == Stage::Mesh;
        if (single) {
            auto& routes = fixture.program.global_memory_routes;
            REQUIRE(routes.size() == 1);
            const u32 first_candidate{routes.front().front()};
            routes.push_back({first_candidate});
            fixture.program.global_memory_route.at(fixture.routed) = 1;
        }
        Backend::Bindings bindings;
        if (use_pipeline_policy) {
            return Vulkan::EmitPipelineShader(profile, {}, fixture.program, bindings, hash);
        }
        return Backend::SPIRV::EmitSPIRV(profile, {}, fixture.program, bindings);
    };

    const auto all_buffers{emit(Stage::Compute, ordinary, true, false)};
    CHECK(emit(Stage::Compute, 0xf98012dc9a4465ffULL, true, true) == all_buffers);
    CHECK(emit(Stage::Compute, 0xe78efceda31f97a8ULL, true, true) == all_buffers);
    CHECK(emit(Stage::Compute, ordinary, false, true) == all_buffers);
    for (Stage stage : {Stage::Task, Stage::Mesh}) {
        CHECK(emit(stage, ordinary, true, true) == emit(stage, ordinary, true, false));
    }
    const auto single{emit(Stage::Compute, ordinary, true, true)};
    const auto selections{SelectionsPerFunction(single)};
    REQUIRE(selections.size() == 10);
    CHECK(selections[7] == 1);
    CHECK(selections[8] == 1);
    const auto searches{NotInlinedCallers(single)};
    REQUIRE(searches.size() == 1);
    CHECK(searches.front().second.size() == 6);
    Dump(all_buffers, "single_candidate_all_buffers");
    Dump(single, "single_candidate_routed");
}

TEST_CASE("Task and mesh programs search the bound buffers inline",
          "[shader][global_memory_routes]") {
    const Stage stage{GENERATE(Stage::Task, Stage::Mesh)};
    Fixture fixture;
    fixture.program.stage = stage;
    fixture.program.info.is_task = stage == Stage::Task;
    fixture.program.info.is_mesh = stage == Stage::Mesh;
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.support_descriptor_aliasing = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    profile.select_global_memory_buffer = true;
    const auto code{Backend::SPIRV::EmitSPIRV(profile, fixture.program)};
    Dump(code, stage == Stage::Task ? "select_task" : "select_mesh");
    CHECK(NotInlinedCallers(code).empty());
}

TEST_CASE("A route through a pointer array keeps the bound slots past those searched",
          "[shader][global_memory_routes]") {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto address = [&](const IR::U32& low, const IR::U32& high) {
        return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(low, high))};
    };
    // c0[0x100 + (i << 3)]: the search takes slots 0x100 to 0x178, another access binds 0x180
    const IR::U32 index{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x300))};
    const IR::U32 slot{ir.IAdd(ir.ShiftLeftLogical(index, ir.Imm32(3)), ir.Imm32(0x100))};
    const IR::U32 high{ir.GetCbuf(ir.Imm32(0), ir.IAdd(slot, ir.Imm32(4)))};
    const IR::U32 value{ir.LoadGlobal32(address(ir.GetCbuf(ir.Imm32(0), slot), high))};
    IR::Inst* const dynamic{value.InstRecursive()};
    ir.WriteGlobal32(address(ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x180)),
                             ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x184))),
                     value);
    ir.WriteGlobal32(
        address(ir.GetCbuf(ir.Imm32(1), ir.Imm32(0)), ir.GetCbuf(ir.Imm32(1), ir.Imm32(4))), value);

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    NativeEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    REQUIRE(dynamic->GetOpcode() == IR::Opcode::LoadGlobal32);
    const auto& descriptors{program.info.storage_buffers_descriptors};
    REQUIRE(descriptors.size() == 18);
    REQUIRE(program.global_memory_route.contains(dynamic));
    std::vector<std::pair<u32, u32>> route;
    const u32 route_index{program.global_memory_route.at(dynamic)};
    for (const u32 descriptor : program.global_memory_routes.at(route_index)) {
        route.emplace_back(descriptors[descriptor].cbuf_index, descriptors[descriptor].cbuf_offset);
    }
    CHECK(route.size() == 17);
    CHECK(std::ranges::find(route, std::pair{0U, 0x180U}) != route.end());
    CHECK(std::ranges::find(route, std::pair{1U, 0U}) == route.end());
}

TEST_CASE("A table pointer assigned under the access's predicate routes among its own buffer",
          "[shader][global_memory_routes]") {
    const bool same = GENERATE(true, false);
    const bool broadcast = GENERATE(false, true);
    ObjectPool<IR::Inst> pool{256};
    IR::Block entry{pool};
    IR::Block definition{pool};
    IR::Block merge{pool};
    IR::Block access{pool};
    IR::Block exit{pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&entry, &definition, &merge, &access, &exit};
    program.post_order_blocks = {&exit, &access, &merge, &definition, &entry};
    for (u32 index = 0; index < program.blocks.size(); ++index) {
        program.blocks[index]->SetOrder(index);
    }
    entry.AddBranch(&definition);
    entry.AddBranch(&merge);
    definition.AddBranch(&merge);
    merge.AddBranch(&access);
    merge.AddBranch(&exit);
    access.AddBranch(&exit);
    const auto word = [](IR::IREmitter& ir, u32 offset) {
        return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset));
    };
    const auto address = [](IR::IREmitter& ir, const IR::U32& low, const IR::U32& high) {
        return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(low, high))};
    };
    const auto phi = [](IR::Block& block) {
        block.AppendNewInst(IR::Opcode::Phi, {});
        IR::Inst* const inst{&block.back()};
        inst->SetFlags(IR::Type::U32);
        return inst;
    };
    IR::IREmitter start{entry};
    start.WriteGlobal32(address(start, word(start, 0x110), word(start, 0x114)), start.Imm32(1));
    const IR::U1 predicate{start.ILessThan(start.LocalInvocationIdX(), start.Imm32(16), false)};
    const IR::U1 update_guard{start.ConditionRef(predicate)};
    // The register held a float (0.5) on the path that does not assign the table pointer.
    IR::IREmitter update{definition};
    const IR::U32 table_low{word(update, 0x180)};
    const IR::U32 table_high{word(update, 0x184)};
    IR::Inst* const low{phi(merge)};
    IR::Inst* const high{phi(merge)};
    low->AddPhiOperand(&entry, start.Imm32(0x3f000000));
    low->AddPhiOperand(&definition, table_low);
    high->AddPhiOperand(&entry, start.Imm32(0));
    high->AddPhiOperand(&definition, table_high);
    IR::IREmitter select{merge};
    const IR::U1 access_predicate{
        same ? predicate : select.ILessThan(select.LocalInvocationIdY(), select.Imm32(4), false)};
    const IR::U1 access_guard{select.ConditionRef(access_predicate)};
    IR::IREmitter load{access};
    const IR::Value entry_words{load.LoadGlobal64(IR::U64{load.IAdd(
        address(load, IR::U32{IR::Value{low}}, IR::U32{IR::Value{high}}), load.Imm64(u64{0x20}))})};
    const IR::U32 loaded_low{load.CompositeExtract(entry_words, 0)};
    // A pointer broadcast from another lane, whose predicate can differ from the access's
    const IR::U32 pointer_low{
        broadcast ? load.ShuffleIndex(loaded_low, load.Imm32(0), load.Imm32(31), load.Imm32(0))
                  : loaded_low};
    const IR::U32 pointer_high{load.CompositeExtract(entry_words, 1)};
    load.WriteGlobal32(address(load, load.IAdd(pointer_low, load.Imm32(16)), pointer_high),
                       load.Imm32(7));
    IR::Inst* const routed{&access.Instructions().back()};
    program.syntax_list = {
        {.data{.block = &entry}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.if_node{update_guard, &definition, &merge}},
         .type = IR::AbstractSyntaxNode::Type::If},
        {.data{.block = &definition}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.end_if{&merge}}, .type = IR::AbstractSyntaxNode::Type::EndIf},
        {.data{.block = &merge}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.if_node{access_guard, &access, &exit}}, .type = IR::AbstractSyntaxNode::Type::If},
        {.data{.block = &access}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.end_if{&exit}}, .type = IR::AbstractSyntaxNode::Type::EndIf},
        {.data{.block = &exit}, .type = IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return},
    };
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    NativeEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    REQUIRE(routed->GetOpcode() == IR::Opcode::WriteGlobal32);
    if (!same || broadcast) {
        // Another predicate can reach the access with the float: every bound buffer
        CHECK_FALSE(program.global_memory_route.contains(routed));
        return;
    }
    REQUIRE(program.global_memory_route.contains(routed));
    const auto& descriptors{program.info.storage_buffers_descriptors};
    const auto& route{program.global_memory_routes.at(program.global_memory_route.at(routed))};
    REQUIRE(route.size() == 1);
    CHECK(descriptors[route.front()].cbuf_offset == 0x180);
    CHECK(descriptors[route.front()].indirect_offset == 0x20);
}

TEST_CASE("A table pointer carried from the previous iteration keeps its unpredicated input",
          "[shader][global_memory_routes]") {
    const bool carried = GENERATE(true, false);
    ObjectPool<IR::Inst> pool{256};
    IR::Block entry{pool};
    IR::Block loop{pool};
    IR::Block definition{pool};
    IR::Block merge{pool};
    IR::Block access{pool};
    IR::Block back{pool};
    IR::Block exit{pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&entry, &loop, &definition, &merge, &access, &back, &exit};
    program.post_order_blocks = {&exit, &back, &access, &merge, &definition, &loop, &entry};
    for (u32 index = 0; index < program.blocks.size(); ++index) {
        program.blocks[index]->SetOrder(index);
    }
    entry.AddBranch(&loop);
    loop.AddBranch(&definition);
    loop.AddBranch(&merge);
    definition.AddBranch(&merge);
    merge.AddBranch(&access);
    merge.AddBranch(&back);
    access.AddBranch(&back);
    back.AddBranch(&loop);
    back.AddBranch(&exit);
    const auto word = [](IR::IREmitter& ir, u32 offset) {
        return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset));
    };
    const auto address = [](IR::IREmitter& ir, const IR::U32& low, const IR::U32& high) {
        return IR::U64{ir.PackUint2x32(ir.CompositeConstruct(low, high))};
    };
    const auto phi = [](IR::Block& block) {
        block.AppendNewInst(IR::Opcode::Phi, {});
        IR::Inst* const inst{&block.back()};
        inst->SetFlags(IR::Type::U32);
        return inst;
    };
    IR::IREmitter start{entry};
    start.WriteGlobal32(address(start, word(start, 0x110), word(start, 0x114)), start.Imm32(1));
    const IR::U32 first_low{word(start, 0x1a0)};
    const IR::U32 first_high{word(start, 0x1a4)};
    IR::Inst* const iteration{phi(loop)};
    IR::Inst* const previous_low{phi(loop)};
    IR::Inst* const previous_high{phi(loop)};
    IR::IREmitter body{loop};
    const IR::U1 predicate{body.IEqual(IR::U32{IR::Value{iteration}}, body.Imm32(1))};
    const IR::U1 update_guard{body.ConditionRef(predicate)};
    IR::IREmitter update{definition};
    const IR::U32 table_low{word(update, 0x180)};
    const IR::U32 table_high{word(update, 0x184)};
    // The register held a float (0.5) on the iterations that do not assign the table pointer.
    IR::Inst* const next_low{phi(merge)};
    IR::Inst* const next_high{phi(merge)};
    next_low->AddPhiOperand(&loop, start.Imm32(0x3f000000));
    next_low->AddPhiOperand(&definition, table_low);
    next_high->AddPhiOperand(&loop, start.Imm32(0));
    next_high->AddPhiOperand(&definition, table_high);
    IR::IREmitter joined{merge};
    const IR::U1 access_guard{joined.ConditionRef(predicate)};
    IR::IREmitter load{access};
    // The previous iteration's pointer was assigned under that iteration's predicate.
    IR::Inst* const low{carried ? previous_low : next_low};
    IR::Inst* const high{carried ? previous_high : next_high};
    const IR::Value entry_words{load.LoadGlobal64(IR::U64{load.IAdd(
        address(load, IR::U32{IR::Value{low}}, IR::U32{IR::Value{high}}), load.Imm64(u64{0x20}))})};
    const IR::U32 pointer_low{load.CompositeExtract(entry_words, 0)};
    const IR::U32 pointer_high{load.CompositeExtract(entry_words, 1)};
    load.WriteGlobal32(address(load, load.IAdd(pointer_low, load.Imm32(16)), pointer_high),
                       load.Imm32(7));
    IR::Inst* const routed{&access.Instructions().back()};
    IR::IREmitter advance{back};
    const IR::U32 next_iteration{advance.IAdd(IR::U32{IR::Value{iteration}}, advance.Imm32(1))};
    iteration->AddPhiOperand(&entry, start.Imm32(0));
    iteration->AddPhiOperand(&back, next_iteration);
    previous_low->AddPhiOperand(&entry, first_low);
    previous_low->AddPhiOperand(&back, IR::Value{next_low});
    previous_high->AddPhiOperand(&entry, first_high);
    previous_high->AddPhiOperand(&back, IR::Value{next_high});
    program.syntax_list = {
        {.data{.block = &entry}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.loop{&loop, &back, &exit}}, .type = IR::AbstractSyntaxNode::Type::Loop},
        {.data{.block = &loop}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.if_node{update_guard, &definition, &merge}},
         .type = IR::AbstractSyntaxNode::Type::If},
        {.data{.block = &definition}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.end_if{&merge}}, .type = IR::AbstractSyntaxNode::Type::EndIf},
        {.data{.block = &merge}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.if_node{access_guard, &access, &back}}, .type = IR::AbstractSyntaxNode::Type::If},
        {.data{.block = &access}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.end_if{&back}}, .type = IR::AbstractSyntaxNode::Type::EndIf},
        {.data{.block = &back}, .type = IR::AbstractSyntaxNode::Type::Block},
        {.data{.repeat{advance.Imm1(true), &loop, &exit}},
         .type = IR::AbstractSyntaxNode::Type::Repeat},
        {.data{.block = &exit}, .type = IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return},
    };
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    NativeEnvironment env;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);

    REQUIRE(routed->GetOpcode() == IR::Opcode::WriteGlobal32);
    if (carried) {
        // The second iteration reaches the access with the float the first one left
        CHECK_FALSE(program.global_memory_route.contains(routed));
        return;
    }
    REQUIRE(program.global_memory_route.contains(routed));
    const auto& descriptors{program.info.storage_buffers_descriptors};
    const auto& route{program.global_memory_routes.at(program.global_memory_route.at(routed))};
    REQUIRE(route.size() == 1);
    CHECK(descriptors[route.front()].cbuf_offset == 0x180);
    CHECK(descriptors[route.front()].indirect_offset == 0x20);
}
