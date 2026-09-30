+++
title = "Interactive Real-Time Fluid Simulator"
template = "article"
date = "September 28, 2026"
description = "Real-time, particle-based fluid simulation."
draft = false
+++

*{{date}}*
# {{title}}

This article is a showcase of my [particle-based fluid simulation implementation](https://github.com/tymorrill17/odin-GraphicsEngine) using the
[smoothed-particle hydrodynamics (SPH) method](https://en.wikipedia.org/wiki/Smoothed-particle_hydrodynamics). The visualization
is done using my own rendering engine (article coming soon) written in [Odin](https://odin-lang.org/).^[Odin is a C-like systems programming
language. It aims to keep the control and simplicity of C while providing modern conveniences. It's a blast to use for graphics and game programming.]

![Particles sloshing around a box](/videos/intro_demo_loop.mp4)

I was inspired to tackle this project by Sebastian Lague's excellent fluid simulation [video](https://youtu.be/rSKMYc1CQHE), from which I was directed
to the following papers for many of the implementation details.

- Müller, Charypar, and Gross, [*Particle-Based Fluid Simulation for Interactive Applications*](https://dl.acm.org/doi/10.5555/846276.846298), SCA 2003.
- Clavet, Beaudoin, and Poulin, [*Particle-Based Viscoelastic Fluid Simulation*](https://doi.org/10.1145/1073368.1073400), SCA 2005.
- Solenthaler and Pajarola, [*Predictive-Corrective Incompressible SPH*](https://doi.org/10.1145/1576246.1531346), SIGGRAPH 2009.

# Smoothed-Particle Hydrodynamics

Smoothed-particle hydrodynamics (SPH) was initially developed for astrophysics. It provides a way to interpolate field quantities of a fluid
using values carried by discrete particles. Each particle has a position, velocity, and acceleration, which are determined by the fluid itself,
along with quantities relevant to the fluid equation like density or viscosity. The total field value may be found at any
point in space by interpolating each particle's contribution weighted by a smoothing kernel. In mathematical notation, for a query point $r$, a scalar field
quantity $A$ can be represented as

$$A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}W(r-r_j, h), \tag{1}$$

where $j$ loops over each particle and $m_j, A_j, \rho_j, r_j$ are the mass, field quantity, density, and position of particle $j$, respectively.
$W(r,h)$ is the smoothing kernel.^[There are many options for the smoothing kernel, and you need not use the same one across all quantities. Some
kernels may behave more favorably towards certain quantities.]

The smoothing kernel should be radially symmetric, with finite support defined by radius $h$.^[$W(r,h)$ will only be nonzero when $0 \le r \le h$.]
It should also be normalized^[$\int_{\Omega} W(r,h)dr = 1$, where $\Omega$ is the entire domain of the simulation.], and ideally have vanishing
values and derivatives at the boundary $h$ so as not to have discontinuities as particles leave and enter its radius.

For our case, let's assume mass is constant between particles ($m_j = 1$ for all $j$) so we can forget about it. That leaves us
with finding $\rho_j$, which we can do using equation $(1)$ itself.

$$\rho_S(r) = \sum_j m_j \frac{\rho_j}{\rho_j}W(r-r_j,h) = \sum_j m_j W(r-r_j,h)$$

You can begin to see the mechanism of the method. There is one more crucial property of $(1)$: we can use it to find the gradient or Laplacian of our
desired quantities as well.

$$ \nabla A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}\nabla W(r-r_j, h) \tag{2} $$

$$ \nabla^2 A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}\nabla^2 W(r-r_j, h) \tag{3} $$

Now, let's take a look at the equation to be solved.

# The Navier-Stokes Equation

Fluids are governed by a set of partial differential equations called the Navier-Stokes equations. For our not-so-lofty aspirations of making
pretty pictures, we will concern ourselves with the following simplified version of the primary equation.^[In other sources, you may see the left-hand side of this
equation in the form $\rho \frac{Dv}{Dt} = \rho \frac{\partial v}{\partial t} + \rho (v\cdot\nabla) v$. We can use the fact that the particles move with the
fluid's velocity field to simplify it.]

$$ \rho\frac{dv}{dt} = -\nabla P + \rho g + \mu \nabla^2 v + \mathbf{f}_e \tag{4} $$

Here, $-\nabla P$ is the pressure force, $\rho g$ is the gravitational force, $\mu \nabla^2 v$ is the viscosity force,^[I am ignoring
the viscosity term $\mu \nabla^2 v$ for now. It will be tackled in a later post.] and $\mathbf{f}_e$ are the
external forces. If you look closely and note that $\rho$ is a mass analogue, you can see Newton's second law in disguise.

$$ \begin{aligned}
         F_{total} &= -\nabla P + \rho g + \mu \nabla^2 v + \mathbf{f}_e \\
                 a &= \frac{dv}{dt} \\
\implies F_{total} &= \rho a
\end{aligned} $$

At each step of the simulation, we are trying to find the acceleration of each particle, with which we may obtain the velocity and position of
the particle by integration.

## Pressure Force

Similarly to density, use equation $(2)$ to find the pressure force.

$$ -\nabla P(r) = -\sum_jm_j\frac{P_j}{\rho_j}\nabla W(r-r_j, h) \tag{5} $$

There are a couple of things to consider here. First, $P_j$ may be obtained from the ideal gas equation
$$P_j = \rho_j k,$$
where $k$ is a constant. It is common to use a modification of the ideal gas equation
$$P_j = k(\rho_j - \rho_0),$$
which will increase the numerical stability of the simulation, since using this with $(6)$ can result in negative pressure, effectively
introducing a crude form of cohesion.

Secondly, equation $(5)$ does not accurately compute the pressure force since it is not symmetric. Consider the interaction
between two particles, $r_i$ and $r_j$. Using $(5)$ and the property of the smoothing kernel $\nabla W(-r,h) = -\nabla W(r,h)$,
Newton's third law implies

$$ \begin{aligned}
& -\nabla P(r_i) = \nabla P(r_j) \\
\implies & -m_j \frac{P_j}{\rho_j} \nabla W(r_i - r_j) = -m_i \frac{P_i}{\rho_i} \nabla W(r_i - r_j) \\
\implies & \frac{P_j}{\rho_j} = \frac{P_i}{\rho_i} \\
\implies & \frac{k(\rho_j - \rho_0)}{\rho_j} = \frac{k(\rho_i - \rho_0)}{\rho_i} \\
\implies & \rho_j = \rho_i,
\end{aligned} $$

which are not guaranteed to be equal. In order to remedy this, use the following substitution for equation $(5)$ that uses the mean
between the two pressure values to force symmetry.

$$-\nabla P(r_i) = -\sum_jm_j\frac{P_i + P_j}{2\rho_j}\nabla W(r_i-r_j, h), \tag{6}$$

## Interaction Force

The title of this article contains the word "interactive," so let's add some interaction to make the simulation
fun to play with. The force will fall off with distance and strongly damp particle motion inside the radius.

```odin
calculate_interaction_force  :: proc(particle_idx: u32,
    particle_positions, particle_velocities: [][2]f32,
    sim_state: ^FluidSimState) -> [2]f32 {

    interaction_acceleration: [2]f32 = 0
    if sim_state.mouse_captured &&
        (sim_state.mouse_left_down || sim_state.mouse_right_down) {

        // RMB pulls the particles in, LMB pushes them away
        interaction_strength := sim_state.mouse_right_down ?
            sim_state.interaction_strength :
            -sim_state.interaction_strength
        interaction_radius := sim_state.interaction_radius

        // Hand is interacting, so find vector from hand to particle & its sq distance
        particle_to_hand := sim_state.mouse_pos - particle_positions[particle_idx]
        sqr_dst          := linalg.dot(particle_to_hand, particle_to_hand)

        // If particle is in interaction radius, change acceleration on particle
        if sqr_dst > 0 && sqr_dst < interaction_radius * interaction_radius {
            dst             := math.sqrt(sqr_dst)
            center_factor   := 1 - dst / interaction_radius
            direction       := particle_to_hand / dst // normalize
            interaction_acceleration += center_factor *
                (direction * interaction_strength - particle_velocities[particle_idx])
        }
    }
    return interaction_acceleration
}
```

Holding the right mouse button down will pull the particles towards the mouse; holding the left mouse button
will push them away.

# Smoothing Kernels

Following (Müller, Charypar, and Gross 2003), we use two different smoothing kernels.

$$ W_{poly6}(r,h)=\frac{315}{\pi 64h^9}\begin{cases}
(h^2-r^2)^3, & |r|\le h\\
0, & \text{otherwise}
\end{cases} $$

is smooth and reminiscent of a Gaussian. This kernel should be used for quantities that should be similar in value when close together,
such as viscosity. It also has the benefit of only using $r^2$, which means we can avoid a square root.

$$ W_{spiky}(r,h)=\frac{15}{\pi h^6}\begin{cases}
(h-r)^3, & |r|\le h\\
0, & \text{otherwise}
\end{cases} $$

is a sharp kernel with a high derivative close to its center. This will cause the values near the center to be large with
a very high rate of change, making the spiky kernel a much better candidate for something like pressure force,
which depends on the derivative of the kernel. $W_{poly6}$, which has a zero derivative at its center, would fail to effectively repel
particles that are close together.

![The smooth polynomial spline kernel and the spiky kernel](/images/kernels2_auto.svg)

# Spatial Partitioning

As it stands, there are several $\mathcal{O}(n^2)$ loops occurring each update, where $n$ is the number of particles. When we interpolate a
quantity using equation $(1)$, the only particles that contribute to the final value $A_S(r)$ are the ones that are within a distance $h$
of the query point $r_{query}$. We only need to loop through the particles that satisfy $|r_{particle} - r_{query}| \le h$.

To easily determine these particles, we will divide the domain into a uniform grid, where each grid cell is of size $h$. Once we do this, we
know that the only particles we need to loop through are the ones which lie in cells adjacent to the query point's cell.

![Only the particles in red located in the 9 cells surrounding the query point are looped over. The rest are ignored](/images/spatial_partitioning_auto.svg)

The only particles for which the smoothing function may be nonzero are the ones colored in red, which reside in cells in or adjacent to the
query point's cell.

To get this partitioned space, a hash function that maps each cell to an integer must be defined. Every update, we will keep track of each
particle’s cell hash and sort the particles based on their cell hash values. This ensures that we can access particles in the same
cell to be looped over quickly and easily.

```odin
update_spatial_lookup :: proc(positions: [][2]f32, sim_state: ^FluidSimState) {
    count               := sim_state.particle_count
    hash_size           := sim_state.hash_size
    cell_size           := sim_state.density_smoothing_radius
    spatial_lookup      := sim_state.spatial_lookup[:count]
    sorted_indices      := sim_state.sorted_particle_index[:count]
    cell_prefix_sum     := sim_state.cell_prefix_sum[:hash_size + 1]
    cell_particle_count := sim_state.cell_particle_count[:hash_size]

    // Capture the grid hash for each particle, keep a histogram of grid hashes
    slice.zero(cell_particle_count)
    for i in 0..<count {
        grid_cell_hash := hash_grid_cell(
            get_grid_cell(positions[i], cell_size), sim_state.hash_mask)
        // Each particle has a spatial lookup value in the form of a hash of the
        // grid cell index. The particles will be sorted based on their grid_cell_hash
        // so that particles in the same cell are adjacent in the array.
        spatial_lookup[i] = grid_cell_hash
        cell_particle_count[grid_cell_hash] += 1
    }

    // Sort using counting sort
    running_total: u32 = 0
    for k in 0..<hash_size {
        cell_prefix_sum[k] = running_total
        cell_particle_count[k] = cell_prefix_sum[k]
        running_total += cell_particle_count[k]
    }
    cell_prefix_sum[hash_size] = running_total
    for i in 0..<count {
        sorted_slot := cell_particle_count[spatial_lookup[i]]
        cell_particle_count[spatial_lookup[i]] += 1
        sorted_indices[sorted_slot] = i
    }
}
```

The above structure allows us to loop through the neighboring cells with ease. We simply find the index of the sorted lookup table that points to
the first particle in the cell, then traverse the table to find the remaining particles.
```odin
NeighborhoodIterator :: struct {
    sim_state:              ^FluidSimState,
    position:               [2]f32,
    particle_positions:     [^][2]f32,
    grid_cell:              [2]i32,
    smoothing_radius_sq:    f32,
    offset_idx:             int,
    offset_count:           int,
    particle_index:         u32,
    last_particle_in_cell:  u32,
}

neighborhood_iterator_next :: proc(it: ^NeighborhoodIterator) ->
    (dist: [2]f32, particle_index: u32, ok: bool) {

    sim_state := it.sim_state
    for {
        if it.particle_index >= it.last_particle_in_cell { // cell has been exhausted
            it.offset_idx += 1 // Next neighboring cell
            if it.offset_idx >= it.offset_count do return {}, 0, false

            // Find the integer offset for the neighboring cells
            offset: [2]i32
            rem := it.offset_idx
            for i in 0..<2 {
                offset[i] = i32(rem % 3) - 1
                rem /= 3
            }
            key := hash_grid_cell(it.grid_cell + offset, sim_state.hash_mask)
            it.particle_index        = sim_state.cell_prefix_sum[key] // start index
            it.last_particle_in_cell = sim_state.cell_prefix_sum[key + 1] // end index
            continue
        }

        particle_index = sim_state.sorted_particle_index[it.particle_index]
        it.particle_index += 1
        dist = it.particle_positions[particle_index] - it.position
        square_dst := linalg.dot(dist, dist) // To avoid sqrt
        if square_dst <= it.smoothing_radius_sq {
            return dist, particle_index, true
        }
    }
}
```

Looping through particles is now much quicker, achieving an algorithmic complexity increase from $\mathcal{O}(n^2)$
to $\mathcal{O}(nk_{avg})$, where $k_{avg}$ is the average number of particles in each particle's neighborhood.

It should be noted that there is a drawback to this approach. Two different cells can hash to the same key, meaning that
we might double count if this happens to two neighboring cells. For a sufficiently large hash table, this should be rare,
but you could check for this if desired.

## Computing Fluid Quantities

Computing the fluid's physical quantities now looks like:

```odin
calculate_density :: proc(particle_idx: u32, particle_positions: [][2]f32,
        sim_state: ^FluidSimState) -> f32 {

    density: f32 = 0.0
    iter := neighborhood_iterator_get(sim_state, particle_idx)
    dist, _, ok := neighborhood_iterator_next(&iter)
    for ok {
        dist_sq := linalg.dot(dist, dist)
        density += kernel_spiky(dist_sq, sim_state.density_smoothing_radius)

        dist, _, ok = neighborhood_iterator_next(&iter)
    }

    return density
}

calculate_pressure_force :: proc(particle_idx: u32, particle_positions: [][2]f32,
        densities: []f32, sim_state: ^FluidSimState) -> [2]f32 {

    force: [2]f32 = 0
    iter := neighborhood_iterator_get(sim_state, particle_idx)
    dist, index, ok := neighborhood_iterator_next(&iter)
    for ok {
        // Particle does not contribute to its own pressure force
        if (index == particle_idx) {
            dist, index, ok = neighborhood_iterator_next(&iter)
            continue
        }
        dist_sq := linalg.dot(dist, dist)

        // If particles occupy the same location, select a random force direction
        dir := dist_sq == 0 ? get_random_dir() : dist / linalg.length(dist)
        pressure := 0.5 *
            ((densities[index] - rest_density) * pressure_constant +
            (densities[particle_idx] - rest_density) * pressure_constant)
        force += pressure * dir *
            kernel_spiky_derivative(dist_sq, sim_state.density_smoothing_radius)
            / densities[index]

        dist, index, ok = neighborhood_iterator_next(&iter)
    }

    return force
}
```

# Updating Positions

We are ultimately interested in how the particles of the system are moving according to the forces of the fluid.
By this point, we have found the acceleration of each particle by calculating the total force acting on it. All
that remains is integrating to find velocity and position. We are solving the following system of differential equations.

$$ \begin{aligned}
    a &= \frac{dv}{dt} \tag{7}\\
    v &= \frac{dr}{dt}
\end{aligned} $$

The simplest way to solve these equations is by Euler's method. Given some differential equation

$$ \frac{dy}{dt} = f(t, y(t)), $$

we can approximate the solution with a sufficiently small step-size $\Delta t$ by

$$ \begin{aligned}
    y_{n+1} &= y_n + \Delta tf(t_n, y(t_n)) \\
    t_n &= \Delta tn
\end{aligned} $$


which we can apply to $(7)$.

$$ \begin{aligned}
    v_{n+1} &= v_n + \Delta ta(t_n,v_n,r_n) \\
    r_{n+1} &= r_n + \Delta tv_{n+1}
\end{aligned} $$

While this method is simple and fast, it also requires a very small timestep to be sufficiently stable and
accurate. One improvement we could make is to use the Improved Euler method (also known as Heun's method) which uses the
above Euler prediction in the differential equation itself to find a better prediction.

$$ \begin{aligned}
    \tilde y_{n+1} &= y_n + \Delta tf(t_n, y(t_n)) \\
    y_{n+1} &= y_n + \frac{\Delta t}{2}[f(t_n, y(t_n)) + f(t_{n+1}, \tilde y_{n+1})]
\end{aligned} $$

Improved Euler involves finding a new set of positions and velocities, updating their spatial partitioning, and
finding the density and new accelerations. This achieves a much more stable solution, allowing for
fewer substeps at the cost of twice as much computation. For our application, however, finding $f(t,y(t))$ is the most
computationally expensive part, so we would like to avoid doing it twice.

Instead, we perform a simpler prediction step. We make a prediction of the velocities and positions solely based on gravity (and maybe viscosity in the future)
before finding the densities. The resulting solution may not be completely accurate, but it is much more stable and is sufficient for our
goal of making pretty pictures.^[In the future, I would like to explore (Solenthaler and Pajarola 2009) for a better alternative.]

$$ \begin{aligned}
    v_{n+1}' &= v_n + \Delta tg \\
    r_{n+1}' &= r_n + \Delta tv_{n+1}'
\end{aligned} $$

These predictions are then used in finding density, pressure, and acceleration. Once the real acceleration is found, it is used
to integrate the real updated velocities and positions, just like in regular old Euler's method.

$$ \begin{aligned}
    v_{n+1} &= v_n + \Delta ta(t_n,v_{n+1}',r_{n+1}') \\
    r_{n+1} &= r_n + \Delta tv_{n+1}
\end{aligned} $$

# The Complete Update Step


Bringing everything together, the complete update step is shown below. This step could be parallelized across multiple CPU cores and/or by using SIMD
over the particle count. Synchronization should occur before and after spatial hash updating, after computing the density, and before the beginning the
next update.

```odin
calculate_acceleration :: proc(particle_idx: u32, particle_positions,
        particle_velocities: [][2]f32, sim_state: ^FluidSimState) -> [2]f32 {

    // Apply interaction force from the mouse
    interaction_acceleration :=
        calculate_interaction_force(particle_idx, particle_positions,
        particle_velocities, sim_state)

    // Get the pressure force and convert it to acceleration by dividing density
    pressure_acceleration :=
        calculate_pressure_force(particle_idx, particle_positions,
        sim_state.density, sim_state) / sim_state.density[particle_idx]

    gravity_dir: [2]f32
    gravity_dir.y = -1
    gravity_acceleration := gravity_dir * sim_state.gravity

    return interaction_acceleration + pressure_acceleration + gravity_acceleration
}

update_step :: proc(sim_state: ^FluidSimState) {
    sub_dt := sim_state.time_step / f32(sim_state.n_substeps)
    for _ in 0..<sim_state.n_steps_per_update {
        for _ in 0..<sim_state.n_substeps {
            // Compute acceleration due to gravity to make position predictions
            gravity_dir: [2]f32
            gravity_dir.y = -1
            gravity_acceleration := gravity_dir * sim_state.gravity
            for i in 0..<sim_state.particle_count {
                sim_state.velocity2[i] =
                    sim_state.velocity[i] + sub_dt * gravity_acceleration
                sim_state.position2[i] =
                    sim_state.position[i] + sub_dt * sim_state.velocity2[i]
            }

            // Update spatial lookup table
            update_spatial_lookup(sim_state.position2, sim_state)

            // Calculate particle densities
            calculate_all_densities(sim_state.position2, sim_state)

            // calculate acceleration, then integrate to find final vel and pos
            for i in 0..<sim_state.particle_count {
                sim_state.acceleration[i] = calculate_acceleration(i,
                    sim_state.position2, sim_state.velocity2, sim_state)
                sim_state.velocity[i] += sub_dt * sim_state.acceleration[i]
                sim_state.position[i] += sub_dt * sim_state.velocity[i]
            }

            // resolve particle-boundary collisions
            // (If the particles leave the boundary, simply move them back in)
            resolve_boundary_collisions(sim_state)
        }
    }
}

```

# Conclusion

After dividing the computation across my desktop's 16 cores, I was able to get about 50,000 particles
running at above 100 frames per second. Considering this is only utilizing a CPU, I am surprised by how fast and stable it is.

![Lots and lots of particles (50,000 of them), oh my!](/videos/final_demo.mp4)

I plan to continue this project, as I had a lot of fun building it. My next goals are to move the simulation to the GPU using compute
shaders, add a third dimension, and render the fluid properly so I can finally stop looking at just these dots.

Beyond that, I would also like to support rigid body interactions so I can experiment with making a boat and convert the project into a little game.

This is my first technical blog post ever; I have been wanting to start this for a long time. Thanks for checking it out and stay tuned for more! :)



