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
cell hash for each particle. The particles are then sorted based on their cell's hash value. This ensures that particles in the same cell are
grouped together in memory and can be looped over quick and easy.

<!-- For example, -->
<!-- $W_{spikey}(r,h)=\frac{15}{\pi h^6}\begin{cases} -->
<!-- (h-r)^2, & |r|\le h\\ -->
<!-- 0, & \text{otherwise} -->
<!-- \end{cases}$ -->
<!-- ![A cubic spline kernel and its gradient](/images/kernels.svg)] with radius $h$ which satisfies -->
