#include "common/common.h"

/* --- Non-textured mesh batch creation --- */

ecs_entity_t flecsEngine_createBatch_mesh_materialIndex(
    ecs_world_t *world,
    ecs_entity_t parent,
    const char *name)
{
    ecs_entity_t batch = ecs_entity(world, { .parent = parent, .name = name });
    ecs_entity_t shader = flecsEngine_shader_pbr(world);

    ecs_query_t *q = ecs_query(world, {
        .entity = batch,
        .terms = {
            { .id = ecs_id(FlecsMesh3Impl), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsWorldTransform3), .src.id = EcsSelf },
            { .id = ecs_id(FlecsAABB), .src.id = EcsSelf },
            { .id = ecs_id(FlecsMaterialId), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsPbrTextures), .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
            { .id = FlecsAlphaBlend, .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
            { .id = ecs_id(FlecsTransmission), .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
            { .id = FlecsDynamicTransform, .oper = EcsOptional },
            { .id = ecs_id(FlecsBufferSlot), .oper = EcsNot },
        },
        .cache_kind = EcsQueryCacheAuto,
        .group_by = EcsIsA,
        .group_by_callback = flecsEngine_mesh_groupByMesh,
        .on_group_create = flecsEngine_mesh_onGroupCreate,
        .on_group_delete = flecsEngine_mesh_onGroupDelete
    });

    ecs_set(world, batch, FlecsRenderBatch, {
        .shader = shader,
        .query = q,
        .vertex_type = ecs_id(FlecsGpuVertexLitUv),
        .extract_callback = flecsEngine_mesh_extract,
        .upload_callback = flecsEngine_mesh_upload,
        .callback = flecsEngine_mesh_render,
        .shadow_callback = flecsEngine_mesh_renderShadow,
        .get_cull_buf = flecsEngine_mesh_getCullBuf,
        .ctx = flecsEngine_mesh_createCtx(FLECS_BATCH_TRACK_STATIC),
        .free_ctx = flecsEngine_mesh_deleteCtx
    });

    return batch;
}

ecs_entity_t flecsEngine_createBatch_mesh_materialData(
    ecs_world_t *world,
    ecs_entity_t parent,
    const char *name)
{
    ecs_entity_t batch = ecs_entity(world, { .parent = parent, .name = name });
    ecs_entity_t shader = flecsEngine_shader_pbr(world);

    ecs_query_t *q = ecs_query(world, {
        .entity = batch,
        .terms = {
            { .id = ecs_id(FlecsMesh3Impl), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsWorldTransform3), .src.id = EcsSelf },
            { .id = ecs_id(FlecsAABB), .src.id = EcsSelf },
            { .id = ecs_id(FlecsRgba), .src.id = EcsSelf, .oper = EcsOptional },
            { .id = ecs_id(FlecsPbrMaterial), .src.id = EcsSelf, .oper = EcsOptional },
            { .id = ecs_id(FlecsEmissive), .src.id = EcsSelf, .oper = EcsOptional },
            { .id = ecs_id(FlecsMaterialId), .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
            { .id = ecs_id(FlecsPbrTextures), .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
            { .id = ecs_id(FlecsTransmission), .src.id = EcsSelf, .oper = EcsNot },
            { .id = FlecsDynamicTransform, .oper = EcsOptional },
            { .id = ecs_id(FlecsBufferSlot), .oper = EcsNot },
        },
        .cache_kind = EcsQueryCacheAuto,
        .group_by = EcsIsA,
        .group_by_callback = flecsEngine_mesh_groupByMesh,
        .on_group_create = flecsEngine_mesh_onGroupCreate,
        .on_group_delete = flecsEngine_mesh_onGroupDelete
    });

    ecs_set(world, batch, FlecsRenderBatch, {
        .shader = shader,
        .query = q,
        .vertex_type = ecs_id(FlecsGpuVertexLitUv),
        .extract_callback = flecsEngine_mesh_extract,
        .upload_callback = flecsEngine_mesh_upload,
        .callback = flecsEngine_mesh_render,
        .shadow_callback = flecsEngine_mesh_renderShadow,
        .get_cull_buf = flecsEngine_mesh_getCullBuf,
        .ctx = flecsEngine_mesh_createCtx(
            FLECS_BATCH_OWNS_MATERIAL | FLECS_BATCH_TRACK_STATIC),
        .free_ctx = flecsEngine_mesh_deleteCtx
    });

    return batch;
}

/* --- Textured mesh batch --- */

ecs_entity_t flecsEngine_createBatch_textured_mesh(
    ecs_world_t *world,
    ecs_entity_t parent,
    const char *name)
{
    ecs_entity_t batch = ecs_entity(world, { .parent = parent, .name = name });
    ecs_entity_t shader = flecsEngine_shader_pbr(world);

    ecs_query_t *q = ecs_query(world, {
        .entity = batch,
        .terms = {
            { .id = ecs_id(FlecsMesh3Impl), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsWorldTransform3), .src.id = EcsSelf },
            { .id = ecs_id(FlecsAABB), .src.id = EcsSelf },
            { .id = ecs_id(FlecsMaterialId), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsPbrTextures), .src.id = EcsUp, .trav = EcsIsA },
            { .id = FlecsAlphaBlend, .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
            { .id = ecs_id(FlecsTransmission), .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
            { .id = FlecsDynamicTransform, .oper = EcsOptional },
            { .id = ecs_id(FlecsBufferSlot), .oper = EcsNot },
        },
        .cache_kind = EcsQueryCacheAuto,
        .group_by = EcsIsA,
        .group_by_callback = flecsEngine_mesh_groupByMesh,
        .on_group_create = flecsEngine_mesh_onGroupCreate,
        .on_group_delete = flecsEngine_mesh_onGroupDelete
    });

    ecs_set(world, batch, FlecsRenderBatch, {
        .shader = shader,
        .query = q,
        .vertex_type = ecs_id(FlecsGpuVertexLitUv),
        .extract_callback = flecsEngine_mesh_extract,
        .upload_callback = flecsEngine_mesh_upload,
        .callback = flecsEngine_mesh_render,
        .shadow_callback = flecsEngine_mesh_renderShadow,
        .get_cull_buf = flecsEngine_mesh_getCullBuf,
        .ctx = flecsEngine_mesh_createCtx(FLECS_BATCH_TRACK_STATIC),
        .free_ctx = flecsEngine_mesh_deleteCtx
    });

    return batch;
}

/* --- Unified transparent mesh batch --- */

typedef struct {
    uint64_t group_id;
    int32_t instance_index;
    float distance_sq;
} flecsEngine_sorted_instance_t;

static int flecsEngine_sortInstanceByDistance(
    const void *a,
    const void *b)
{
    const flecsEngine_sorted_instance_t *ia = a;
    const flecsEngine_sorted_instance_t *ib = b;
    /* Back-to-front: farthest first. Break ties by group to cluster
     * same-mesh instances together and reduce GPU state switches. */
    if (ia->distance_sq < ib->distance_sq) return 1;
    if (ia->distance_sq > ib->distance_sq) return -1;
    if (ia->group_id < ib->group_id) return -1;
    if (ia->group_id > ib->group_id) return 1;
    return 0;
}

typedef struct {
    flecsEngine_batch_t buffers;
    ecs_entity_t self_entity;
} flecsEngine_transparent_mesh_ctx_t;

static flecsEngine_transparent_mesh_ctx_t* flecsEngine_transparent_mesh_createCtx(
    ecs_entity_t self_entity)
{
    flecsEngine_transparent_mesh_ctx_t *ctx =
        ecs_os_calloc_t(flecsEngine_transparent_mesh_ctx_t);
    flecsEngine_batch_init(&ctx->buffers,
        FLECS_BATCH_NO_GPU_CULL);
    ctx->self_entity = self_entity;
    return ctx;
}

static void flecsEngine_transparent_mesh_deleteCtx(void *ptr)
{
    flecsEngine_transparent_mesh_ctx_t *ctx = ptr;
    flecsEngine_batch_fini(&ctx->buffers);
    ecs_os_free(ctx);
}

static void* flecsEngine_transparent_mesh_getCullBuf(
    const FlecsRenderBatch *batch)
{
    flecsEngine_transparent_mesh_ctx_t *tctx = batch->ctx;
    return &tctx->buffers;
}

static void flecsEngine_transparent_mesh_render(
    const ecs_world_t *world,
    const FlecsEngineImpl *engine,
    const FlecsRenderViewImpl *view_impl,
    const WGPURenderPassEncoder pass,
    const FlecsRenderBatch *batch)
{
    FLECS_TRACY_ZONE_BEGIN("TransparentMeshRender");

    flecsEngine_transparent_mesh_ctx_t *tctx = batch->ctx;
    flecsEngine_batch_bindMaterialGroup(
        (FlecsEngineImpl*)engine, pass, &tctx->buffers);
    flecsEngine_batch_bindInstanceGroup(
        (FlecsEngineImpl*)engine, pass, &tctx->buffers);

    const ecs_map_t *groups = ecs_query_get_groups(batch->query);
    ecs_assert(groups != NULL, ECS_INTERNAL_ERROR, NULL);

    int32_t total_instances = 0;
    ecs_map_iter_t git = ecs_map_iter(groups);
    while (ecs_map_next(&git)) {
        uint64_t group_id = ecs_map_key(&git);
        if (!group_id) continue;
        flecsEngine_batch_group_t *ctx =
            ecs_query_get_group_ctx(batch->query, group_id);
        if (!ctx || !ctx->view.count) continue;
        total_instances += ctx->view.count;
    }

    if (!total_instances) {
        FLECS_TRACY_ZONE_END;
        return;
    }

    flecsEngine_sorted_instance_t *sorted =
        ecs_os_malloc_n(flecsEngine_sorted_instance_t, total_instances);

    float cam_x = view_impl->camera_pos[0];
    float cam_y = view_impl->camera_pos[1];
    float cam_z = view_impl->camera_pos[2];

    int32_t si = 0;
    git = ecs_map_iter(groups);
    while (ecs_map_next(&git)) {
        uint64_t group_id = ecs_map_key(&git);
        if (!group_id) continue;

        flecsEngine_batch_group_t *ctx =
            ecs_query_get_group_ctx(batch->query, group_id);
        if (!ctx || !ctx->view.count) continue;

        for (int32_t j = 0; j < ctx->view.count; j ++) {
            uint32_t slot = (uint32_t)(ctx->view.offset + j);
            const FlecsGpuTransform *t =
                &tctx->buffers.buffers.cpu_transforms[slot];
            float dx = t->c3.x - cam_x;
            float dy = t->c3.y - cam_y;
            float dz = t->c3.z - cam_z;

            sorted[si].group_id = group_id;
            sorted[si].instance_index = (int32_t)slot;
            sorted[si].distance_sq = dx * dx + dy * dy + dz * dz;
            si ++;
        }
    }

    qsort(sorted, (size_t)total_instances,
        sizeof(flecsEngine_sorted_instance_t),
        flecsEngine_sortInstanceByDistance);

    uint32_t *sorted_slots = ecs_os_malloc_n(uint32_t, total_instances);
    for (int32_t i = 0; i < total_instances; i ++) {
        sorted_slots[i] = (uint32_t)sorted[i].instance_index;
    }
    wgpuQueueWriteBuffer(engine->queue,
        tctx->buffers.buffers.gpu_visible_slots, 0,
        sorted_slots,
        (uint64_t)total_instances * sizeof(uint32_t));
    ecs_os_free(sorted_slots);

    const FlecsRenderBatchImpl *self_impl =
        ecs_get(world, tctx->self_entity, FlecsRenderBatchImpl);
    wgpuRenderPassEncoderSetPipeline(pass, self_impl->pipeline_hdr);
    if (engine->textures.fallback_bind_group) {
        wgpuRenderPassEncoderSetBindGroup(
            pass, 1, engine->textures.fallback_bind_group, 0, NULL);
    }

    uint64_t active_group = 0;
    flecsEngine_batch_group_t *active_ctx = NULL;
    int8_t last_bucket = -1;

    for (int32_t i = 0; i < total_instances; i ++) {
        uint64_t group_id = sorted[i].group_id;

        if (group_id != active_group) {
            active_group = group_id;
            active_ctx = ecs_query_get_group_ctx(batch->query, group_id);
            ecs_assert(active_ctx != NULL, ECS_INTERNAL_ERROR, NULL);

            if (!active_ctx->mesh.vertex_uv_buffer ||
                !active_ctx->mesh.index_buffer ||
                !active_ctx->mesh.index_count)
            {
                active_ctx = NULL;
                continue;
            }

            if (active_ctx->texture_bucket == FLECS_ENGINE_BUCKET_INVALID) {
                active_ctx = NULL;
                continue;
            }

            int8_t b = active_ctx->texture_bucket;
            WGPUBindGroup bg = engine->textures.fallback_bind_group;
            if (b >= 0 && b < FLECS_ENGINE_TEXTURE_BUCKET_COUNT) {
                WGPUBindGroup bb = engine->textures.bucket_bind_groups[b];
                if (bb) bg = bb;
            } else {
                b = -1;
            }
            if (b != last_bucket && bg) {
                wgpuRenderPassEncoderSetBindGroup(pass, 1, bg, 0, NULL);
                last_bucket = b;
            }

            wgpuRenderPassEncoderSetVertexBuffer(
                pass, 0, active_ctx->mesh.vertex_uv_buffer,
                0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetIndexBuffer(
                pass, active_ctx->mesh.index_buffer, WGPUIndexFormat_Uint32,
                0, WGPU_WHOLE_SIZE);
        }
        if (!active_ctx) continue;

        wgpuRenderPassEncoderSetVertexBuffer(
            pass, 1, tctx->buffers.buffers.gpu_visible_slots,
            (uint64_t)i * sizeof(uint32_t), sizeof(uint32_t));

        wgpuRenderPassEncoderDrawIndexed(
            pass, active_ctx->mesh.index_count, 1, 0, 0, 0);
    }

    ecs_os_free(sorted);

    FLECS_TRACY_ZONE_END;
}

ecs_entity_t flecsEngine_createBatch_mesh_transparent(
    ecs_world_t *world,
    ecs_entity_t parent,
    const char *name)
{
    ecs_entity_t batch = ecs_entity(world, {
        .parent = parent, .name = name });

    ecs_entity_t shader = flecsEngine_shader_pbr(world);

    ecs_query_t *q = ecs_query(world, {
        .entity = batch,
        .terms = {
            { .id = ecs_id(FlecsMesh3Impl), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsWorldTransform3), .src.id = EcsSelf },
            { .id = ecs_id(FlecsAABB), .src.id = EcsSelf },
            { .id = ecs_id(FlecsMaterialId), .src.id = EcsUp, .trav = EcsIsA },
            { .id = FlecsAlphaBlend, .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsTransmission), .src.id = EcsUp, .trav = EcsIsA,
                .oper = EcsNot },
        },
        .cache_kind = EcsQueryCacheAuto,
        .group_by = EcsIsA,
        .group_by_callback = flecsEngine_mesh_groupByMesh,
        .on_group_create = flecsEngine_mesh_onGroupCreate,
        .on_group_delete = flecsEngine_mesh_onGroupDelete
    });

    ecs_set(world, batch, FlecsRenderBatch, {
        .shader = shader,
        .query = q,
        .vertex_type = ecs_id(FlecsGpuVertexLitUv),
        .depth_test = WGPUCompareFunction_Less,
        .cull_mode = WGPUCullMode_None,
        .blend = {
            .color = {
                .operation = WGPUBlendOperation_Add,
                .srcFactor = WGPUBlendFactor_SrcAlpha,
                .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha
            },
            .alpha = {
                .operation = WGPUBlendOperation_Add,
                .srcFactor = WGPUBlendFactor_One,
                .dstFactor = WGPUBlendFactor_OneMinusSrcAlpha
            }
        },
        .depth_write = false,
        .extract_callback = flecsEngine_mesh_extract,
        .upload_callback = flecsEngine_mesh_upload,
        .callback = flecsEngine_transparent_mesh_render,
        .get_cull_buf = flecsEngine_transparent_mesh_getCullBuf,
        .ctx = flecsEngine_transparent_mesh_createCtx(batch),
        .free_ctx = flecsEngine_transparent_mesh_deleteCtx,
        .render_after_snapshot = true
    });

    return batch;
}

/* --- Transmissive mesh batch --- */

ecs_entity_t flecsEngine_createBatch_mesh_transmission(
    ecs_world_t *world,
    ecs_entity_t parent,
    const char *name)
{
    ecs_entity_t batch = ecs_entity(world, { .parent = parent, .name = name });
    ecs_entity_t shader = flecsEngine_shader_pbrTransmission(world);

    ecs_query_t *q = ecs_query(world, {
        .entity = batch,
        .terms = {
            { .id = ecs_id(FlecsMesh3Impl), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsWorldTransform3), .src.id = EcsSelf },
            { .id = ecs_id(FlecsAABB), .src.id = EcsSelf },
            { .id = ecs_id(FlecsMaterialId), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsTransmission), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsTransmission), .src.id = EcsSelf,
                .oper = EcsNot },
        },
        .cache_kind = EcsQueryCacheAuto,
        .group_by = EcsIsA,
        .group_by_callback = flecsEngine_mesh_groupByMesh,
        .on_group_create = flecsEngine_mesh_onGroupCreate,
        .on_group_delete = flecsEngine_mesh_onGroupDelete
    });

    ecs_set(world, batch, FlecsRenderBatch, {
        .shader = shader,
        .query = q,
        .vertex_type = ecs_id(FlecsGpuVertexLitUv),
        .depth_write = false,
        .extract_callback = flecsEngine_mesh_extract,
        .upload_callback = flecsEngine_mesh_upload,
        .callback = flecsEngine_mesh_render,
        .get_cull_buf = flecsEngine_mesh_getCullBuf,
        .ctx = flecsEngine_mesh_createCtx(FLECS_BATCH_TRACK_STATIC),
        .free_ctx = flecsEngine_mesh_deleteCtx,
        .render_after_snapshot = true,
        .needs_transmission = true
    });

    return batch;
}

ecs_entity_t flecsEngine_createBatch_mesh_transmissionData(
    ecs_world_t *world,
    ecs_entity_t parent,
    const char *name)
{
    ecs_entity_t batch = ecs_entity(world, { .parent = parent, .name = name });
    ecs_entity_t shader = flecsEngine_shader_pbrTransmission(world);

    ecs_query_t *q = ecs_query(world, {
        .entity = batch,
        .terms = {
            { .id = ecs_id(FlecsMesh3Impl), .src.id = EcsUp, .trav = EcsIsA },
            { .id = ecs_id(FlecsWorldTransform3), .src.id = EcsSelf },
            { .id = ecs_id(FlecsAABB), .src.id = EcsSelf },
            { .id = ecs_id(FlecsRgba), .src.id = EcsSelf, .oper = EcsOptional },
            { .id = ecs_id(FlecsPbrMaterial), .src.id = EcsSelf, .oper = EcsOptional },
            { .id = ecs_id(FlecsEmissive), .src.id = EcsSelf, .oper = EcsOptional },
            { .id = ecs_id(FlecsTransmission), .src.id = EcsSelf },
            { .id = ecs_id(FlecsMaterialId), .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
            { .id = ecs_id(FlecsPbrTextures), .src.id = EcsUp, .trav = EcsIsA, .oper = EcsNot },
        },
        .cache_kind = EcsQueryCacheAuto,
        .group_by = EcsIsA,
        .group_by_callback = flecsEngine_mesh_groupByMesh,
        .on_group_create = flecsEngine_mesh_onGroupCreate,
        .on_group_delete = flecsEngine_mesh_onGroupDelete
    });

    ecs_set(world, batch, FlecsRenderBatch, {
        .shader = shader,
        .query = q,
        .vertex_type = ecs_id(FlecsGpuVertexLitUv),
        .depth_write = false,
        .extract_callback = flecsEngine_mesh_extract,
        .upload_callback = flecsEngine_mesh_upload,
        .callback = flecsEngine_mesh_render,
        .get_cull_buf = flecsEngine_mesh_getCullBuf,
        .ctx = flecsEngine_mesh_createCtx(
            FLECS_BATCH_DEFAULT |
            FLECS_BATCH_OWNS_MATERIAL |
            FLECS_BATCH_OWNS_TRANSMISSION),
        .free_ctx = flecsEngine_mesh_deleteCtx,
        .render_after_snapshot = true,
        .needs_transmission = true
    });

    return batch;
}
