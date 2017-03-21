#include "Scene/scene_tracker.h"
#include "Scene/scene1.h"
#include "Scene/camera.h"
#include "Scene/light.h"
#include "Scene/shape.h"
#include "Scene/material.h"
#include "Scene/texture.h"
#include "CLW/clwscene.h"
#include "Scene/Collector/collector.h"
#include "iterator.h"

#include <chrono>
#include <memory>
#include <stack>
#include <vector>
#include <array>

using namespace RadeonRays;

namespace Baikal
{
    SceneTracker::SceneTracker(CLWContext context, int devidx)
        : m_context(context)
        , m_default_material(new SingleBxdf(SingleBxdf::BxdfType::kLambert))
    {
        // Get raw CL data out of CLW context
        cl_device_id id = m_context.GetDevice(devidx).GetID();
        cl_command_queue queue = m_context.GetCommandQueue(devidx);

        // Create intersection API
        m_api = CreateFromOpenClContext(m_context, id, queue);

        m_api->SetOption("acc.type", "fatbvh");
        m_api->SetOption("bvh.builder", "sah");

        m_default_material->SetInputValue("albedo", float4(0.5f, 0.6f, 0.5f, 1.f));
    }

    SceneTracker::~SceneTracker()
    {
        // Delete API
        IntersectionApi::Delete(m_api);
    }

    ClwScene& SceneTracker::CompileScene(Scene1 const& scene, Collector& mat_collector, Collector& tex_collector) const
    {
        // The overall approach is:
        // 1) Check if materials have changed, update collector if yes
        // 2) Check if textures have changed, update collector if yes
        // Note, that material are collected from shapes (potentially recursively).
        // Textures are collected from materials and lights.
        // As soon as we updated both collectors we have established
        // correct pointer to buffer index mapping for both materials and textures.
        // As soon as we have this mapping we are analyzing dirty flags and
        // updating necessary parts.

        // We need to make sure collectors are empty before proceeding
        mat_collector.Clear();
        tex_collector.Clear();

        // Create shape and light iterators
        std::unique_ptr<Iterator> shape_iter(scene.CreateShapeIterator());
        std::unique_ptr<Iterator> light_iter(scene.CreateLightIterator());

        auto default_material = m_default_material.get();
        // Collect materials from shapes first
        mat_collector.Collect(shape_iter.get(),
            // This function adds all materials to resulting map
            // recursively via Material dependency API
            [default_material](void const* item) -> std::set<void const*>
        {
            // Resulting material set
            std::set<void const*> mats;
            // Material stack
            std::stack<Material const*> material_stack;

            // Get material from current shape
            auto shape = reinterpret_cast<Shape const*>(item);
            auto material = shape->GetMaterial();

            // If shape does not have a material, use default one
            if (!material)
            {
                material = default_material;
            }

            // Push to stack as an initializer
            material_stack.push(material);

            // Drain the stack
            while (!material_stack.empty())
            {
                // Get current material
                Material const* m = material_stack.top();
                material_stack.pop();

                // Emplace into the set
                mats.emplace(m);

                // Create dependency iterator
                std::unique_ptr<Iterator> mat_iter(m->CreateMaterialIterator());

                // Push all dependencies into the stack
                for (; mat_iter->IsValid(); mat_iter->Next())
                {
                    material_stack.push(mat_iter->ItemAs<Material const>());
                }
            }

            // Return resulting set
            return mats;
        });

        // Commit stuff (we can iterate over it after commit has happened)
        mat_collector.Commit();

        // Now we need to collect textures from our materials
        // Create material iterator
        std::unique_ptr<Iterator> mat_iter(mat_collector.CreateIterator());

        // Collect textures from materials
        tex_collector.Collect(mat_iter.get(),
            [](void const* item) -> std::set<void const*>
        {
            // Texture set
            std::set<void const*> textures;

            auto material = reinterpret_cast<Material const*>(item);

            // Create texture dependency iterator
            std::unique_ptr<Iterator> tex_iter(material->CreateTextureIterator());

            // Emplace all dependent textures
            for (; tex_iter->IsValid(); tex_iter->Next())
            {
                textures.emplace(tex_iter->ItemAs<Texture const>());
            }

            // Return resulting set
            return textures;
        });


        // Collect textures from lights
        tex_collector.Collect(light_iter.get(),
            [](void const* item) -> std::set<void const*>
        {
            // Resulting set
            std::set<void const*> textures;

            auto light = reinterpret_cast<Light const*>(item);

            // Create texture dependency iterator
            std::unique_ptr<Iterator> tex_iter(light->CreateTextureIterator());

            // Emplace all dependent textures
            for (; tex_iter->IsValid(); tex_iter->Next())
            {
                textures.emplace(tex_iter->ItemAs<Texture const>());
            }

            // Return resulting set
            return textures;
        });

        // Commit textures
        tex_collector.Commit();

        // Try to find scene in cache first
        auto iter = m_scene_cache.find(&scene);

        if (iter == m_scene_cache.cend())
        {
            // If not found create scene entry in cache
            auto res = m_scene_cache.emplace(std::make_pair(&scene, ClwScene()));

            // Recompile all the stuff into cached scene
            RecompileFull(scene, mat_collector, tex_collector, res.first->second);

            // Load intersector data
            ReloadIntersector(scene, res.first->second);

            // Set scene as current
            m_current_scene = &scene;

            // Drop all dirty flags for the scene
            scene.ClearDirtyFlags();

            // Drop dirty flags for materials
            mat_collector.Finalize([](void const* item)
            {
                auto material = reinterpret_cast<Material const*>(item);
                material->SetDirty(false);
            });

            // Return the scene
            return res.first->second;
        }
        else
        {
            // Exctract cached scene entry
            auto& out = iter->second;
            auto dirty = scene.GetDirtyFlags();

            // Check if we have valid camera
            auto camera = scene.GetCamera();

            if (!camera)
            {
                throw std::runtime_error("No camera in the scene");
            }

            // Check if camera parameters have been changed
            auto camera_changed = camera->IsDirty();

            // Update camera if needed
            if (dirty & Scene1::kCamera || camera_changed)
            {
                UpdateCamera(scene, mat_collector, tex_collector, out);
            }

            {
                // Check if we have lights in the scene
                std::unique_ptr<Iterator> light_iter(scene.CreateLightIterator());

                if (!light_iter->IsValid())
                {
                    throw std::runtime_error("No lights in the scene");
                }


                // Check if light parameters have been changed
                bool lights_changed = false;

                for (; light_iter->IsValid(); light_iter->Next())
                {
                    auto light = light_iter->ItemAs<Light const>();

                    if (light->IsDirty())
                    {
                        lights_changed = true;
                        break;
                    }
                }


                // Update lights if needed
                if (dirty & Scene1::kLights || lights_changed)
                {
                    UpdateLights(scene, mat_collector, tex_collector, out);
                }
            }

            {
                // Check if we have shapes in the scene
                std::unique_ptr<Iterator> shape_iter(scene.CreateShapeIterator());

                if (!shape_iter->IsValid())
                {
                    throw std::runtime_error("No shapes in the scene");
                }

                // Check if shape parameters have been changed
                bool shapes_changed = false;

                for (; shape_iter->IsValid(); shape_iter->Next())
                {
                    auto shape = shape_iter->ItemAs<Shape const>();

                    if (shape->IsDirty())
                    {
                        shapes_changed = true;
                        break;
                    }
                }

                // Update shapes if needed
                if (dirty & Scene1::kShapes || shapes_changed)
                {
                    UpdateShapes(scene, mat_collector, tex_collector, out);

                    // Recreate intersector shapes
                    UpdateIntersector(scene, out);

                    // Attach shapes to API
                    ReloadIntersector(scene, out);
                }
            }

            // If materials need an update, do it.
            // We are passing material dirty state detection function in there.
            if (!out.material_bundle ||
                mat_collector.NeedsUpdate(out.material_bundle.get(),
                    [](void const* ptr)->bool
            {
                auto mat = reinterpret_cast<Material const*>(ptr);
                return mat->IsDirty();
            }
                ))
            {
                UpdateMaterials(scene, mat_collector, tex_collector, out);
            }

            // If textures need an update, do it.
            if (tex_collector.GetNumItems() > 0 && (
                !out.texture_bundle ||
                tex_collector.NeedsUpdate(out.texture_bundle.get(), [](void const* ptr) {
                auto tex = reinterpret_cast<Texture const*>(ptr);
                return tex->IsDirty(); })))
            {
                UpdateTextures(scene, mat_collector, tex_collector, out);
            }

                // Set current scene
                if (m_current_scene != &scene)
                {
                    // If we are changing current scene need
                    // to reload the intersector
                    ReloadIntersector(scene, out);

                    m_current_scene = &scene;
                }

                // Make sure to clear dirty flags
                scene.ClearDirtyFlags();

                // Clear material dirty flags
                mat_collector.Finalize([](void const* item)
                {
                    auto material = reinterpret_cast<Material const*>(item);
                    material->SetDirty(false);
                });

                // Return the scene
                return out;
        }
    }

    static void SplitMeshesAndInstances(Iterator* shape_iter, std::set<Mesh const*>& meshes, std::set<Instance const*>& instances, std::set<Mesh const*>& excluded_meshes)
    {
        // Clear all sets
        meshes.clear();
        instances.clear();
        excluded_meshes.clear();

        // Prepare instance check lambda
        auto is_instance = [](Shape const* shape)
        {
            if (dynamic_cast<Instance const*>(shape))
            {
                return true;
            }
            else
            {
                return false;
            }
        };

        auto instance_cast = [](Shape const* shape)
        {
            return dynamic_cast<Instance const*>(shape);
        };

        for (; shape_iter->IsValid(); shape_iter->Next())
        {
            auto shape = shape_iter->ItemAs<Shape const>();

            if (!is_instance(shape))
            {
                meshes.emplace(static_cast<Mesh const*>(shape));
            }
            else
            {
                instances.emplace(static_cast<Instance const*>(shape));
            }
        }

        for (auto& i : instances)
        {
            auto base_mesh = static_cast<Mesh const*>(i->GetBaseShape());
            if (meshes.find(base_mesh) == meshes.cend())
            {
                excluded_meshes.emplace(base_mesh);
            }
        }
    }

    static std::size_t GetShapeIdx(Iterator* shape_iter, Shape const* shape)
    {
        std::set<Mesh const*> meshes;
        std::set<Mesh const*> excluded_meshes;
        std::set<Instance const*> instances;
        SplitMeshesAndInstances(shape_iter, meshes, instances, excluded_meshes);

        std::size_t idx = 0;
        for (auto& i : meshes)
        {
            if (i == shape)
            {
                return idx;
            }

            ++idx;
        }

        for (auto& i : excluded_meshes)
        {
            if (i == shape)
            {
                return idx;
            }

            ++idx;
        }

        for (auto& i : instances)
        {
            if (i == shape)
            {
                return idx;
            }

            ++idx;
        }

        return -1;
    }

    void SceneTracker::UpdateIntersector(Scene1 const& scene, ClwScene& out) const
    {
        // Detach and delete all shapes
        for (auto& shape : out.isect_shapes)
        {
            m_api->DetachShape(shape);
            m_api->DeleteShape(shape);
        }

        // Clear shapes cache
        out.isect_shapes.clear();
        // Only visible shapes are attached to the API.
        // So excluded meshes are pushed into isect_shapes, but
        // not to visible_shapes.
        out.visible_shapes.clear();

        // Create new shapes
        std::unique_ptr<Iterator> shape_iter(scene.CreateShapeIterator());

        if (!shape_iter->IsValid())
        {
            throw std::runtime_error("No shapes in the scene");
        }

        // Split all shapes into meshes and instances sets.
        std::set<Mesh const*> meshes;
        // Excluded shapes are shapes which are not in the scene,
        // but references by at least one instance.
        std::set<Mesh const*> excluded_meshes;
        std::set<Instance const*> instances;
        SplitMeshesAndInstances(shape_iter.get(), meshes, instances, excluded_meshes);

        // Keep shape->rr shape association for 
        // instance base shape lookup.
        std::map<Shape const*, RadeonRays::Shape*> rr_shapes;

        // Start from ID 1
        // Handle meshes
        int id = 1;
        for (auto& iter : meshes)
        {
            auto mesh = iter;

            auto shape = m_api->CreateMesh(
                // Vertices starting from the first one
                (float*)mesh->GetVertices(),
                // Number of vertices
                static_cast<int>(mesh->GetNumVertices()),
                // Stride
                sizeof(float3),
                // TODO: make API signature const
                reinterpret_cast<int const*>(mesh->GetIndices()),
                // Index stride
                0,
                // All triangles
                nullptr,
                // Number of primitives
                static_cast<int>(mesh->GetNumIndices() / 3)
            );

            auto transform = mesh->GetTransform();
            shape->SetTransform(transform, inverse(transform));
            shape->SetId(id++);
            out.isect_shapes.push_back(shape);
            out.visible_shapes.push_back(shape);
            rr_shapes[mesh] = shape;
        }

        // Handle excluded meshes
        for (auto& iter : excluded_meshes)
        {
            auto mesh = iter;

            auto shape = m_api->CreateMesh(
                // Vertices starting from the first one
                (float*)mesh->GetVertices(),
                // Number of vertices
                static_cast<int>(mesh->GetNumVertices()),
                // Stride
                sizeof(float3),
                // TODO: make API signature const
                reinterpret_cast<int const*>(mesh->GetIndices()),
                // Index stride
                0,
                // All triangles
                nullptr,
                // Number of primitives
                static_cast<int>(mesh->GetNumIndices() / 3)
            );

            auto transform = mesh->GetTransform();
            shape->SetTransform(transform, inverse(transform));
            shape->SetId(id++);
            out.isect_shapes.push_back(shape);
            rr_shapes[mesh] = shape;
        }

        // Handle instances
        for (auto& iter: instances)
        {
            auto instance = iter;
            auto rr_mesh = rr_shapes[instance->GetBaseShape()];
            auto shape = m_api->CreateInstance(rr_mesh);

            auto transform = instance->GetTransform();
            shape->SetTransform(transform, inverse(transform));
            shape->SetId(id++);
            out.isect_shapes.push_back(shape);
            out.visible_shapes.push_back(shape);
        }
    }

    void SceneTracker::UpdateCamera(Scene1 const& scene, Collector& mat_collector, Collector& tex_collector, ClwScene& out) const
    {
        // TODO: support different camera types here
        auto camera = static_cast<PerspectiveCamera const*>(scene.GetCamera());

        // TODO: remove this
        out.camera_type = camera->GetAperture() > 0.f ? CameraType::kPhysical : CameraType::kDefault;

        // Update camera data
        ClwScene::Camera* data = nullptr;

        // Map GPU camera buffer
        m_context.MapBuffer(0, out.camera, CL_MAP_WRITE, &data).Wait();

        // Copy camera parameters
        data->forward = camera->GetForwardVector();
        data->up = camera->GetUpVector();
        data->right = camera->GetRightVector();
        data->p = camera->GetPosition();
        data->aperture = camera->GetAperture();
        data->aspect_ratio = camera->GetAspectRatio();
        data->dim = camera->GetSensorSize();
        data->focal_length = camera->GetFocalLength();
        data->focus_distance = camera->GetFocusDistance();
        data->zcap = camera->GetDepthRange();

        // Unmap camera buffer
        m_context.UnmapBuffer(0, out.camera, data);

        // Drop camera dirty flag
        camera->SetDirty(false);
    }

    void SceneTracker::UpdateShapes(Scene1 const& scene, Collector& mat_collector, Collector& tex_collector, ClwScene& out) const
    {
        std::size_t num_vertices = 0;
        std::size_t num_normals = 0;
        std::size_t num_uvs = 0;
        std::size_t num_indices = 0;
        std::size_t num_material_ids = 0;

        std::size_t num_vertices_written = 0;
        std::size_t num_normals_written = 0;
        std::size_t num_uvs_written = 0;
        std::size_t num_indices_written = 0;
        std::size_t num_matids_written = 0;
        std::size_t num_shapes_written = 0;

        std::unique_ptr<Iterator> shape_iter(scene.CreateShapeIterator());

        // Sort shapes into meshes and instances sets.
        std::set<Mesh const*> meshes;
        // Excluded meshes are meshes which are not in the scene, 
        // but are references by at least one instance.
        std::set<Mesh const*> excluded_meshes;
        std::set<Instance const*> instances;
        SplitMeshesAndInstances(shape_iter.get(), meshes, instances, excluded_meshes);

        // Calculate GPU array sizes. Do that only for meshes,
        // since instances do not occupy space in vertex buffers.
        // However instances still have their own material ids.
        for (auto& iter : meshes)
        {
            auto mesh = iter;

            num_vertices += mesh->GetNumVertices();
            num_normals += mesh->GetNumNormals();
            num_uvs += mesh->GetNumUVs();
            num_indices += mesh->GetNumIndices();
            num_material_ids += mesh->GetNumIndices() / 3;
        }

        // Excluded meshes still occupy space in vertex buffers.
        for (auto& iter : excluded_meshes)
        {
            auto mesh = iter;

            num_vertices += mesh->GetNumVertices();
            num_normals += mesh->GetNumNormals();
            num_uvs += mesh->GetNumUVs();
            num_indices += mesh->GetNumIndices();
            num_material_ids += mesh->GetNumIndices() / 3;
        }

        // Instances only occupy material IDs space.
        for (auto& iter : instances)
        {
            auto instance = iter;
            auto mesh = static_cast<Mesh const*>(instance->GetBaseShape());
            num_material_ids += mesh->GetNumIndices() / 3;
        }

        // Create CL arrays
        out.vertices = m_context.CreateBuffer<float3>(num_vertices, CL_MEM_READ_ONLY);
        out.normals = m_context.CreateBuffer<float3>(num_normals, CL_MEM_READ_ONLY);
        out.uvs = m_context.CreateBuffer<float2>(num_uvs, CL_MEM_READ_ONLY);
        out.indices = m_context.CreateBuffer<int>(num_indices, CL_MEM_READ_ONLY);

        // Total number of entries in shapes GPU array
        auto num_shapes = meshes.size() + excluded_meshes.size() + instances.size();
        out.shapes = m_context.CreateBuffer<ClwScene::Shape>(num_shapes, CL_MEM_READ_ONLY);
        out.materialids = m_context.CreateBuffer<int>(num_material_ids, CL_MEM_READ_ONLY);

        float3* vertices = nullptr;
        float3* normals = nullptr;
        float2* uvs = nullptr;
        int* indices = nullptr;
        int* matids = nullptr;
        ClwScene::Shape* shapes = nullptr;

        // Map arrays and prepare to write data
        m_context.MapBuffer(0, out.vertices, CL_MAP_WRITE, &vertices);
        m_context.MapBuffer(0, out.normals, CL_MAP_WRITE, &normals);
        m_context.MapBuffer(0, out.uvs, CL_MAP_WRITE, &uvs);
        m_context.MapBuffer(0, out.indices, CL_MAP_WRITE, &indices);
        m_context.MapBuffer(0, out.materialids, CL_MAP_WRITE, &matids);
        m_context.MapBuffer(0, out.shapes, CL_MAP_WRITE, &shapes).Wait();

        // Keep associated shapes data for instance look up.
        // We retrieve data from here while serializing instances,
        // using base shape lookup.
        std::map<Mesh const*, ClwScene::Shape> shape_data;
        // Handle meshes
        for (auto& iter : meshes)
        {
            auto mesh = iter;

            // Get pointers data
            auto mesh_vertex_array = mesh->GetVertices();
            auto mesh_num_vertices = mesh->GetNumVertices();

            auto mesh_normal_array = mesh->GetNormals();
            auto mesh_num_normals = mesh->GetNumNormals();

            auto mesh_uv_array = mesh->GetUVs();
            auto mesh_num_uvs = mesh->GetNumUVs();

            auto mesh_index_array = mesh->GetIndices();
            auto mesh_num_indices = mesh->GetNumIndices();

            // Prepare shape descriptor
            ClwScene::Shape shape;
            shape.numprims = static_cast<int>(mesh_num_indices / 3);
            shape.startvtx = static_cast<int>(num_vertices_written);
            shape.startidx = static_cast<int>(num_indices_written);
            shape.start_material_idx = static_cast<int>(num_matids_written);

            auto transform = mesh->GetTransform();
            shape.transform.m0 = { transform.m00, transform.m01, transform.m02, transform.m03 };
            shape.transform.m1 = { transform.m10, transform.m11, transform.m12, transform.m13 };
            shape.transform.m2 = { transform.m20, transform.m21, transform.m22, transform.m23 };
            shape.transform.m3 = { transform.m30, transform.m31, transform.m32, transform.m33 };

            shape.linearvelocity = float3(0.0f, 0.f, 0.f);
            shape.angularvelocity = float3(0.f, 0.f, 0.f, 1.f);

            shape_data[mesh] = shape;

            std::copy(mesh_vertex_array, mesh_vertex_array + mesh_num_vertices, vertices + num_vertices_written);
            num_vertices_written += mesh_num_vertices;

            std::copy(mesh_normal_array, mesh_normal_array + mesh_num_normals, normals + num_normals_written);
            num_normals_written += mesh_num_normals;

            std::copy(mesh_uv_array, mesh_uv_array + mesh_num_uvs, uvs + num_uvs_written);
            num_uvs_written += mesh_num_uvs;

            std::copy(mesh_index_array, mesh_index_array + mesh_num_indices, indices + num_indices_written);
            num_indices_written += mesh_num_indices;

            shapes[num_shapes_written++] = shape;

            // Check if mesh has a material and use default if not
            auto material = mesh->GetMaterial();
            if (!material)
            {
                material = m_default_material.get();
            }

            auto matidx = mat_collector.GetItemIndex(material);
            std::fill(matids + num_matids_written, matids + num_matids_written + mesh_num_indices / 3, matidx);

            num_matids_written += mesh_num_indices / 3;

            // Drop dirty flag
            mesh->SetDirty(false);
        }

        // Excluded shapes are handled in almost the same way
        // except materials.
        for (auto& iter : excluded_meshes)
        {
            auto mesh = iter;

            // Get pointers data
            auto mesh_vertex_array = mesh->GetVertices();
            auto mesh_num_vertices = mesh->GetNumVertices();

            auto mesh_normal_array = mesh->GetNormals();
            auto mesh_num_normals = mesh->GetNumNormals();

            auto mesh_uv_array = mesh->GetUVs();
            auto mesh_num_uvs = mesh->GetNumUVs();

            auto mesh_index_array = mesh->GetIndices();
            auto mesh_num_indices = mesh->GetNumIndices();

            // Prepare shape descriptor
            ClwScene::Shape shape;
            shape.numprims = static_cast<int>(mesh_num_indices / 3);
            shape.startvtx = static_cast<int>(num_vertices_written);
            shape.startidx = static_cast<int>(num_indices_written);
            shape.start_material_idx = static_cast<int>(num_matids_written);

            auto transform = mesh->GetTransform();
            shape.transform.m0 = { transform.m00, transform.m01, transform.m02, transform.m03 };
            shape.transform.m1 = { transform.m10, transform.m11, transform.m12, transform.m13 };
            shape.transform.m2 = { transform.m20, transform.m21, transform.m22, transform.m23 };
            shape.transform.m3 = { transform.m30, transform.m31, transform.m32, transform.m33 };

            shape.linearvelocity = float3(0.0f, 0.f, 0.f);
            shape.angularvelocity = float3(0.f, 0.f, 0.f, 1.f);

            shape_data[mesh] = shape;

            std::copy(mesh_vertex_array, mesh_vertex_array + mesh_num_vertices, vertices + num_vertices_written);
            num_vertices_written += mesh_num_vertices;

            std::copy(mesh_normal_array, mesh_normal_array + mesh_num_normals, normals + num_normals_written);
            num_normals_written += mesh_num_normals;

            std::copy(mesh_uv_array, mesh_uv_array + mesh_num_uvs, uvs + num_uvs_written);
            num_uvs_written += mesh_num_uvs;

            std::copy(mesh_index_array, mesh_index_array + mesh_num_indices, indices + num_indices_written);
            num_indices_written += mesh_num_indices;

            shapes[num_shapes_written++] = shape;

            // We do not need materials for excluded shapes, we never shade them.
            std::fill(matids + num_matids_written, matids + num_matids_written + mesh_num_indices / 3, -1);

            num_matids_written += mesh_num_indices / 3;

            // Drop dirty flag
            mesh->SetDirty(false);
        }

        // Handle instances
        for (auto& iter : instances)
        {
            auto instance = iter;
            auto base_shape = static_cast<Mesh const*>(instance->GetBaseShape());
            auto material = instance->GetMaterial();
            auto transform = instance->GetTransform();
            auto mesh_num_indices = base_shape->GetNumIndices();

            // Here shape_data is guaranteed to contain
            // info for base_shape since we have serialized it
            // above in a different pass.
            ClwScene::Shape shape = shape_data[base_shape];
            // Instance has its own material part.
            shape.start_material_idx = static_cast<int>(num_matids_written);

            // Instance has its own transform.
            shape.transform.m0 = { transform.m00, transform.m01, transform.m02, transform.m03 };
            shape.transform.m1 = { transform.m10, transform.m11, transform.m12, transform.m13 };
            shape.transform.m2 = { transform.m20, transform.m21, transform.m22, transform.m23 };
            shape.transform.m3 = { transform.m30, transform.m31, transform.m32, transform.m33 };

            shape.linearvelocity = float3(0.0f, 0.f, 0.f);
            shape.angularvelocity = float3(0.f, 0.f, 0.f, 1.f);

            shapes[num_shapes_written++] = shape;

            // If instance do not have a material, use default one.
            if (!material)
            {
                material = m_default_material.get();
            }

            auto mat_idx = mat_collector.GetItemIndex(material);
            std::fill(matids + num_matids_written, matids + num_matids_written + mesh_num_indices / 3, mat_idx);

            num_matids_written += mesh_num_indices / 3;

            // Drop dirty flag
            instance->SetDirty(false);
        }

        m_context.UnmapBuffer(0, out.vertices, vertices);
        m_context.UnmapBuffer(0, out.normals, normals);
        m_context.UnmapBuffer(0, out.uvs, uvs);
        m_context.UnmapBuffer(0, out.indices, indices);
        m_context.UnmapBuffer(0, out.materialids, matids);
        m_context.UnmapBuffer(0, out.shapes, shapes).Wait();
    }

    void SceneTracker::UpdateMaterials(Scene1 const& scene, Collector& mat_collector, Collector& tex_collector, ClwScene& out) const
    {
        // Get new buffer size
        std::size_t mat_buffer_size = mat_collector.GetNumItems();

        // Recreate material buffer if it needs resize
        if (mat_buffer_size > out.materials.GetElementCount())
        {
            // Create material buffer
            out.materials = m_context.CreateBuffer<ClwScene::Material>(mat_buffer_size, CL_MEM_READ_ONLY);
        }

        ClwScene::Material* materials = nullptr;
        std::size_t num_materials_written = 0;

        // Map GPU materials buffer
        m_context.MapBuffer(0, out.materials, CL_MAP_WRITE, &materials).Wait();

        // Serialize
        {
            // Update material bundle first to be able to track differences
            out.material_bundle.reset(mat_collector.CreateBundle());

            // Create material iterator
            std::unique_ptr<Iterator> mat_iter(mat_collector.CreateIterator());

            // Iterate and serialize
            for (; mat_iter->IsValid(); mat_iter->Next())
            {
                WriteMaterial(mat_iter->ItemAs<Material const>(), mat_collector, tex_collector, materials + num_materials_written);
                ++num_materials_written;
            }
        }

        // Unmap material buffer
        m_context.UnmapBuffer(0, out.materials, materials);
    }

    void SceneTracker::RecompileFull(Scene1 const& scene, Collector& mat_collector, Collector& tex_collector, ClwScene& out) const
    {
        // This is usually unnecessary, but just in case we reuse "out" parameter here
        for (auto& s : out.isect_shapes)
        {
            m_api->DeleteShape(s);
        }

        out.isect_shapes.clear();

        // Create camera buffer
        out.camera = m_context.CreateBuffer<ClwScene::Camera>(1, CL_MEM_READ_ONLY);

        UpdateCamera(scene, mat_collector, tex_collector, out);

        UpdateLights(scene, mat_collector, tex_collector, out);

        UpdateShapes(scene, mat_collector, tex_collector, out);

        UpdateMaterials(scene, mat_collector, tex_collector, out);

        UpdateTextures(scene, mat_collector, tex_collector, out);

        UpdateIntersector(scene, out);

        // Temporary code
        ClwScene::Volume vol = { (ClwScene::VolumeType)1, (ClwScene::PhaseFunction)0, 0, 0, {0.09f, 0.09f, 0.09f}, {0.1f, 0.1f, 0.1f}, {0.0f, 0.0f, 0.0f} };

        out.volumes = m_context.CreateBuffer<ClwScene::Volume>(1, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, &vol);
    }

    void SceneTracker::ReloadIntersector(Scene1 const& scene, ClwScene& inout) const
    {
        m_api->DetachAll();

        for (auto& s : inout.visible_shapes)
        {
            m_api->AttachShape(s);
        }

        m_api->Commit();
    }

    void SceneTracker::UpdateTextures(Scene1 const& scene, Collector& mat_collector, Collector& tex_collector, ClwScene& out) const
    {
        // Get new buffer size
        std::size_t tex_buffer_size = tex_collector.GetNumItems();
        std::size_t tex_data_buffer_size = 0;

        if (tex_buffer_size == 0)
        {
            out.textures = m_context.CreateBuffer<ClwScene::Texture>(1, CL_MEM_READ_ONLY);
            out.texturedata = m_context.CreateBuffer<char>(1, CL_MEM_READ_ONLY);
            return;
        }

        // Recreate material buffer if it needs resize
        if (tex_buffer_size > out.textures.GetElementCount())
        {
            // Create material buffer
            out.textures = m_context.CreateBuffer<ClwScene::Texture>(tex_buffer_size, CL_MEM_READ_ONLY);
        }

        ClwScene::Texture* textures = nullptr;
        std::size_t num_textures_written = 0;

        // Map GPU materials buffer
        m_context.MapBuffer(0, out.textures, CL_MAP_WRITE, &textures).Wait();

        // Update material bundle first to be able to track differences
        out.texture_bundle.reset(tex_collector.CreateBundle());

        // Create material iterator
        std::unique_ptr<Iterator> tex_iter(tex_collector.CreateIterator());

        // Iterate and serialize
        for (; tex_iter->IsValid(); tex_iter->Next())
        {
            auto tex = tex_iter->ItemAs<Texture const>();

            WriteTexture(tex, tex_data_buffer_size, textures + num_textures_written);

            ++num_textures_written;

            tex_data_buffer_size += tex->GetSizeInBytes();
        }

        // Unmap material buffer
        m_context.UnmapBuffer(0, out.textures, textures);

        // Recreate material buffer if it needs resize
        if (tex_data_buffer_size > out.texturedata.GetElementCount())
        {
            // Create material buffer
            out.texturedata = m_context.CreateBuffer<char>(tex_data_buffer_size, CL_MEM_READ_ONLY);
        }

        char* data = nullptr;
        std::size_t num_bytes_written = 0;

        tex_iter->Reset();

        // Map GPU materials buffer
        m_context.MapBuffer(0, out.texturedata, CL_MAP_WRITE, &data).Wait();

        // Write texture data for all textures
        for (; tex_iter->IsValid(); tex_iter->Next())
        {
            auto tex = tex_iter->ItemAs<Texture const>();

            WriteTextureData(tex, data + num_bytes_written);

            num_bytes_written += tex->GetSizeInBytes();
        }

        // Unmap material buffer
        m_context.UnmapBuffer(0, out.texturedata, data);
    }

    // Convert Material:: types to ClwScene:: types
    static ClwScene::Bxdf GetMaterialType(Material const* material)
    {
        // Distinguish between single bxdf materials and compound ones
        if (auto bxdf = dynamic_cast<SingleBxdf const*>(material))
        {
            switch (bxdf->GetBxdfType())
            {
            case SingleBxdf::BxdfType::kZero: return ClwScene::Bxdf::kZero;
            case SingleBxdf::BxdfType::kLambert: return ClwScene::Bxdf::kLambert;
            case SingleBxdf::BxdfType::kEmissive: return ClwScene::Bxdf::kEmissive;
            case SingleBxdf::BxdfType::kPassthrough: return ClwScene::Bxdf::kPassthrough;
            case SingleBxdf::BxdfType::kTranslucent: return ClwScene::Bxdf::kTranslucent;
            case SingleBxdf::BxdfType::kIdealReflect: return ClwScene::Bxdf::kIdealReflect;
            case SingleBxdf::BxdfType::kIdealRefract: return ClwScene::Bxdf::kIdealRefract;
            case SingleBxdf::BxdfType::kMicrofacetGGX: return ClwScene::Bxdf::kMicrofacetGGX;
            case SingleBxdf::BxdfType::kMicrofacetBeckmann: return ClwScene::Bxdf::kMicrofacetBeckmann;
            case SingleBxdf::BxdfType::kMicrofacetRefractionGGX: return ClwScene::Bxdf::kMicrofacetRefractionGGX;
            case SingleBxdf::BxdfType::kMicrofacetRefractionBeckmann: return ClwScene::Bxdf::kMicrofacetRefractionBeckmann;
            }
        }
        else if (auto mat = dynamic_cast<MultiBxdf const*>(material))
        {
            switch (mat->GetType())
            {
            case MultiBxdf::Type::kMix: return ClwScene::Bxdf::kMix;
            case MultiBxdf::Type::kLayered: return ClwScene::Bxdf::kLayered;
            case MultiBxdf::Type::kFresnelBlend: return ClwScene::Bxdf::kFresnelBlend;
            }
        }
        else
        {
            return ClwScene::Bxdf::kZero;
        }

        return ClwScene::Bxdf::kZero;
    }

    void SceneTracker::WriteMaterial(Material const* material, Collector& mat_collector, Collector& tex_collector, void* data) const
    {
        auto clw_material = reinterpret_cast<ClwScene::Material*>(data);

        // Convert material type and sidedness
        auto type = GetMaterialType(material);

        clw_material->type = type;

        switch (type)
        {
        case ClwScene::Bxdf::kZero:
            clw_material->kx = RadeonRays::float4();
            break;

            // We need to convert roughness for the following materials
        case ClwScene::Bxdf::kMicrofacetGGX:
        case ClwScene::Bxdf::kMicrofacetBeckmann:
        case ClwScene::Bxdf::kMicrofacetRefractionGGX:
        case ClwScene::Bxdf::kMicrofacetRefractionBeckmann:
        {
            Material::InputValue value = material->GetInputValue("roughness");

            if (value.type == Material::InputType::kFloat4)
            {
                clw_material->ns = value.float_value.x;
                clw_material->nsmapidx = -1;
            }
            else if (value.type == Material::InputType::kTexture)
            {
                clw_material->nsmapidx = value.tex_value ? tex_collector.GetItemIndex(value.tex_value) : -1;
            }
            else
            {
                // TODO: should not happen
                assert(false);
            }

            // Intentionally missing break here
        }

        // For the rest we need to conver albedo, normal map, fresnel factor, ior
        case ClwScene::Bxdf::kLambert:
        case ClwScene::Bxdf::kEmissive:
        case ClwScene::Bxdf::kPassthrough:
        case ClwScene::Bxdf::kTranslucent:
        case ClwScene::Bxdf::kIdealRefract:
        case ClwScene::Bxdf::kIdealReflect:
        {
            Material::InputValue value = material->GetInputValue("albedo");

            if (value.type == Material::InputType::kFloat4)
            {
                clw_material->kx = value.float_value;
                clw_material->kxmapidx = -1;
            }
            else if (value.type == Material::InputType::kTexture)
            {
                clw_material->kxmapidx = value.tex_value ? tex_collector.GetItemIndex(value.tex_value) : -1;
            }
            else
            {
                // TODO: should not happen
                assert(false);
            }

            value = material->GetInputValue("normal");

            if (value.type == Material::InputType::kTexture && value.tex_value)
            {
                clw_material->nmapidx = tex_collector.GetItemIndex(value.tex_value);
                clw_material->bump_flag = 0;
            }
            else
            {
                value = material->GetInputValue("bump");

                if (value.type == Material::InputType::kTexture && value.tex_value)
                {
                    clw_material->nmapidx = tex_collector.GetItemIndex(value.tex_value);
                    clw_material->bump_flag = 1;
                }
                else
                {
                    clw_material->nmapidx = -1;
                    clw_material->bump_flag = 0;
                }
            }

            value = material->GetInputValue("fresnel");

            if (value.type == Material::InputType::kFloat4)
            {
                clw_material->fresnel = value.float_value.x > 0 ? 1.f : 0.f;
            }
            else
            {
                clw_material->fresnel = 0.f;
            }

            value = material->GetInputValue("ior");

            if (value.type == Material::InputType::kFloat4)
            {
                clw_material->ni = value.float_value.x;
            }
            else
            {
                clw_material->ni = 1.f;
            }

            value = material->GetInputValue("roughness");

            if (value.type == Material::InputType::kFloat4)
            {
                clw_material->ns = value.float_value.x;
            }
            else
            {
                clw_material->ns = 0.99f;
            }

            break;
        }

        // For compound materials we need to convert dependencies
        // and weights.
        case ClwScene::Bxdf::kMix:
        case ClwScene::Bxdf::kFresnelBlend:
        {
            Material::InputValue value0 = material->GetInputValue("base_material");
            Material::InputValue value1 = material->GetInputValue("top_material");

            if (value0.type == Material::InputType::kMaterial &&
                value1.type == Material::InputType::kMaterial)
            {
                clw_material->brdfbaseidx = mat_collector.GetItemIndex(value0.mat_value);
                clw_material->brdftopidx = mat_collector.GetItemIndex(value1.mat_value);
            }
            else
            {
                // Should not happen
                assert(false);
            }

            if (type == ClwScene::Bxdf::kMix)
            {
                clw_material->fresnel = 0.f;

                Material::InputValue value = material->GetInputValue("weight");

                if (value.type == Material::InputType::kTexture)
                {
                    clw_material->nsmapidx = tex_collector.GetItemIndex(value.tex_value);
                }
                else
                {
                    clw_material->nsmapidx = -1;
                    clw_material->ns = value.float_value.x;
                }
            }
            else
            {
                clw_material->fresnel = 1.f;

                Material::InputValue value = material->GetInputValue("ior");

                if (value.type == Material::InputType::kFloat4)
                {
                    clw_material->ni = value.float_value.x;
                }
                else
                {
                    // Should not happen
                    assert(false);
                }
            }
        }

        default:
            break;
        }

        material->SetDirty(false);
    }

    // Convert Light:: types to ClwScene:: types
    static int GetLightType(Light const* light)
    {

        if (dynamic_cast<PointLight const*>(light))
        {
            return ClwScene::kPoint;
        }
        else if (dynamic_cast<DirectionalLight const*>(light))
        {
            return ClwScene::kDirectional;
        }
        else if (dynamic_cast<SpotLight const*>(light))
        {
            return ClwScene::kSpot;
        }
        else if (dynamic_cast<ImageBasedLight const*>(light))
        {
            return ClwScene::kIbl;
        }
        else
        {
            return ClwScene::LightType::kArea;
        }
    }

    void SceneTracker::WriteLight(Scene1 const& scene, Light const* light, Collector& tex_collector, void* data) const
    {
        auto clw_light = reinterpret_cast<ClwScene::Light*>(data);

        auto type = GetLightType(light);

        clw_light->type = type;

        switch (type)
        {
        case ClwScene::kPoint:
        {
            clw_light->p = light->GetPosition();
            clw_light->intensity = light->GetEmittedRadiance();
            break;
        }

        case ClwScene::kDirectional:
        {
            clw_light->d = light->GetDirection();
            clw_light->intensity = light->GetEmittedRadiance();
            break;
        }

        case ClwScene::kSpot:
        {
            clw_light->p = light->GetPosition();
            clw_light->d = light->GetDirection();
            clw_light->intensity = light->GetEmittedRadiance();

            auto cone_shape = static_cast<SpotLight const*>(light)->GetConeShape();
            clw_light->ia = cone_shape.x;
            clw_light->oa = cone_shape.y;
            break;
        }

        case ClwScene::kIbl:
        {
            // TODO: support this
            clw_light->multiplier = static_cast<ImageBasedLight const*>(light)->GetMultiplier();
            auto tex = static_cast<ImageBasedLight const*>(light)->GetTexture();
            clw_light->tex = tex_collector.GetItemIndex(tex);
            clw_light->texdiffuse = clw_light->tex;
            break;
        }

        case ClwScene::kArea:
        {
            // TODO: optimize this linear search
            auto shape = static_cast<AreaLight const*>(light)->GetShape();

            std::unique_ptr<Iterator> shape_iter(scene.CreateShapeIterator());

            auto idx = GetShapeIdx(shape_iter.get(), shape);

            clw_light->shapeidx = static_cast<int>(idx);
            clw_light->primidx = static_cast<int>(static_cast<AreaLight const*>(light)->GetPrimitiveIdx());
            break;
        }


        default:
            assert(false);
            break;
        }
    }

    void SceneTracker::UpdateLights(Scene1 const& scene, Collector& mat_collector, Collector& tex_collector, ClwScene& out) const
    {
        std::size_t num_lights_written = 0;

        auto num_lights = scene.GetNumLights();

        // Create light buffer if needed
        if (num_lights > out.lights.GetElementCount())
        {
            out.lights = m_context.CreateBuffer<ClwScene::Light>(num_lights, CL_MEM_READ_ONLY);
        }

        ClwScene::Light* lights = nullptr;

        m_context.MapBuffer(0, out.lights, CL_MAP_WRITE, &lights).Wait();

        std::unique_ptr<Iterator> light_iter(scene.CreateLightIterator());

        // Disable IBL by default
        out.envmapidx = -1;

        // Serialize
        {
            for (; light_iter->IsValid(); light_iter->Next())
            {
                auto light = light_iter->ItemAs<Light const>();
                WriteLight(scene, light, tex_collector, lights + num_lights_written);
                ++num_lights_written;

                // Find and update IBL idx
                auto ibl = dynamic_cast<ImageBasedLight const*>(light_iter->ItemAs<Light const>());
                if (ibl)
                {
                    out.envmapidx = static_cast<int>(num_lights_written - 1);
                }

                light->SetDirty(false);
            }
        }

        m_context.UnmapBuffer(0, out.lights, lights);

        out.num_lights = static_cast<int>(num_lights_written);
    }


    // Convert texture format into ClwScene:: types
    static ClwScene::TextureFormat GetTextureFormat(Texture const* texture)
    {
        switch (texture->GetFormat())
        {
        case Texture::Format::kRgba8: return ClwScene::TextureFormat::RGBA8;
        case Texture::Format::kRgba16: return ClwScene::TextureFormat::RGBA16;
        case Texture::Format::kRgba32: return ClwScene::TextureFormat::RGBA32;
        default: return ClwScene::TextureFormat::RGBA8;
        }
    }

    void SceneTracker::WriteTexture(Texture const* texture, std::size_t data_offset, void* data) const
    {
        auto clw_texture = reinterpret_cast<ClwScene::Texture*>(data);

        auto dim = texture->GetSize();

        clw_texture->w = dim.x;
        clw_texture->h = dim.y;
        clw_texture->fmt = GetTextureFormat(texture);
        clw_texture->dataoffset = static_cast<int>(data_offset);
    }

    void SceneTracker::WriteTextureData(Texture const* texture, void* data) const
    {
        auto begin = texture->GetData();
        auto end = begin + texture->GetSizeInBytes();
        std::copy(begin, end, static_cast<char*>(data));
    }
}
