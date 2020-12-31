#include "edyn/serialization/paged_triangle_mesh_s11n.hpp"
#include "edyn/serialization/triangle_mesh_s11n.hpp"
#include "edyn/serialization/static_tree_s11n.hpp"
#include "edyn/serialization/math_s11n.hpp"
#include "edyn/serialization/std_s11n.hpp"
#include "edyn/serialization/memory_archive.hpp"
#include "edyn/parallel/job_dispatcher.hpp"

namespace edyn {

std::string get_submesh_path(const std::string &paged_triangle_mesh_path, size_t index) {
    auto submesh_path = paged_triangle_mesh_path;
    auto dot_pos = submesh_path.find(".");

    if (dot_pos != std::string::npos) {
        submesh_path.insert(dot_pos, std::to_string(index));
    } else {
        submesh_path += std::to_string(index);
    }

    return submesh_path;
}

void paged_triangle_mesh_file_output_archive::operator()(triangle_mesh &tri_mesh) {
    switch(m_mode) {
    case paged_triangle_mesh_serialization_mode::embedded:
        serialize(*this, tri_mesh);
        break;
    case paged_triangle_mesh_serialization_mode::external: {
        auto tri_mesh_path = get_submesh_path(m_path, m_triangle_mesh_index);
        auto archive = file_output_archive(tri_mesh_path);
        serialize(archive, tri_mesh);
        break;
    }
    }
    ++m_triangle_mesh_index;
}

void serialize(paged_triangle_mesh_file_output_archive &archive, 
               paged_triangle_mesh &paged_tri_mesh) {
    archive(paged_tri_mesh.m_tree);
    auto num_submeshes = paged_tri_mesh.m_cache.size();
    archive(num_submeshes);

    for (auto &entry : paged_tri_mesh.m_cache) {
        archive(entry.num_vertices);
        archive(entry.num_indices);
    }

    archive(archive.m_mode);

    if (archive.m_mode == paged_triangle_mesh_serialization_mode::embedded) {
        size_t tri_mesh_offset = 0;
        for (size_t i = 0; i < num_submeshes; ++i) {
            archive(tri_mesh_offset);
            auto tri_mesh_size = serialization_sizeof(*paged_tri_mesh.m_cache[i].trimesh);
            tri_mesh_offset += tri_mesh_size;
        }
    }

    for (auto &entry : paged_tri_mesh.m_cache) {
        archive(*entry.trimesh);
    }
}

void serialize(paged_triangle_mesh_file_input_archive &archive, 
               paged_triangle_mesh &paged_tri_mesh) {
    archive(paged_tri_mesh.m_tree);

    size_t num_submeshes;
    archive(num_submeshes);
    paged_tri_mesh.m_cache.resize(num_submeshes);

    for (size_t i = 0; i < num_submeshes; ++i) {
        auto &entry = paged_tri_mesh.m_cache[i];
        archive(entry.num_vertices);
        archive(entry.num_indices);
    }

    archive(archive.m_mode);

    if (archive.m_mode == paged_triangle_mesh_serialization_mode::embedded) {
        archive.m_offsets.resize(num_submeshes);

        for (size_t i = 0; i < num_submeshes; ++i) {
            archive(archive.m_offsets[i]);
        }

        archive.m_base_offset = archive.tell_position();
    }

    // Resize LRU queue to have the number of submeshes.
    paged_tri_mesh.m_lru_indices.resize(num_submeshes);
    std::iota(paged_tri_mesh.m_lru_indices.begin(), 
              paged_tri_mesh.m_lru_indices.end(), 0);

    paged_tri_mesh.m_is_loading_submesh.resize(num_submeshes, false);
}

template<typename Archive>
void serialize(Archive &archive, load_mesh_context &ctx) {
    archive(ctx.m_index);
    archive(ctx.m_input);
    archive(ctx.m_mesh);
    archive(ctx.m_scheduler);
}

void paged_triangle_mesh_file_input_archive::load(size_t index) {
    auto ctx = load_mesh_context();
    ctx.m_input = reinterpret_cast<intptr_t>(this);
    ctx.m_index = index;
    ctx.m_mesh = reinterpret_cast<intptr_t>(new triangle_mesh);
    ctx.m_scheduler = job_dispatcher::global().get_current_scheduler();

    auto j = job();
    j.func = &load_mesh_job_func;
    auto archive = fixed_memory_output_archive(j.data.data(), j.data.size());
    serialize(archive, ctx);
    job_dispatcher::global().async(j);
}

void load_mesh_job_func(job::data_type &data) {
    load_mesh_context ctx;
    auto archive = memory_input_archive(data.data(), data.size());
    serialize(archive, ctx);

    auto *input = reinterpret_cast<paged_triangle_mesh_file_input_archive *>(ctx.m_input);
    auto *mesh = reinterpret_cast<triangle_mesh *>(ctx.m_mesh);

    switch(input->m_mode) {
    case paged_triangle_mesh_serialization_mode::embedded:
        input->seek_position(input->m_base_offset + input->m_offsets[ctx.m_index]);
        serialize(*input, *mesh);
        break;
    case paged_triangle_mesh_serialization_mode::external: {
        auto tri_mesh_path = get_submesh_path(input->m_path, ctx.m_index);
        auto tri_mesh_archive = file_input_archive(tri_mesh_path);
        serialize(tri_mesh_archive, *mesh);
        break;
    }
    }

    auto finish_job = job();
    std::copy(data.begin(), data.end(), finish_job.data.begin());
    finish_job.func = &finish_load_mesh_job_func;
    ctx.m_scheduler.push(finish_job);
}

void finish_load_mesh_job_func(job::data_type &data) {
    load_mesh_context ctx;
    auto archive = memory_input_archive(data.data(), data.size());
    serialize(archive, ctx);

    auto *mesh = reinterpret_cast<triangle_mesh *>(ctx.m_mesh);
    auto mesh_ptr = std::unique_ptr<triangle_mesh>(mesh);
    
    auto *input = reinterpret_cast<paged_triangle_mesh_file_input_archive *>(ctx.m_input);
    input->m_loaded_mesh_signal.publish(ctx.m_index, mesh_ptr);
}

}