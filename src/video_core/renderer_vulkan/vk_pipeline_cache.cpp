// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ranges>

#include "common/config.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern std::unique_ptr<Vulkan::RendererVulkan> renderer;

namespace Vulkan {

using Shader::VsOutput;

[[nodiscard]] inline u64 HashCombine(const u64 seed, const u64 hash) {
    return seed ^ (hash + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformTexelBuffer, 128},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageTexelBuffer, 128},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

void GatherVertexOutputs(Shader::VertexRuntimeInfo& info,
                         const AmdGpu::Liverpool::VsOutputControl& ctl) {
    const auto add_output = [&](VsOutput x, VsOutput y, VsOutput z, VsOutput w) {
        if (x != VsOutput::None || y != VsOutput::None || z != VsOutput::None ||
            w != VsOutput::None) {
            info.outputs.emplace_back(Shader::VsOutputMap{x, y, z, w});
        }
    };
    // VS_OUT_MISC_VEC
    add_output(ctl.use_vtx_point_size ? VsOutput::PointSprite : VsOutput::None,
               ctl.use_vtx_edge_flag
                   ? VsOutput::EdgeFlag
                   : (ctl.use_vtx_gs_cut_flag ? VsOutput::GsCutFlag : VsOutput::None),
               ctl.use_vtx_kill_flag
                   ? VsOutput::KillFlag
                   : (ctl.use_vtx_render_target_idx ? VsOutput::GsMrtIndex : VsOutput::None),
               ctl.use_vtx_viewport_idx ? VsOutput::GsVpIndex : VsOutput::None);
    // VS_OUT_CCDIST0
    add_output(ctl.IsClipDistEnabled(0)
                   ? VsOutput::ClipDist0
                   : (ctl.IsCullDistEnabled(0) ? VsOutput::CullDist0 : VsOutput::None),
               ctl.IsClipDistEnabled(1)
                   ? VsOutput::ClipDist1
                   : (ctl.IsCullDistEnabled(1) ? VsOutput::CullDist1 : VsOutput::None),
               ctl.IsClipDistEnabled(2)
                   ? VsOutput::ClipDist2
                   : (ctl.IsCullDistEnabled(2) ? VsOutput::CullDist2 : VsOutput::None),
               ctl.IsClipDistEnabled(3)
                   ? VsOutput::ClipDist3
                   : (ctl.IsCullDistEnabled(3) ? VsOutput::CullDist3 : VsOutput::None));
    // VS_OUT_CCDIST1
    add_output(ctl.IsClipDistEnabled(4)
                   ? VsOutput::ClipDist4
                   : (ctl.IsCullDistEnabled(4) ? VsOutput::CullDist4 : VsOutput::None),
               ctl.IsClipDistEnabled(5)
                   ? VsOutput::ClipDist5
                   : (ctl.IsCullDistEnabled(5) ? VsOutput::CullDist5 : VsOutput::None),
               ctl.IsClipDistEnabled(6)
                   ? VsOutput::ClipDist6
                   : (ctl.IsCullDistEnabled(6) ? VsOutput::CullDist6 : VsOutput::None),
               ctl.IsClipDistEnabled(7)
                   ? VsOutput::ClipDist7
                   : (ctl.IsCullDistEnabled(7) ? VsOutput::CullDist7 : VsOutput::None));
}

Shader::RuntimeInfo PipelineCache::BuildRuntimeInfo(Shader::Stage stage) {
    auto info = Shader::RuntimeInfo{stage};
    const auto& regs = liverpool->regs;
    switch (stage) {
    case Shader::Stage::Vertex: {
        info.num_user_data = regs.vs_program.settings.num_user_regs;
        info.num_input_vgprs = regs.vs_program.settings.vgpr_comp_cnt;
        info.num_allocated_vgprs = regs.vs_program.settings.num_vgprs * 4;
        GatherVertexOutputs(info.vs_info, regs.vs_output_control);
        info.vs_info.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == Liverpool::ClipSpace::MinusWToW;
        break;
    }
    case Shader::Stage::Fragment: {
        info.num_user_data = regs.ps_program.settings.num_user_regs;
        info.num_allocated_vgprs = regs.ps_program.settings.num_vgprs * 4;
        std::ranges::transform(graphics_key.mrt_swizzles, info.fs_info.mrt_swizzles.begin(),
                               [](Liverpool::ColorBuffer::SwapMode mode) {
                                   return static_cast<Shader::MrtSwizzle>(mode);
                               });
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.fs_info.inputs.push_back({
                .param_index = u8(ps_inputs[i].input_offset.Value()),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            });
        }
        break;
    }
    case Shader::Stage::Compute: {
        const auto& cs_pgm = regs.cs_program;
        info.num_user_data = cs_pgm.settings.num_user_regs;
        info.num_allocated_vgprs = regs.cs_program.settings.num_vgprs * 4;
        info.cs_info.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                       cs_pgm.num_thread_z.full};
        info.cs_info.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                    cs_pgm.IsTgidEnabled(2)};
        info.cs_info.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetMasterSemaphore(), DescriptorHeapSizes} {
    profile = Shader::Profile{
        .supported_spirv = instance.ApiVersion() >= VK_API_VERSION_1_3 ? 0x00010600U : 0x00010500U,
        .subgroup_size = instance.SubgroupSize(),
        .support_explicit_workgroup_layout = true,
    };
    pipeline_cache = instance.GetDevice().createPipelineCacheUnique({});
}

PipelineCache::~PipelineCache() = default;

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline() {
    const auto& regs = liverpool->regs;
    // Tessellation is unsupported so skip the draw to avoid locking up the driver.
    if (regs.primitive_type == Liverpool::PrimitiveType::PatchPrimitive) {
        return nullptr;
    }
    // There are several cases (e.g. FCE, FMask/HTile decompression) where we don't need to do an
    // actual draw hence can skip pipeline creation.
    if (regs.color_control.mode == Liverpool::ColorControl::OperationMode::EliminateFastClear) {
        LOG_TRACE(Render_Vulkan, "FCE pass skipped");
        return nullptr;
    }
    if (regs.color_control.mode == Liverpool::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        return nullptr;
    }
    if (regs.primitive_type == Liverpool::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        return nullptr;
    }
    if (!RefreshGraphicsKey()) {
        return nullptr;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(graphics_key);
    if (is_new) {
        it.value() = graphics_pipeline_pool.Create(instance, scheduler, desc_heap, graphics_key,
                                                   *pipeline_cache, infos, modules);
    }
    return it->second;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        it.value() = compute_pipeline_pool.Create(instance, scheduler, desc_heap, *pipeline_cache,
                                                  compute_key, *infos[0], modules[0]);
    }
    return it->second;
}

bool ShouldSkipShader(u64 shader_hash, const char* shader_type) {
    static constexpr std::array<u64, 0> skip_hashes = {};

    // FIFA 14 Skips: 0xec602a8fee029fd0, 0x65854b2b21f19601, 0x793f1066476b16c9
    
    // UFC 1 Skips to get to the menu: 0x81ac71121916cef0, 0x72e540be7eaacd3, 0xa1e9015cb60883dc
    /* UFC 1 Skips to get to the gameplay: 0x3abf50ba16091f46, 0x3163fb9f52f4ede7,
        0x3abf50baddd4decb, 0x3abf50baf7f1ece7, 0x3163fb9f31b0a23e, 0xa037e80424ab5c6a,
        0x3163fb9f22a7308f, 0xfd5f44ab5be41430, 0xc8c2e96278f8ac90, 0xeda81c06943b0688,
        0x761938cf605eaee7, 0x5a86bb695ed32814, 0x714c57b840eb27db, 0xfd5f44ab06133ddd,
        0x1c224b467f37fe5a, 0xeda81c068e2f2c38, 0xdc4c674e3ac0cbac, 0xf5745bb828a867e8,
        0xd0a7339a6e967afb, 0x5a86bb69a96404e5, 0x84e5a4db9411f6d9, 0x965ad37d47fd370d,
        0x1e8f6e6a488add30, 0xed260a310c79683c, 0xeda81c062603bf0b, 0x1e8f6e6aae9d46c2,
        0x1c224b46d5a85909, 0xdc4c674e20e58f76, 0x965ad37d72d727b3, 0xf5745bb80cd446e3,
        0x5a86bb69e9da619a, 0xc8c2e9625aca97f3, 0x714c57b8a300804d, 0xcb936483d673971f,
        0xed260a31f8f6c682, 0xf337842af939ea3a, 0x17c8dd0bcb5fd88e, 0xd76d80a5897d8664,
        0x84e5a4dbc7c0c591, 0xd0a7339ac88a0808, 0x9554eb53af248d49, 0xca743277c769f815,
        0xdc4c674e153218fe, 0x307d19ec8da36694, 0xf5745bb80cee46e5, 0x932fdcdd1fd1d60c,
        0x965ad37df78c6090, 0x330fc1531bb3473b, 0xed260a31f78c6090, */
    

    if (std::ranges::contains(skip_hashes, shader_hash)) {
        //LOG_WARNING(Render_Vulkan, "Skipped {} shader hash {:#x}.", shader_type, shader_hash);
        return true;
    }
    return false;
}

bool PipelineCache::RefreshGraphicsKey() {
    std::memset(&graphics_key, 0, sizeof(GraphicsPipelineKey));

    auto& regs = liverpool->regs;
    auto& key = graphics_key;

    key.depth_stencil = regs.depth_control;
    key.depth_stencil.depth_write_enable.Assign(regs.depth_control.depth_write_enable.Value() &&
                                                !regs.depth_render_control.depth_clear_enable);
    key.depth_bias_enable = regs.polygon_control.NeedsBias();

    const auto& db = regs.depth_buffer;
    const auto ds_format = LiverpoolToVK::DepthFormat(db.z_info.format, db.stencil_info.format);
    if (db.z_info.format != AmdGpu::Liverpool::DepthBuffer::ZFormat::Invalid) {
        key.depth_format = ds_format;
    } else {
        key.depth_format = vk::Format::eUndefined;
    }
    if (regs.depth_control.depth_enable) {
        key.depth_stencil.depth_enable.Assign(key.depth_format != vk::Format::eUndefined);
    }
    key.stencil = regs.stencil_control;

    if (db.stencil_info.format != AmdGpu::Liverpool::DepthBuffer::StencilFormat::Invalid) {
        key.stencil_format = key.depth_format;
    } else {
        key.stencil_format = vk::Format::eUndefined;
    }
    if (key.depth_stencil.stencil_enable) {
        key.depth_stencil.stencil_enable.Assign(key.stencil_format != vk::Format::eUndefined);
    }
    key.prim_type = regs.primitive_type;
    key.enable_primitive_restart = regs.enable_primitive_restart & 1;
    key.primitive_restart_index = regs.primitive_restart_index;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.cull_mode = regs.polygon_control.CullingMode();
    key.clip_space = regs.clipper_control.clip_space;
    key.front_face = regs.polygon_control.front_face;
    key.num_samples = regs.aa_config.NumSamples();

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::Liverpool::ColorControl::OperationMode::Disable;

    // `RenderingInfo` is assumed to be initialized with a contiguous array of valid color
    // attachments. This might be not a case as HW color buffers can be bound in an arbitrary
    // order. We need to do some arrays compaction at this stage
    key.color_formats.fill(vk::Format::eUndefined);
    key.blend_controls.fill({});
    key.write_masks.fill({});
    key.mrt_swizzles.fill(Liverpool::ColorBuffer::SwapMode::Standard);

    // First pass of bindings check to idenitfy formats and swizzles and pass them to rhe shader
    // recompiler.
    for (auto cb = 0u, remapped_cb = 0u; cb < Liverpool::NumColorBuffers; ++cb) {
        auto const& col_buf = regs.color_buffers[cb];
        if (skip_cb_binding || !col_buf || !regs.color_target_mask.GetMask(cb)) {
            continue;
        }
        const auto base_format =
            LiverpoolToVK::SurfaceFormat(col_buf.info.format, col_buf.NumFormat());
        const bool is_vo_surface = renderer->IsVideoOutSurface(col_buf);
        key.color_formats[remapped_cb] = LiverpoolToVK::AdjustColorBufferFormat(
            base_format, col_buf.info.comp_swap.Value(), false /*is_vo_surface*/);
        if (base_format == key.color_formats[remapped_cb]) {
            key.mrt_swizzles[remapped_cb] = col_buf.info.comp_swap.Value();
        }

        ++remapped_cb;
    }

    u32 binding{};
    for (u32 i = 0; i < MaxShaderStages; i++) {
        if (!regs.stage_enable.IsStageEnabled(i)) {
            key.stage_hashes[i] = 0;
            infos[i] = nullptr;
            continue;
        }
        auto* pgm = regs.ProgramForStage(i);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[i] = 0;
            infos[i] = nullptr;
            continue;
        }
        const auto* bininfo = Liverpool::GetBinaryInfo(*pgm);
        if (!bininfo->Valid()) {
            LOG_WARNING(Render_Vulkan, "Invalid binary info structure!");
            key.stage_hashes[i] = 0;
            infos[i] = nullptr;
            continue;
        }
        if (ShouldSkipShader(bininfo->shader_hash, "graphics")) {
            return false;
        }
        const auto stage = Shader::StageFromIndex(i);
        const auto params = Liverpool::GetParams(*pgm);

        if (stage != Shader::Stage::Vertex && stage != Shader::Stage::Fragment) {
            return false;
        }

        static bool TessMissingLogged = false;
        if (auto* pgm = regs.ProgramForStage(3);
            regs.stage_enable.IsStageEnabled(3) && pgm->Address() != 0) {
            if (!TessMissingLogged) {
                LOG_WARNING(Render_Vulkan, "Tess pipeline compilation skipped");
                TessMissingLogged = true;
            }
            return false;
        }

        std::tie(infos[i], modules[i], key.stage_hashes[i]) = GetProgram(stage, params, binding);
    }

    const auto* fs_info = infos[u32(Shader::Stage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;

    // Second pass to fill remain CB pipeline key data
    for (auto cb = 0u, remapped_cb = 0u; cb < Liverpool::NumColorBuffers; ++cb) {
        auto const& col_buf = regs.color_buffers[cb];
        if (skip_cb_binding || !col_buf || !regs.color_target_mask.GetMask(cb) ||
            (key.mrt_mask & (1u << cb)) == 0) {
            key.color_formats[cb] = vk::Format::eUndefined;
            key.mrt_swizzles[cb] = Liverpool::ColorBuffer::SwapMode::Standard;
            continue;
        }

        key.blend_controls[remapped_cb] = regs.blend_control[cb];
        key.blend_controls[remapped_cb].enable.Assign(key.blend_controls[remapped_cb].enable &&
                                                      !col_buf.info.blend_bypass);
        key.write_masks[remapped_cb] = vk::ColorComponentFlags{regs.color_target_mask.GetMask(cb)};
        key.cb_shader_mask.SetMask(remapped_cb, regs.color_shader_mask.GetMask(cb));

        ++remapped_cb;
    }
    return true;
}

bool PipelineCache::RefreshComputeKey() {
    u32 binding{};
    const auto* cs_pgm = &liverpool->regs.cs_program;
    const auto cs_params = Liverpool::GetParams(*cs_pgm);
    if (ShouldSkipShader(cs_params.hash, "compute")) {
        return false;
    }
    std::tie(infos[0], modules[0], compute_key) =
        GetProgram(Shader::Stage::Compute, cs_params, binding);
    return true;
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info,
                                              const Shader::RuntimeInfo& runtime_info,
                                              std::span<const u32> code, size_t perm_idx,
                                              u32& binding) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    if (Config::dumpShaders()) {
        DumpShader(code, info.pgm_hash, info.stage, perm_idx, "bin");
    }

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    if (Config::dumpShaders()) {
        DumpShader(spv, info.pgm_hash, info.stage, perm_idx, "spv");
    }

    const auto module = CompileSPV(spv, instance.GetDevice());
    const auto name = fmt::format("{}_{:#x}_{}", info.stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    return module;
}

std::tuple<const Shader::Info*, vk::ShaderModule, u64> PipelineCache::GetProgram(
    Shader::Stage stage, Shader::ShaderParams params, u32& binding) {
    const auto runtime_info = BuildRuntimeInfo(stage);
    auto [it_pgm, new_program] = program_cache.try_emplace(params.hash);
    if (new_program) {
        Program* program = program_pool.Create(stage, params);
        u32 start_binding = binding;
        const auto module = CompileModule(program->info, runtime_info, params.code, 0, binding);
        const auto spec = Shader::StageSpecialization(program->info, runtime_info, start_binding);
        program->AddPermut(module, std::move(spec));
        it_pgm.value() = program;
        return std::make_tuple(&program->info, module, HashCombine(params.hash, 0));
    }

    Program* program = it_pgm->second;
    const auto& info = program->info;
    const auto spec = Shader::StageSpecialization(info, runtime_info, binding);
    size_t perm_idx = program->modules.size();
    vk::ShaderModule module{};

    const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it == program->modules.end()) {
        auto new_info = Shader::Info(stage, params);
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);
        program->AddPermut(module, std::move(spec));
    } else {
        binding += info.NumBindings();
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
    }
    return std::make_tuple(&info, module, HashCombine(params.hash, perm_idx));
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage,
                               size_t perm_idx, std::string_view ext) {
    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}_{}.{}", stage, hash, perm_idx, ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Write};
    file.WriteSpan(code);
}

} // namespace Vulkan
