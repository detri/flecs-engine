#include "common.h"

static void flecsEngine_batch_bindMaterialGroupForSet(
    FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_t *buf,
    const flecsEngine_batch_buffers_t *bb)
{
    WGPUBindGroup group = (buf->flags & FLECS_BATCH_OWNS_MATERIAL)
        ? bb->gpu_material_bind_group
        : flecsEngine_materialBind_ensure(engine);

    if (group) {
        wgpuRenderPassEncoderSetBindGroup(pass, 2, group, 0, NULL);
    }
}

static void flecsEngine_batch_bindInstanceGroupForSet(
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_buffers_t *bb,
    uint32_t binding)
{
    if (bb->gpu_instance_bind_group) {
        wgpuRenderPassEncoderSetBindGroup(
            pass, binding, bb->gpu_instance_bind_group, 0, NULL);
    }
}

void flecsEngine_batch_bindMaterialGroup(
    FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_t *buf)
{
    flecsEngine_batch_bindMaterialGroupForSet(engine, pass, buf, &buf->buffers);
}

void flecsEngine_batch_bindInstanceGroup(
    FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_t *buf)
{
    (void)engine;
    flecsEngine_batch_bindInstanceGroupForSet(pass, &buf->buffers, 3);
}

void flecsEngine_batch_bindInstanceGroupShadow(
    FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_t *buf)
{
    (void)engine;
    flecsEngine_batch_bindInstanceGroupForSet(pass, &buf->buffers, 1);
}

void flecsEngine_batch_bindMaterialGroupStatic(
    FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_t *buf)
{
    flecsEngine_batch_bindMaterialGroupForSet(
        engine, pass, buf, &buf->static_buffers);
}

void flecsEngine_batch_bindInstanceGroupStatic(
    FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_t *buf)
{
    (void)engine;
    flecsEngine_batch_bindInstanceGroupForSet(pass, &buf->static_buffers, 3);
}

void flecsEngine_batch_bindInstanceGroupShadowStatic(
    FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_t *buf)
{
    (void)engine;
    flecsEngine_batch_bindInstanceGroupForSet(pass, &buf->static_buffers, 1);
}

static void flecsEngine_batch_group_drawViewForSet(
    const flecsEngine_batch_group_t *ctx,
    const flecsEngine_batch_buffers_t *bb,
    const flecsEngine_batch_view_t *view,
    const WGPURenderPassEncoder pass,
    int view_idx,
    WGPUBuffer vertex_buffer)
{
    int32_t src_count = view->count;
    if (!src_count || view->group_idx < 0) {
        return;
    }

    if (!bb->gpu_visible_slots || !bb->gpu_indirect_args) {
        return;
    }

    if (!vertex_buffer || !ctx->mesh.index_buffer || !ctx->mesh.index_count) {
        return;
    }

    int32_t capacity = bb->capacity;
    int32_t group_count = bb->group_count;

    uint64_t slot_offset =
        ((uint64_t)view_idx * (uint64_t)capacity
            + (uint64_t)view->offset) * sizeof(uint32_t);
    uint64_t slot_size = (uint64_t)src_count * sizeof(uint32_t);

    uint64_t args_offset =
        ((uint64_t)view_idx * (uint64_t)group_count
            + (uint64_t)view->group_idx)
        * sizeof(flecsEngine_gpuDrawArgs_t);

    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 0, vertex_buffer, 0, WGPU_WHOLE_SIZE);
    wgpuRenderPassEncoderSetVertexBuffer(
        pass, 1, bb->gpu_visible_slots, slot_offset, slot_size);

    wgpuRenderPassEncoderSetIndexBuffer(
        pass, ctx->mesh.index_buffer, WGPUIndexFormat_Uint32, 0,
        WGPU_WHOLE_SIZE);

    if (ctx->batch && (ctx->batch->flags & FLECS_BATCH_NO_GPU_CULL)) {
        wgpuRenderPassEncoderDrawIndexed(
            pass,
            (uint32_t)ctx->mesh.index_count,
            (uint32_t)src_count,
            0,
            0,
            0);
        return;
    }

    wgpuRenderPassEncoderDrawIndexedIndirect(
        pass, bb->gpu_indirect_args, args_offset);
}

static void flecsEngine_batch_group_drawView(
    const flecsEngine_batch_group_t *ctx,
    const WGPURenderPassEncoder pass,
    int view_idx,
    WGPUBuffer vertex_buffer)
{
    const flecsEngine_batch_t *buf = ctx->batch;
    if (!buf) return;
    flecsEngine_batch_group_drawViewForSet(
        ctx, &buf->buffers, &ctx->view, pass, view_idx, vertex_buffer);
}

static void flecsEngine_batch_group_drawViewStatic(
    const flecsEngine_batch_group_t *ctx,
    const WGPURenderPassEncoder pass,
    int view_idx,
    WGPUBuffer vertex_buffer)
{
    const flecsEngine_batch_t *buf = ctx->batch;
    if (!buf) return;
    flecsEngine_batch_group_drawViewForSet(
        ctx, &buf->static_buffers, &ctx->static_view,
        pass, view_idx, vertex_buffer);
}

void flecsEngine_batch_group_draw(
    const FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_group_t *ctx)
{
    (void)engine;
    flecsEngine_batch_group_drawView(
        ctx, pass, 0, ctx->mesh.vertex_uv_buffer);
}

void flecsEngine_batch_group_drawShadow(
    const FlecsEngineImpl *engine,
    const FlecsRenderViewImpl *view_impl,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_group_t *ctx)
{
    (void)engine;
    int cascade = view_impl->shadow.current_cascade;
    flecsEngine_batch_group_drawView(
        ctx, pass, 1 + cascade, ctx->mesh.vertex_buffer);
}

void flecsEngine_batch_group_drawStatic(
    const FlecsEngineImpl *engine,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_group_t *ctx)
{
    (void)engine;
    flecsEngine_batch_group_drawViewStatic(
        ctx, pass, 0, ctx->mesh.vertex_uv_buffer);
}

void flecsEngine_batch_group_drawShadowStatic(
    const FlecsEngineImpl *engine,
    const FlecsRenderViewImpl *view_impl,
    const WGPURenderPassEncoder pass,
    const flecsEngine_batch_group_t *ctx)
{
    (void)engine;
    int cascade = view_impl->shadow.current_cascade;
    flecsEngine_batch_group_drawViewStatic(
        ctx, pass, 1 + cascade, ctx->mesh.vertex_buffer);
}

flecsEngine_batch_t* flecsEngine_batch_getCullBuf(
    const FlecsRenderBatch *batch)
{
    /* Polymorphic over batch ctx type. Mesh batches store the
     * flecsEngine_batch_t directly as ctx; primitive batches embed it. */
    if (!batch || !batch->ctx) {
        return NULL;
    }
    if (batch->get_cull_buf) {
        return batch->get_cull_buf(batch);
    }
    return NULL;
}
