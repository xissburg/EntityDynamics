# Edyn Design Document

This document describes the general engine architecture. It is a bit of a brainstorming document and does not reflect the current state of the library. The ideas presented here are planned to be implemented in the near future.

# Introduction

_Edyn_ (pronounced as _eh-dyin'_) stands for _Entity Dynamics_ and it is a real-time physics engine focused on multi-threaded, networked and distributed simulation of massive dynamic worlds. It is organized as an _entity-component system_ (ECS) using the amazing [EnTT](https://github.com/skypjack/entt) library.

## The ECS approach

Typical physics engines will offer explicit means to create objects such as rigid bodies, whereas in _Edyn_ object creation is implicit due to the entity-component design. A rigid body is created from the bottom up, by associating its parts to a single entity, such as:

```cpp
entt::registry registry;
auto entity = registry.create();
registry.emplace<edyn::dynamic_tag>(entity);
registry.emplace<edyn::position>(entity, 0, 3, 0);
registry.emplace<edyn::orientation>(entity, edyn::quaternion_axis_angle({0, 1, 0}, edyn::to_radians(30)));
registry.emplace<edyn::linvel>(entity, edyn::vector3_zero);
registry.emplace<edyn::angvel>(entity, 0, 0.314, 0);
auto mass = edyn::scalar{50};
registry.emplace<edyn::mass>(entity, mass);
auto &box = registry.emplace<edyn::box_shape>(entity, edyn::vector3{0.5, 0.2, 0.4});
registry.emplace<edyn::inertia>(entity, edyn::moment_of_inertia(box, mass));
registry.emplace<edyn::material>(entity, 0.2, 0.9); // Restitution and friction.
registry.emplace<edyn::linacc>(entity, edyn::gravity_earth);
```

There's no explicit mention of a rigid body in the code, but during the physics update all entities that have a combination of the components assigned above will be treated as a rigid body and their state will be update over time as expected. The update may be carried as follows:

```cpp
// Apply gravity acceleration, increasing linear velocity
auto view = registry.view<edyn::linvel, const edyn::linacc, const edyn::dynamic_tag>();
view.each([&dt] (auto entity, edyn::linvel &vel, const edyn::linacc &acc, [[maybe_unused]] auto) {
  vel += acc * dt;
});
// ...
// Move entity with its linear velocity
auto view = registry.view<edyn::position, const edyn::linvel, const edyn::dynamic_tag>();
view.each([&dt] (auto entity, edyn::position &pos, const edyn::linvel &vel, [[maybe_unused]] auto) {
  pos += vel * dt;
});
// ...
// Rotate entity with its angular velocity
auto view = registry.view<edyn::orientation, const edyn::angvel, const edyn::dynamic_tag>();
view.each([&dt] (auto entity, edyn::orientation &orn, const edyn::angvel &vel, [[maybe_unused]] auto) {
  orn = edyn::integrate(orn, vel, dt);
});
```

Assigning each component to every rigid body entity individually quickly becomes a daunting task which is prone to errors, thus utility functions are provided for common tasks such as creating rigid bodies:

```cpp
// Equivalent to implicit example above.
auto def = edyn::rigidbody_def();
def.kind = edyn::rigidbody_kind::rb_dynamic;
def.position = {0, 3, 0};
def.orientation = edyn::quaternion_axis_angle({0, 1, 0}, edyn::to_radians(30));
def.linvel = edyn::vector3_zero;
def.angvel = {0, 0.314, 0};
def.mass = 50;
def.shape = edyn::box_shape{0.5, 0.2, 0.4}; // Shape is optional.
def.update_inertia();
def.restitution = 0.2;
def.friction = 0.9;
def.gravity = edyn::gravity_earth;
auto entity = edyn::make_rigidbody(registry, def);
```

It is not necessary to assign a shape to a rigid body. That enables the simulation to contain implicit rigid bodies (not to be confused with the meaning of implicit from above) which are not visually present in the simulation and don't participate in collision detection, but instead are connected to other bodies via constraints and are used to generate forces that affect the primary entities that users interact with. As an example, this can be useful to simulate drivetrain components in a vehicle.

# Foundation

The library can be built with single- or double-precision floating point. `edyn::scalar` is simply a `using` declaration equals to `float` or `double` which is set according to the `EDYN_DOUBLE_PRECISION` compilation option. _build_settings.hpp_ is generated during build from _cmake/build_settings.h.in_ so that invocations are linked to the correct definition.

`edyn::vector3` is the vector type for positions and directions.

`edyn::vector2` is the vector type for 2D positions and directions, which is used in some of the intersection algorithms for collision detection when the problem is projected onto a plane.

`edyn::quaternion` is the quaternion type for orientations.

`edyn::matrix3x3` is the 3x3 matrix also used for orientations and orthonormal bases.

_SIMD_ implementations of the above are planned.

# Constraints

Constraints create a relationship between degrees of freedom of rigid bodies, preventing them from moving beyond the allowed range. Constraints are defined as simple structs which hold the `entt::entity` of the bodies it connects and any other specific data such as pivot points in object space. A function that prepares constraints must be provided for each type. These preparation functions go over the list of constraints of that type and configure one or more constraint rows for each constraint. These functions are called by the constraint solver right before the solver iterations.

Constraints can also have an iteration function which is called on each iteration of the constraint solver. This is only needed if the constraint adjusts its rows based on changes to velocity during the iterations, e.g. the `edyn::contact_constraint` recalculates the limits of the friction constraint row based on what is the current impulse on the normal constraint row. This is not used by most constraints and should be used with care since it can be expensive.

There is no flexibility when it comes to adding new constraints to the library. If that's needed, it'll be necessary to fork the project and add the new constraint internally.

A traditional Sequential Impulse constraint solver is used.

# Collision detection and response

Collision detection is split in two phases: broad-phase and narrow-phase. In broad-phase potential collision pairs are found by checking if the AABBs of different entities are intersecting. Later, in the narrow-phase, closest points are calculated for these pairs.

During broad-phase, intersections between the AABBs of all entities are found using a _dynamic bounding volume tree_, according to [Dynamic Bounding Volume Hierarchies, Erin Catto, GDC 2019](https://box2d.org/files/ErinCatto_DynamicBVH_Full.pdf) and [Box2D](https://github.com/erincatto/box2d/blob/master/include/box2d/b2_dynamic_tree.h) [b2DynamicTree](https://github.com/erincatto/box2d/blob/master/src/collision/b2_dynamic_tree.cpp). The AABBs are inflated by a threshold so that contact pairs can be generated before the bodies start to penetrate thus generating contact points in advance. For any new intersection, an entity is created and a `edyn::contact_manifold` component is assigned to it. For any AABB intersection that ceased to exist, the entity is destroyed thus also destroying all components associated with it. The AABB is inflated a bit more when looking for separation to avoid a situation where they'd join and separate repeatedly. This is sometimes called _hysteresis_.

In narrow-phase, closest point calculation is performed for the rigid body pair in all `edyn::contact_manifold`s. The _Separating-Axis Theorem (SAT)_ is employed. A _GJK_ implementation is planned but _SAT_ is preferred due to greater control and precision and better ability to debug and reason about the code. For each new contact point, an entity is created with a `edyn::contact_point` and a `edyn::contact_constraint` components. This allows better separation of concerns. The `edyn::contact_constraint` uses the information in the associated `edyn::contact_point` to setup its constraint rows in the preparation function.

Contact point persistence is important for stability and to preserve continuity over time. New contact points that are near existing points get merged together, thus extending the lifetime of an existing contact and reusing the previously applied impulse for _warm starting_ later in the constraint solver. Contact points that are separating (in either tangential or normal directions) are destroyed.

Entities that don't have a shape also don't have a `edyn::AABB` assigned to them and thus are ignored in broad-phase which leads to no collisions ever happening. Rigid bodies without a shape are considered _implicit_.

To enable collision response for an entity, a `edyn::material` component must be assigned which basically contains the _restitution_ and _friction coefficient_. Otherwise, the entity behaves as a _sensor_, i.e. collision detection is performed but no impulses are applied and intersection events can still be observed.

Due to the multi-threaded design of _Edyn_, the broad-phase and narrow-phase are setup in a non-conventional way. The main thread performs broad-phase among islands using the AABB of the root node of their dynamic tree and then drills down to perform finer queries between entities in different islands. Narrow-phase is only performed in the island workers. More details are discussed in the [Multi-threading](#merging-islands) section.

## Separating-Axis Theorem and Implementation

The _Separating-Axis Theorem (SAT)_ states that if there is one axis where the intervals resulted from the projection of two convex shapes on this axis do not intersect, then the shapes also do not intersect. The projections on the axis can be used to determine the signed distance along that axis. The axis with largest signed distance gives us the _minimum translation vector_, which is a minimal displacement that can be applied to either shape to bring them into contact if they're not intersecting or separate them if they're intersecting. Using this axis, the closest features can be found on each shape and a contact manifold can be assembled.

The general structure of a _SAT_ implementation initially searches for the axis that gives the largest signed distance between the projections of the two shapes _A_ and _B_. The direction is always chosen to point outside of _B_ and towards _A_. Then the projection of _A_ is taken as the negative of the largest projection onto the opposite direction and the projection of _B_ is taken as the largest projection onto the axis. This gives us the maximal projection along the axis for both shapes and the distance is simply the difference between the projection of _A_ and the projection of _B_. If the largest distance is bigger than a threshold (usually 2cm), the collision is ignored and no contact points are generated. Otherwise, the _support features_ of each shape are found along that direction. Then the closest points between the support features are calculated as the contact points.

The support feature is a _feature_ that's located furthest along a direction. A _feature_ is a simpler element of a shape, such as vertex, edge and face on a box or polyhedron, cap face, side edge and cap edge on a cylinder. This concept allows the collision detection to be split in two steps: first the closest features are found, then the closest points between the two features are found. Features are simpler and allows for the exact contact points to be calculated in a more manageable way.

The features are intersected on a case-by-case basis and contact points are inserted into the resulting manifold which holds a limited number of points. If the manifold is full, it has to replace an existing point by the new, or leave it as is. The deciding factor is the area of the contact polygon, which is tries to maximize. A contact manifold with larger area tends to give greater stability.

## Collision Events

Collision events can be observed by listening to `on_construct<entt::contact_point>` or `on_destroy<entt::contact_point>` in the `entt::registry` to observe contact construction and destruction, respectively.

Contact points are not updated in the main registry continuously since that would be wasteful by default. In some cases it's desirable to have information such as the contact position, normal vector and applied normal and/of friction impulse in every frame, which can be used to play sounds or to drive a particle system, for example. If updated contact information is necessary, a `edyn::continuous` component must be assigned to the contact point when it's constructed and it must be marked as dirty so the changes can be propagated to the island worker where it resides. Another possibility is to assign a `edyn::continuous_contacts_tag` to a rigid body entity so every contact involving this entity will be automatically marked as continuous. This tag will be assigned to any new rigid bodies whose `edyn::rigidbody_def` has the `continuous_contacts` property set to true.

Due to the order of the internal updates (see [The Physics Step](#the-physics-step) for details), the contact constraint is not created immediately after a new contact point, which means that when an `on_construct<entt::contact_point>` is triggered, the collision impulse will not be available. If that information is required, listen to construction of `edyn::constraint_impulse` instead. These are created at the beginning of the next step and the solver will assign the applied impulse to it. It contains an array of impulses for each constraint row. In case of a `edyn::contact_constraint`, the first element is the normal impulse and the second is the friction impulse.

# Shapes

The physical shape of a rigid body can be any of the `edyn::*_shape` components, which are assigned directly to the rigid body entity. Along with the shape of specific type, a `edyn::shape_index` is assigned which can be used to read the shape an entity contains using the `edyn::visit_shape` function.

It's necessary to fork the project and modify the code to add custom shapes. It is also necessary to provide a `edyn::collide` function for every permutation of the custom shape with all existing shapes.

## Polyhedrons and rotated mesh optimization

To avoid having to rotate every vertex position and face normal when doing closest point calculation involving polyhedrons, they are rotated only once after the simulation step and are cached in a `edyn::rotated_mesh`. These rotated values can be reused in multiple collision tests in a single step (note that not all collision tests use these values since most of them are done in the polyhedron's object space).

Unlike the `edyn::convex_mesh` held by a polyhedron, the `edyn::rotated_mesh` is mutable and is only meaningful to the entity it is assigned to, whereas the `edyn::convex_mesh` is immutable and thread-safe and can be shared among multiple polyhedrons. Thus, a new `edyn::rotated_mesh` is created for every new polyhedron in the `edyn::island_worker`. Having the same instance being shared with other workers would not be a problem for dynamic entities, since they can only be present in one worker at a time. However, that's not true for kinematic objects, which can hold a polyhedron shape and be presented in multiple threads.

The polyhedron keeps a weak reference to the `edyn::rotated_mesh` thus the `edyn::island_worker` actually owns the rotated meshes and is responsible for keeping them alive until the polyhedron is destroyed. They are stored in `edyn::rotated_mesh_list` components because `edyn::compound_shape`s can hold multiple polyhedrons, thus it is necessary to be able to store a list of `edyn::rotated_mesh`es for them. The first `edyn::rotated_mesh_list` is assigned to the entity holding the shape itself. New entities are created for the next ones and are linked to the previous. When the original entity is deleted, all linked `edyn::rotated_mesh_list` are deleted in succession.

## Triangle mesh shape

The `edyn::triangle_mesh` represents a (usually large) concave mesh of triangles. It contains a static bounding volume tree which provides a quicker way to find all triangles that intersect a given AABB.

The concept of Voronoi regions is used to prevent internal edge collisions. The normal vector of all three adjacent triangles is stored for each triangle. Using the adjacent normal, it is possible to tell whether a direction (separating axis or minimum translation vector) lies in a valid region. If the axis is not in the voronoi region of the closest triangle feature, it is projected onto it so a valid direction is used.

Triangle meshes can be set up with a list of vertices and indices and then it calculates everything that's need with an invocation of `edyn::triangle_mesh::initialize()`. The list of vertices and indices can be loaded from an `*.obj` file using `edyn::load_tri_mesh_from_obj`. Loading from an `*.obj` can be slow because of parsing and recalculation of all internal properties of a triangle mesh, such as triangle normals, edge normals and vertex tangents. To speed things up, the triangle mesh can be written into a binary file using an output archive:

```cpp
auto output = edyn::file_output_archive("trimesh.bin");
edyn::serialize(output, trimesh);
```

And then loaded quickly using an input archive:

```cpp
auto input = edyn::file_input_archive("trimesh.bin");
edyn::serialize(input, trimesh);
```

## Paged triangle mesh shape

For the shape of the world's terrain, a triangle mesh shape is usually the best choice. For larger worlds, it is interesting to split up this terrain in smaller chunks and load them in and out of the world as needed. The `edyn::paged_triangle_mesh` offers a deferred loading mechanism that will load chunks of a concave triangle mesh as dynamic objects enter their bounding boxes. It keeps a static bounding volume tree with one `edyn::triangle_mesh` on each leaf node and loads them on demand.

It can be created from a list of vertices and indices using the `edyn::create_paged_triangle_mesh` function, which will split the large mesh into smaller chunks. Right after the call, all submeshes will be loaded into the cache which allows it to be fully written to a binary file using a `edyn::paged_triangle_mesh_file_output_archive`. The cache can be cleared afterwards calling `edyn::paged_triangle_mesh::clear_cache()`. Now the mesh can be loaded quickly from file using a `edyn::paged_triangle_mesh_file_input_archive`.

As dynamic entities move into the AABB of the submeshes, it will ask the loader to load the triangle mesh for that region if it's not available yet. It uses a `edyn::triangle_mesh_page_loader_base` to load the required triangle mesh (usually asynchronously) and then will assign a `edyn::triangle_mesh` to the node when done. Since it might take time to load the mesh from file and deserialize it, the query AABB should be inflated to prevent collisions from being missed.

When there are no dynamic entities in the AABB of the submesh, it becomes a candidate for unloading.

In the creation process of a `edyn::paged_triangle_mesh`, the whole mesh is loaded into a single `edyn::triangle_mesh`. Then, it's split up into smaller chunks during the construction of the static bounding volume tree of submeshes, which is configured to continue splitting until the number of triangles in a node is under a certain threshold. For each leaf node, a new `edyn::triangle_mesh` is created containing only the triangles in that node. The submeshes require a special initialization procedure so that adjacency with other submeshes can be accounted for. This part will take already calculated information from the global triangle mesh and assign that directly into the submesh, particularly adjacent triangle normals, which are crucial to prevent internal edge collisions at the submesh boundaries.

# Multi-threading

The multi-threading model seeks to maximize parallelization by splitting up the simulation into independent chunks that can be run separately in different threads with the least amount of synchronization as possible. These chunks are called _islands_ where entities in one island _A_ cannot affect the state of entities in another island _B_, and thus _A_ and _B_ can be executed in parallel.

In the application's main thread there should be one _master registry_ which contains all entities and components in the physics world. The goal is to minimize the amount of work done in the main thread and offload as much as possible to worker threads. As such, each island is dispatched to be executed in an _island worker_ which has its own private registry. Since an `entt::registry` cannot be trivially thread-safe, each island must have its own and later the results of the simulation steps must be sent back to the main thread so that the components can be updated into the main registry. This allows the multi-threading to be transparent to users of the library since their logic can simply operate in the main registry without considering the fact that the simulation is happening in the background (in most cases).

The _island workers_ are jobs that are scheduled to be run in worker threads. In each invocation of the _island worker_ main function, it performs one simulation step (if the difference between the current time and the last time it performed a step is greater than the fixed delta time) and reschedules itself to run again at a later time to perform the steps for that island continuously. This means unlike a typical physics engine, there's no main loop where the simulation steps are performed, all islands are stepped forward independently of any central synchronization mechanism.

## Job System

_Edyn_ has its own job system it uses for parallelizing tasks and running background jobs. The `edyn::job_dispatcher` manages a set of workers which are each associated with a background thread. When a job is scheduled it pushes it into the queue of the least busy worker.

Job queues can also exist in any other thread. This allows scheduling tasks to run in specific threads which is particularly useful in asynchronous invocations that need to return a response in the thread that initiated the asynchronous task. To schedule a job to run in a specific thread, the `std::thread::id` must be passed as the first argument of `edyn::job_dispatcher::async`. It is necessary to allocate a queue for the thread by calling `edyn::job_dispatcher::assure_current_queue` and then also call `edyn::job_dispatcher::once_current_queue` periodically to execute the pending jobs scheduled to run in the current thread.

Jobs are a central part of the multi-threaded aspects of the engine and thus are expected to be small and quick to run, and they should **never** wait or sleep. The goal is to keep the worker queues always moving at a fast pace and avoid hogging the queue thus making any subsequent job wait for too long, or having a situation where one queue is backed up by a couple jobs while others are empty. _Job stealing_ is a possibility in this case but it introduces additional complexity which makes it too difficult to model it as a lockfree queue. Thus, if a job has to perform too much work, it should split it up and use a technique where the job stores its progress state and reschedules itself and then continues execution in the next run. If a job needs to run a for-loop, it should invoke `edyn::parallel_for_async`, where one of the parameters is a job to be dispatched once the for loop is done, which can be the calling job itself, and then immediately return, allowing the next job in the queue to run. When the job is executed again, it's important to know where it was left at thus it's necessary to store a progress state and continue from there.

A job is comprised of a fixed size data buffer and a function pointer that takes that buffer as its single parameter. The worker simply calls the job's function with the data buffer as a parameter. It is responsibility of the job's function to deserialize the buffer into the expected data format and then execute the actual logic. This is to keep things simple and lightweight and to support lockfree queues in the future. If the job data does not fit into the fixed size buffer, it should allocate it dynamically and write the address of the data into the buffer. In this case, manual memory management is necessary thus, it's important to remember to deallocate the data after the job is done.

Using the `edyn::job_scheduler` it is possible to schedule a job to run after a delay. The `edyn::job_scheduler` keeps a queue of pending jobs sorted by desired execution time and it uses a `std::condition_variable` to block execution until the next timed job is ready invoking `std::condition_variable::wait_for` and then schedules it using a `edyn::job_dispatcher`. To create a repeating job that is executed every _dt_ seconds, it's necessary to have the job reschedule itself to run again at a later time once it finishes processing. This technique is used for running periodic tasks such as the _island workers_.

## Simulation Islands

Dynamic entities that cannot immediately affect the motion of others can be simulated in isolation. More precisely, two dynamic entities _A_ and _B_ which are not connected via constraints are not capable of immediately affecting the motion of each other. That means, the motion of _A_ and _B_ is independent and thus could be performed in two separate threads.

An _island_ is a set of entities that are connected via constraints, directly or indirectly. The motion of one dynamic entity in an island will likely have an effect on the motion of all other dynamic entities in the island, thus the constraints in one island have to be solved together.

### The Island Node Graph

Islands are modeled as a graph, where the rigid bodies are nodes and the constraints and contact manifolds are edges. The graph is stored in a data structure outside of the ECS, `edyn::island_graph`, where nodes and edges have a numerical id, i.e. `edyn::island_graph::index_type`. This is an undirected, non-weighted graph which allows multiple edges between nodes. Node entities are assigned a `edyn::graph_node` and edges are assigned a `edyn::graph_edge` which hold the id of the node or edge in the graph. This allows a conversion from node/edge index to entity and vice-versa.

Individual islands can be found using the concept of _connected components_ from graph theory. Islands are represented by an entity with a `edyn::island` component and all node and edge entities have a `edyn::island_resident` component (or `edyn::multi_island_resident` for non-procedural entities) which holds the entity id of the island where they're located at the moment. As nodes are created, destroyed or modified, islands can split into two or more islands, and they can also merge with other islands.

Nodes are categorized as _connecting_ and _non-connecting_. When traversing the graph to calculate the connected components, a node is visited first and then its neighbors that haven't been visited yet are added to a list of nodes to be visited next. If the node is _non-connecting_, the neighbors aren't added to the list of nodes to be visited. The non-procedural entities have their corresponding graph nodes marked as non-connecting because a procedural entity cannot affect the state of another procedural entity _through_ a non-procedural entity, so during graph traversal, the code doesn't walk _through_ a non-connecting node. For example, if there are multiple dynamic entities laying on a static floor, there shouldn't be a single connected component but instead, there should be one connected component for each set of procedural nodes that are touching each other and the static floor should be present in all of them.

Only the relevant entities are included in the graph. For example, the contact points of a contact manifold are not treated as edges, i.e. they don't have a `edyn::graph_edge` component, only the manifold has., since that would be redundant and make the graph more complex that it needs to be. The contact points are treated as _children_ of the contact manifold and they're sent wherever the manifold goes when moving things between islands. The same is true for the constraint rows in a constraint.

### Procedural Nodes

Nodes that have their state calculated by the physics simulation are characterized as _procedural_ using the `edyn::procedural_tag`. These nodes can only be present in one island, which means that if a connection is created between two procedural nodes that reside in different islands, the islands have to be merged into one. Later, if this connection is destroyed, the island can be again split into two. Dynamic rigid bodies and constraints must have a `edyn::procedural_tag` assigned to them. Non-procedural nodes can be present in multiple islands at the same time, since they are effectively read-only from the island's perspective. These are usually the static and kinematic entities.

Entities that are part of an island need a way to tell what island they're in. Procedural entities can only be present in one island at any given moment, thus they have an `edyn::island_resident` assigned to them, whereas non-procedural entities can be in multiple islands simultaneously, thus they have a `edyn::multi_island_resident` instead. These components are only assigned in the _island coordinator_ since the _island worker_ only manages one island (more details in the next sections).

The presence of non-procedural nodes in multiple islands means that their data will have to be duplicated multiple times. In general this is not much data so duplication shouldn't be a concern. One case where it can be desirable to not duplicate data due to its size, is where a static entity is linked to a triangle mesh shape containing thousands of triangles. In this case, the `edyn::triangle_mesh` keeps a `std::shared_ptr` to its triangle data so when it's duplicated, the bulk of its data is reused. This data is going to be accessed from multiple threads thus it must be thread-safe. For a `edyn::triangle_mesh` the data is immutable. A `edyn::paged_triangle_mesh` has a dynamic page loading mechanism which must be secured using mutexes.

### Island Coordinator

In the main thread, an `edyn::island_coordinator` manages all running islands and is responsible for creating, merging and splitting islands, applying island updates onto the main registry and dispatching messages to islands. When new nodes are created, the `edyn::island_coordinator` analyzes the graph to find out which islands these nodes should be inserted into, which could result in new islands being created, or islands being merged. Later, it sends out all the data of the new entities to the respective `edyn::island_worker`s using a `edyn::island_delta`, which holds a set of changes that can be imported into the island's private registry. These comprise entities created and destroyed, and components that were created, updated and destroyed.

The `edyn::island_worker` will then send back data to the main thread containing the updated state of the rigid bodies and other entities which can be replaced in the main registry. It is important to note that the simulation keeps moving forward in the background and the information in the main registry is not exactly the same as the data in the worker registry.

When a physics component is modified, the changes have to be propagated to its respective `edyn::island_worker`. This can be done by assigning a `edyn::dirty` component to the changed entity and specifying which components have changed calling `edyn::dirty::updated<edyn::linvel, edyn::position>`, for example. An alternative is to call `edyn::refresh()` with the entity and components that need to be updated.

### Island Worker

The `edyn::island_worker` is a job that is run in the `edyn::job_dispatcher` which performs the actual physics simulation. They are created by the `edyn::island_coordinator` when new islands are created and destroyed when islands are destroyed. They have a private `entt::registry` where the simulation data for its only island is stored.

The `edyn::island worker` has a message queue (single producer, single consumer) where it exchanges messages with the `edyn::island_coordinator`. During each update, it accumulates all relevant changes that have happened to its private registry into a `edyn::island_delta` and at the end, it sends this delta to the coordinator which can be imported into the main registry to update the corresponding entities. For certain components, it may be desired to get an update after every step of the simulation, such as `edyn::position` and `edyn::orientation`. To make this happen it is necessary to assign a `edyn::continuous` component to the entity and choose the components that should be put in a `edyn::island_delta` after every step and sent back to the coordinator. By default, the `edyn::make_rigidbody` function assigns a `edyn::continuous` component to the entity with the `edyn::position`, `edyn::orientation`, `edyn::linvel` and `edyn::angvel` set to be updated after every step. Contact points, for example, change in every step of the simulation, but they're not sent back to the coordinator by default. Thus, if the contact point information is needed continuously (e.g. to play sounds or special effects at the contact point location), it is necessary to assign a `edyn::continuous` component to it when it is created (which can be done by observing `entt::registry::on_construct<edyn::contact_point>()`). This mechanism allows the shared information to be tailored to the application's needs and minimize the amount of data that needs to be shared between coordinator and worker.

Using the job system, each island can be scheduled to run the simulation in the background in separate threads taking advantage of the job system's load balancing. In the `edyn::island_worker` main function, it performs a single step of the simulation instead of all steps necessary to bring the simulation to the current time to minimize the amount of time spent in the worker queue. If the timestamp of the current step plus the fixed delta time is after the current time, it means the island worker can rest for a little bit and in this case it reschedules itself to run at a later time. Otherwise it dispatches itself to run again as soon as possible. The simulation step is not necessarily performed in one shot (see [Parallel-Substeps](#parallel-sub-steps))

The `edyn::island_worker` is dynamically allocated by the `edyn::island_coordinator` and it manages its own lifetime. When it is asked to be terminated by the coordinator by means of `edyn::island_worker_context::terminate()`, it will deallocate itself when it's executed again in a worker thread. This eliminates the need for the coordinator to manage the worker's lifetime which would require synchronizing its termination.

### The Physics Step

The physics simulation step happens in each island worker independently. This is the order of major updates:

- Process messages and island deltas, thus synchronizing state with the main registry, which might create new entities in the local registry.
- Detect collision for new manifolds that have been imported from the main registry.
- Create contact constraints for new contact points. This includes new points generated in the collision detection of imported manifolds and contact points that were created in the previous physics step.
- Apply external forces such as gravity.
- Solve constraints.
- Apply constraint impulses.
- Integrate velocities to obtain new positions and orientations.
- Now that rigid bodies have moved, run the broad-phase collision detection to create/destroy collision pairs (i.e. `edyn::contact_manifold`).
- Perform narrow-phase collision detection to generate new contact points. Contact constraints are not created yet.
- Send island delta to coordinator.

### Splitting Islands

When an island is found to have more than one connected component existing in it, it becomes a candidate for splitting. The connected components are calculated using all entities, procedural and non-procedural. Whenever the graph changes, the worker flags the topology of the graph as changed and eventually, in a future step after a delay, it calculates whether there is more than one connected component in the graph. If that is the case, it sends an `edyn::msg::split_island` message to the coordinator and it sets itself in a _splitting_ state, where it cannot be scheduled to run. It has to wait for the coordinator to perform the split before continuing the simulation.

The split must be done in the coordinator due to the possibilities of race conditions that make it nearly impossible to do everything in the worker, mainly because what the worker sees at the moment it calculates the connected components does not reflect the full state of the simulation, since the coordinator could be merging another island into it at the same time. Entities that are in the other island being merged into this worker could reference entities that were moved into one of the islands that was a product of the split, thus making it very difficult to tie things back together.

When processing the `edyn::msg::split_island`, the coordinator invokes the worker's split function in the main thread, which is safe to do since the worker won't be running in this case. Part of the process involves first processing any pending messages in the worker side, which could include processing an `edyn::island_delta` which merges new entities into this worker. Then the graph is complete and ready to be split. The `edyn::entity_graph` performs the split internally and then the biggest connected component is chosen to stay in this island and the smaller ones are returned to the coordinator so new islands can be created for those. The entities in the smaller connected components are removed from the worker's private registry. The worker starts running again right after. Back in the coordinator, it first needs to process any pending messages from the worker, because a few things change in the worker's private registry during the split and those changes are added to an `edyn::island_delta` and enqueued in the output message queue of that worker. One such change is the `edyn::tree_view` of that worker's island, which changes after entities are deleted and needs to be updated in the main registry before proceeding because otherwise the `edyb::broadphase_main` would consider entities that have moved into another island to still be present in the island that was split. Another important change are the _entity mappings_ that are created as a result of a merge which could've happened in the initial pending message processing preceding the split. These entity mappings are needed to convert the entities in the connected components from the worker registry into the coordinator registry. Finally, the coordinator can create a new island worker for each connected component.

### Merging Islands

The `edyn::island_coordinator` looks for entities from different islands that could collide during broad-phase collision detection. For any new pairs that are found, it is necessary to merge the islands together because one island is unaware of any other. Merging is done by selecting the biggest among the islands being merged and moving all entities from the other islands into it by inserting them into a `edyn::island_delta` and sending it over to the existing big island. Then the other islands can be destroyed and the workers can be terminated.

The island worker runs a broad-phase collision detection using two dynamic trees: one for procedural entities and another one for non-procedural entities. In every update of the `edyn::broadphase_worker`, it looks for AABB intersections between each procedural entity and the leaf nodes of both trees. The procedural tree has the AABB of its nodes updated prior to that and later the worker includes a `edyn::tree_view` of the procedural tree in the `edyn::island_delta` so the coordinator can query the same tree when looking for collision pairs between different islands.

The main thread runs its broad-phase using `edyn::broadphase_main`, which is responsible for creating `edyn::contact_manifold`s between nodes that reside in different islands, thus creating the connection between islands since they are unaware of each other. These manifolds are inserted as edges in the main `edyn::island_graph`. As new `edyn::contact_manifold`s are created, the `edyn::island_coordinator` merges islands since they now form a single connected component.

`edyn::broadphase_main` has one dynamic tree for island AABBs and another dynamic tree for non-procedural entities. The former contains nodes for each island which are constructed from the AABB of the root node of the `edyn::tree_view` of that island, which is the AABB enclosing all procedural entities in the island. For each awake island, it queries the island dynamic tree to find other islands that could be intersecting. For each pair of islands that are found to be intersecting, it uses the `edyn::tree_view` of both to find pairs of procedural entities that could collide. It picks the smaller of the two trees and iterates its leaf nodes and for each leaf it queries the other tree to find intersecting pairs. Then, it queries the non-procedural tree to find non-procedural entities that could collide with procedural entities in the island. Then it once again drills down a level and queries the island `edyn::tree_view` to find collision pairs.

### Entity Mapping

Since each `edyn::island_worker` has its own registry where entities from the main registry are replicated, it is necessary to map `entt::entity` (i.e. entity identifiers) from one registry to another, since entities cannot just be the same in different registries. This is called _entity mapping_ and is done using an `edyn::entity_map`, which allows entities to be converted from _remote_ to _local_ and vice-versa.

Of special consideration are components that have `entt::entity`s in them, such as the `edyn::contact_manifold`. These `entt::entity`s must be mapped from _remote_ to _local_ before being imported. A custom `edyn::merge` function must be provided for these components which uses the provided entity map argument to update the child entities.

If the island worker creates a new entity (e.g. when a new contact point is created), it won't yet have an entity mapping for it, since there's no corresponding entity in the main registry yet. It will be added to the current `edyn::island_delta` as a created entity and when received on the coordinator, a new entity will be instantiated to be mapped into this. Then, still in the coordinator, in its current `edyn::island_delta`, it will add an entity mapping between these two and when its received back in the island worker (where the entity was originally created), the entity mapping can be added locally and it will now know what that entity id is in the coordinator. Then, whenever it receives an update from the coordinator with that remote entity id, it can map it to a local entity id.

### Timing Concerns

Running the simulation in parallel means that the state of entities in the main registry will not be at the same point in time. However, all entities in one island are synchronized at the same point in time, which is given away by the `edyn::island_timestamp` of an island. Then, for presentation the state must be interpolated/extrapolated according to the time in that island. That is done internally using `edyn::present_position` and `edyn::present_orientation` which are updated in every `edyn::update` and those should be used for presentation instead of `edyn::position` or `edyn::orientation`.

When merging the registries of multiple `edyn::island_worker`s onto the main registry their timestamp might differ slightly. This might lead to broad-phase pairs not initiating at the exact right time, which means that when islands are merged together, the entities could be at a different point in time. When entities are moved into another island, they have to be updated to reach the current timestamp in that island. The `edyn::island_delta` contains the timestamp which tells at which point in time the entities contained in it were in that state. The entities must be imported into the island's registry and then, if the timestamp in the `edyn::island_delta` is before the island's timestamp, all the other objects in the island must be _disabled_ (by assigning a `edyn::disabled_tag` to them) and the local simulation must be stepped forward enough times until the island's timestamp. If the island's timestamp is before the `edyn::island_delta`, then the entities that came with the delta must be _disabled_ and the local simulation must be stepped until the delta's timestamp (the latter is not expected to happen). Then, the other entities can be enabled again (the ones that were not disabled before this special stepping procedure started) and the simulation can continue as normal.

The _step number_ is an unsigned integer which starts at zero and is incremented in every step of the simulation in each island. It corresponds to what point in time the simulation is at. All entities in an island will have their state correspond to the same step number at all times.

### Parallel Sub-steps

Splitting the simulation into islands is not enough to maximize resource utilization in certain cases, such as a big pyramid of boxes, which is a single island and would be run in a single island worker, thus turning it into a single-threaded solution. It is advantageous to further split the island step into sub-steps which can run in parallel, one of which is collision detection, which can be fully parallelized using a `edyn::parallel_for_async`. The island worker keeps an internal state to mark the progress of a single simulation step and the island worker job is passed as a completion job to the `edyn::parallel_for_async` so when the parallel for is completed, the island worker is scheduled to run again and it can continue the simulation step where it was left at.

The constraint solver iterations are also rather expensive in this case, though more difficult to parallelize since there's a dependency among some of the constraints. For example, in a chain of rigid bodies connected by a simple joint such as `A-(α)-B-(β)-C`, the constraints `α` and `β` cannot be solved in parallel because both depend on body `B`. However, in a chain such as `A-(α)-B-(β)-C-(γ)-D`, the constraints `α` and `γ` can be solved in parallel because they don't have any rigid body in common, and then constraint `β` can be solved in a subsequent step (this is in one iteration of the solver, where 10 iterations is the default). That means the constraint graph, where each rigid body is a node and each constraint is an edge connecting two rigid bodies, can be split into a number of connected subsets which can be solved in parallel and the remaining constraints that connect bodies in different subsets can be solved afterwards. The picture below illustrates the concept:

![ConstraintGraphPartition](https://xissburg.com/images/ConstraintGraphPartition.svg)

The constraints contained within the subsets A and B (i.e. constraints connecting two nodes that are both in the same subset, blue edges) can be solved as a group in two separate threads (that is one iteration). Later, the constraints in the _seam_ that connect A and B together (i.e. constraints connecting two nodes that lie in different subsets, red edges) can be solved once the jobs that solve A and B are completed. The process is then repeated for each solver iteration.

### Sleeping

Another function of islands is to allow entities to _sleep_ when they're inactive (not moving, or barely moving). As stated before, an island is a set of entities where the motion of one can immediately affect all others, thus when none of these entities are moving, nothing is going to move, so it's wasteful to do motion integration and constraint resolution for an island in this state. In that case the island is put to sleep by assigning a `edyn::sleeping_tag` to all entities in the island. The `edyn::island_worker` stops rescheduling itself when it is set to sleep. It must be waken up by the coordinator when needed, such as in case of termination or when something changes in that island.

## Parallel-for

The `edyn::parallel_for` and `edyn::parallel_for_async` functions split a range into sub-ranges and invoke the provided callable for these sub-ranges in different worker threads. It is used internally to parallelize computations such as collision detection between distinct pairs of rigid bodies. Users of the library are also free to use these functions to accelerate their for loops.

The difference between `edyn::parallel_for` and `edyn::parallel_for_async` is that the former blocks the current thread until all the work is done and the latter returns immediately and it takes a _completion job_ as parameter which will be dispatched when the work is done. `edyn::parallel_for` also runs a portion of the for loop in the calling thread.

Each instance of a parallel for job increments an atomic integer with the chunk size and if that's still within valid range, it proceeds to run a for loop for that chunk. It then repeats this process until the whole range is covered. This ensures that, even if one of the jobs is very far behind in a work queue, the for loop continues making progress. Thus it's possible that by the time a job is executed, the loop had already been completed, and in the async case the only thing it does is to decrement the atomic reference counter which when it reaches zero, it deallocates the context object. Also in the async case, when a chunk completes, a _completed_ atomic is incremented and when it reaches the total size of the loop, it dispatches the completion job.

## Parallel-reduce

_TODO_

# Networking

The networking model follows a client-server architecture where the server is authoritative but gives the client freedom to directly set the state of the entities it owns in some situations. The goal is to synchronize the simulation on both ends, accounting for network latency and jitter.

The server is the source of truth, containing the actual physics simulation encompassing all entities and components including extra external components used by systems provided by the user of this library. The server has to frequently broadcast to all clients a snapshot of the dynamic components which change very often such as position and velocity. Steady components, which change casually, have to be broadcast only when updated.

The server also receives data from the clients, which can be commands to be applied to the entities owned by that client and also component state to be set onto the entities in the server, which the server will have to decide whether to apply them or not.

When receiving data from the server, the received state will be in the past due to network latency, thus the client must _extrapolate_ it before merging it onto the local simulation.

## Server receives data from client

The server expects users to send the dynamic state of the entities the client owns frequently. This state will be applied to the server registry at the right time if the client has permission do so at the moment.

The server simulation runs in the past with respect to the client. This allows the state received from the client to be applied at the right time in the server despite network jitter. More precisely, the state received from the client is accumulated in a _jitter buffer_ (aka _playout delay buffer_) and is applied onto the server registry when the time comes. To match the timing, a step number_is used, instead of a timestamp. This requires the simulation to run with fixed timesteps which must be equal in both ends. This helps maintaining a decent degree of determinism. Ideally, the client states are applied on the server side exactly at the same rate they're applied in the client side, thus leading to the same result on both ends.

The step number is obviously different for each client and server but they increase at the same rate. Using the _round trip time_ (RTT) one can calculate a _step delta_ which can be added to the local step number to convert it to the remote step number.

If the length of the jitter buffer in unit of time is smaller than half the RTT, it will happen that the client state will be applied as soon as it arrives in the server because in that case it should have been applied before it had arrived in the server (which is obviously not possible). That means, the amount of time in the past the server simulation must run has to be bigger than half the maximum RTT among all clients.

If the jitter buffer is too long, that means a bigger problem for the clients because the data they'll get from the server will be older and they'll have to extrapolate more to bring it to the current time which is more costly and errors in prediction are increased. More on extrapolation on the next sections.

To avoid having a jitter buffer that's big enough for all clients, clients are split into groups that are close enough and could potentially interact with one another through collisions or perhaps other custom constraints created on the fly. The size of the jitter buffer has to be adjusted based on the maximum half RTT of the clients who own an entity in that group. Could be something like 120% of the maximum half RTT. The groups are calculated exactly like in the broad-phase collision detection, though using much larger AABBs. The jitter buffer is kept in the island where the client's entities are contained in its length in time is adjusted by the coordinator as the RTT changes.

The reason why groups are used instead of islands is that islands are only merged when they are too close to one another. In the case where a client with small RTT is near another client with a large RTT, in the server world their location could be off significantly (e.g. two vehicles driving side by side) due to the different jitter buffer lengths so it's possible collisions would be missed. Creating groups by using larger AABBs prepares the world for those situations.

The concept of _ownership_ is central to decentralized control and conflict resolution in the server simulation. Dynamic entities can have an owning client, which means that the client has exclusive control over that entity and can directly modify its components. The entities permanently owned by a client are usually the ones created by it or created for it. Those are the entities that are directly controlled by the client, such as a character or a vehicle.

The client will send snapshots of its owned entities to the server which will verify these and possibly apply them to its simulation, in the corresponding group. In the case where there are no other entities owned by another client in the same group, the snapshot will go through a verification process where the components are compared to their current value and they're checked for hacking attempts where the values would be unrealistic. These components will be discarded.

This direct state application means that the client can run the simulation locally without the need to apply and extrapolate state from the server most of the time (for the entities owned by the client). Extrapolation is only necessary when there are other entities owned by another client in the same island.

The client has temporary ownership of all entities in its island, except when an entity owned by another client is also in this island, where in this case the server takes ownership of all entities to avoid conflicting simulation results. In other words, an island can only be owned by one client at a time. If two clients try to take ownership of the same island, the server intervenes and takes ownership to itself.

The client will send state updates to the server for all entities in its island so that the temporarily owned entities will also be synchronized without disrupting the local client simulation.

In the case where the server takes ownership of an island, none of the physics components in the client snapshots will be accepted. Only external components assigned by the external application which is using _Edyn_ will be applied, such as inputs, which will then be handled by the custom systems that can be attached to _Edyn_.

The server will hand ownership back to the client when the dynamic state sent by the client converges towards that of the server {this sounds complex}.

## Server sends data to client

The server will periodically capture snapshots of relevant islands and send them out to clients. Which islands will be sent and how often is determined by the client's _points of interest_.

The point of interest is simply an entity that has a `edyn::position` component. This means a dynamic or kinematic object can be set as a point of interest, such as the character or vehicle controlled by the user which is the only one point of interest that will exist in most cases. Extra points of interest are necessary in case the user can operate a free-look camera and move around to see the world somewhere else.

All entities in a radius around the point of interest will be included in the snapshot. Not all entities are necessarily included in one snapshot. If the server is multi-threaded, this will be done separately per island, which will result in a bunch of smaller snapshot packets being sent for the same step number. These packets don't need to be reliably delivered.

## Client sends data to server

Clients will periodically send snapshots of its owned entities to the server. Dynamic components will be sent often in a stream (unreliable packets). Steady components will be sent when they change (reliable packets).

One important piece that has to be managed by the user of the library are user inputs which will be also applied in the server. It's important that an input history is sent so that the chance of an input not reaching the server is lowered in case of packet loss.

## Client receives data from server

Snapshots sent by the server in the snapshot stream do not contain all components of each entity in it. These snapshots only contain the dynamic components that change very often, such as position and velocity. However, entities often have more components which are necessary to correctly simulate them. Thus, the client must check whether it already has obtained the full information of those entities and if not, query the server for the full thing. The server should respond with a snapshot containing the requested data and this can be loaded into the local registry which will instantiate the full entity and now it's ready to be properly updated.

Upon receiving snapshots from the server, the client is required to extrapolate these to the future before merging them onto its local simulation since the server simulation stays in the past plus there's latency to compensate for. The amount of time to extrapolate forward is calculated based on the snapshot step number converted to the local time frame.

Extrapolation is performed in a background worker thread. It is the same as a typical island task except that it is not synchronized with the global timer. Once it's done, the result comes back to the world/coordinator which has to perhaps step it forward once or twice to make the step number match and then it overrides its components with the extrapolated data. Islands now have to be recalculated. The islands that contain the entities that were extrapolated must be updated with the new data.

In case the server had taken ownership of an entity that's owned by the client, this entity will be contained in the snapshot. In this case it is necessary to also apply all inputs during extrapolation, thus a local input history has to be kept to be used here.

The server snapshot will usually contain a single island. If the extrapolation is too large, the entities in it would possibly collide with an existing object from another local island and the collision would be missed or detected only when merging. A solution would be to keep a history of all objects so that islands nearby the server snapshot island can be rolled back and extrapolated with it, but that seems to be an extra amount of processing and storage to be done for little benefit. As long as the extrapolations are not too large (which usually shouldn't be), this won't be a significant problem. This might be interesting to do for the client's entities though.

Extrapolation will introduce visible errors and disturbances when the extrapolate result doesn't match the previous local state very closely. A discontinuity error is calculated before the components are replaced by the extrapolated version and that is provided in a `edyn::discontinuity` component which can be used by the graphics engine to smooth out these errors by adding them to the next future states. The discontinuity decays towards zero over time.

# Clusters

Multiple server instances can run in different machines in the same local area network (LAN) and balance load. The principles of distributing work among all machine are similar to that of multi-threading.

# Distributed simulation

Several server clusters can exist in different geographical locations to allow for better network performance for local clients. Clusters communicate among themselves to synchronize the redundant simulation of the persistent world.
