+++
title = "Interactive Real-Time Fluid Simulator"
template = "article"
date = "August 21, 2026"
description = "Real-time, particle-based fluid simulation implementation."
draft = true
+++

*{{date}}*
# {{title}}

This is an overview of my implementation of a particle-based fluid simulation using the smoothed-particle hydrodymanics (SPH) method.
This version is implemented using multiple cores on the CPU via multithreading. The visualization is done using my own [renderering engine](/articles/renderer.html)
The program is written in [Odin](https://odin-lang.org/).^[Odin is a C-like systems programming language. It aims to keep the control and simplicity of C while
providing modern conveniences. It's a blast to use for graphics and game programming.]

I decided to go with the SPH method because it is fast, relatively stable, and straightforward to parallelize.

TODO: Add reference videos and papers, put a demo video front and center to grab attention

# Smoothed-Particle Hydrodynamics

Smoothed-Particle Hydrodynamics (SPH) was initially developed for astrophysics. It provides a way to interpolate field quantities of a fluid
using values carried by discrete particles. Each particle has a position, velocity, and acceleration, which are determined by the fluid itself,
along with quantities relevant to the fluid equation like density or viscosity. The total field value may be found at any
point in space by interpolating each particle's contribution weighted by a smoothing kernel. In mathematical notation, for a query point $r$, a scalar field
quantity $A$ can be represented as

$$A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}W(r-r_j, h), \tag{1}$$

where $j$ loops over each particle and $m_j, A_j, \rho_j, r_j$ are the mass, field quantity, density, and position of particle $j$, respectively.
$W(r,h)$ is the smoothing kernel.^[There are many options for the smoothing kernel, and you need not use the same one across all quantities. Some
kernels may behave more favorably towards certain quantities.]

The smoothing kernel should be radially symmetric, with finite support defined by radius $h$.^[$W(r,h)$ will only be nonzero when $0 \le r \le h$]
It should also be normalized^[$\int_{\Omega} W(r,h)dr = 1$, where $\Omega$ is the entire domain of the simulation.] and ideally should have vanishing
values and derivatives at the boundary $h$ so as not to have discontinuities as particles leave and enter its radius.

For our case, let's assume mass is constant between particles, $m_j = 1$, so we can forget about it. That leaves us
with finding $\rho_j$, which we can do using equation $(1)$ itself.

$$\rho_S(r) = \sum_j m_j \frac{\rho_j}{\rho_j}W(r-r_j,h) = \sum_j m_j W(r-r_j,h)$$

You can begin to see the mechanism of the method. There is one more crucial property of $(1)$: we can use it to find the gradient or Laplacian of our
desired quantities as well.

$$ \nabla A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}\nabla W(r-r_j, h) \tag{2} $$

$$ \nabla^2 A_S(r) = \sum_jm_j\frac{A_j}{\rho_j}\nabla^2 W(r-r_j, h) \tag{3} $$

Now let's take a look at the equation to be solved.

# The Navier-Stokes Equation

Fluids are governed by a set of partial differential equations called the Navier-Stokes equations. For our not-so-lofty aspirations of making
pretty pictures, we will concern ourselves with the following simplified version of the primary equation:

$$ \rho\frac{dv}{dt} = -\nabla P + \rho g + \mu \nabla^2 v + \textbf{f}_e \tag{4} $$

Here, $-\nabla P$ is the pressure force, $\rho g$ is the gravitational force, $\mu \nabla^2 v$ is the viscosity term,^[This implementation ignores
the viscosity term $\mu \nabla^2 v$ for now, since it can cause the problem to become ill-posed. It will be tackled in a later post.] and $\bf{f}_e$ are the
external forces. If you look closely, and note that $\rho$ is a mass analogue, you can see Newton's Second Law in disguise.

$$ \begin{aligned}
         F_{total} &= -\nabla P + \rho g + \mu \nabla^2 v + \textbf{f}_e \\
                 a &= \frac{dv}{dt} \\
\implies F_{total} &= \rho a
\end{aligned} $$

Each step of the simulation, we are trying to find the acceleration of each particle, with which we may obtain the velocity and position of
the particle by integration.

## Pressure Force

Just like with density, use equation $(1)$ to find the pressure force.

$$ -\nabla P(r) = -\sum_jm_j\frac{P_j}{\rho_j}\nabla W(r-r_j, h) \tag{5} $$

There are a couple of things to consider here. First, $P_j$ may be obtained from the ideal gas equation
$$P_j = \rho_j k,$$
where $k$ is a constant. It is common to use a modification of the ideal gas equation
$$P_j = k(\rho_j - \rho_0),$$
which has no effect on the pressure force since it is a gradient, but is more numerically stable.

Second, equation $(5)$ does not accurately compute the pressure force since it is not symmetric. Consider the interaction
between two particles, $r_i$ and $r_j$. Using $(5)$ and the properties of the smoothing kernel $W$,^[$\nabla W(0, h) = 0$
and $\nabla W(-r,h) = -\nabla W(r,h)$] Newton's Third Law says

$$ \begin{aligned}
& -\nabla P(r_i) = \nabla P(r_j) \\
\implies & -m_j \frac{P_j}{\rho_j} \nabla W(r_i - r_j) = -m_i \frac{P_i}{\rho_i} \nabla W(r_i - r_j) \\
\implies & \frac{P_j}{\rho_j} = \frac{P_i}{\rho_i} \\
\implies & \frac{k(p_j - p_0)}{\rho_j} = \frac{k(p_j - p_0)}{\rho_i} \\
\implies & \rho_j = \rho_i
\end{aligned} $$

which are not guaranteed to be equal. In order to remedy this, use the following substitution for equation $(5)$ that uses the mean
between the two pressure values to force symmetry.

$$-\nabla P(r_i) = -\sum_jm_j\frac{P_i + P_j}{2\rho_j}\nabla W(r-r_j, h), \tag{6}$$

## Interaction Force

The title of this article contains the word "interactive," so let's add some interaction to make the simulation
fun to play with.

```odin
calculate_interaction_force  :: proc(particle_idx: u32,
    particle_positions, particle_velocities: [][$N]f32,
    sim_state: ^FluidSimState(N)) -> [N]f32 {

    interaction_acceleration: [N]f32 = 0
    if sim_state.mouse_captured &&
        (sim_state.mouse_left_down || sim_state.mouse_right_down) {

        // RMB pulls the particles in, LMB button pushes them away
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

Holding the right mouse button down will pull the particles towards the mouse, holding the left mouse button
will push them away. This is done by just adding an acceleration vector toward or away from the mouse scaled
by an interaction force constant and the distance from the mouse pointer.

# Smoothing Kernels

Following the [reference paper](https://matthias-research.github.io/pages/publications/sca03.pdf), we use
two different smoothing kernels.

$$ W_{poly6}(r,h)=\frac{315}{\pi 64h^9}\begin{cases}
(h^2-r^2)^3, & |r|\le h\\
0, & \text{otherwise}
\end{cases} $$

is smooth and reminicient of a Gaussian, and is used for all instances of equation $(1)$ except those having
to do with pressure force. It also has the benefit of only haaving $r^2$ present, which means we can avoid
a square root.

$$ W_{spiky}(r,h)=\frac{15}{\pi h^6}\begin{cases}
(h-r)^2, & |r|\le h\\
0, & \text{otherwise}
\end{cases} $$

is a sharp kernel with a high derivative close to its center. For pressure force computation we want the
influence of very close particles to be much higher to avoid particles clustering together, hence we use
the spiky kernel.

![The smooth polynomial spline kernel and the spiky kernel](/images/kernels_auto.svg)

# Spatial Partitioning

As it stands, there are several $\mathcal{O}(N^2)$ loops happening each update, where $N$ is the number of particles. When we interpolate a
quantity using equation $(1)$, the only particles that contribute to the final value $A_S(r)$ are the ones that are within a distance $h$
to the query point $r_{query}$. We only need to loop through the particles that satisfy $|r_{particle} - r_{query}| \le h$.

To easily determine these particles, we will divide the domain into a uniform grid, where each grid cell is of size $h$. Once we do this, we
know that the only particles we need to loop through are the ones which lie in cells adjacent to the query point's cell.

![Only the particles in red located in the 9 cells surrounding the query point are looped over. The rest are ignored](/images/spatial_partitioning_auto.svg)

The only particles for which the smoothing function may be nonzero are the ones colored in red, which reside in cells in or adjacent to the
query point's cell.

To get this partitioned space, we first define a hash function that maps each cell to an integer. Every update step, we will keep track of the
cell hash for each particle. The particles are then sorted based on their cell's hash value. This ensures that we can access particles in the same
cell to be looped over quick and easy. Here is what the code looks like for building this spatial hashing:

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

This structure allows us to loop through the neighboring cells with ease. We simply find the index of the sorted lookup table that points to
the first particle in the cell, then traverse through the sorted lookup table to find the rest of the particles. Here is the code for traversal
over all particles in each neighbor:

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

This results in an algorithmic improvement from $\mathcal{O(n^2)}$ to $\mathcal{O(nk_{avg})}$, where $k_{avg}$
is the average number of particles in each particle's neighborhood. There are certainly a few more improvements
that could be made to this part of the algorithm to further increase speed-up. As of now, the particle positions
do not change location when sorted, only their indices, so we do not benefit from a hot cache. Perhaps there is
a different data structure we could use to help with this, but that will have to be left for a later post.

# Updating Positions

We are ultimately interested in how the particles of the system are moving according to the forces of the fluid.
By this point, we have found the acceleration of each particle by calculating the total force acting on it and all
that is left is to integrate to find velocity and position. We are solving the following set of differential equations:

$$ \begin{aligned}
    a &= \frac{dv}{dt} \tag{7}\\
    v &= \frac{dr}{dt}
\end{aligned} $$

The simplest way to solve these equations is by Euler's method. Given some differential equation

$$ \frac{dy}{dt} = f(t, y(t)), $$

we can approximate the solution with a sufficiently small step-size $h$ by

$$ \begin{aligned}
    y_{n+1} &= y_n + hf(t_n, y(t_n)) \\
    t_n &= hn
\end{aligned} $$


Which we can apply to $(7)$.

$$ \begin{aligned}
    v_{n+1} &= v_n + ha(t_n,v_n,r_n) \\
    r_{n+1} &= r_n + hv_{n+1}
\end{aligned} $$

While this method is simple and fast, it also requires a very small time step to be sufficiently stable and
accurate. One improvement we can make is to use Improved Euler (also known as Heun's Method) which uses the
above Euler prediction in the differential equation itself to find a better prediction.

$$ \begin{aligned}
    \tilde y_{n+1} &= y_n + hf(t_n, y(t_n)) \\
    y_{n+1} &= y_n + \frac{h}{2}[f(t_n, y(t_n)) + f(t_{n+1}, \tilde y_{n+1})]
\end{aligned} $$

Improved Euler means finding a new set of positions and velocities, updating their spatial partitioning,
finding the density, and the new accelerations. It’s twice the amount of work, but it is much more stable
than just using Euler, and we can get away with way fewer substeps to the simulation to maintain stability.^[
In the future, I would like to explore [potentially better alternatives](https://dl.acm.org/doi/10.1145/1576246.1531346?__cf_chl_tk=FmRE4skdUxTty3c2JwixiYNQVAOnlfa6XghKBm_172g-1788030003-1.0.1.1-MzrU81LqlD9GkAfv3jCGpnukY1lasNbuBbndQxVdook)
to finding the implicit step than just computing all the quantities multiple times. A faster prediction
might allow us to use an even better integration algorithm like [RK4](https://en.wikipedia.org/wiki/Runge%E2%80%93Kutta_methods).]


# The Complete Update Step

Putting everything together, here is the complete update step. Using multiple threads allows you to split up each particle on a different core,
but still requires you to communicate before sorting twice each step.

```odin
update_step :: proc(sim_state: ^FluidSimState) {
    sub_dt := sim_state.time_step / f32(sim_state.n_substeps)
    half_dt := sub_dt * 0.5
    for _ in 0..<sim_state.n_steps_per_update {
        for _ in 0..<sim_state.n_substeps {
            // Update spatial lookup table
            update_spatial_lookup(sim_state.position, sim_state)

            // Calculate particle densities
            calculate_all_densities(sim_state.position, sim_state)

            // Get acceleration
            sim_state.acceleration = calculate_all_accelerations(sim_state.position,
                sim_state.velocity, sim_state)

            // Use Euler's Method to get velocity and position predictions
            for i in 0..<sim_state.particle_count {
                sim_state.velocity_prediction[i] =
                    sim_state.velocity[i] + sub_dt * sim_state.acceleration[i]
                sim_state.position_prediction[i] =
                    sim_state.position[i] + sub_dt * sim_state.velocity_prediction[i]
            }

            // Update spatial lookup for predicted particles array
            update_spatial_lookup(sim_state.position_prediction, sim_state)

            // Calculate particle densities for particle predictions
            calculate_all_densities(sim_state.position_prediction, sim_state)

            // Get acceleration again..
            sim_state.acceleration_prediction = calculate_all_accelerations(
                sim_state.position_prediction, sim_state.velocity_prediction,
                sim_state)

            // Implicit Euler step. Use predictions to find actual next pos and vel
            for i in 0..<sim_state.particle_count {
                sim_state.velocity[i] += half_dt *
                    (sim_state.acceleration[i] + sim_state.acceleration_prediction[i])
                sim_state.position[i] += half_dt *
                    (sim_state.velocity[i] + sim_state.velocity_prediction[i])
            }

            // resolve particle-boundary collisions
            resolve_boundary_collisions(sim_state)
        }
    }
}

```

